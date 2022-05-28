#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"
#include "symbol.h"

namespace {
MethodType k_method_type = "textDocument/documentHighlight";

struct InTextDocumentDocumentHighlight : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    LsTextDocumentPositionParams params;
};
MAKE_REFLECT_STRUCT(InTextDocumentDocumentHighlight, id, params);
REGISTER_IN_MESSAGE(InTextDocumentDocumentHighlight);

struct OutTextDocumentDocumentHighlight
    : public LsOutMessage<OutTextDocumentDocumentHighlight> {
    LsRequestId id;
    std::vector<LsDocumentHighlight> result;
};
MAKE_REFLECT_STRUCT(OutTextDocumentDocumentHighlight, jsonrpc, id, result);

struct HandlerTextDocumentDocumentHighlight
    : BaseMessageHandler<InTextDocumentDocumentHighlight> {
    MethodType GetMethodType() const override { return k_method_type; }
    void Run(InTextDocumentDocumentHighlight* request) override {
        QueryId::File file_id;
        QueryFile* file;
        if (!FindFileOrFail(db, project, request->id,
                            request->params.text_document.uri.GetAbsolutePath(),
                            &file, &file_id)) {
            return;
        }

        WorkingFile* working_file =
            working_files->GetFileByFilename(file->def->path);

        OutTextDocumentDocumentHighlight out;
        out.id = request->id;

        for (QueryId::SymbolRef sym : FindSymbolsAtLocation(
                 working_file, file, request->params.position)) {
            // Found symbol. Return references to highlight.
            EachOccurrence(db, sym, true, [&](QueryId::LexicalRef ref) {
                if (ref.file != file_id) return;
                if (optional<LsLocation> ls_loc =
                        GetLsLocation(db, working_files, ref)) {
                    LsDocumentHighlight highlight;
                    highlight.range = ls_loc->range;
                    if (ref.role & role::Write)
                        highlight.kind = ls_document_highlight_kind::Write;
                    else if (ref.role & role::Read)
                        highlight.kind = ls_document_highlight_kind::Read;
                    else
                        highlight.kind = ls_document_highlight_kind::Text;
                    out.result.push_back(highlight);
                }
            });
            break;
        }

        QueueManager::WriteStdout(k_method_type, out);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentDocumentHighlight);
}  // namespace
