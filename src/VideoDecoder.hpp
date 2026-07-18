#pragma once

#include <atomic>
#include <algorithm>
#include "ThreadSafeQueue.hpp"
#include "MetricRing.hpp"
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}


enum class ResolutionOption {
    ORIGINAL = 0,
    R_360P,   // 640x360
    R_480P,   // 854x480
    R_720P,   // 1280x720
    R_1080P,  // 1920x1080
    R_1440P,  // 2560x1440
    R_4K,     // 3840x2160
    COUNT
};

inline void getTargetDimensions(ResolutionOption option, int nativeW, int nativeH, int& targetW, int& targetH) {
    if (option == ResolutionOption::ORIGINAL || nativeW <= 0 || nativeH <= 0) {
        targetW = nativeW;
        targetH = nativeH;
        return;
    }
    
    int boxW = 0;
    int boxH = 0;
    switch (option) {
        case ResolutionOption::R_360P:  boxW = 640;  boxH = 360;  break;
        case ResolutionOption::R_480P:  boxW = 854;  boxH = 480;  break;
        case ResolutionOption::R_720P:  boxW = 1280; boxH = 720;  break;
        case ResolutionOption::R_1080P: boxW = 1920; boxH = 1080; break;
        case ResolutionOption::R_1440P: boxW = 2560; boxH = 1440; break;
        case ResolutionOption::R_4K:    boxW = 3840; boxH = 2160; break;
        default:
            targetW = nativeW;
            targetH = nativeH;
            return;
    }
    
    double scale = std::min(static_cast<double>(boxW) / nativeW, static_cast<double>(boxH) / nativeH);
    targetW = static_cast<int>(nativeW * scale);
    targetH = static_cast<int>(nativeH * scale);
    
    // YUV formats require width and height to be even
    targetW = (targetW / 2) * 2;
    targetH = (targetH / 2) * 2;
    if (targetW < 2) targetW = 2;
    if (targetH < 2) targetH = 2;
}

class VideoDecoder {
private:
    AVCodecParameters* m_codecParams;
    AVCodecContext* m_codecCtx;
    SwsContext* m_swsCtx;
    
    ThreadSafeQueue<AVPacket*>& m_queue;
    AVRational m_timeBase;
    int64_t m_startTime;
    
    AVFrame* m_decodedFrame;
    AVFrame* m_yuvFrame;
    uint8_t* m_yuvBuffer;
    int m_yuvBufferSize;
    int m_allocatedWidth;
    int m_allocatedHeight;
    AVPixelFormat m_allocatedFormat;

    double m_currentFramePts;
    std::atomic<bool> m_flushRequested;
    bool m_startTimeSaved;
    std::atomic<bool> m_seeking;
    int m_consecutiveEagainCount;
    int m_hardwareRecoveryAttempts;
    MetricRing<256>& m_decodeTimeRing;
    MetricRing<256>& m_convertTimeRing;
    std::atomic<bool>& m_profilingEnabled;
    std::chrono::steady_clock::time_point m_decodeStart;
    bool m_hasDecodeStart = false;

    static bool isHardwareDecoder(const AVCodec* codec) noexcept;
    static bool isHardwarePixelFormat(AVPixelFormat fmt);
    bool fallbackToSoftware();
    bool reopenHardwareDecoder();
    bool recoverHardwareDecoder();

public:
    VideoDecoder(AVCodecParameters* codecParams, 
                 AVRational timeBase, 
                 int64_t startTime,
                 ThreadSafeQueue<AVPacket*>& queue,
                 MetricRing<256>& decodeTimeRing,
                 MetricRing<256>& convertTimeRing,
                 std::atomic<bool>& profilingEnabled);
    VideoDecoder(AVCodecParameters* codecParams, 
                 AVRational timeBase, 
                 int64_t startTime,
                 ThreadSafeQueue<AVPacket*>& queue,
                 std::atomic<uint64_t>* decodeTimeTracker = nullptr);
    ~VideoDecoder();

    bool init();
    
    // Decode the next video frame from the queue.
    // Returns true if a frame was successfully decoded and stored in m_yuvFrame.
    bool decodeNextFrame();
    
    void flush();
    bool convertFrame(ResolutionOption option = ResolutionOption::ORIGINAL);

    // Getters
    AVFrame* getYUVFrame() const { return m_yuvFrame; }
    double getCurrentFramePts() const { return m_currentFramePts; }
    int getWidth() const { return m_codecCtx ? m_codecCtx->width : 0; }
    int getHeight() const { return m_codecCtx ? m_codecCtx->height : 0; }
    bool isSeeking() const { return m_seeking.load(); }
    void setSeeking(bool seeking) { m_seeking.store(seeking); }
    std::string getPixelFormatName() const;
    bool isHardware() const { return m_codecCtx ? isHardwareDecoder(m_codecCtx->codec) : false; }
};

extern bool g_disableHardwareDecoders;
