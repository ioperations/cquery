#include "code_complete_cache.h"

void CodeCompleteCache::WithLock(std::function<void()> action) {
    std::lock_guard<std::mutex> lock(m_mutex);
    action();
}

bool CodeCompleteCache::IsCacheValid(LsTextDocumentPositionParams position) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cached_path == position.text_document.uri.GetAbsolutePath() &&
           m_cached_completion_position == position.position;
}

void CodeCompleteCache::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cached_path.reset();
    m_cached_completion_position.reset();
    m_cached_results.clear();
}
