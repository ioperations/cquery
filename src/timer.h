#pragma once

#include <optional.h>

#include <chrono>
#include <string>

struct Timer {
    using Clock = std::chrono::high_resolution_clock;

    static long long GetCurrentTimeInMilliseconds();

    // Creates a new timer. A timer is always running.
    Timer();

    // Returns elapsed microseconds.
    long long ElapsedMicroseconds() const;
    // Returns elapsed microseconds and restarts/resets the timer.
    long long ElapsedMicrosecondsAndReset();
    // Restart/reset the timer.
    void Reset();
    // Resets timer and prints a message like "<foo> took 5ms"
    void ResetAndPrint(const std::string& message);
    // Pause the timer.
    void Pause();
    // Resume the timer after it has been paused.
    void Resume();

    // Raw start time.
    optional<std::chrono::time_point<Clock>> m_start;
    // Elapsed time.
    long long m_elapsed = 0;
};

struct ScopedPerfTimer {
    ScopedPerfTimer(const std::string& message);
    ~ScopedPerfTimer();

    Timer m_timer;
    std::string m_message;
};
