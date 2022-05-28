#pragma once

#include <iosfwd>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

enum class PipelineStatus {
    // The file is has not been processed by the import pipeline in any way.
    kNotSeen,
    // The file is currently in the pipeline but has not been added to querydb
    // yet.
    kProcessingInitialImport,
    // The file is imported, but not currently in the pipeline.
    kImported,
    // The file is imported and also being updated, ie, it is currently in the
    // pipeline.
    kProcessingUpdate
};
std::ostream& operator<<(std::ostream& os, const PipelineStatus& status);

// Manages files inside of the indexing pipeline so we don't have the same file
// being imported multiple times.
struct ImportManager {
    PipelineStatus GetStatus(const std::string& path);

    // Attempt to atomically set a new status from an existing status.
    // |status_map| is a function which receives the current status as input,
    // and returns a new status. If the new status is different, then this
    // function will return true, otherwise false.

    template <typename TFn>
    bool SetStatusAtomicNoLock_(const std::string& path, TFn status_map) {
        // Get the current pipeline status.
        PipelineStatus current_status = PipelineStatus::kNotSeen;
        {
            auto it = m_status.find(path);
            if (it != m_status.end()) current_status = it->second;
        }

        // Determine the new status based on the current status.
        PipelineStatus new_status = status_map(current_status);

        // Only set the status if it changed.
        if (new_status == current_status) return false;
        m_status[path] = new_status;
        return true;
    }
    template <typename TFn>
    bool SetStatusAtomic(const std::string& path, TFn status_map) {
        std::unique_lock<std::shared_timed_mutex> lock(m_status_mutex);
        return SetStatusAtomicNoLock_(path, status_map);
    }
    template <typename TFn>
    void SetStatusAtomicBatch(const std::vector<std::string>& paths,
                              TFn status_map) {
        std::unique_lock<std::shared_timed_mutex> lock(m_status_mutex);
        for (auto& path : paths) SetStatusAtomicNoLock_(path, status_map);
    }

    // TODO: use shared_mutex
    std::shared_timed_mutex m_status_mutex;
    std::unordered_map<std::string, PipelineStatus> m_status;
};
