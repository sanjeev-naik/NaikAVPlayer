#pragma once

#include "audio/dsp/LoudnessMeter.hpp"
#include "audio/dsp/DspMath.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace naikav::dsp {

// Applies a heavily-smoothed gain correction toward a target integrated
// loudness (LUFS), driven by LoudnessMeter's live EBU R128 measurement.
// Runs as its own pipeline stage after the DSP chain (EQ/compressor/
// limiter/crossover), matching the plan's "DSP chain -> loudness
// normalization" ordering -- it corrects the *overall* level of the
// already-processed signal, not a per-band or dynamics decision.
//
// The smoothing time constant is deliberately on the order of seconds, not
// milliseconds: unlike the Limiter/Compressor, which must react fast
// enough to catch transients, loudness normalization is meant to drift the
// program's overall level toward the target over time, never audibly
// "pump" in response to a single loud or quiet passage.
//
// This is primarily a real-time/streaming measurement: absent priming (see
// primeWithPrescannedLufs() below), it only knows the loudness of what's
// been played so far in the current continuous playback segment, not the
// whole file in advance, so the applied gain ramps in over kSmoothingSeconds
// as the streaming measurement converges. A seek calls reset(), since the
// stream is no longer temporally continuous with whatever was measured
// before.
//
// Two-pass mode: primeWithPrescannedLufs() supplies the whole-file
// integrated loudness measured in advance (see LoudnessPrescan.hpp, which
// decodes the file once up front through the same EBU R128 metering used
// here). Once primed, process() uses that fixed value instead of the
// still-converging streaming measurement, so the correct gain applies from
// the very first block -- fixing both the "wrong for the first few
// seconds" startup behavior and the reset-to-zero-gain that would
// otherwise happen on every seek. The live LoudnessMeter keeps running
// alongside this (for momentary-LUFS HUD display), but no longer drives
// the applied gain once primed.
//
// Metering does NOT run on the audio thread. process() only copies the
// block into a lock-free SPSC ring; serviceMetering(), called from a
// normal thread, drains that ring into the EBU R128 filter graph.
//
// It used to call LoudnessMeter::feed() inline, which pushed a frame
// through libavfilter, ran ebur128's K-weighting over every channel and
// allocated an AVDictionary of metadata per emitted frame -- all on the
// SDL audio callback. The spare-graph swap removed the *reset* cost but
// left that steady-state cost on every single block. Nothing about the
// measurement needs to be there: the applied gain moves on a 3-second
// time constant, so metering that lags by a frame or two is invisible,
// while an allocator call on the callback thread is not.
//
// Disabled by default: process() then does nothing at all -- it neither
// touches the signal nor queues anything for metering.
class LoudnessNormalizer {
public:
    void configure(int channels, double sampleRate) {
        m_meter.configure(channels, static_cast<int>(sampleRate));
        m_channels = channels;
        m_sampleRate = sampleRate;

        // Half a second of audio. serviceMetering() is driven at UI frame
        // rate (~16 ms), so this is ~30x the headroom a normal service
        // interval needs -- enough to ride out a long render-thread stall
        // without dropping measurement data.
        const int perSecond = (sampleRate > 0.0 && channels > 0)
                                  ? static_cast<int>(sampleRate) * channels
                                  : 48000 * 2;
        const int capacity = nextPowerOfTwo(std::max(1024, perSecond / 2));
        m_ring.assign(static_cast<size_t>(capacity), 0.0f);
        m_ringMask = static_cast<size_t>(capacity) - 1;
        m_writePos.store(0, std::memory_order_relaxed);
        m_readPos.store(0, std::memory_order_relaxed);
        m_ringFlushGen.store(0, std::memory_order_relaxed);
        m_servicedFlushGen = 0;
        m_meterScratch.assign(static_cast<size_t>(capacity), 0.0f);
        m_currentGainDb.store(0.0f, std::memory_order_relaxed);
        m_appliedGainLinear = 1.0f;
        m_hasPrescan.store(false, std::memory_order_relaxed);
        m_prescannedLufs.store(-120.0, std::memory_order_relaxed);
    }

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    // Common targets: -23 LUFS (EBU R128 broadcast), -16 LUFS (streaming,
    // e.g. Spotify/YouTube-ish), -14 LUFS (some streaming platforms).
    void setTargetLufs(float lufs) { m_targetLufs = lufs; }
    float getTargetLufs() const { return m_targetLufs; }

    // Supplies a whole-file integrated LUFS measurement obtained in advance
    // (see LoudnessPrescan::prescanIntegratedLufs()), switching this
    // instance into two-pass mode: process() computes the target gain from
    // this fixed value and jumps m_currentGainDb straight to it, so there's
    // no ramp-up period. A no-op measurement (the -70 LUFS silence gate, or
    // below) is accepted but simply yields zero correction rather than
    // being rejected -- the caller has already done the real validation.
    void primeWithPrescannedLufs(double integratedLufs) {
        m_prescannedLufs.store(integratedLufs, std::memory_order_relaxed);
        m_hasPrescan.store(true, std::memory_order_release);
        m_currentGainDb.store(computeTargetGainDb(integratedLufs), std::memory_order_relaxed);
        // Arm the per-sample ramp at the same gain, exactly as reset()
        // does. process() interpolates from m_appliedGainLinear toward
        // this block's gain, so leaving it at unity here would fade the
        // correction in across the first block instead of applying it
        // from the first sample -- which is precisely the startup ramp
        // two-pass mode exists to avoid.
        m_appliedGainLinear = std::pow(10.0f, m_currentGainDb.load(std::memory_order_relaxed) / 20.0f);
    }

    bool hasPrescannedLoudness() const { return m_hasPrescan.load(std::memory_order_acquire); }

    // Discards any prescanned value, reverting to real-time-only
    // measurement. Not called automatically anywhere in this class --
    // callers opening a new file get a fresh instance (or should call this
    // explicitly) rather than carrying a stale prescan across files.
    void clearPrescan() {
        m_hasPrescan.store(false, std::memory_order_relaxed);
        m_prescannedLufs.store(-120.0, std::memory_order_relaxed);
    }

    // For HUD/diagnostics display. Once primed, the whole-file prescanned
    // value is authoritative and available immediately, rather than
    // reporting "measuring..." (the -70 sentinel) until the live meter
    // converges.
    double getMeasuredIntegratedLufs() const {
        return m_hasPrescan.load(std::memory_order_acquire)
                   ? m_prescannedLufs.load(std::memory_order_relaxed)
                   : m_meter.getIntegratedLufs();
    }
    double getMeasuredMomentaryLufs() const { return m_meter.getMomentaryLufs(); }
    float getCurrentGainDb() const { return m_currentGainDb.load(std::memory_order_relaxed); }

    // Real-time safe: the meter swaps to a pre-built spare graph rather
    // than rebuilding one inline (see LoudnessMeter::requestReset()).
    void reset() {
        m_meter.requestReset();
        // Anything still queued describes pre-seek audio, which is no
        // longer temporally continuous with what comes next. Bumping the
        // generation tells serviceMetering() to discard the backlog rather
        // than fold two unrelated parts of the stream into one measurement.
        m_ringFlushGen.fetch_add(1, std::memory_order_release);
        // A prescanned value is still valid after a seek (it describes the
        // whole file, not just what's been streamed so far), so re-arm the
        // envelope at the already-correct gain instead of dropping back to
        // zero and ramping back in.
        const float resetGainDb = m_hasPrescan.load(std::memory_order_acquire)
                                      ? computeTargetGainDb(m_prescannedLufs.load(std::memory_order_relaxed))
                                      : 0.0f;
        m_currentGainDb.store(resetGainDb, std::memory_order_relaxed);
        m_appliedGainLinear = std::pow(10.0f, resetGainDb / 20.0f);
    }

    // True while the meter has a retired filter graph waiting to be
    // rebuilt off the audio thread. See serviceMeterRebuild().
    bool needsMeterRebuild() const { return m_meter.needsRebuild(); }

    // MUST be called from a non-real-time thread (PlayerController's
    // per-frame poll does this). Rebuilds the retired graph so the next
    // seek can swap into it in O(1) again.
    void serviceMeterRebuild() { m_meter.serviceRebuild(); }

    // Drains whatever process() has queued into the EBU R128 graph.
    //
    // MUST be called from a non-real-time thread; this is where the
    // libavfilter push, the K-weighting and ebur128's per-frame metadata
    // allocation actually happen. AudioDecoder::serviceDeferredMaintenance()
    // calls it, which PlayerController already polls once per frame.
    //
    // Returns the number of frames metered, for tests and diagnostics.
    int serviceMetering() {
        if (m_channels <= 0 || m_ring.empty()) return 0;

        // A reset() since the last service means the queued audio is from
        // before a seek: drop it wholesale. This can also discard a little
        // post-reset audio that raced in, which costs at most one service
        // interval of measurement and never produces a wrong reading.
        const uint32_t gen = m_ringFlushGen.load(std::memory_order_acquire);
        if (gen != m_servicedFlushGen) {
            m_servicedFlushGen = gen;
            m_readPos.store(m_writePos.load(std::memory_order_acquire),
                            std::memory_order_release);
            return 0;
        }

        const size_t w = m_writePos.load(std::memory_order_acquire);
        size_t r = m_readPos.load(std::memory_order_relaxed);
        size_t available = w - r;
        if (available == 0) return 0;

        // Whole frames only -- the producer always writes complete blocks,
        // so this is already frame-aligned, but be explicit about it.
        available -= available % static_cast<size_t>(m_channels);
        if (available == 0) return 0;

        // Linearize out of the ring, then hand the meter one contiguous run.
        if (m_meterScratch.size() < available) m_meterScratch.resize(available);
        const size_t first = std::min(available, m_ring.size() - (r & m_ringMask));
        std::memcpy(m_meterScratch.data(), m_ring.data() + (r & m_ringMask),
                    first * sizeof(float));
        if (available > first) {
            std::memcpy(m_meterScratch.data() + first, m_ring.data(),
                        (available - first) * sizeof(float));
        }
        m_readPos.store(r + available, std::memory_order_release);

        const int frames = static_cast<int>(available / static_cast<size_t>(m_channels));
        m_meter.feed(m_meterScratch.data(), frames);
        return frames;
    }

    // Blocks that never reached the meter because the ring was full --
    // i.e. serviceMetering() was starved. Diagnostics only; a nonzero
    // value means the measurement saw slightly less audio than played,
    // not that anything is wrong with the audio itself.
    uint64_t getMeteringDrops() const { return m_meterDrops.load(std::memory_order_relaxed); }

    // In-place processing of an interleaved float buffer. No-op (not even
    // measuring) while disabled, matching the rest of this DSP pipeline's
    // "disabled = truly zero cost" convention.
    void process(float* interleaved, int numFrames) {
        if (!m_enabled || numFrames <= 0 || m_channels <= 0) {
            return;
        }

        // Queue for metering instead of metering here. Kept running even
        // in two-pass mode, purely for momentary-LUFS HUD display -- it no
        // longer drives the applied gain once primed.
        queueForMetering(interleaved, numFrames);

        const bool hasPrescan = m_hasPrescan.load(std::memory_order_acquire);
        double measured = hasPrescan ? m_prescannedLufs.load(std::memory_order_relaxed)
                                     : m_meter.getIntegratedLufs();
        if (!hasPrescan && measured <= -70.0) {
            // EBU R128's absolute silence gate value -- not enough audio
            // has been fed yet for a real real-time reading.
            return;
        }

        const float targetGainDb = computeTargetGainDb(measured);

        double blockSeconds = static_cast<double>(numFrames) / m_sampleRate;
        // Two-pass mode already knows the right gain (see
        // primeWithPrescannedLufs()), so there's nothing to ramp toward --
        // only the real-time path smooths in over kSmoothingSeconds as its
        // measurement keeps shifting.
        float coeff = hasPrescan ? 0.0f : static_cast<float>(std::exp(-blockSeconds / kSmoothingSeconds));
        const float smoothedGainDb =
            coeff * m_currentGainDb.load(std::memory_order_relaxed) + (1.0f - coeff) * targetGainDb;
        m_currentGainDb.store(smoothedGainDb, std::memory_order_relaxed);

        const float targetLinear = std::pow(10.0f, smoothedGainDb / 20.0f);
        const int totalSamples = numFrames * m_channels;

        // Interpolate from the gain the previous block ended on to this
        // block's gain, rather than applying a new constant to every
        // sample. A per-block step is a discontinuity in the waveform at
        // every block boundary -- small here, because the smoothing time
        // constant is seconds, but a discontinuity is a discontinuity and
        // there is no reason to leave one in when the ramp costs one add
        // per sample.
        if (m_appliedGainLinear == targetLinear) {
            if (targetLinear == 1.0f) return; // exactly unity: nothing to do
            for (int i = 0; i < totalSamples; ++i) {
                interleaved[i] *= targetLinear;
            }
            return;
        }

        const float step = (targetLinear - m_appliedGainLinear) / static_cast<float>(numFrames);
        float g = m_appliedGainLinear;
        for (int f = 0; f < numFrames; ++f) {
            float* frame = interleaved + static_cast<size_t>(f) * m_channels;
            for (int ch = 0; ch < m_channels; ++ch) {
                frame[ch] *= g;
            }
            g += step;
        }
        m_appliedGainLinear = targetLinear;
    }

private:
    // Audio-thread side of the metering handoff: a bounded copy into the
    // SPSC ring and one release store. No locks, no allocation, no libav.
    void queueForMetering(const float* interleaved, int numFrames) {
        if (m_ring.empty() || numFrames <= 0) return;
        const size_t n = static_cast<size_t>(numFrames) * static_cast<size_t>(m_channels);

        const size_t w = m_writePos.load(std::memory_order_relaxed);
        const size_t r = m_readPos.load(std::memory_order_acquire);
        if (n > m_ring.size() - (w - r)) {
            // Consumer starved. Drop this block rather than overwrite audio
            // it has not read yet -- a short gap in the measurement is
            // recoverable, a torn read is not.
            m_meterDrops.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const size_t offset = w & m_ringMask;
        const size_t first = std::min(n, m_ring.size() - offset);
        std::memcpy(m_ring.data() + offset, interleaved, first * sizeof(float));
        if (n > first) {
            std::memcpy(m_ring.data(), interleaved + first, (n - first) * sizeof(float));
        }
        m_writePos.store(w + n, std::memory_order_release);
    }

    static constexpr double kSmoothingSeconds = 3.0;

    // Sanity bound, not a substitute for the Limiter: without this, a
    // brief very-quiet passage right after enabling normalization could
    // otherwise compute an extreme correction before the smoothing above
    // has a chance to temper it.
    float computeTargetGainDb(double measuredLufs) const {
        return std::clamp(static_cast<float>(m_targetLufs - measuredLufs), -24.0f, 24.0f);
    }

    LoudnessMeter m_meter;

    // Lock-free SPSC ring carrying audio from the callback thread to
    // serviceMetering(). Monotonic counters masked on use, so "how much is
    // queued" is a plain subtraction that stays correct across wraparound.
    std::vector<float> m_ring;
    size_t m_ringMask = 0;
    std::atomic<size_t> m_writePos{0};
    std::atomic<size_t> m_readPos{0};
    std::atomic<uint32_t> m_ringFlushGen{0};
    std::atomic<uint64_t> m_meterDrops{0};
    uint32_t m_servicedFlushGen = 0;      // consumer-owned
    std::vector<float> m_meterScratch;    // consumer-owned

    int m_channels = 0;
    double m_sampleRate = 48000.0;
    float m_targetLufs = -16.0f;
    bool m_enabled = false;
    // Updated once per block on the audio callback thread and read from
    // the UI thread for HUD display, with no lock on either side -- so it
    // is atomic rather than a plain float. Relaxed ordering: it is an
    // independent scalar that establishes no ordering with anything else,
    // and one block of staleness is invisible against a smoothing time
    // constant measured in seconds.
    std::atomic<float> m_currentGainDb{0.0f};

    // Written on the audio callback thread (primeWithPrescannedLufs, via
    // AudioDecoder::applyPendingDspSettings) and read from the UI thread
    // for HUD display. Atomic for the same reason m_currentGainDb is: a
    // plain read across threads is a race, not merely a stale number.
    std::atomic<bool> m_hasPrescan{false};
    std::atomic<double> m_prescannedLufs{-120.0};
    // Gain actually applied to the last sample of the previous block, so
    // this block can ramp from it instead of stepping.
    float m_appliedGainLinear = 1.0f;
};

} // namespace naikav::dsp
