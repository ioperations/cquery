#include <loguru.hpp>

#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"

namespace {
MethodType k_method_type = "textDocument/implementation";

struct InTextDocumentImplementation : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    LsTextDocumentPositionParams params;
};
MAKE_REFLECT_STRUCT(InTextDocumentImplementation, id, params);
REGISTER_IN_MESSAGE(InTextDocumentImplementation);

struct HandlerTextDocumentImplementation
    : BaseMessageHandler<InTextDocumentImplementation> {
    MethodType GetMethodType() const override { return k_method_type; }

    void Run(InTextDocumentImplementation* request) override {
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
            if (sym.kind == SymbolKind::Type) {
                QueryType& type = db->GetType(sym);
                out.result = GetLsLocations(db, working_files,
                                            GetDeclarations(db, type.derived));
                break;
            }
            if (sym.kind == SymbolKind::Func) {
                QueryFunc& func = db->GetFunc(sym);
                out.result = GetLsLocations(db, working_files,
                                            GetDeclarations(db, func.derived));
                break;
            }
        }

        if (out.result.size() >= g_config->xref.maxNum)
            out.result.resize(g_config->xref.maxNum);
        QueueManager::WriteStdout(k_method_type, out);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentImplementation);
}  // namespace
