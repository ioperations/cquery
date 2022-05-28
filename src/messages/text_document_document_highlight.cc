#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"
#include "symbol.h"

namespace {
MethodType kMethodType = "textDocument/documentHighlight";

struct In_TextDocumentDocumentHighlight : public RequestInMessage {
    MethodType GetMethodType() const override { return kMethodType; }
    LsTextDocumentPositionParams params;
};
MAKE_REFLECT_STRUCT(In_TextDocumentDocumentHighlight, id, params);
REGISTER_IN_MESSAGE(In_TextDocumentDocumentHighlight);

struct Out_TextDocumentDocumentHighlight
    : public LsOutMessage<Out_TextDocumentDocumentHighlight> {
    LsRequestId id;
    std::vector<LsDocumentHighlight> result;
};
MAKE_REFLECT_STRUCT(Out_TextDocumentDocumentHighlight, jsonrpc, id, result);

struct Handler_TextDocumentDocumentHighlight
    : BaseMessageHandler<In_TextDocumentDocumentHighlight> {
    MethodType GetMethodType() const override { return kMethodType; }
    void Run(In_TextDocumentDocumentHighlight* request) override {
        QueryId::File file_id;
        QueryFile* file;
        if (!FindFileOrFail(db, project, request->id,
                            request->params.text_document.uri.GetAbsolutePath(),
                            &file, &file_id)) {
            return;
        }

        WorkingFile* working_file =
            working_files->GetFileByFilename(file->def->path);

        Out_TextDocumentDocumentHighlight out;
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

        QueueManager::WriteStdout(kMethodType, out);
    }
};
REGISTER_MESSAGE_HANDLER(Handler_TextDocumentDocumentHighlight);
}  // namespace
