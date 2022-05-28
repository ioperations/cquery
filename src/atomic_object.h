#pragma once

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>

// A object which can be stored and taken from atomically.
template <class T>
struct AtomicObject {
    void Set(std::unique_ptr<T> t) {
        std::lock_guard<std::mutex> lock(mutex);
        value = std::move(t);
        cv.notify_one();
    }

    void SetIfEmpty(std::unique_ptr<T> t) {
        std::lock_guard<std::mutex> lock(mutex);
        if (value) return;

        value = std::move(t);
        cv.notify_one();
    }

    std::unique_ptr<T> Take() {
        std::unique_lock<std::mutex> lock(mutex);
        while (!value) {
            // release lock as long as the wait and reaquire it afterwards.
            cv.wait(lock);
        }

        return std::move(value);
    }

    template <typename TAction>
    void WithLock(TAction action) {
        std::unique_lock<std::mutex> lock(mutex);
        bool had_value = !!value;
        action(value);
        bool has_value = !!value;

        if (had_value != has_value) cv.notify_one();
    }

   private:
    std::unique_ptr<T> value;
    mutable std::mutex mutex;
    std::condition_variable cv;
};
