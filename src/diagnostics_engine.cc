#include "diagnostics_engine.h"

#include <chrono>

#include "queue_manager.h"

void DiagnosticsEngine::Init() {
    m_frequency_ms = g_config->diagnostics.frequencyMs;
    m_match = std::make_unique<GroupMatch>(g_config->diagnostics.whitelist,
                                           g_config->diagnostics.blacklist);
}

void DiagnosticsEngine::Publish(WorkingFiles* working_files, std::string path,
                                std::vector<lsDiagnostic> diagnostics) {
    // Cache diagnostics so we can show fixits.
    working_files->DoActionOnFile(path, [&](WorkingFile* working_file) {
        if (working_file) working_file->m_diagnostics = diagnostics;
    });

    int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch())
            .count();
    if (m_frequency_ms >= 0 && (m_next_publish <= now || diagnostics.empty()) &&
        m_match->IsMatch(path)) {
        m_next_publish = now + m_frequency_ms;

        Out_TextDocumentPublishDiagnostics out;
        out.params.uri = LsDocumentUri::FromPath(path);
        out.params.diagnostics = diagnostics;
        QueueManager::WriteStdout(kMethodType_TextDocumentPublishDiagnostics,
                                  out);
    }
}
