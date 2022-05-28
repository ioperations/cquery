#include <stdint.h>

#include "clang_complete.h"
#include "code_complete_cache.h"
#include "message_handler.h"
#include "queue_manager.h"
#include "timer.h"

namespace {
MethodType k_method_type = "textDocument/signatureHelp";

struct InTextDocumentSignatureHelp : public RequestInMessage {
    MethodType GetMethodType() const override { return k_method_type; }
    LsTextDocumentPositionParams params;
};
MAKE_REFLECT_STRUCT(InTextDocumentSignatureHelp, id, params);
REGISTER_IN_MESSAGE(InTextDocumentSignatureHelp);

// Represents a parameter of a callable-signature. A parameter can
// have a label and a doc-comment.
struct LsParameterInformation {
    // The label of this parameter. Will be shown in
    // the UI.
    std::string label;

    // The human-readable doc-comment of this parameter. Will be shown
    // in the UI but can be omitted.
    optional<std::string> documentation;
};
MAKE_REFLECT_STRUCT(LsParameterInformation, label, documentation);

// Represents the signature of something callable. A signature
// can have a label, like a function-name, a doc-comment, and
// a set of parameters.
struct LsSignatureInformation {
    // The label of this signature. Will be shown in
    // the UI.
    std::string label;

    // The human-readable doc-comment of this signature. Will be shown
    // in the UI but can be omitted.
    optional<std::string> documentation;

    // The parameters of this signature.
    std::vector<LsParameterInformation> parameters;
};
MAKE_REFLECT_STRUCT(LsSignatureInformation, label, documentation, parameters);

// Signature help represents the signature of something
// callable. There can be multiple signature but only one
// active and only one active parameter.
struct LsSignatureHelp {
    // One or more signatures.
    std::vector<LsSignatureInformation> signatures;

    // The active signature. If omitted or the value lies outside the
    // range of `signatures` the value defaults to zero or is ignored if
    // `signatures.length === 0`. Whenever possible implementors should
    // make an active decision about the active signature and shouldn't
    // rely on a default value.
    // In future version of the protocol this property might become
    // mandantory to better express this.
    optional<int> active_signature;

    // The active parameter of the active signature. If omitted or the value
    // lies outside the range of `signatures[activeSignature].parameters`
    // defaults to 0 if the active signature has parameters. If
    // the active signature has no parameters it is ignored.
    // In future version of the protocol this property might become
    // mandantory to better express the active parameter if the
    // active signature does have any.
    optional<int> active_parameter;
};
MAKE_REFLECT_STRUCT(LsSignatureHelp, signatures, active_signature,
                    active_parameter);

struct OutTextDocumentSignatureHelp
    : public LsOutMessage<OutTextDocumentSignatureHelp> {
    LsRequestId id;
    LsSignatureHelp result;
};
MAKE_REFLECT_STRUCT(OutTextDocumentSignatureHelp, jsonrpc, id, result);

struct HandlerTextDocumentSignatureHelp : MessageHandler {
    MethodType GetMethodType() const override { return k_method_type; }

    void Run(std::unique_ptr<InMessage> message) override {
        auto* request =
            static_cast<InTextDocumentSignatureHelp*>(message.get());
        LsTextDocumentPositionParams& params = request->params;
        WorkingFile* file = working_files->GetFileByFilename(
            params.text_document.uri.GetAbsolutePath());
        std::string search;
        int active_param = 0;
        if (file) {
            LsPosition completion_position;
            search = file->FindClosestCallNameInBuffer(
                params.position, &active_param, &completion_position);
            params.position = completion_position;
        }
        if (search.empty()) return;

        InTextDocumentSignatureHelp* msg =
            static_cast<InTextDocumentSignatureHelp*>(message.release());
        ClangCompleteManager::OnComplete callback =
            [this, msg, search, active_param](
                const LsRequestId& id,
                const std::vector<lsCompletionItem>& results,
                bool is_cached_result) {
                OutTextDocumentSignatureHelp out;
                out.id = id;

                for (auto& result : results) {
                    if (result.label != search) continue;

                    LsSignatureInformation signature;
                    signature.label = result.detail;
                    for (auto& parameter : result.parameters_) {
                        LsParameterInformation ls_param;
                        ls_param.label = parameter;
                        signature.parameters.push_back(ls_param);
                    }
                    out.result.signatures.push_back(signature);
                }

                // Prefer the signature with least parameter count but still
                // larger than active_param.
                out.result.active_signature = 0;
                if (out.result.signatures.size()) {
                    size_t num_parameters = SIZE_MAX;
                    for (size_t i = 0; i < out.result.signatures.size(); ++i) {
                        size_t t = out.result.signatures[i].parameters.size();
                        if (active_param < t && t < num_parameters) {
                            out.result.active_signature = int(i);
                            num_parameters = t;
                        }
                    }
                }

                // Set signature to what we parsed from the working file.
                out.result.active_parameter = active_param;

                Timer timer;
                QueueManager::WriteStdout(k_method_type, out);

                if (!is_cached_result) {
                    signature_cache->WithLock([&]() {
                        signature_cache->m_cached_path =
                            msg->params.text_document.uri.GetAbsolutePath();
                        signature_cache->m_cached_completion_position =
                            msg->params.position;
                        signature_cache->m_cached_results = results;
                    });
                }

                delete msg;
            };

        if (signature_cache->IsCacheValid(params)) {
            signature_cache->WithLock([&]() {
                callback(request->id, signature_cache->m_cached_results,
                         true /*is_cached_result*/);
            });
        } else {
            clang_complete->CodeComplete(request->id, params,
                                         std::move(callback));
        }
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentSignatureHelp);
}  // namespace
