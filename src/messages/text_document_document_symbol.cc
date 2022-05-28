#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"

namespace {
MethodType kMethodType = "textDocument/documentSymbol";

struct lsDocumentSymbolParams {
    LsTextDocumentIdentifier textDocument;
};
MAKE_REFLECT_STRUCT(lsDocumentSymbolParams, textDocument);

struct In_TextDocumentDocumentSymbol : public RequestInMessage {
    MethodType GetMethodType() const override { return kMethodType; }
    lsDocumentSymbolParams params;
};
MAKE_REFLECT_STRUCT(In_TextDocumentDocumentSymbol, id, params);
REGISTER_IN_MESSAGE(In_TextDocumentDocumentSymbol);

struct Out_TextDocumentDocumentSymbol
    : public LsOutMessage<Out_TextDocumentDocumentSymbol> {
    lsRequestId id;
    std::vector<LsSymbolInformation> result;
};
MAKE_REFLECT_STRUCT(Out_TextDocumentDocumentSymbol, jsonrpc, id, result);

struct Handler_TextDocumentDocumentSymbol
    : BaseMessageHandler<In_TextDocumentDocumentSymbol> {
    MethodType GetMethodType() const override { return kMethodType; }
    void Run(In_TextDocumentDocumentSymbol* request) override {
        Out_TextDocumentDocumentSymbol out;
        out.id = request->id;

        QueryFile* file;
        QueryId::File file_id;
        if (!FindFileOrFail(db, project, request->id,
                            request->params.textDocument.uri.GetAbsolutePath(),
                            &file, &file_id)) {
            return;
        }

        for (QueryId::SymbolRef sym : file->def->outline) {
            optional<LsSymbolInformation> info =
                GetSymbolInfo(db, working_files, sym, true /*use_short_name*/);
            if (!info) continue;
            if (sym.kind == SymbolKind::Var) {
                QueryVar& var = db->GetVar(sym);
                auto* def = var.AnyDef();
                if (!def || !def->spell) continue;
                // Ignore local variables.
                if (def->spell->kind == SymbolKind::Func &&
                    def->storage != storage_class::Static &&
                    def->storage != storage_class::Extern)
                    continue;
            }

            if (optional<LsLocation> location = GetLsLocation(
                    db, working_files,
                    QueryId::LexicalRef(sym.range, sym.id, sym.kind, sym.role,
                                        file_id))) {
                info->location = *location;
                out.result.push_back(*info);
            }
        }

        QueueManager::WriteStdout(kMethodType, out);
    }
};
REGISTER_MESSAGE_HANDLER(Handler_TextDocumentDocumentSymbol);
}  // namespace
