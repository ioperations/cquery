#include <loguru.hpp>

#include "cache_manager.h"
#include "clang_complete.h"
#include "message_handler.h"
#include "project.h"
#include "queue_manager.h"
#include "timer.h"
#include "working_files.h"

namespace {
MethodType k_method_type = "workspace/didChangeConfiguration";

struct LsDidChangeConfigurationParams {
    bool placeholder;
};
MAKE_REFLECT_STRUCT(LsDidChangeConfigurationParams, placeholder);

struct InWorkspaceDidChangeConfiguration : public NotificationInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    LsDidChangeConfigurationParams params;
};
MAKE_REFLECT_STRUCT(InWorkspaceDidChangeConfiguration, params);
REGISTER_IN_MESSAGE(InWorkspaceDidChangeConfiguration);

struct HandlerWorkspaceDidChangeConfiguration
    : BaseMessageHandler<InWorkspaceDidChangeConfiguration> {
    MethodType GetMethodType() const override { return k_method_type; }
    void Run(InWorkspaceDidChangeConfiguration* request) override {
        Timer time;
        project->Load(g_config->projectRoot);
        time.ResetAndPrint("[perf] Loaded compilation entries (" +
                           std::to_string(project->entries.size()) + " files)");

        time.Reset();
        project->Index(QueueManager::Instance(), working_files, LsRequestId());
        time.ResetAndPrint(
            "[perf] Dispatched workspace/didChangeConfiguration index "
            "requests");

        clang_complete->FlushAllSessions();
        LOG_S(INFO) << "Flushed all clang complete sessions";
    }
};
REGISTER_MESSAGE_HANDLER(HandlerWorkspaceDidChangeConfiguration);
}  // namespace
