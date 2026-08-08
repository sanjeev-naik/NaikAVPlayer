#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <cstdint>
#include <mutex>
#include "ThreadSafeQueue.hpp"
#include "Demuxer.hpp"
#include "AudioDecoder.hpp"
#include "VideoDecoder.hpp"
#include "PipelineMetrics.hpp"
#include <chrono>

extern "C" {
#include <libavutil/frame.h>
}

struct DecodedFrame {
    AVFrame* frame = nullptr;
    double pts = 0.0;
    int width = 0;
    int height = 0;
};

extern bool g_videoThreadEnabled;

enum class PlayerState {
    UNINITIALIZED,
    OPENED,
    PLAYING,
    PAUSED,
    ENDED,
    ERROR_STATE
};

class PlayerController {
private:
    std::string m_filename;
    // Atomic: written by the UI thread, read by the video decode thread
    std::atomic<PlayerState> m_state;
    
    // Packet queues
    ThreadSafeQueue<AVPacket*> m_videoQueue;
    ThreadSafeQueue<AVPacket*> m_audioQueue;
    
    // Sub-modules
    std::unique_ptr<Demuxer> m_demuxer;
    std::unique_ptr<AudioDecoder> m_audioDecoder;
    std::unique_ptr<VideoDecoder> m_videoDecoder;
    
    bool m_hasAudio;
    bool m_hasVideo;

    // Fallback clock for video-only files
    // Atomic: also written by the video thread when a seek catch-up finishes
    std::atomic<double> m_videoClock; // in seconds
    std::atomic<double> m_lastSystemTime;
    
    float m_volume;

    bool m_loopEnabled;

    // Background video decoding thread
    std::thread m_videoThread;
    std::atomic<bool> m_videoThreadRunning;
    bool m_videoThreadEnabled;
    // mutable: also taken by const getters (getVideoWidth/Height,
    // isVideoHardware) that read m_videoDecoder's codec context, which the
    // video thread can free and reopen with a fresh session when recovering
    // from a hardware decode failure.
    mutable std::mutex m_videoDecoderMutex;
    std::atomic<bool> m_seeking;
    std::atomic<bool> m_seeked;
    ThreadSafeQueue<DecodedFrame> m_decodedFrameQueue;

    // Seek catch-up. Every seek LANDs: the player repositions immediately
    // and decodes from the preceding keyframe up to the target frame without
    // displaying anything in between, so the jump appears instantaneous in
    // both directions. Audio stays muted during the catch-up and resumes in
    // sync on the target.
    std::atomic<SeekCatchupMode> m_catchupMode;
    std::atomic<double> m_catchupTarget; // final seek position (seconds)
    std::atomic<double> m_catchupPos;    // position reported while catching up
    std::atomic<bool> m_resumeAfterCatchup;
    // Bumped as the last step of every seek. A frame whose decode began under
    // an older epoch belongs to the pre-seek stream and must be discarded,
    // never displayed or matched against the catch-up target.
    std::atomic<uint64_t> m_catchupEpoch;
    std::mutex m_catchupMutex;   // serializes catch-up begin/retarget/finish
    std::unique_ptr<PipelineMetrics> m_metrics;
    std::chrono::steady_clock::time_point m_seekStartTime;
    uint64_t m_seekStartEpoch = 0;

    std::atomic<ResolutionOption> m_resolutionOption;

    void loadSettings();
    void saveSettings();
    void videoThreadLoop();
    void instantSeek(double seconds);
    void finishCatchup(double resumePts);

    double getSystemTimeInSeconds() const;
    
    // Timing instrumentation (in microseconds)
    std::atomic<uint64_t> m_videoDecodeTimeUs{0};
    std::atomic<uint64_t> m_audioDecodeTimeUs{0};
    std::atomic<uint64_t> m_videoRenderTimeUs{0};
    std::atomic<uint64_t> m_presentTimeUs{0};
    std::atomic<uint64_t> m_framePacingUs{0};

public:
    PlayerController();
    ~PlayerController();

    bool openFile(const std::string& filename);
    void play();
    void pause();
    void seek(double seconds);
    void stop();
    
    void updateClockForVideoOnly();

    // Getters
    PlayerState getState() const { return m_state; }
    const std::string& getFilename() const { return m_filename; }
    double getCurrentTime();
    double getDuration() const;
    int getVideoWidth() const;
    int getVideoHeight() const;
    std::string getVideoCodecName() const;
    std::string getVideoPixelFormat() const;
    ColorPipelineInfo getColorInfo() const;
    bool isVideoHardware() const;
    bool isSeeking() const;
    bool isCatchingUp() const { return m_catchupMode.load() != SeekCatchupMode::NONE; }
    // Base position for relative seeks: the pending catch-up target if one is
    // active (so repeated +10s presses stack), otherwise the current time.
    double getSeekReferenceTime();
    std::string getAudioCodecName() const;
    double getAudioClock();
    double getVideoClock() const;
    bool hasAudio() const { return m_hasAudio; }
    bool hasVideo() const { return m_hasVideo; }
    bool isEOF() const;
    
    VideoDecoder* getVideoDecoder() const { return m_videoDecoder.get(); }
    ThreadSafeQueue<DecodedFrame>& getDecodedFrameQueue() { return m_decodedFrameQueue; }
    
    // Setters
    void setVolume(float volume);
    void setLoopEnabled(bool enabled) { m_loopEnabled = enabled; }
    bool isLoopEnabled() const { return m_loopEnabled; }
    bool hasSeeked() const { return m_seeked.load(); }
    void clearSeeked() { m_seeked.store(false); }

    ResolutionOption getResolutionOption() const { return m_resolutionOption.load(); }
    void setResolutionOption(ResolutionOption option);
    int getPlaybackWidth() const;
    int getPlaybackHeight() const;

    // Queue depths
    size_t getVideoPacketQueueSize() const { return m_videoQueue.size(); }
    size_t getVideoPacketQueueCapacity() const { return m_videoQueue.capacity(); }
    size_t getAudioPacketQueueSize() const { return m_audioQueue.size(); }
    size_t getAudioPacketQueueCapacity() const { return m_audioQueue.capacity(); }
    size_t getVideoFrameQueueSize() const { return m_decodedFrameQueue.size(); }
    size_t getVideoFrameQueueCapacity() const { return m_decodedFrameQueue.capacity(); }
    size_t getAudioFrameQueueSize() const;
    static size_t getAudioFrameQueueCapacity() { return 48000; } // target scale for visual (48k samples ~ 1 sec)

    // Timing setters/getters
    void setVideoRenderTimeUs(uint64_t us) { m_videoRenderTimeUs.store(us); }
    void setPresentTimeUs(uint64_t us) { m_presentTimeUs.store(us); }
    void setFramePacingUs(uint64_t us) { m_framePacingUs.store(us); }

    double getVideoDecodeTimeMs() const {
        float latestUs = 0.0f;
        if (m_metrics && m_metrics->m_decodeTimePerFrameUs.snapshot(&latestUs, 1) > 0) {
            return latestUs / 1000.0;
        }
        return 0.0;
    }
    double getAudioDecodeTimeMs() const { return m_audioDecodeTimeUs.load() / 1000.0; }
    double getVideoRenderTimeMs() const { return m_videoRenderTimeUs.load() / 1000.0; }
    double getPresentTimeMs() const { return m_presentTimeUs.load() / 1000.0; }
    double getFramePacingMs() const { return m_framePacingUs.load() / 1000.0; }

    PipelineMetrics& getPipelineMetrics() { return *m_metrics; }
    const PipelineMetrics& getPipelineMetrics() const { return *m_metrics; }
};
