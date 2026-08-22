#pragma once

#include <cmath>
#include <algorithm>

#include "audio/dsp/DspMath.hpp"

namespace naikav::dsp {

// Feedforward soft-knee compressor. Gain reduction is computed once per
// frame from the loudest channel in that frame ("linked" multichannel
// detection) and applied equally to every channel, so multichannel/stereo
// content doesn't shift its image as the gain moves. Uses the standard
// soft-knee gain computer from Giannoulis, Massberg & Reiss, "Digital
// Dynamic Range Compressor Design -- A Tutorial and Analysis" (2012),
// which is smooth (C1-continuous) across the knee rather than kinking at
// threshold like a hard-knee compressor.
//
// The detected level is smoothed before it reaches the gain computer (see
// LevelDetector in DspMath.hpp). A raw per-frame peak sees a sustained
// tone cross zero twice per cycle, so the gain envelope ripples at twice
// the signal frequency -- amplitude modulation, i.e. added harmonic and
// intermodulation distortion, worst on low-frequency content where the
// period is long relative to the attack time. NoiseGate already carried a
// detector for exactly this reason; this one did not, and inherited the
// problem into all three of MultibandCompressor's bands too. The shared
// detector is asymmetric (instant attack, smoothed release) so it does
// this without delaying genuine transients.
//
// Defaults (ratio 1:1) are a true no-op regardless of threshold, and
// process() returns immediately in that state rather than computing a
// gain it already knows is unity -- these stages run on the audio callback
// thread, where "transparent" and "free" are not the same thing.
class Compressor {
public:
    void configure(int channels, double sampleRate) {
        m_channels = channels;
        m_sampleRate = sampleRate;
        m_detector.configure(sampleRate);
        m_makeupGlide.configure(sampleRate, m_makeupGainDb);
        m_bypassGlide.configure(sampleRate);
        updateTimeConstants();
        reset();
    }

    // Peak-referenced, not RMS -- see LevelDetector in DspMath.hpp. For
    // continuous material this lands within a couple of dB of programme
    // level; for transient-heavy material the stage engages at a lower
    // average level than the number suggests.
    void setThresholdDb(float db) { m_thresholdDb = db; }
    void setRatio(float ratio) { m_ratio = std::max(1.0f, ratio); }
    void setKneeDb(float db) { m_kneeDb = std::max(0.0f, db); }
    void setMakeupGainDb(float db) {
        if (!std::isfinite(db)) return;
        m_makeupGainDb = db;
        // Glided in dB rather than assigned: makeup is a dragged slider
        // applied straight to every sample, so a bare assignment stepped
        // the output once per UI frame of the drag -- measured at 7x the
        // waveform's own per-sample slope for a single 6 dB change.
        m_makeupGlide.setTarget(db);
    }
    void setAttackMs(float ms) {
        m_attackMs = std::max(0.01f, ms);
        updateTimeConstants();
    }
    void setReleaseMs(float ms) {
        m_releaseMs = std::max(0.01f, ms);
        updateTimeConstants();
    }

    // True when this stage provably cannot change the signal: at 1:1 the
    // gain computer returns 0 dB for every input level, so the only
    // remaining term is the makeup gain.
    bool isInert() const {
        return m_ratio <= 1.0f && m_makeupGainDb == 0.0f;
    }

    void reset() {
        m_envelopeDb = 0.0f; // 0 dB = no gain reduction, i.e. the inert/idle state
        m_detector.reset();
        m_makeupGlide.reset();
        m_bypassGlide.reset();
    }

    // In-place processing of an interleaved float buffer.
    void process(float* interleaved, int numFrames) {
        if (m_channels <= 0 || numFrames <= 0) return;
        m_makeupGlide.markPrimed();
        if (isInert()) {
            // Keep the envelope and detector at rest so re-enabling the
            // stage starts from a known state rather than a stale one.
            m_envelopeDb = 0.0f;
            m_detector.reset();
            // ...but glide whatever reduction was in force back to unity
            // first. Returning outright dropped it in a single sample --
            // 43x the waveform's own slope on a ratio change from 8:1 to
            // 1:1. See BypassGainGlide.
            m_bypassGlide.applyGlide(interleaved, numFrames, m_channels);
            return;
        }

        for (int f = 0; f < numFrames; ++f) {
            float* frame = interleaved + static_cast<size_t>(f) * m_channels;

            const float level = m_detector.process(framePeak(frame, m_channels));
            const float levelDb = fastLinearToDb(level + 1e-9f);

            const float gr = staticGainReductionDb(levelDb);

            // One-pole attack/release smoothing of the gain-reduction
            // envelope: moving toward a larger reduction is "attack",
            // moving back toward 0 is "release".
            const float coeff = (gr < m_envelopeDb) ? m_attackCoeff : m_releaseCoeff;
            m_envelopeDb = coeff * m_envelopeDb + (1.0f - coeff) * gr;

            const float totalDb = m_envelopeDb + m_makeupGlide.next();
            const float gainLinear = (totalDb == 0.0f) ? 1.0f : fastDbToLinear(totalDb);
            for (int ch = 0; ch < m_channels; ++ch) {
                frame[ch] *= gainLinear;
            }
            // Remembered so that if the next block finds this stage inert,
            // it has somewhere to glide down from rather than snapping.
            m_bypassGlide.track(gainLinear);
        }
    }

private:
    float staticGainReductionDb(float levelDb) const {
        const float over = levelDb - m_thresholdDb;
        if (2.0f * over < -m_kneeDb) {
            return 0.0f;
        }
        if (m_kneeDb > 0.0f && 2.0f * std::fabs(over) <= m_kneeDb) {
            const float t = over + m_kneeDb / 2.0f;
            return (1.0f / m_ratio - 1.0f) * (t * t) / (2.0f * m_kneeDb);
        }
        return (1.0f / m_ratio - 1.0f) * over;
    }

    void updateTimeConstants() {
        if (m_sampleRate <= 0.0) return;
        m_attackCoeff = std::exp(-1.0f / (static_cast<float>(m_sampleRate) * (m_attackMs / 1000.0f)));
        m_releaseCoeff = std::exp(-1.0f / (static_cast<float>(m_sampleRate) * (m_releaseMs / 1000.0f)));
    }

    int m_channels = 0;
    double m_sampleRate = 48000.0;

    float m_thresholdDb = 0.0f; // 0 dBFS: with default ratio 1:1, threshold is irrelevant anyway
    float m_ratio = 1.0f;       // 1:1 = inert (true no-op) by default
    float m_kneeDb = 6.0f;
    float m_makeupGainDb = 0.0f;

    float m_attackMs = 10.0f;
    float m_releaseMs = 100.0f;
    float m_attackCoeff = 0.0f;
    float m_releaseCoeff = 0.0f;

    LevelDetector m_detector;
    SmoothedParam m_makeupGlide;
    BypassGainGlide m_bypassGlide;
    float m_envelopeDb = 0.0f; // 0 dB = no gain reduction, i.e. the inert/idle state
};

} // namespace naikav::dsp
