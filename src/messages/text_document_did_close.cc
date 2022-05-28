#include "clang_complete.h"
#include "message_handler.h"
#include "queue_manager.h"
#include "working_files.h"

namespace {
MethodType k_method_type = "textDocument/didClose";

struct InTextDocumentDidClose : public NotificationInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    struct Params {
        LsTextDocumentIdentifier text_document;
    };
    Params params;
};
MAKE_REFLECT_STRUCT(InTextDocumentDidClose::Params, text_document);
MAKE_REFLECT_STRUCT(InTextDocumentDidClose, params);
REGISTER_IN_MESSAGE(InTextDocumentDidClose);

struct HandlerTextDocumentDidClose
    : BaseMessageHandler<InTextDocumentDidClose> {
    MethodType GetMethodType() const override { return k_method_type; }

    void Run(InTextDocumentDidClose* request) override {
        AbsolutePath path = request->params.text_document.uri.GetAbsolutePath();

        // Clear any diagnostics for the file.
        Out_TextDocumentPublishDiagnostics out;
        out.params.uri = request->params.text_document.uri;
        QueueManager::WriteStdout(k_method_type, out);

        // Remove internal state.
        working_files->OnClose(request->params.text_document);
        clang_complete->NotifyClose(path);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentDidClose);
}  // namespace
