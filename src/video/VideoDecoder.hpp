#pragma once

#include <string>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include "core/ThreadSafeQueue.hpp"
#include "core/MetricRing.hpp"
#include "video/ToneMapper.hpp"
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
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

struct ColorPipelineInfo {
    std::string colorSpace = "Unspecified";
    std::string colorPrimaries = "Unspecified";
    std::string transferChar = "Unspecified";
    std::string colorRange = "Unspecified";
    std::string chromaSubsampling = "Unknown";
    std::string pixelFormat = "Unknown";
    int bitDepth = 8;
    std::string hdrType = "SDR";
    bool isHDR = false;
    // True once an HDR frame has actually been converted to SDR by the
    // tone mapper. isHDR alone only says what the source claims; this
    // says what the pipeline did about it, which is what the diagnostics
    // HUD needs to report honestly.
    bool toneMapped = false;
    // Peaks the tone mapper resolved for this source, in nits. Only
    // meaningful while toneMapped is true.
    float toneMapSourceNits = 0.0f;
    float toneMapTargetNits = 0.0f;
};

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
    int m_allocatedTargetWidth;
    int m_allocatedTargetHeight;

    // HDR -> SDR conversion state. Two extra scaling contexts bracket the
    // tone mapper: the source's HDR YUV is unpacked to 16-bit RGB (where
    // tone mapping is meaningful), and the resulting SDR RGB is packed
    // back into the 8-bit YUV420P the rest of the pipeline expects. Kept
    // separate from m_swsCtx so the ordinary SDR path's cached
    // dimensions/format are never disturbed by an HDR file.
    naikav::video::ToneMapper m_toneMapper;
    SwsContext* m_hdrToRgbCtx = nullptr;
    SwsContext* m_rgbToYuvCtx = nullptr;
    AVFrame* m_hdrRgbFrame = nullptr;  // RGB48 (native endian), BT.2020, HDR-encoded
    AVFrame* m_sdrRgbFrame = nullptr;  // RGB24, BT.709, tone mapped
    int m_hdrSrcWidth = 0;
    int m_hdrSrcHeight = 0;
    AVPixelFormat m_hdrSrcFormat = AV_PIX_FMT_NONE;
    AVColorRange m_hdrSrcRange = AVCOL_RANGE_UNSPECIFIED;
    int m_hdrTargetWidth = 0;
    int m_hdrTargetHeight = 0;
    // Whether the most recently converted frame went through the tone
    // mapper. Read by getColorInfo(), which the UI thread calls under the
    // same mutex that guards convertFrame(), so a plain bool is enough.
    bool m_lastFrameToneMapped = false;

    void releaseHdrContexts();
    bool toneMapToYuv(const AVFrame* srcFrame, int targetW, int targetH,
                      const naikav::video::HdrToneMapSettings& settings);

    std::atomic<double> m_currentFramePts;
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

    // Live pointer to the demuxer's seek-generation counter (see
    // Demuxer::attachSeekGeneration / m_seekGeneration comment in
    // Demuxer.hpp). When set, a packet whose opaque-tagged generation
    // doesn't match this counter's current value is dropped as soon as it's
    // popped, before ever reaching the codec -- so a frame decoded from
    // before the most recent seek can never be produced in the first place.
    std::atomic<uint64_t>* m_seekGeneration = nullptr;

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

    // Must be called before decodeNextFrame() is first used from another
    // thread, since the decode loop isn't synchronized with this pointer
    // assignment. See m_seekGeneration above.
    void attachSeekGeneration(std::atomic<uint64_t>* gen) { m_seekGeneration = gen; }

    // Decode the next video frame from the queue.
    // Returns true if a frame was successfully decoded and stored in m_yuvFrame.
    bool decodeNextFrame();
    
    void flush();
    bool convertFrame(ResolutionOption option = ResolutionOption::ORIGINAL,
                      naikav::video::HdrToneMapSettings toneMap =
                          naikav::video::HdrToneMapSettings{});

    // The HDR transfer function a frame carries, or None for SDR. Static
    // and frame-driven rather than codec-driven: HLG and PQ are per-frame
    // properties, and a stream can carry frames that disagree with the
    // container's declared characteristics.
    static naikav::video::HdrTransfer hdrTransferOf(const AVFrame* frame);

    // Mastering-display peak luminance in nits, or 0 when the frame
    // carries no mastering metadata.
    static float masteringPeakNits(const AVFrame* frame);

    // Getters
    AVFrame* getYUVFrame() const { return m_yuvFrame; }
    double getCurrentFramePts() const { return m_currentFramePts.load(std::memory_order_relaxed); }
    int getWidth() const { return m_codecCtx ? m_codecCtx->width : 0; }
    int getHeight() const { return m_codecCtx ? m_codecCtx->height : 0; }
    bool isSeeking() const { return m_seeking.load(); }
    void setSeeking(bool seeking) { m_seeking.store(seeking); }
    std::string getPixelFormatName() const;
    bool isHardware() const { return m_codecCtx ? isHardwareDecoder(m_codecCtx->codec) : false; }
    ColorPipelineInfo getColorInfo() const;
};

extern bool g_disableHardwareDecoders;
