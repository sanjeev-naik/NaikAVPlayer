#pragma once

#include "LoudnessMeter.hpp"
#include <algorithm>
#include <cmath>

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
// Disabled by default: process() only feeds the meter (keeping it cheap
// but idle-safe) and never touches the signal until enabled.
class LoudnessNormalizer {
public:
    void configure(int channels, double sampleRate) {
        m_meter.configure(channels, static_cast<int>(sampleRate));
        m_channels = channels;
        m_sampleRate = sampleRate;
        m_currentGainDb = 0.0f;
        m_hasPrescan = false;
        m_prescannedLufs = -120.0;
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
        m_hasPrescan = true;
        m_prescannedLufs = integratedLufs;
        m_currentGainDb = computeTargetGainDb(integratedLufs);
    }

    bool hasPrescannedLoudness() const { return m_hasPrescan; }

    // Discards any prescanned value, reverting to real-time-only
    // measurement. Not called automatically anywhere in this class --
    // callers opening a new file get a fresh instance (or should call this
    // explicitly) rather than carrying a stale prescan across files.
    void clearPrescan() {
        m_hasPrescan = false;
        m_prescannedLufs = -120.0;
    }

    // For HUD/diagnostics display. Once primed, the whole-file prescanned
    // value is authoritative and available immediately, rather than
    // reporting "measuring..." (the -70 sentinel) until the live meter
    // converges.
    double getMeasuredIntegratedLufs() const {
        return m_hasPrescan ? m_prescannedLufs : m_meter.getIntegratedLufs();
    }
    double getMeasuredMomentaryLufs() const { return m_meter.getMomentaryLufs(); }
    float getCurrentGainDb() const { return m_currentGainDb; }

    void reset() {
        m_meter.reset();
        // A prescanned value is still valid after a seek (it describes the
        // whole file, not just what's been streamed so far), so re-arm the
        // envelope at the already-correct gain instead of dropping back to
        // zero and ramping back in.
        m_currentGainDb = m_hasPrescan ? computeTargetGainDb(m_prescannedLufs) : 0.0f;
    }

    // In-place processing of an interleaved float buffer. No-op (not even
    // measuring) while disabled, matching the rest of this DSP pipeline's
    // "disabled = truly zero cost" convention.
    void process(float* interleaved, int numFrames) {
        if (!m_enabled || numFrames <= 0 || m_channels <= 0) {
            return;
        }

        // Kept running even in two-pass mode, purely for momentary-LUFS
        // HUD display -- it no longer drives the applied gain once primed.
        m_meter.feed(interleaved, numFrames);

        double measured = m_hasPrescan ? m_prescannedLufs : m_meter.getIntegratedLufs();
        if (!m_hasPrescan && measured <= -70.0) {
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
        float coeff = m_hasPrescan ? 0.0f : static_cast<float>(std::exp(-blockSeconds / kSmoothingSeconds));
        m_currentGainDb = coeff * m_currentGainDb + (1.0f - coeff) * targetGainDb;

        float gainLinear = std::pow(10.0f, m_currentGainDb / 20.0f);
        int totalSamples = numFrames * m_channels;
        for (int i = 0; i < totalSamples; ++i) {
            interleaved[i] *= gainLinear;
        }
    }

private:
    static constexpr double kSmoothingSeconds = 3.0;

    // Sanity bound, not a substitute for the Limiter: without this, a
    // brief very-quiet passage right after enabling normalization could
    // otherwise compute an extreme correction before the smoothing above
    // has a chance to temper it.
    float computeTargetGainDb(double measuredLufs) const {
        return std::clamp(static_cast<float>(m_targetLufs - measuredLufs), -24.0f, 24.0f);
    }

    LoudnessMeter m_meter;
    int m_channels = 0;
    double m_sampleRate = 48000.0;
    float m_targetLufs = -16.0f;
    bool m_enabled = false;
    float m_currentGainDb = 0.0f;

    bool m_hasPrescan = false;
    double m_prescannedLufs = -120.0;
};

} // namespace naikav::dsp
