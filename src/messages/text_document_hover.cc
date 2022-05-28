#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"

namespace {
MethodType k_method_type = "textDocument/hover";

// Find the comments for |sym|, if any.
optional<LsMarkedString> GetComments(QueryDatabase* db,
                                     QueryId::SymbolRef sym) {
    auto make = [](std::string_view comment) -> optional<LsMarkedString> {
        LsMarkedString result;
        result.value = std::string(comment.data(), comment.length());
        return result;
    };

    optional<LsMarkedString> result;
    WithEntity(db, sym, [&](const auto& entity) {
        if (const auto* def = entity.AnyDef()) {
            if (!def->comments.empty()) result = make(def->comments);
        }
    });
    return result;
}

// Returns the hover or detailed name for `sym`, if any.
optional<LsMarkedString> GetHoverOrName(QueryDatabase* db,
                                        const std::string& language,
                                        QueryId::SymbolRef sym) {
    auto make = [&](std::string_view comment) {
        LsMarkedString result;
        result.language = language;
        result.value = std::string(comment.data(), comment.length());
        return result;
    };

    optional<LsMarkedString> result;
    WithEntity(db, sym, [&](const auto& entity) {
        if (const auto* def = entity.AnyDef()) {
            if (!def->hover.empty())
                result = make(def->hover);
            else if (!def->detailed_name.empty())
                result = make(def->detailed_name);
        }
    });
    return result;
}

struct InTextDocumentHover : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    LsTextDocumentPositionParams params;
};
MAKE_REFLECT_STRUCT(InTextDocumentHover, id, params);
REGISTER_IN_MESSAGE(InTextDocumentHover);

struct OutTextDocumentHover : public LsOutMessage<OutTextDocumentHover> {
    struct Result {
        std::vector<LsMarkedString> contents;
        optional<LsRange> range;
    };

    LsRequestId id;
    optional<Result> result;
};
MAKE_REFLECT_STRUCT(OutTextDocumentHover::Result, contents, range);
MAKE_REFLECT_STRUCT_OPTIONALS_MANDATORY(OutTextDocumentHover, jsonrpc, id,
                                        result);

struct HandlerTextDocumentHover : BaseMessageHandler<InTextDocumentHover> {
    MethodType GetMethodType() const override { return k_method_type; }
    void Run(InTextDocumentHover* request) override {
        QueryFile* file;
        if (!FindFileOrFail(db, project, request->id,
                            request->params.text_document.uri.GetAbsolutePath(),
                            &file)) {
            return;
        }

        WorkingFile* working_file =
            working_files->GetFileByFilename(file->def->path);

        OutTextDocumentHover out;
        out.id = request->id;

        for (QueryId::SymbolRef sym : FindSymbolsAtLocation(
                 working_file, file, request->params.position)) {
            // Found symbol. Return hover.
            optional<LsRange> ls_range = GetLsRange(
                working_files->GetFileByFilename(file->def->path), sym.range);
            if (!ls_range) continue;

            optional<LsMarkedString> comments = GetComments(db, sym);
            optional<LsMarkedString> hover =
                GetHoverOrName(db, file->def->language, sym);
            if (comments || hover) {
                out.result = OutTextDocumentHover::Result();
                out.result->range = *ls_range;
                if (comments) out.result->contents.push_back(*comments);
                if (hover) out.result->contents.push_back(*hover);
                break;
            }
        }

        QueueManager::WriteStdout(k_method_type, out);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentHover);
}  // namespace
