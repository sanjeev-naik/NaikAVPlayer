#pragma once

#include "core/FFmpegCompat.hpp"

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

#include <atomic>
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
// Two things here exist because feed() runs on the SDL audio callback
// thread:
//
//   - The input and output AVFrames are members, reused across calls. The
//     previous version allocated a fresh AVFrame and its sample buffer on
//     every call, memcpied the block into it, freed it, then allocated a
//     second frame to drain the sink and freed that too -- four FFmpeg
//     allocations per block, on the real-time thread.
//
//   - reset() does NOT rebuild the graph inline. Discarding ebur128's
//     history means constructing a new graph (avfilter_graph_alloc, three
//     create_filter calls with string argument parsing, two links, and
//     avfilter_graph_config, which runs FFmpeg's format negotiation) --
//     milliseconds of work, one to two orders of magnitude over a callback
//     budget, and it used to run inside the audio callback on every seek
//     while holding the DSP mutex. A guaranteed dropout. Instead a spare
//     graph is built up front; requestReset() swaps to it in O(1) on the
//     audio thread and flags the retired one, and serviceRebuild() --
//     called from the main thread, see PlayerController -- rebuilds it
//     into the next spare.
class LoudnessMeter {
public:
    ~LoudnessMeter() {
        destroyGraph(m_active);
        destroyGraph(m_spare);
        if (m_inFrame) av_frame_free(&m_inFrame);
        if (m_outFrame) av_frame_free(&m_outFrame);
    }

    bool configure(int channels, int sampleRate) {
        destroyGraph(m_active);
        destroyGraph(m_spare);
        m_channels = channels;
        m_sampleRate = sampleRate;
        m_pts = 0;
        m_momentaryLufs.store(-120.0, std::memory_order_relaxed);
        m_integratedLufs.store(-120.0, std::memory_order_relaxed);
        m_rebuildPending.store(false, std::memory_order_relaxed);
        m_spareReady.store(false, std::memory_order_relaxed);

        // The ebur128 filter prints a multi-line "Integrated loudness /
        // Loudness range / True peak" summary via av_log(..., AV_LOG_INFO)
        // on every graph teardown, regardless of the framelog=quiet option
        // below (that option only covers its *per-frame* stat logging).
        // Since a meter is created whenever any AudioDecoder is (see
        // AudioDecoder::init()), that would otherwise print on every file
        // open/close even when normalization is never enabled. This app
        // has no other use for FFmpeg's own INFO-level log chatter (no
        // existing av_log_set_level call anywhere else in the codebase),
        // so dropping the global threshold to WARNING is a safe, one-time,
        // idempotent fix rather than something narrower per-filter -- FFmpeg
        // doesn't expose a per-AVFilterContext log level for this.
        av_log_set_level(AV_LOG_WARNING);

        if (!buildGraph(m_active)) {
            return false;
        }

        if (!allocateFrames()) {
            destroyGraph(m_active);
            return false;
        }

        // Best-effort: a spare makes seeks free, but the meter still works
        // (falling back to a synchronous rebuild) if this cannot be built.
        if (buildGraph(m_spare)) {
            m_spareReady.store(true, std::memory_order_release);
        }
        return true;
    }

    // Pushes interleaved float samples through the graph and updates the
    // latest momentary/integrated readings from whatever frames the
    // ebur128 filter emits in response (it passes audio through with a
    // roughly 100ms internal hop, independent of the caller's chunk size).
    //
    // Allocation-free in steady state: the sample buffer only grows when a
    // block larger than any seen before arrives.
    void feed(const float* interleaved, int numFrames) {
        if (!m_active.graph || numFrames <= 0 || !m_inFrame || !m_outFrame) {
            return;
        }
        if (!ensureFrameCapacity(numFrames)) {
            return;
        }

        m_inFrame->nb_samples = numFrames;
        std::memcpy(m_inFrame->data[0], interleaved,
                    static_cast<size_t>(numFrames) * m_channels * sizeof(float));
        m_inFrame->pts = m_pts;
        m_pts += numFrames;

        // KEEP_REF so the source does not take ownership of our reusable
        // frame -- without it the buffer would be consumed and we would be
        // back to allocating one per call.
        const int sendRet = av_buffersrc_add_frame_flags(
            m_active.srcCtx, m_inFrame, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (sendRet < 0) {
            return;
        }

        drainSink();
    }

    // Pushes end-of-stream through the graph and drains whatever the
    // filter still holds. Without this the last partial ~100ms gating
    // window is never emitted, so a whole-file scan silently omits it --
    // see prescanIntegratedLufs(), whose entire purpose is an accurate
    // whole-file number.
    void flush() {
        if (!m_active.graph || !m_outFrame) return;
        // A null frame signals end-of-stream. Nothing useful to do if the
        // source rejects it -- drain whatever the filter already produced
        // either way.
        const int flushRet = av_buffersrc_add_frame(m_active.srcCtx, nullptr);
        (void)flushRet;
        drainSink();
    }

    // -70.0 (EBU R128's absolute silence gate) until enough audio has been
    // fed to produce a real reading -- callers should treat that as "no
    // measurement yet", not a genuine loudness value.
    double getMomentaryLufs() const { return m_momentaryLufs.load(std::memory_order_relaxed); }
    double getIntegratedLufs() const { return m_integratedLufs.load(std::memory_order_relaxed); }

    // Real-time safe. Discards all measurement history by switching to a
    // pre-built spare graph, and flags the retired one for rebuilding by
    // serviceRebuild() on a non-real-time thread. Call on seek/flush:
    // post-seek audio is no longer temporally continuous with whatever was
    // measured before, so carrying that history forward would mix loudness
    // from two unrelated parts of the stream.
    //
    // If no spare is ready yet (a second seek before the first rebuild
    // finished), the readings are invalidated but the graph keeps running:
    // the numbers are reported as "no measurement" rather than as a wrong
    // measurement, and correct themselves once the spare lands.
    void requestReset() {
        m_pts = 0;
        m_momentaryLufs.store(-120.0, std::memory_order_relaxed);
        m_integratedLufs.store(-120.0, std::memory_order_relaxed);

        if (m_spareReady.load(std::memory_order_acquire)) {
            Graph tmp = m_active;
            m_active = m_spare;
            m_spare = tmp;
            m_spareReady.store(false, std::memory_order_release);
            m_rebuildPending.store(true, std::memory_order_release);
        }
    }

    // True while a retired graph is waiting to be rebuilt.
    bool needsRebuild() const { return m_rebuildPending.load(std::memory_order_acquire); }

    // Rebuilds the retired graph into the next spare. MUST be called from
    // a non-real-time thread (see PlayerController's per-frame poll).
    //
    // Safe without a lock: while m_rebuildPending is true, m_spareReady is
    // false, so the audio thread will not touch m_spare -- this thread
    // owns it exclusively for the duration.
    void serviceRebuild() {
        if (!m_rebuildPending.load(std::memory_order_acquire)) return;
        destroyGraph(m_spare);
        const bool ok = buildGraph(m_spare);
        m_rebuildPending.store(false, std::memory_order_release);
        if (ok) {
            m_spareReady.store(true, std::memory_order_release);
        }
    }

    // Synchronous full reset, for non-real-time callers (tests, the
    // offline prescan) that want the history gone right now and do not
    // care about the cost. Audio-thread callers must use requestReset().
    void reset() {
        if (m_channels > 0 && m_sampleRate > 0) {
            configure(m_channels, m_sampleRate);
        }
    }

private:
    struct Graph {
        AVFilterGraph* graph = nullptr;
        AVFilterContext* srcCtx = nullptr;
        AVFilterContext* r128Ctx = nullptr;
        AVFilterContext* sinkCtx = nullptr;
    };

    bool allocateFrames() {
        if (!m_inFrame) m_inFrame = av_frame_alloc();
        if (!m_outFrame) m_outFrame = av_frame_alloc();
        if (!m_inFrame || !m_outFrame) return false;
        m_frameCapacity = 0; // force a (re)allocation on the first feed
        return true;
    }

    // Grows the reusable input frame's sample buffer when a larger block
    // arrives than any seen so far. Steady-state playback hands the same
    // block size every time, so this runs once.
    bool ensureFrameCapacity(int numFrames) {
        if (m_frameCapacity >= numFrames) return true;
        av_frame_unref(m_inFrame);
        m_inFrame->format = AV_SAMPLE_FMT_FLT;
        m_inFrame->sample_rate = m_sampleRate;
        naikavSetFrameChannelLayout(m_inFrame, m_channels);
        m_inFrame->nb_samples = numFrames;
        if (av_frame_get_buffer(m_inFrame, 0) < 0) {
            m_frameCapacity = 0;
            return false;
        }
        m_frameCapacity = numFrames;
        return true;
    }

    void drainSink() {
        while (av_buffersink_get_frame(m_active.sinkCtx, m_outFrame) >= 0) {
            readMetadata(m_outFrame);
            av_frame_unref(m_outFrame);
        }
    }

    bool buildGraph(Graph& g) {
        g.graph = avfilter_graph_alloc();
        if (!g.graph) {
            return false;
        }

        const AVFilter* abuffer = avfilter_get_by_name("abuffer");
        const AVFilter* ebur128 = avfilter_get_by_name("ebur128");
        const AVFilter* abuffersink = avfilter_get_by_name("abuffersink");
        if (!abuffer || !ebur128 || !abuffersink) {
            destroyGraph(g);
            return false;
        }

        char layoutDesc[64];
        naikavDescribeDefaultChannelLayout(m_channels, layoutDesc,
                                           sizeof(layoutDesc));

        char args[256];
        std::snprintf(args, sizeof(args),
                      "time_base=1/%d:sample_rate=%d:sample_fmt=flt:channel_layout=%s",
                      m_sampleRate, m_sampleRate, layoutDesc);

        // framelog=quiet: without it, the filter prints a multi-line
        // "Integrated loudness / Loudness range / True peak" summary via
        // av_log on every teardown -- console spam on every file
        // open/close in normal use, since a meter exists (see configure()
        // callers) regardless of whether normalization is ever enabled. We
        // read the same numbers ourselves via frame metadata instead.
        if (avfilter_graph_create_filter(&g.srcCtx, abuffer, "src", args, nullptr, g.graph) < 0 ||
            avfilter_graph_create_filter(&g.r128Ctx, ebur128, "r128", "metadata=1:peak=none:framelog=quiet", nullptr, g.graph) < 0 ||
            avfilter_graph_create_filter(&g.sinkCtx, abuffersink, "sink", nullptr, nullptr, g.graph) < 0 ||
            avfilter_link(g.srcCtx, 0, g.r128Ctx, 0) < 0 ||
            avfilter_link(g.r128Ctx, 0, g.sinkCtx, 0) < 0 ||
            avfilter_graph_config(g.graph, nullptr) < 0) {
            destroyGraph(g);
            return false;
        }
        return true;
    }

    void readMetadata(const AVFrame* frame) {
        if (const AVDictionaryEntry* m = av_dict_get(frame->metadata, "lavfi.r128.M", nullptr, 0)) {
            m_momentaryLufs.store(std::atof(m->value), std::memory_order_relaxed);
        }
        if (const AVDictionaryEntry* i = av_dict_get(frame->metadata, "lavfi.r128.I", nullptr, 0)) {
            m_integratedLufs.store(std::atof(i->value), std::memory_order_relaxed);
        }
    }

    static void destroyGraph(Graph& g) {
        if (g.graph) {
            avfilter_graph_free(&g.graph);
        }
        g.graph = nullptr;
        g.srcCtx = g.r128Ctx = g.sinkCtx = nullptr;
    }

    Graph m_active;
    Graph m_spare;
    std::atomic<bool> m_spareReady{false};
    std::atomic<bool> m_rebuildPending{false};

    AVFrame* m_inFrame = nullptr;
    AVFrame* m_outFrame = nullptr;
    int m_frameCapacity = 0;

    int m_channels = 0;
    int m_sampleRate = 0;
    int64_t m_pts = 0;
    // Written on the audio callback thread (readMetadata, via feed()) and
    // read from the UI thread for HUD display, with no lock on either
    // side. Plain doubles here were a data race: a torn or stale read is
    // undefined behaviour, not merely an out-of-date number. Relaxed
    // ordering is sufficient -- these are independent scalars that carry
    // no happens-before relationship with any other state.
    std::atomic<double> m_momentaryLufs{-120.0};
    std::atomic<double> m_integratedLufs{-120.0};
};

} // namespace naikav::dsp
