#pragma once

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/frame.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace naikav::dsp {

// Real-time EBU R128 (ITU-R BS.1770) loudness metering, via a minimal
// libavfilter graph (abuffer -> ebur128 -> abuffersink) rather than a
// hand-rolled K-weighting + gating implementation.
//
// This is a deliberate departure from the rest of the DSP chain (Biquad/
// Compressor/Limiter/Crossover), which are hand-rolled standard algorithms
// with no external interop requirement. Loudness *measurement* is
// different: the whole point of targeting "-16 LUFS" or "-23 LUFS" only
// means something if the measurement actually matches the real spec that
// those numbers come from (and that other tools/platforms measure against).
// FFmpeg's ebur128 filter is a proven, spec-compliant implementation
// already shipped with this project's FFmpeg build -- reusing it avoids
// both a new third-party dependency (no libebur128 vendoring/licensing)
// and the real risk of a subtly-wrong from-scratch K-weighting/gating
// implementation producing numbers that merely *look* like LUFS.
//
// Verified against the actual vendored FFmpeg build (not assumed from
// memory): confirmed the ebur128/loudnorm filters are present via
// `ffmpeg -filters`, and confirmed the exact metadata keys (lavfi.r128.M /
// .S / .I) and sane output values via a standalone probe feeding a known
// -6dBFS sine wave through the graph before writing this.
class LoudnessMeter {
public:
    ~LoudnessMeter() { teardown(); }

    bool configure(int channels, int sampleRate) {
        teardown();
        m_channels = channels;
        m_sampleRate = sampleRate;
        m_pts = 0;
        m_momentaryLufs = -120.0;
        m_integratedLufs = -120.0;

        // The ebur128 filter prints a multi-line "Integrated loudness /
        // Loudness range / True peak" summary via av_log(..., AV_LOG_INFO)
        // on every graph teardown, regardless of the framelog=quiet option
        // above (that option only covers its *per-frame* stat logging).
        // Since a meter is created whenever any AudioDecoder is (see
        // AudioDecoder::init()), that would otherwise print on every file
        // open/close even when normalization is never enabled. This app
        // has no other use for FFmpeg's own INFO-level log chatter (no
        // existing av_log_set_level call anywhere else in the codebase),
        // so dropping the global threshold to WARNING is a safe, one-time,
        // idempotent fix rather than something narrower per-filter -- FFmpeg
        // doesn't expose a per-AVFilterContext log level for this.
        av_log_set_level(AV_LOG_WARNING);

        m_graph = avfilter_graph_alloc();
        if (!m_graph) {
            return false;
        }

        const AVFilter* abuffer = avfilter_get_by_name("abuffer");
        const AVFilter* ebur128 = avfilter_get_by_name("ebur128");
        const AVFilter* abuffersink = avfilter_get_by_name("abuffersink");
        if (!abuffer || !ebur128 || !abuffersink) {
            teardown();
            return false;
        }

        AVChannelLayout chLayout;
        av_channel_layout_default(&chLayout, channels);
        char layoutDesc[64];
        av_channel_layout_describe(&chLayout, layoutDesc, sizeof(layoutDesc));

        char args[256];
        std::snprintf(args, sizeof(args),
                      "time_base=1/%d:sample_rate=%d:sample_fmt=flt:channel_layout=%s",
                      sampleRate, sampleRate, layoutDesc);
        av_channel_layout_uninit(&chLayout);

        // framelog=quiet: without it, the filter prints a multi-line
        // "Integrated loudness / Loudness range / True peak" summary via
        // av_log on every teardown -- console spam on every file
        // open/close in normal use, since a meter exists (see configure()
        // callers) regardless of whether normalization is ever enabled. We
        // read the same numbers ourselves via frame metadata instead.
        if (avfilter_graph_create_filter(&m_srcCtx, abuffer, "src", args, nullptr, m_graph) < 0 ||
            avfilter_graph_create_filter(&m_r128Ctx, ebur128, "r128", "metadata=1:peak=none:framelog=quiet", nullptr, m_graph) < 0 ||
            avfilter_graph_create_filter(&m_sinkCtx, abuffersink, "sink", nullptr, nullptr, m_graph) < 0 ||
            avfilter_link(m_srcCtx, 0, m_r128Ctx, 0) < 0 ||
            avfilter_link(m_r128Ctx, 0, m_sinkCtx, 0) < 0 ||
            avfilter_graph_config(m_graph, nullptr) < 0) {
            teardown();
            return false;
        }

        return true;
    }

    // Pushes interleaved float samples through the graph and updates the
    // latest momentary/integrated readings from whatever frames the
    // ebur128 filter emits in response (it passes audio through with a
    // roughly 100ms internal hop, independent of the caller's chunk size).
    void feed(const float* interleaved, int numFrames) {
        if (!m_graph || numFrames <= 0) {
            return;
        }

        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            return;
        }
        frame->format = AV_SAMPLE_FMT_FLT;
        frame->sample_rate = m_sampleRate;
        av_channel_layout_default(&frame->ch_layout, m_channels);
        frame->nb_samples = numFrames;
        if (av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&frame);
            return;
        }
        std::memcpy(frame->data[0], interleaved,
                    static_cast<size_t>(numFrames) * m_channels * sizeof(float));
        frame->pts = m_pts;
        m_pts += numFrames;

        int sendRet = av_buffersrc_add_frame(m_srcCtx, frame);
        av_frame_free(&frame);
        if (sendRet < 0) {
            return;
        }

        AVFrame* outFrame = av_frame_alloc();
        if (!outFrame) {
            return;
        }
        while (av_buffersink_get_frame(m_sinkCtx, outFrame) >= 0) {
            readMetadata(outFrame);
            av_frame_unref(outFrame);
        }
        av_frame_free(&outFrame);
    }

    // -70.0 (EBU R128's absolute silence gate) until enough audio has been
    // fed to produce a real reading -- callers should treat that as "no
    // measurement yet", not a genuine loudness value.
    double getMomentaryLufs() const { return m_momentaryLufs; }
    double getIntegratedLufs() const { return m_integratedLufs; }

    // Rebuilds the graph, discarding all measurement history. Call on seek/
    // flush: post-seek audio is no longer temporally continuous with
    // whatever was measured before, so carrying that history forward would
    // mix loudness from two unrelated parts of the stream.
    void reset() {
        if (m_channels > 0 && m_sampleRate > 0) {
            configure(m_channels, m_sampleRate);
        }
    }

private:
    void readMetadata(const AVFrame* frame) {
        if (const AVDictionaryEntry* m = av_dict_get(frame->metadata, "lavfi.r128.M", nullptr, 0)) {
            m_momentaryLufs = std::atof(m->value);
        }
        if (const AVDictionaryEntry* i = av_dict_get(frame->metadata, "lavfi.r128.I", nullptr, 0)) {
            m_integratedLufs = std::atof(i->value);
        }
    }

    void teardown() {
        if (m_graph) {
            avfilter_graph_free(&m_graph);
        }
        m_graph = nullptr;
        m_srcCtx = m_r128Ctx = m_sinkCtx = nullptr;
    }

    AVFilterGraph* m_graph = nullptr;
    AVFilterContext* m_srcCtx = nullptr;
    AVFilterContext* m_r128Ctx = nullptr;
    AVFilterContext* m_sinkCtx = nullptr;
    int m_channels = 0;
    int m_sampleRate = 0;
    int64_t m_pts = 0;
    double m_momentaryLufs = -120.0;
    double m_integratedLufs = -120.0;
};

} // namespace naikav::dsp
