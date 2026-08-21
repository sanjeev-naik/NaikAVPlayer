#pragma once

#include "subtitle/SubtitleTrack.hpp"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace naikav {
namespace subtitle {

class SubtitleDecoder {
private:
    AVCodecContext* m_codecCtx = nullptr;
    AVRational m_timeBase{0, 1};
    int64_t m_startTime = 0;

    mutable std::mutex m_mutex;
    std::vector<SubtitleEvent> m_events;

    bool m_isExternal = false;
    std::string m_externalFilePath;

    const std::atomic<uint64_t>* m_seekGeneration = nullptr;
    uint64_t m_lastObservedGeneration = 0;

    void parseSrtVttFallback(const std::string& filepath);
    void parseAssSsaFallback(const std::string& filepath);
    static std::string extractTextFromAss(const char* assLine);

public:
    SubtitleDecoder();
    ~SubtitleDecoder();

    // Disable copy
    SubtitleDecoder(const SubtitleDecoder&) = delete;
    SubtitleDecoder& operator=(const SubtitleDecoder&) = delete;

    // Initialize for an embedded subtitle stream
    bool init(const AVCodecParameters* codecParams, AVRational timeBase, int64_t startTime);

    // Load and decode an external subtitle file (.srt, .vtt, .ass, .ssa, .sub)
    bool loadExternalFile(const std::string& filepath);

    // Process a packet from the demuxer (for embedded stream)
    void processPacket(AVPacket* pkt);

    // Query active subtitle text for the current presentation timestamp (with delay offset)
    std::string getActiveSubtitleText(double currentPts, double delaySeconds = 0.0) const;

    // Query active events
    std::vector<SubtitleEvent> getActiveEvents(double currentPts, double delaySeconds = 0.0) const;

    // Clear runtime/cached events and flush decoder
    void flush();

    // Full shutdown/cleanup
    void reset();

    void attachSeekGeneration(const std::atomic<uint64_t>* seekGen) {
        m_seekGeneration = seekGen;
    }

    bool isExternal() const { return m_isExternal; }
    const std::string& getExternalFilePath() const { return m_externalFilePath; }
    size_t getEventCount() const;
};

} // namespace subtitle
} // namespace naikav
