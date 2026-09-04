#pragma once

#include <string>
#include <atomic>
#include <algorithm>
#include <cmath>
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
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/dovi_meta.h>
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

// Rounding applied to the display box before the fit below. A window
// being dragged reports a new size on essentially every pixel of the
// drag, and each distinct tone-mapping target reallocates the
// intermediate frame and forces the renderer to rebuild its texture.
// Quantising the box keeps that to one rebuild per 64 px of drag.
inline constexpr int kDisplayCapStep = 64;

// Shrink a tone-mapping target so it never exceeds what the display area
// can actually resolve, preserving aspect ratio.
//
// Tone mapping costs strictly per output pixel, and the result is scaled
// to fit the window before it is ever seen -- so mapping a 3840x2160
// frame into a 1024x576 window does about seven times the work the
// letterbox blit then throws away. Passing dispW/dispH <= 0 disables the
// cap, which is what every caller outside the player (tests, the bench
// harness) gets by default.
//
// Only ever downscales: a small source in a large window is left alone
// rather than being upsampled on the CPU to fill it.
inline void capToDisplaySize(int& targetW, int& targetH, int dispW, int dispH) {
    if (dispW <= 0 || dispH <= 0 || targetW <= 0 || targetH <= 0) {
        return;
    }

    const int boxW = ((dispW + kDisplayCapStep - 1) / kDisplayCapStep) * kDisplayCapStep;
    const int boxH = ((dispH + kDisplayCapStep - 1) / kDisplayCapStep) * kDisplayCapStep;
    if (boxW >= targetW && boxH >= targetH) {
        return;
    }

    const double scale = std::min(static_cast<double>(boxW) / targetW,
                                  static_cast<double>(boxH) / targetH);
    int cappedW = static_cast<int>(targetW * scale);
    int cappedH = static_cast<int>(targetH * scale);

    // Even dimensions, matching getTargetDimensions(): the tone mapper's
    // output is packed RGB and would not care, but the unpack ahead of it
    // still resamples subsampled chroma.
    cappedW = (cappedW / 2) * 2;
    cappedH = (cappedH / 2) * 2;
    if (cappedW < 2) cappedW = 2;
    if (cappedH < 2) cappedH = 2;

    targetW = cappedW;
    targetH = cappedH;
}

// Pick the peak brightness to tone map *from*, given the three sources
// that can claim to know it. Returns 0 when none of them do, which leaves
// the tone mapper on its own default.
//
// `explicitNits` is the user's override and always wins -- it exists
// precisely for files whose metadata is wrong.
//
// Otherwise the two metadata figures answer different questions.
// Mastering display max_luminance is the peak of the *monitor the film
// was graded on*; MaxCLL (content light level) is the brightest pixel
// that actually occurs in the *content*. They routinely disagree by a
// lot: a grade delivered on a 4000-nit reference monitor whose brightest
// shot only reaches 800 nits carries both numbers. Mapping from 4000 then
// spends most of the tone curve's range on brightness the file never
// uses, and the picture comes out dim.
//
// So prefer MaxCLL, and use the mastering peak as a ceiling on it --
// content cannot be brighter than the display it was graded on, and a
// MaxCLL above it is metadata that disagrees with itself. Either may be
// absent, in which case the other is used alone.
// `dynamicNits` is the peak of *this frame* as reported by HDR10+ or
// Dolby Vision dynamic metadata, or 0 when the file carries none. It
// takes precedence over the static figures -- that is the entire point
// of per-frame metadata: a dark scene in a 4000-nit grade should be
// mapped as the dark scene it is, not as though every frame reached the
// grade's peak.
//
// It is capped by the mastering display, which is a physical ceiling,
// but deliberately *not* by MaxCLL. The two describe the same quantity
// at different granularity and are generally written by different tools,
// so they disagree by small margins in real files -- the HDR10+ test
// clip reports a per-frame 700 nits against a MaxCLL of 683. Letting the
// coarser static figure clamp the finer per-frame one would discard the
// dynamic metadata on every frame that disagrees at all, which is most
// of them. MaxCLL stays what it always was: the static fallback for
// files with no dynamic metadata at all.
inline float selectSourcePeakNits(float explicitNits, float masteringNits,
                                  float contentLightNits,
                                  float dynamicNits = 0.0f) {
    if (explicitNits > 0.0f) {
        return explicitNits;
    }

    float staticPeak = 0.0f;
    if (contentLightNits > 0.0f && masteringNits > 0.0f) {
        staticPeak = std::min(contentLightNits, masteringNits);
    } else if (contentLightNits > 0.0f) {
        staticPeak = contentLightNits;
    } else if (masteringNits > 0.0f) {
        staticPeak = masteringNits;
    }

    if (dynamicNits > 0.0f) {
        return masteringNits > 0.0f ? std::min(dynamicNits, masteringNits)
                                    : dynamicNits;
    }
    return staticPeak;
}

// ---------------------------------------------------------------------
// Adaptive tone-map resolution
// ---------------------------------------------------------------------
//
// capToDisplaySize() above stops the pipeline mapping more pixels than
// the window can show, which is enough whenever the window is the only
// thing making the work too big. It is not enough when the *source* is
// too heavy for the machine: 4K60 AV1 has to be decoded in software on
// hardware with no AV1 decoder, and decode alone then wants most of the
// CPU. Tone mapping the full window on top of that overruns the frame
// budget, frames arrive late, and the late-frame drop turns a steady
// picture into a lurching one -- measured on the 60 fps LG demo clip,
// on-screen delivery swung between 10 and 44 fps with gaps up to 200 ms,
// which reads as flashing rather than as a low frame rate.
//
// Dropping frames is the wrong lever there, because the expensive part
// (the decode) has already been paid before the drop decision is made.
// Shrinking the tone-mapping target is the right one: it is the only
// cost in the chain that scales smoothly, and a slightly softer picture
// that holds its frame rate beats a sharp one that stutters.
inline constexpr double kAdaptiveShrinkFraction = 0.70;  // of frame budget
inline constexpr double kAdaptiveGrowFraction = 0.45;
inline constexpr float kAdaptiveMinScale = 0.40f;
inline constexpr float kAdaptiveScaleStep = 0.85f;
inline constexpr int kAdaptiveShrinkFrames = 3;   // react quickly
inline constexpr int kAdaptiveGrowFrames = 45;    // recover slowly

// Tracks how much to shrink the tone-mapping target for the machine to
// hold the source's frame rate. Asymmetric on purpose: three overrunning
// frames are enough to step down, but it takes a couple of seconds of
// headroom to step back up, so the picture does not visibly breathe
// every time one frame happens to be expensive.
class AdaptiveToneMapScale {
public:
    // convertMs: what the last tone-mapped frame actually cost.
    // budgetMs:  1000 / source fps, or <= 0 when the rate is unknown, in
    //            which case the scale is left exactly as it is.
    float update(double convertMs, double budgetMs) {
        if (budgetMs <= 0.0 || convertMs <= 0.0) {
            return m_scale;
        }
        if (convertMs > budgetMs * kAdaptiveShrinkFraction) {
            m_under = 0;
            if (++m_over >= kAdaptiveShrinkFrames) {
                m_over = 0;
                m_scale = std::max(m_scale * kAdaptiveScaleStep, kAdaptiveMinScale);
            }
        } else if (convertMs < budgetMs * kAdaptiveGrowFraction) {
            m_over = 0;
            if (++m_under >= kAdaptiveGrowFrames) {
                m_under = 0;
                m_scale = std::min(m_scale / kAdaptiveScaleStep, 1.0f);
            }
        } else {
            // Inside the deadband: this is where we want to sit.
            m_over = 0;
            m_under = 0;
        }
        return m_scale;
    }

    void reset() {
        m_scale = 1.0f;
        m_over = 0;
        m_under = 0;
    }
    float scale() const { return m_scale; }

private:
    float m_scale = 1.0f;
    int m_over = 0;
    int m_under = 0;
};

// Shrink a target by the adaptive scale, keeping it even (the unpack
// still resamples subsampled chroma) and never below 2 px.
inline void applyAdaptiveScale(int& targetW, int& targetH, float scale) {
    if (scale >= 1.0f || scale <= 0.0f || targetW <= 0 || targetH <= 0) {
        return;
    }
    int w = static_cast<int>(targetW * scale);
    int h = static_cast<int>(targetH * scale);
    w = (w / 2) * 2;
    h = (h / 2) * 2;
    targetW = (w < 2) ? 2 : w;
    targetH = (h < 2) ? 2 : h;
}

// Damping for the per-frame peak above.
//
// Feeding a raw per-frame peak straight into the tone curve makes the
// picture pump: consecutive frames within one shot differ enough in
// maxscl that the roll-off visibly breathes, which is more distracting
// than the static mapping it replaces. So the peak is smoothed, and only
// a large jump -- a cut -- is followed immediately.
inline constexpr float kDynamicPeakAlpha = 0.15f;      // per-frame EMA weight
inline constexpr float kDynamicPeakSnapRatio = 1.5f;   // treat as a scene cut
inline constexpr float kDynamicPeakQuantNits = 25.0f;  // LUT rebuild step

// Smooths the per-frame peak and quantises it, so that the tone mapper's
// tables are rebuilt only when the value moves meaningfully rather than
// on every frame (a rebuild is ~10k transcendental evaluations).
class DynamicPeakTracker {
public:
    // Returns the peak to configure the tone mapper with, or 0 when there
    // is no dynamic metadata to act on.
    float update(float framePeakNits) {
        if (framePeakNits <= 0.0f) {
            return 0.0f;
        }
        if (m_smoothed <= 0.0f) {
            m_smoothed = framePeakNits;
        } else {
            // A cut can move the peak by orders of magnitude, and easing
            // into it over a dozen frames would be a visible fade. Only
            // within-shot drift is smoothed.
            const float ratio = framePeakNits / m_smoothed;
            if (ratio > kDynamicPeakSnapRatio || ratio < 1.0f / kDynamicPeakSnapRatio) {
                m_smoothed = framePeakNits;
            } else {
                m_smoothed += (framePeakNits - m_smoothed) * kDynamicPeakAlpha;
            }
        }
        // Quantise upward so the reported peak is never below the frame's
        // smoothed value, which would clip highlights.
        const float steps = std::ceil(m_smoothed / kDynamicPeakQuantNits);
        return std::max(steps * kDynamicPeakQuantNits, kDynamicPeakQuantNits);
    }

    // Called on a seek: the frame after a jump has nothing to do with the
    // one before it, so the smoothed state must not leak across.
    void reset() { m_smoothed = 0.0f; }

    float smoothedNits() const { return m_smoothed; }

private:
    float m_smoothed = 0.0f;
};

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
    // True when the source peak above came from this frame's HDR10+ /
    // Dolby Vision metadata rather than the file's static figures. Same
    // rule as toneMapped: report what the pipeline did, not what the
    // source merely claimed to carry.
    bool toneMapDynamic = false;
    // The smoothed per-frame peak itself, before it is reconciled with
    // the static metadata. 0 when there is no dynamic metadata.
    float toneMapDynamicNits = 0.0f;
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

    // HDR -> SDR conversion state. One scaling context feeds the tone
    // mapper: the source's HDR YUV is unpacked to 16-bit RGB, where tone
    // mapping is meaningful. The mapper then writes 8-bit RGB straight
    // into the output frame, which the renderer uploads as an RGB
    // texture -- converting it back to YUV first only to have the GPU
    // undo that is a whole extra pass over the frame for nothing.
    //
    // Kept separate from m_swsCtx so the ordinary SDR path's cached
    // dimensions/format are never disturbed by an HDR file. Allocated
    // through sws_alloc_context() rather than sws_getContext() because
    // only the former exposes `threads`: this conversion is the single
    // most expensive step in 4K HDR playback and swscale will only
    // slice it across cores when driven by sws_scale_frame().
    naikav::video::ToneMapper m_toneMapper;
    SwsContext* m_hdrToRgbCtx = nullptr;
    AVFrame* m_hdrRgbFrame = nullptr;  // RGB48 (native endian), BT.2020, HDR-encoded
    int m_hdrTargetWidth = 0;
    int m_hdrTargetHeight = 0;
    // Whether the most recently converted frame went through the tone
    // mapper. Read by getColorInfo(), which the UI thread calls under the
    // same mutex that guards convertFrame(), so a plain bool is enough.
    bool m_lastFrameToneMapped = false;
    // Smoothed HDR10+/Dolby Vision peak, and the value the last converted
    // frame actually used (0 = the frame was mapped from static
    // metadata). Both touched only on the decode thread; the HUD reads
    // the latter the same racy-but-benign way as m_lastFrameToneMapped.
    // Static HDR metadata from the stream header, used when the decoded
    // frame carries none. See streamMasteringPeakNits().
    float m_streamMasteringNits = 0.0f;
    float m_streamContentLightNits = 0.0f;
    void readStreamHdrMetadata();
    void computeFrameBudget();

public:
    // Frame rate of the source, used to size the adaptive tone-map
    // budget. Call before init(); 0 means "unknown", which disables
    // adaptation. Kept separate from the constructor so the bench and the
    // tests, which build a decoder straight from codec parameters, are
    // unaffected.
    void setSourceFrameRate(double fps) { m_sourceFrameRate = fps; }

    // Static HDR peaks recovered from the bitstream by the demuxer, used
    // when neither the frame nor the stream header carries them -- the
    // hardware-decoded-Matroska case. Call before init(). 0 = unknown.
    void setProbedHdrMetadata(float masteringNits, float contentLightNits) {
        m_probedMasteringNits = masteringNits;
        m_probedContentLightNits = contentLightNits;
    }

private:
    double m_sourceFrameRate = 0.0;
    float m_probedMasteringNits = 0.0f;
    float m_probedContentLightNits = 0.0f;

    AdaptiveToneMapScale m_adaptiveScale;
    // 1000 / source fps, or 0 when the stream does not declare a rate.
    double m_frameBudgetMs = 0.0;

    DynamicPeakTracker m_dynamicPeak;
    float m_lastFrameDynamicPeak = 0.0f;

    void releaseHdrContexts();
    bool toneMapFrame(const AVFrame* srcFrame, int targetW, int targetH,
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
    // displayWidth/displayHeight are the size, in pixels, of the area the
    // frame will be drawn into. They cap the HDR tone-mapping resolution
    // (see capToDisplaySize) and are ignored on the SDR path, which has
    // to honour the resolution selector exactly. 0 means "unknown", which
    // disables the cap.
    bool convertFrame(ResolutionOption option = ResolutionOption::ORIGINAL,
                      naikav::video::HdrToneMapSettings toneMap =
                          naikav::video::HdrToneMapSettings{},
                      int displayWidth = 0, int displayHeight = 0);

    // The HDR transfer function a frame carries, or None for SDR. Static
    // and frame-driven rather than codec-driven: HLG and PQ are per-frame
    // properties, and a stream can carry frames that disagree with the
    // container's declared characteristics.
    static naikav::video::HdrTransfer hdrTransferOf(const AVFrame* frame);

    // Mastering-display peak luminance in nits, or 0 when the frame
    // carries no mastering metadata.
    static float masteringPeakNits(const AVFrame* frame);

    // MaxCLL -- the brightest pixel present anywhere in the content, in
    // nits -- or 0 when the frame carries no content-light-level
    // metadata. Combined with the mastering peak by
    // selectSourcePeakNits(); see the reasoning there.
    static float contentLightPeakNits(const AVFrame* frame);

    // Peak luminance of *this frame* from dynamic metadata -- HDR10+
    // (SMPTE ST 2094-40) or the Dolby Vision RPU's L1 block -- or 0 when
    // the frame carries neither. Unlike the two static figures above this
    // changes shot to shot, which is what makes tone mapping "dynamic";
    // run it through DynamicPeakTracker before use.
    static float dynamicPeakNits(const AVFrame* frame);

    // The same two static figures read from the *stream* rather than a
    // frame. Hardware decoders (QSV in particular) hand back frames with
    // no side data attached at all, so without this fallback an HDR file
    // decoded on the GPU is tone mapped from the generic default instead
    // of from what the file actually says. Filled once in init().
    float streamMasteringPeakNits() const { return m_streamMasteringNits; }
    float streamContentLightPeakNits() const { return m_streamContentLightNits; }

    // Current adaptive tone-map scale (1.0 = full target). Diagnostic.
    float adaptiveToneMapScale() const { return m_adaptiveScale.scale(); }

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
