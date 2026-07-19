#pragma once
#include "MetricRing.hpp"
#include <atomic>

struct PipelineMetrics {
    alignas(64) std::atomic<bool> m_profilingEnabled{false};

    void setProfilingEnabled(bool enabled) {
        m_profilingEnabled.store(enabled, std::memory_order_relaxed);
    }

    // Queue depths (M1, M2, M3)
    alignas(64) std::atomic<int> m_videoPacketQueueDepth{0};
    alignas(64) std::atomic<int> m_audioPacketQueueDepth{0};
    alignas(64) std::atomic<int> m_decodedFrameQueueDepth{0};

    // Rings (M4, M5, M6, M7, M9)
    MetricRing<256> m_demuxTimePerPacketUs;
    MetricRing<256> m_decodeTimePerFrameUs;
    MetricRing<256> m_convertTimeUs;
    MetricRing<256> m_uploadTimeUs;
    MetricRing<256> m_avClockOffsetMs;
    MetricRing<256> m_seekLatencyMs;

    // Counter (M8)
    alignas(64) std::atomic<uint64_t> m_framesDroppedCount{0};

    void recordDemuxTime(float us) {
        if (m_profilingEnabled.load(std::memory_order_relaxed)) {
            m_demuxTimePerPacketUs.record(us);
        }
    }

    void recordDecodeTime(float us) {
        if (m_profilingEnabled.load(std::memory_order_relaxed)) {
            m_decodeTimePerFrameUs.record(us);
        }
    }

    void recordConvertTime(float us) {
        if (m_profilingEnabled.load(std::memory_order_relaxed)) {
            m_convertTimeUs.record(us);
        }
    }

    void recordUploadTime(float us) {
        if (m_profilingEnabled.load(std::memory_order_relaxed)) {
            m_uploadTimeUs.record(us);
        }
    }

    void recordClockOffset(float ms) {
        if (m_profilingEnabled.load(std::memory_order_relaxed)) {
            m_avClockOffsetMs.record(ms);
        }
    }

    void recordSeekLatency(float ms) {
        if (m_profilingEnabled.load(std::memory_order_relaxed)) {
            m_seekLatencyMs.record(ms);
        }
    }

    void incrementFramesDropped() {
        m_framesDroppedCount.fetch_add(1, std::memory_order_relaxed);
    }
};
