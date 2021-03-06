#include <loguru/loguru.hpp>

#include "cache_manager.h"
#include "clang_complete.h"
#include "message_handler.h"
#include "project.h"
#include "queue_manager.h"
#include "working_files.h"

namespace {
MethodType k_method_type = "textDocument/didChange";

struct InTextDocumentDidChange : public NotificationInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    LsTextDocumentDidChangeParams params;
};

MAKE_REFLECT_STRUCT(InTextDocumentDidChange, params);
REGISTER_IN_MESSAGE(InTextDocumentDidChange);

struct HandlerTextDocumentDidChange
    : BaseMessageHandler<InTextDocumentDidChange> {
    MethodType GetMethodType() const override { return k_method_type; }

    void Run(InTextDocumentDidChange* request) override {
        AbsolutePath path = request->params.text_document.uri.GetAbsolutePath();
        working_files->OnChange(request->params);
        if (g_config->enableIndexOnDidChange) {
            WorkingFile* working_file = working_files->GetFileByFilename(path);
            Project::Entry entry = project->FindCompilationEntryForFile(path);
            QueueManager::Instance()->index_request.Enqueue(
                Index_Request(
                    entry.filename, entry.args, true /*is_interactive*/,
                    working_file->buffer_content, ICacheManager::Make()),
                true /*priority*/);
        }
        clang_complete->NotifyEdit(path);
        clang_complete->DiagnosticsUpdate(path);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentDidChange);
}  // namespace
