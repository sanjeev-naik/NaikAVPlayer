#pragma once
#include <atomic>
#include <cstddef>
#include <algorithm>

template <size_t N = 256>
class MetricRing {
    static_assert((N & (N - 1)) == 0, "Ring buffer size N must be a power of 2");
private:
    alignas(64) std::atomic<float> m_data[N]{};
    alignas(64) std::atomic<size_t> m_head{0};

public:
    MetricRing() {
        for (size_t i = 0; i < N; ++i) {
            m_data[i].store(0.0f, std::memory_order_relaxed);
        }
    }

    // Disable copying
    MetricRing(const MetricRing&) = delete;
    MetricRing& operator=(const MetricRing&) = delete;

    void record(float value) {
        size_t h = m_head.load(std::memory_order_relaxed);
        m_data[h & (N - 1)].store(value, std::memory_order_relaxed);
        m_head.store(h + 1, std::memory_order_release);
    }

    size_t snapshot(float* out, size_t max) const {
        size_t h = m_head.load(std::memory_order_acquire);
        if (h == 0) {
            return 0;
        }
        size_t count = std::min({static_cast<size_t>(h), N, max});
        for (size_t i = 0; i < count; ++i) {
            size_t idx = (h - 1 - i) & (N - 1);
            out[count - 1 - i] = m_data[idx].load(std::memory_order_relaxed);
        }
        return count;
    }

    size_t getHead() const {
        return m_head.load(std::memory_order_relaxed);
    }
};
