#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

template <typename T>
class ThreadSafeQueue {
private:
    std::queue<T> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cond_pop;
    std::condition_variable m_cond_push;
    size_t m_maxSize;
    bool m_aborted;

public:
    explicit ThreadSafeQueue(size_t maxSize = 100) 
        : m_maxSize(maxSize), m_aborted(false) {}

    ~ThreadSafeQueue() {
        abort();
    }

    // Push an item. Blocks if the queue has reached max size.
    // Returns false if the queue was aborted.
    bool push(T value) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cond_push.wait(lock, [this]() { 
            return m_queue.size() < m_maxSize || m_aborted; 
        });

        if (m_aborted) {
            return false;
        }

        m_queue.push(value);
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
        m_cond_push.notify_one();
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
        m_cond_push.notify_all();
    }
};
