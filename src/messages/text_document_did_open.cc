#include <loguru.hpp>

#include "cache_manager.h"
#include "clang_complete.h"
#include "include_complete.h"
#include "message_handler.h"
#include "project.h"
#include "queue_manager.h"
#include "timer.h"
#include "working_files.h"

namespace {
MethodType k_method_type = "textDocument/didOpen";

// Open, view, change, close file
struct InTextDocumentDidOpen : public NotificationInMessage {
    MethodType GetMethodType() const override { return k_method_type; }

    struct Params {
        LsTextDocumentItem text_document;

        // cquery extension
        // If specified (e.g. ["clang++", "-DM", "a.cc"]), it overrides the
        // project entry (e.g. loaded from compile_commands.json or .cquery).
        std::vector<std::string> args;
    };
    Params params;
};
MAKE_REFLECT_STRUCT(InTextDocumentDidOpen::Params, text_document, args);
MAKE_REFLECT_STRUCT(InTextDocumentDidOpen, params);
REGISTER_IN_MESSAGE(InTextDocumentDidOpen);

struct HandlerTextDocumentDidOpen : BaseMessageHandler<InTextDocumentDidOpen> {
    MethodType GetMethodType() const override { return k_method_type; }

    void Run(InTextDocumentDidOpen* request) override {
        // NOTE: This function blocks code lens. If it starts taking a long time
        // we will need to find a way to unblock the code lens request.
        const auto& params = request->params;
        Timer time;
        AbsolutePath path = params.text_document.uri.GetAbsolutePath();
        if (ShouldIgnoreFileForIndexing(path)) return;

        std::shared_ptr<ICacheManager> cache_manager = ICacheManager::Make();
        WorkingFile* working_file = working_files->OnOpen(params.text_document);
        optional<std::string> cached_file_contents =
            cache_manager->LoadCachedFileContents(path);
        if (cached_file_contents)
            working_file->SetIndexContent(*cached_file_contents);

        QueryFile* file = nullptr;
        FindFileOrFail(db, project, nullopt, path, &file);
        if (file && file->def) {
            EmitInactiveLines(working_file, file->def->inactive_regions);
            EmitSemanticHighlighting(db, semantic_cache, working_file, file);
        }

        time.ResetAndPrint(
            "[querydb] Loading cached index file for DidOpen (blocks "
            "CodeLens)");

        include_complete->AddFile(working_file->filename);

        // Submit new index request.
        Project::Entry entry = project->FindCompilationEntryForFile(path);
        QueueManager::Instance()->index_request.Enqueue(
            Index_Request(entry.filename,
                          params.args.size() ? params.args : entry.args,
                          true /*is_interactive*/, params.text_document.text,
                          cache_manager),
            true /*priority*/);

        if (params.args.size()) {
            project->SetFlagsForFile(params.args, path);
        }

        // Clear any existing completion state and preload completion.
        clang_complete->FlushSession(entry.filename);
        clang_complete->NotifyView(path);
    }
};
REGISTER_MESSAGE_HANDLER(HandlerTextDocumentDidOpen);
}  // namespace
