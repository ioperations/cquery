#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"

namespace {
MethodType k_method_type = "$cquery/vars";

struct InCqueryVars : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }

    LsTextDocumentPositionParams params;
};
MAKE_REFLECT_STRUCT(InCqueryVars, id, params);
REGISTER_IN_MESSAGE(InCqueryVars);

struct HandlerCqueryVars : BaseMessageHandler<InCqueryVars> {
    MethodType GetMethodType() const override { return k_method_type; }

    void Run(InCqueryVars* request) override {
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
        for (QueryId::SymbolRef sym : FindSymbolsAtLocation(
                 working_file, file, request->params.position)) {
            switch (sym.kind) {
                default:
                    break;
                case SymbolKind::Var: {
                    const QueryVar::Def* def = db->GetVar(sym).AnyDef();
                    if (!def || !def->type) continue;
                }
                // fallthrough
                case SymbolKind::Type: {
                    QueryType& type = db->GetType(sym);
                    out.result = GetLsLocations(
                        db, working_files, GetDeclarations(db, type.instances));
                    break;
                }
            }
        }
        QueueManager::WriteStdout(k_method_type, out);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerCqueryVars);
}  // namespace
