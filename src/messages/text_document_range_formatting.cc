#include <loguru.hpp>

#include "clang_format.h"
#include "lex_utils.h"
#include "message_handler.h"
#include "queue_manager.h"
#include "working_files.h"

namespace {
MethodType k_method_type = "textDocument/rangeFormatting";

struct LsTextDocumentRangeFormattingParams {
    LsTextDocumentIdentifier text_document;
    LsRange range;
    LsFormattingOptions options;
};
MAKE_REFLECT_STRUCT(LsTextDocumentRangeFormattingParams, text_document, range,
                    options);

struct InTextDocumentRangeFormatting : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    LsTextDocumentRangeFormattingParams params;
};
MAKE_REFLECT_STRUCT(InTextDocumentRangeFormatting, id, params);
REGISTER_IN_MESSAGE(InTextDocumentRangeFormatting);

struct OutTextDocumentRangeFormatting
    : public LsOutMessage<OutTextDocumentRangeFormatting> {
    LsRequestId id;
    std::vector<LsTextEdit> result;
};
MAKE_REFLECT_STRUCT(OutTextDocumentRangeFormatting, jsonrpc, id, result);

struct HandlerTextDocumentRangeFormatting
    : BaseMessageHandler<InTextDocumentRangeFormatting> {
    MethodType GetMethodType() const override { return k_method_type; }

    void Run(InTextDocumentRangeFormatting* request) override {
        OutTextDocumentRangeFormatting response;
        response.id = request->id;

        WorkingFile* working_file = working_files->GetFileByFilename(
            request->params.text_document.uri.GetAbsolutePath());

        int start_offset = GetOffsetForPosition(request->params.range.start,
                                                working_file->buffer_content);
        int end_offset = GetOffsetForPosition(request->params.range.end,
                                              working_file->buffer_content);
        response.result =
            RunClangFormat(working_file->filename, working_file->buffer_content,
                           start_offset, end_offset);

        QueueManager::WriteStdout(k_method_type, response);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentRangeFormatting);
}  // namespace
