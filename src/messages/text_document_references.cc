#include <loguru.hpp>

#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"

namespace {
MethodType k_method_type = "textDocument/references";

struct InTextDocumentReferences : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    struct LsReferenceContext {
        // Include the declaration of the current symbol.
        bool include_declaration;
        // Include references with these |Role| bits set.
        role role = role::All;
    };
    struct Params {
        LsTextDocumentIdentifier text_document;
        LsPosition position;
        LsReferenceContext context;
    };

    Params params;
};
MAKE_REFLECT_STRUCT(InTextDocumentReferences::LsReferenceContext,
                    include_declaration, role);
MAKE_REFLECT_STRUCT(InTextDocumentReferences::Params, text_document, position,
                    context);
MAKE_REFLECT_STRUCT(InTextDocumentReferences, id, params);
REGISTER_IN_MESSAGE(InTextDocumentReferences);

struct HandlerTextDocumentReferences
    : BaseMessageHandler<InTextDocumentReferences> {
    MethodType GetMethodType() const override { return k_method_type; }

    void Run(InTextDocumentReferences* request) override {
        QueryFile* file;
        if (!FindFileOrFail(db, project, request->id,
                            request->params.text_document.uri.GetAbsolutePath(),
                            &file)) {
            return;
        }

        WorkingFile* working_file =
            working_files->GetFileByFilename(file->def->path);

        OutLocationList out;
        out.id = request->id;

        for (const QueryId::SymbolRef& sym : FindSymbolsAtLocation(
                 working_file, file, request->params.position)) {
            // Found symbol. Return references.
            EachOccurrenceWithParent(
                db, sym, request->params.context.include_declaration,
                [&](QueryId::LexicalRef ref, ls_symbol_kind parent_kind) {
                    if (ref.role & request->params.context.role)
                        if (optional<LsLocation> ls_loc =
                                GetLsLocation(db, working_files, ref)) {
                            out.result.push_back(*ls_loc);
                        }
                });
            break;
        }

        if (out.result.empty())
            for (const IndexInclude& include : file->def->includes)
                if (include.line == request->params.position.line) {
                    // |include| is the line the cursor is on.
                    for (QueryFile& file1 : db->files)
                        if (file1.def)
                            for (const IndexInclude& include1 :
                                 file1.def->includes)
                                if (include1.resolved_path ==
                                    include.resolved_path) {
                                    // Another file |file1| has the same include
                                    // line.
                                    LsLocation result;
                                    result.uri = LsDocumentUri::FromPath(
                                        file1.def->path);
                                    result.range.start.line =
                                        result.range.end.line = include1.line;
                                    out.result.push_back(std::move(result));
                                    break;
                                }
                    break;
                }

        if ((int)out.result.size() >= g_config->xref.maxNum)
            out.result.resize(g_config->xref.maxNum);
        QueueManager::WriteStdout(k_method_type, out);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentReferences);
}  // namespace
