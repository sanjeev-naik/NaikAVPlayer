#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <chrono>

template <typename T>
class ThreadSafeQueue {
private:
    std::queue<T> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cond_pop;
    std::condition_variable m_cond_push;
    size_t m_maxSize;
    bool m_aborted;
    std::atomic<int>* m_depthTracker = nullptr;

public:
    explicit ThreadSafeQueue(size_t maxSize = 100) 
        : m_maxSize(maxSize), m_aborted(false) {}

    ~ThreadSafeQueue() {
        abort();
    }

    // Push an item. Blocks if the queue has reached max size.
    // Returns false if the queue was aborted.
    bool push(const T& value) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cond_push.wait(lock, [this]() { 
            return m_queue.size() < m_maxSize || m_aborted; 
        });

        if (m_aborted) {
            return false;
        }

        m_queue.push(value);
        if (m_depthTracker) {
            m_depthTracker->store(static_cast<int>(m_queue.size()), std::memory_order_relaxed);
        }
        m_cond_pop.notify_one();
        return true;
    }

    // Pop an item. Blocks if the queue is empty.
    // Returns false if the queue was aborted.
    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cond_pop.wait(lock, [this]() { 
            return !m_queue.empty() || m_aborted; 
        });

        if (m_aborted || m_queue.empty()) {
            return false;
        }

        value = std::move(m_queue.front());
        m_queue.pop();
        if (m_depthTracker) {
            m_depthTracker->store(static_cast<int>(m_queue.size()), std::memory_order_relaxed);
        }
        m_cond_push.notify_one();
        return true;
    }

    // Push without blocking. If the queue is full, the oldest queued item is
    // dropped (via dropCleanup, if given) to make room for the new one.
    // Used where the consumer may be intentionally idle for a while (e.g. an
    // audio device paused during a seek catch-up) and a producer thread that
    // also feeds other queues must never stall waiting for room here.
    // Returns false only if the queue was aborted.
    bool push_drop_oldest(const T& value, std::function<void(T&)> dropCleanup = nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_aborted) {
            return false;
        }
        if (m_queue.size() >= m_maxSize) {
            T oldest = std::move(m_queue.front());
            m_queue.pop();
            if (dropCleanup) {
                dropCleanup(oldest);
            }
        }
        m_queue.push(value);
        if (m_depthTracker) {
            m_depthTracker->store(static_cast<int>(m_queue.size()), std::memory_order_relaxed);
        }
        m_cond_pop.notify_one();
        return true;
    }

    // Push, waiting up to timeoutMs for room to free up. If it's still full
    // when the wait expires, drops the oldest queued item (via dropCleanup,
    // if given) to make room rather than blocking indefinitely.
    //
    // This is the structural backstop for every producer thread that also
    // has other work to do (e.g. a demuxer thread feeding two queues, or a
    // decode thread whose packet queue depends on it staying unblocked): no
    // matter what stalls the consumer -- a paused device, a wedged decoder,
    // a bug not yet found -- this call always returns within timeoutMs, so
    // the producer can never be stuck waiting forever on a single queue.
    // Returns false only if the queue was aborted.
    bool push_wait_or_drop(const T& value, std::chrono::milliseconds timeoutMs,
                            std::function<void(T&)> dropCleanup = nullptr) {
        std::unique_lock<std::mutex> lock(m_mutex);
        bool gotRoom = m_cond_push.wait_for(lock, timeoutMs, [this]() {
            return m_queue.size() < m_maxSize || m_aborted;
        });

        if (m_aborted) {
            return false;
        }

        if (!gotRoom && m_queue.size() >= m_maxSize && !m_queue.empty()) {
            T oldest = std::move(m_queue.front());
            m_queue.pop();
            if (dropCleanup) {
                dropCleanup(oldest);
            }
        }

        m_queue.push(value);
        if (m_depthTracker) {
            m_depthTracker->store(static_cast<int>(m_queue.size()), std::memory_order_relaxed);
        }
        m_cond_pop.notify_one();
        return true;
    }

    // Try to pop an item without blocking.
    // Returns false if the queue is empty or aborted.
    bool try_pop(T& value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_aborted || m_queue.empty()) {
            return false;
        }

        value = std::move(m_queue.front());
        m_queue.pop();
        if (m_depthTracker) {
            m_depthTracker->store(static_cast<int>(m_queue.size()), std::memory_order_relaxed);
        }
        m_cond_push.notify_one();
        return true;
    }

    // Check if the queue is empty (thread-safe)
    bool empty() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    // Get current size (thread-safe)
    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    // Get max capacity of the queue
    size_t capacity() const {
        return m_maxSize;
    }

    void attachDepthMirror(std::atomic<int>* tracker = nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_depthTracker = tracker;
        if (m_depthTracker) {
            m_depthTracker->store(static_cast<int>(m_queue.size()), std::memory_order_relaxed);
        }
    }

    // Peek the front item without popping.
    // Returns false if empty or aborted.
    bool peek(T& value) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty() || m_aborted) {
            return false;
        }
        value = m_queue.front();
        return true;
    }

    // Abort all operations. Wakes up any waiting threads.
    void abort() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_aborted = true;
        }
        m_cond_pop.notify_all();
        m_cond_push.notify_all();
    }

    // Reset the aborted state and clear elements
    void reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_aborted = false;
        while (!m_queue.empty()) {
            m_queue.pop();
        }
        if (m_depthTracker) {
            m_depthTracker->store(0, std::memory_order_relaxed);
        }
        m_cond_push.notify_all();
    }

    // Clear contents and apply a cleanup function to each element (e.g. freeing memory)
    void clear(std::function<void(T&)> cleanupFunc = nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_queue.empty()) {
            T value = std::move(m_queue.front());
            m_queue.pop();
            if (cleanupFunc) {
                cleanupFunc(value);
            }
        }
        if (m_depthTracker) {
            m_depthTracker->store(0, std::memory_order_relaxed);
        }
        m_cond_push.notify_all();
    }
};
