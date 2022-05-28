#include <doctest/doctest.h>

#include <loguru.hpp>
#include <pugixml.hpp>

#include "clang_format.h"
#include "message_handler.h"
#include "platform.h"
#include "queue_manager.h"
#include "working_files.h"

namespace {
MethodType k_method_type = "textDocument/formatting";

struct InTextDocumentFormatting : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    struct Params {
        LsTextDocumentIdentifier text_document;
        LsFormattingOptions options;
    };
    Params params;
};
MAKE_REFLECT_STRUCT(InTextDocumentFormatting::Params, text_document, options);
MAKE_REFLECT_STRUCT(InTextDocumentFormatting, id, params);
REGISTER_IN_MESSAGE(InTextDocumentFormatting);

struct OutTextDocumentFormatting
    : public LsOutMessage<OutTextDocumentFormatting> {
    LsRequestId id;
    std::vector<LsTextEdit> result;
};
MAKE_REFLECT_STRUCT(OutTextDocumentFormatting, jsonrpc, id, result);

struct HandlerTextDocumentFormatting
    : BaseMessageHandler<InTextDocumentFormatting> {
    MethodType GetMethodType() const override { return k_method_type; }
    void Run(InTextDocumentFormatting* request) override {
        OutTextDocumentFormatting response;
        response.id = request->id;

        WorkingFile* working_file = working_files->GetFileByFilename(
            request->params.text_document.uri.GetAbsolutePath());
        response.result =
            RunClangFormat(working_file->filename, working_file->buffer_content,
                           nullopt /*start_offset*/, nullopt /*end_offset*/);

        QueueManager::WriteStdout(k_method_type, response);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentFormatting);
}  // namespace
