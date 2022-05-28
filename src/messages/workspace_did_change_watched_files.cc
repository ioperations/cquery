#include <loguru/loguru.hpp>

#include "cache_manager.h"
#include "clang_complete.h"
#include "message_handler.h"
#include "project.h"
#include "queue_manager.h"
#include "working_files.h"

namespace {
MethodType k_method_type = "workspace/didChangeWatchedFiles";

enum class lsFileChangeType {
    Created = 1,
    Changed = 2,
    Deleted = 3,
};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
MAKE_REFLECT_TYPE_PROXY(lsFileChangeType);
#pragma clang diagnostic pop

struct LsFileEvent {
    LsDocumentUri uri;
    lsFileChangeType type;
};
MAKE_REFLECT_STRUCT(LsFileEvent, uri, type);

struct LsDidChangeWatchedFilesParams {
    std::vector<LsFileEvent> changes;
};
MAKE_REFLECT_STRUCT(LsDidChangeWatchedFilesParams, changes);

struct InWorkspaceDidChangeWatchedFiles : public NotificationInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    LsDidChangeWatchedFilesParams params;
};
MAKE_REFLECT_STRUCT(InWorkspaceDidChangeWatchedFiles, params);
REGISTER_IN_MESSAGE(InWorkspaceDidChangeWatchedFiles);

struct HandlerWorkspaceDidChangeWatchedFiles
    : BaseMessageHandler<InWorkspaceDidChangeWatchedFiles> {
    MethodType GetMethodType() const override { return k_method_type; }
    void Run(InWorkspaceDidChangeWatchedFiles* request) override {
        for (LsFileEvent& event : request->params.changes) {
            AbsolutePath path = event.uri.GetAbsolutePath();
            auto it = project->absolute_path_to_entry_index_.find(path);
            if (it == project->absolute_path_to_entry_index_.end()) continue;
            const Project::Entry& entry = project->entries[it->second];
            bool is_interactive =
                working_files->GetFileByFilename(entry.filename) != nullptr;
            switch (event.type) {
                case lsFileChangeType::Created:
                case lsFileChangeType::Changed: {
                    QueueManager::Instance()->index_request.Enqueue(
                        Index_Request(path, entry.args, is_interactive, nullopt,
                                      ICacheManager::Make()),
                        false /*priority*/);
                    if (is_interactive) clang_complete->NotifySave(path);
                    break;
                }
                case lsFileChangeType::Deleted:
                    QueueManager::Instance()->index_request.Enqueue(
                        Index_Request(path, entry.args, is_interactive,
                                      std::string(), ICacheManager::Make()),
                        false /*priority*/);
                    break;
            }
        }
    }
};
REGISTER_MESSAGE_HANDLER(HandlerWorkspaceDidChangeWatchedFiles);
}  // namespace
