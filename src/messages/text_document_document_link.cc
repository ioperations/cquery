#include <loguru.hpp>

#include "lex_utils.h"
#include "message_handler.h"
#include "queue_manager.h"
#include "working_files.h"

namespace {
MethodType k_method_type = "textDocument/documentLink";

struct InTextDocumentDocumentLink : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    struct Params {
        // The document to provide document links for.
        LsTextDocumentIdentifier text_document;
    };
    Params params;
};
MAKE_REFLECT_STRUCT(InTextDocumentDocumentLink::Params, text_document);
MAKE_REFLECT_STRUCT(InTextDocumentDocumentLink, id, params);
REGISTER_IN_MESSAGE(InTextDocumentDocumentLink);

// A document link is a range in a text document that links to an internal or
// external resource, like another text document or a web site.
struct LsDocumentLink {
    // The range this link applies to.
    LsRange range;
    // The uri this link points to. If missing a resolve request is sent later.
    optional<LsDocumentUri> target;
};
MAKE_REFLECT_STRUCT(LsDocumentLink, range, target);

struct OutTextDocumentDocumentLink
    : public LsOutMessage<OutTextDocumentDocumentLink> {
    LsRequestId id;
    std::vector<LsDocumentLink> result;
};
MAKE_REFLECT_STRUCT(OutTextDocumentDocumentLink, jsonrpc, id, result);

struct HandlerTextDocumentDocumentLink
    : BaseMessageHandler<InTextDocumentDocumentLink> {
    MethodType GetMethodType() const override { return k_method_type; }
    void Run(InTextDocumentDocumentLink* request) override {
        OutTextDocumentDocumentLink out;
        out.id = request->id;

        if (g_config->showDocumentLinksOnIncludes &&
            !ShouldIgnoreFileForIndexing(
                request->params.text_document.uri.GetAbsolutePath())) {
            QueryFile* file;
            if (!FindFileOrFail(
                    db, project, request->id,
                    request->params.text_document.uri.GetAbsolutePath(),
                    &file)) {
                return;
            }

            WorkingFile* working_file = working_files->GetFileByFilename(
                request->params.text_document.uri.GetAbsolutePath());
            if (!working_file) {
                LOG_S(WARNING)
                    << "Unable to find working file "
                    << request->params.text_document.uri.GetAbsolutePath();
                return;
            }
            for (const IndexInclude& include : file->def->includes) {
                optional<int> buffer_line =
                    working_file->GetBufferPosFromIndexPos(include.line,
                                                           nullptr, false);
                if (!buffer_line) continue;

                // Subtract 1 from line because querydb stores 1-based lines but
                // vscode expects 0-based lines.
                optional<LsRange> between_quotes = ExtractQuotedRange(
                    *buffer_line, working_file->buffer_lines[*buffer_line]);
                if (!between_quotes) continue;

                LsDocumentLink link;
                link.target = LsDocumentUri::FromPath(include.resolved_path);
                link.range = *between_quotes;
                out.result.push_back(link);
            }
        }

        QueueManager::WriteStdout(k_method_type, out);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentDocumentLink);
}  // namespace
