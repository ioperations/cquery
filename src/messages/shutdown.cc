#include "message_handler.h"
#include "queue_manager.h"

namespace {
MethodType k_method_type = "shutdown";

struct InShutdown : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
};
MAKE_REFLECT_STRUCT(InShutdown, id);
REGISTER_IN_MESSAGE(InShutdown);

struct OutShutdown : public LsOutMessage<OutShutdown> {
    lsRequestId id;
    JsonNull result;
};
MAKE_REFLECT_STRUCT(OutShutdown, jsonrpc, id, result);

struct HandlerShutdown : BaseMessageHandler<InShutdown> {
    MethodType GetMethodType() const override { return k_method_type; }
    void Run(InShutdown* request) override {
        OutShutdown out;
        out.id = request->id;
        QueueManager::WriteStdout(k_method_type, out);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerShutdown);
}  // namespace
