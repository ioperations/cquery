#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"

namespace {
MethodType k_method_type = "textDocument/typeDefinition";

struct InTextDocumentTypeDefinition : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    LsTextDocumentPositionParams params;
};
MAKE_REFLECT_STRUCT(InTextDocumentTypeDefinition, id, params);
REGISTER_IN_MESSAGE(InTextDocumentTypeDefinition);

struct HandlerTextDocumentTypeDefinition
    : BaseMessageHandler<InTextDocumentTypeDefinition> {
    MethodType GetMethodType() const override { return k_method_type; }
    void Run(InTextDocumentTypeDefinition* request) override {
        QueryFile* file;
        if (!FindFileOrFail(db, project, request->id,
                            request->params.text_document.uri.GetAbsolutePath(),
                            &file, nullptr)) {
            return;
        }
        WorkingFile* working_file =
            working_files->GetFileByFilename(file->def->path);

        OutLocationList out;
        out.id = request->id;
        for (QueryId::SymbolRef sym : FindSymbolsAtLocation(
                 working_file, file, request->params.position)) {
            AnyId id = sym.id;
            switch (sym.kind) {
                case SymbolKind::Var: {
                    const QueryVar::Def* def = db->GetVar(sym).AnyDef();
                    if (!def || !def->type) continue;
                    id = *def->type;
                }
                    // fallthrough
                case SymbolKind::Type: {
                    QueryType& type = db->GetType({id, SymbolKind::Type});
                    for (const auto& def : type.def)
                        if (def.spell) {
                            if (auto ls_loc = GetLsLocation(db, working_files,
                                                            *def.spell))
                                out.result.push_back(*ls_loc);
                        }
                    break;
                }
                default:
                    break;
            }
        }

        QueueManager::WriteStdout(k_method_type, out);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentTypeDefinition);

}  // namespace
