#pragma once

#include <optional.h>

#include <mutex>

#include "lsp_completion.h"

// Cached completion information, so we can give fast completion results when
// the user erases a character. vscode will resend the completion request if
// that happens.
struct CodeCompleteCache {
    // NOTE: Make sure to access these variables under |WithLock|.
    optional<AbsolutePath> m_cached_path;
    optional<LsPosition> m_cached_completion_position;
    std::vector<lsCompletionItem> m_cached_results;

    std::mutex m_mutex;

    void WithLock(std::function<void()> action);
    bool IsCacheValid(LsTextDocumentPositionParams position);
    void Clear();
};
