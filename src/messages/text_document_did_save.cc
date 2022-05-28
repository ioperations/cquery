#include <loguru/loguru.hpp>

#include "cache_manager.h"
#include "clang_complete.h"
#include "message_handler.h"
#include "project.h"
#include "queue_manager.h"

namespace {
MethodType k_method_type = "textDocument/didSave";

struct InTextDocumentDidSave : public NotificationInMessage {
    MethodType GetMethodType() const override { return k_method_type; }

    struct Params {
        // The document that was saved.
        LsTextDocumentIdentifier text_document;

        // Optional the content when saved. Depends on the includeText value
        // when the save notifcation was requested.
        // std::string text;
    };
    Params params;
};
MAKE_REFLECT_STRUCT(InTextDocumentDidSave::Params, text_document);
MAKE_REFLECT_STRUCT(InTextDocumentDidSave, params);
REGISTER_IN_MESSAGE(InTextDocumentDidSave);

struct HandlerTextDocumentDidSave : BaseMessageHandler<InTextDocumentDidSave> {
    MethodType GetMethodType() const override { return k_method_type; }

    void Run(InTextDocumentDidSave* request) override {
        AbsolutePath path = request->params.text_document.uri.GetAbsolutePath();
        if (ShouldIgnoreFileForIndexing(path)) return;

        // Send out an index request, and copy the current buffer state so we
        // can update the cached index contents when the index is done.
        //
        // We also do not index if there is already an index request or if
        // the client requested indexing on didChange instead.
        //
        // TODO: Cancel outgoing index request. Might be tricky to make
        //       efficient since we have to cancel.
        //    - we could have an |atomic<int> active_cancellations| variable
        //      that all of the indexers check before accepting an index. if
        //      zero we don't slow down fast-path. if non-zero we acquire
        //      mutex and check to see if we should skip the current request.
        //      if so, ignore that index response.
        // TODO: send as priority request
        if (!g_config->enableIndexOnDidChange) {
            Project::Entry entry = project->FindCompilationEntryForFile(path);
            QueueManager::Instance()->index_request.Enqueue(
                Index_Request(entry.filename, entry.args,
                              true /*is_interactive*/, nullopt,
                              ICacheManager::Make()),
                true /*priority*/);
        }

        clang_complete->NotifySave(path);
        clang_complete->DiagnosticsUpdate(path);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentDidSave);
}  // namespace
