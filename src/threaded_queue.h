#pragma once

#include <optional.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>

#include "utils.h"

// TODO: cleanup includes.

struct MultiQueueWaiter;

struct BaseThreadQueue {
    virtual ~BaseThreadQueue() = default;

    virtual bool IsEmpty() = 0;

    std::shared_ptr<MultiQueueWaiter> waiter;
};

// std::lock accepts two or more arguments. We define an overload for one
// argument.
namespace std {
template <typename Lockable>
void lock(Lockable& l) {
    l.lock();
}
}  // namespace std

template <typename... Queue>
struct MultiQueueLock {
    MultiQueueLock(Queue... lockable) : m_tuple{lockable...} { lock(); }
    ~MultiQueueLock() { unlock(); }
    void lock() { lock_impl(typename std::index_sequence_for<Queue...>{}); }
    void unlock() { unlock_impl(typename std::index_sequence_for<Queue...>{}); }

   private:
    template <size_t... Is>
    void lock_impl(std::index_sequence<Is...>) {
        std::lock(std::get<Is>(m_tuple)->mutex...);
    }

    template <size_t... Is>
    void unlock_impl(std::index_sequence<Is...>) {
        (void)std::initializer_list<int>{
            (std::get<Is>(m_tuple)->mutex.unlock(), 0)...};
    }

    std::tuple<Queue...> m_tuple;
};

struct MultiQueueWaiter {
    static bool HasState(std::initializer_list<BaseThreadQueue*> queues);

    bool ValidateWaiter(std::initializer_list<BaseThreadQueue*> queues);

    template <typename... BaseThreadQueue>
    void Wait(BaseThreadQueue... queues) {
        assert(ValidateWaiter({queues...}));

        MultiQueueLock<BaseThreadQueue...> l(queues...);
        while (!HasState({queues...})) cv.wait(l);
    }

    std::condition_variable_any cv;
};

// A threadsafe-queue. http://stackoverflow.com/a/16075550
template <class T>
struct ThreadedQueue : public BaseThreadQueue {
   public:
    ThreadedQueue() : ThreadedQueue(std::make_shared<MultiQueueWaiter>()) {}

    explicit ThreadedQueue(std::shared_ptr<MultiQueueWaiter> waiter)
        : m_total_count(0) {
        this->waiter = waiter;
    }

    // Returns the number of elements in the queue. This is lock-free.
    size_t Size() const { return m_total_count; }

    // Add an element to the queue.
    void Enqueue(T&& t, bool priority) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (priority)
                m_priority.push_back(std::move(t));
            else
                m_queue.push_back(std::move(t));
            ++m_total_count;
        }
        waiter->cv.notify_one();
    }

    // Add a set of elements to the queue.
    void EnqueueAll(std::vector<T>&& elements, bool priority) {
        if (elements.empty()) return;

        {
            std::lock_guard<std::mutex> lock(mutex);
            m_total_count += elements.size();
            for (T& element : elements) {
                if (priority)
                    m_priority.push_back(std::move(element));
                else
                    m_queue.push_back(std::move(element));
            }
            elements.clear();
        }

        waiter->cv.notify_all();
    }

    // Returns true if the queue is empty. This is lock-free.
    bool IsEmpty() { return m_total_count == 0; }

    // Get the first element from the queue. Blocks until one is available.
    T Dequeue() {
        std::unique_lock<std::mutex> lock(mutex);
        waiter->cv.wait(
            lock, [&]() { return !m_priority.empty() || !m_queue.empty(); });

        auto execute = [&](std::deque<T>* q) {
            auto val = std::move(q->front());
            q->pop_front();
            --m_total_count;
            return val;
        };
        if (!m_priority.empty()) return execute(&m_priority);
        return execute(&m_queue);
    }

    // Get the first element from the queue without blocking. Returns a null
    // value if the queue is empty.
    optional<T> TryDequeue(bool priority) {
        std::lock_guard<std::mutex> lock(mutex);

        auto pop = [&](std::deque<T>* q) {
            auto val = std::move(q->front());
            q->pop_front();
            --m_total_count;
            return val;
        };

        auto get_result = [&](std::deque<T>* first,
                              std::deque<T>* second) -> optional<T> {
            if (!first->empty()) return pop(first);
            if (!second->empty()) return pop(second);
            return nullopt;
        };

        if (priority) return get_result(&m_priority, &m_queue);
        return get_result(&m_queue, &m_priority);
    }

    template <typename Fn>
    void Iterate(Fn fn) {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& entry : m_priority) fn(entry);
        for (auto& entry : m_queue) fn(entry);
    }

    mutable std::mutex mutex;

   private:
    std::atomic<int> m_total_count;
    std::deque<T> m_priority;
    std::deque<T> m_queue;
};
