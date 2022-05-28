#include "lsp_code_action.h"
#include "message_handler.h"
#include "query_utils.h"
#include "queue_manager.h"

namespace {
MethodType k_method_type = "workspace/executeCommand";

struct InWorkspaceExecuteCommand : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    LsCommand<LsCodeLensCommandArguments> params;
};
MAKE_REFLECT_STRUCT(InWorkspaceExecuteCommand, id, params);
REGISTER_IN_MESSAGE(InWorkspaceExecuteCommand);

struct OutWorkspaceExecuteCommand
    : public LsOutMessage<OutWorkspaceExecuteCommand> {
    lsRequestId id;
    std::vector<LsLocation> result;
};
MAKE_REFLECT_STRUCT(OutWorkspaceExecuteCommand, jsonrpc, id, result);

struct HandlerWorkspaceExecuteCommand
    : BaseMessageHandler<InWorkspaceExecuteCommand> {
    MethodType GetMethodType() const override { return k_method_type; }
    void Run(InWorkspaceExecuteCommand* request) override {
        const auto& params = request->params;
        OutWorkspaceExecuteCommand out;
        out.id = request->id;
        if (params.command == "cquery._applyFixIt") {
        } else if (params.command == "cquery._autoImplement") {
        } else if (params.command == "cquery._insertInclude") {
        } else if (params.command == "cquery.showReferences") {
            out.result = params.arguments.locations;
        }

        QueueManager::WriteStdout(k_method_type, out);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerWorkspaceExecuteCommand);

}  // namespace
