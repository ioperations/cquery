#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"

namespace {
MethodType k_method_type = "textDocument/documentSymbol";

struct LsDocumentSymbolParams {
    LsTextDocumentIdentifier text_document;
};
MAKE_REFLECT_STRUCT(LsDocumentSymbolParams, text_document);

struct InTextDocumentDocumentSymbol : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    LsDocumentSymbolParams params;
};
MAKE_REFLECT_STRUCT(InTextDocumentDocumentSymbol, id, params);
REGISTER_IN_MESSAGE(InTextDocumentDocumentSymbol);

struct OutTextDocumentDocumentSymbol
    : public LsOutMessage<OutTextDocumentDocumentSymbol> {
    LsRequestId id;
    std::vector<LsSymbolInformation> result;
};
MAKE_REFLECT_STRUCT(OutTextDocumentDocumentSymbol, jsonrpc, id, result);

struct HandlerTextDocumentDocumentSymbol
    : BaseMessageHandler<InTextDocumentDocumentSymbol> {
    MethodType GetMethodType() const override { return k_method_type; }
    void Run(InTextDocumentDocumentSymbol* request) override {
        OutTextDocumentDocumentSymbol out;
        out.id = request->id;

        QueryFile* file;
        QueryId::File file_id;
        if (!FindFileOrFail(db, project, request->id,
                            request->params.text_document.uri.GetAbsolutePath(),
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

        QueueManager::WriteStdout(k_method_type, out);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentDocumentSymbol);
}  // namespace
