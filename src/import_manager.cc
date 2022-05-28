#include "import_manager.h"

#include <mutex>
#include <ostream>

#include "assert.h"

std::ostream& operator<<(std::ostream& os, const PipelineStatus& status) {
    switch (status) {
        case PipelineStatus::kNotSeen:
            os << "kNotSeen";
            break;
        case PipelineStatus::kProcessingInitialImport:
            os << "kProcessingInitialImport";
            break;
        case PipelineStatus::kImported:
            os << "kImported";
            break;
        case PipelineStatus::kProcessingUpdate:
            os << "kProcessingUpdate";
            break;
        default:
            assert(false);
    }
    return os;
}

PipelineStatus ImportManager::GetStatus(const std::string& path) {
    // Try reading the value
    {
        std::shared_lock<std::shared_timed_mutex> lock(m_status_mutex);
        auto it = m_status.find(path);
        if (it != m_status.end()) return it->second;
    }
    return PipelineStatus::kNotSeen;
}
