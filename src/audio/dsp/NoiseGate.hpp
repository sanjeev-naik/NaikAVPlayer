#pragma once

#include <cmath>
#include <algorithm>

#include "audio/dsp/DspMath.hpp"

namespace naikav::dsp {

// Feedforward downward noise gate/expander -- the mirror image of
// Compressor: instead of reducing gain *above* a threshold, this reduces
// gain *below* one, attenuating room noise/hiss/bleed during quiet
// passages while leaving everything above the threshold untouched. Uses
// the same soft-knee gain-computer shape as Compressor (smooth, not a
// hard on/off switch), just applied on the other side of the threshold.
//
// Gain reduction is computed once per frame from the loudest channel in
// that frame ("linked" multichannel detection), same as Compressor, so
// multichannel/stereo content doesn't shift its image as the gate
// opens/closes.
//
// The detected level is smoothed before it reaches the gain computer.
// Without that, a sustained tone's instantaneous sample value passes
// through exactly zero twice per cycle, and a raw per-sample peak detector
// would read that as "below threshold" every single cycle and chatter the
// gate open/closed continuously, even on loud, steady content.
//
// The shared LevelDetector (see DspMath.hpp) is asymmetric: instantaneous
// attack, smoothed release. The symmetric version this class used to carry
// rode through zero-crossings correctly but also delayed the detector's
// *rise* by the full 8 ms, which blunted exactly the onsets the gate's
// deliberately-fast attack was chosen to protect. Instant attack keeps the
// anti-chatter behavior (which only needs the release side) without that
// cost, and Compressor now shares the same detector.
//
// Attack/release are the opposite way round from Compressor's: opening
// (moving back toward 0 dB reduction, i.e. unmuting) is the *fast* side
// so onsets aren't clipped, and closing (moving toward more reduction) is
// the *slow* side so decay tails aren't chopped off abruptly or the gate
// doesn't chatter on a signal hovering near the threshold.
//
// Attenuation when fully closed is bounded by a "range" control, the
// standard gate parameter, and that bound is what makes the attack time
// mean anything. The gain computer is unbounded by nature: at ratio 20:1
// with the detector reading digital silence (-180 dB), the raw target is
// about -2470 dB. The envelope smoother is a one-pole *in the dB domain*,
// so it has to traverse that whole range before the gate is audibly open
// again -- measured at 20 ms still fully muted and ~40 ms to reach -1 dB,
// against a configured 5 ms attack. Every word or note attack after a
// pause was swallowed, which is precisely what a fast attack is chosen to
// prevent. Clamping the computer to -kDefaultRangeDb bounds the traversal:
// the same one-pole now reaches -8 dB in 10 ms and -1 dB in 20 ms, while
// 60 dB of attenuation is still far below audibility for the noise the
// gate exists to remove.
//
// Default ratio (1:1) is a true no-op regardless of threshold, and
// process() returns immediately in that state rather than paying for a
// gain it already knows is unity.
class NoiseGate {
public:
    // Deep enough that gated noise is inaudible, shallow enough that the
    // envelope reopens within the attack time it was given.
    static constexpr float kDefaultRangeDb = 60.0f;

    void configure(int channels, double sampleRate) {
        m_channels = channels;
        m_sampleRate = sampleRate;
        m_detector.configure(sampleRate);
        m_bypassGlide.configure(sampleRate);
        updateTimeConstants();
        reset();
    }

    // Peak-referenced, not RMS -- see LevelDetector in DspMath.hpp.
    void setThresholdDb(float db) { m_thresholdDb = db; }
    void setRatio(float ratio) { m_ratio = std::max(1.0f, ratio); }
    void setKneeDb(float db) { m_kneeDb = std::max(0.0f, db); }

    // Maximum attenuation applied while the gate is closed ("range" on a
    // hardware gate). Bounds how far the dB-domain envelope has to travel
    // to reopen, so it directly governs how much of an onset survives --
    // see the class comment. Lower = gentler gating that reopens sooner.
    void setRangeDb(float db) {
        if (!std::isfinite(db)) return;
        m_rangeDb = std::clamp(db, 6.0f, 96.0f);
    }
    float getRangeDb() const { return m_rangeDb; }
    void setAttackMs(float ms) {
        m_attackMs = std::max(0.01f, ms);
        updateTimeConstants();
    }
    void setReleaseMs(float ms) {
        m_releaseMs = std::max(0.01f, ms);
        updateTimeConstants();
    }

    // At 1:1 the gain computer returns 0 dB for every input level, so the
    // stage cannot change the signal regardless of threshold.
    bool isInert() const { return m_ratio <= 1.0f; }

    void reset() {
        m_envelopeDb = 0.0f; // 0 dB = fully open/no reduction, i.e. the inert/idle state
        m_detector.reset();
        m_bypassGlide.reset();
    }

    // In-place processing of an interleaved float buffer.
    void process(float* interleaved, int numFrames) {
        if (m_channels <= 0 || numFrames <= 0) return;
        if (isInert()) {
            m_envelopeDb = 0.0f;
            m_detector.reset();
            // Glide whatever attenuation the gate was holding back to
            // unity rather than releasing it in one sample -- measured at
            // 36x the waveform's own slope on a ratio change from 8:1 to
            // 1:1. Same reasoning as Compressor; see BypassGainGlide.
            m_bypassGlide.applyGlide(interleaved, numFrames, m_channels);
            return;
        }

        for (int f = 0; f < numFrames; ++f) {
            float* frame = interleaved + static_cast<size_t>(f) * m_channels;

            const float level = m_detector.process(framePeak(frame, m_channels));
            const float levelDb = fastLinearToDb(level + 1e-9f);

            const float gr = staticGainReductionDb(levelDb);

            // Opening (gr > envelope, i.e. less reduction) uses the fast
            // attack coefficient; closing (gr < envelope) uses the slower
            // release -- the inverse of Compressor's coefficient choice.
            const float coeff = (gr > m_envelopeDb) ? m_attackCoeff : m_releaseCoeff;
            m_envelopeDb = coeff * m_envelopeDb + (1.0f - coeff) * gr;

            const float gainLinear = (m_envelopeDb == 0.0f) ? 1.0f : fastDbToLinear(m_envelopeDb);
            for (int ch = 0; ch < m_channels; ++ch) {
                frame[ch] *= gainLinear;
            }
            // Remembered so a later inert block has somewhere to glide
            // down from rather than snapping to unity.
            m_bypassGlide.track(gainLinear);
        }
    }

private:
    float staticGainReductionDb(float levelDb) const {
        const float under = m_thresholdDb - levelDb; // positive when below threshold
        if (2.0f * under < -m_kneeDb) {
            return 0.0f; // comfortably above threshold: fully open
        }
        float gr;
        if (m_kneeDb > 0.0f && 2.0f * std::fabs(under) <= m_kneeDb) {
            const float t = under + m_kneeDb / 2.0f;
            gr = -(m_ratio - 1.0f) * (t * t) / (2.0f * m_kneeDb);
        } else {
            gr = -(m_ratio - 1.0f) * under;
        }
        // Range floor. Without it the target runs to hundreds of negative
        // dB on silence, and the dB-domain envelope below spends tens of
        // milliseconds climbing back -- see the class comment.
        return std::max(gr, -m_rangeDb);
    }

    void updateTimeConstants() {
        if (m_sampleRate <= 0.0) return;
        m_attackCoeff = std::exp(-1.0f / (static_cast<float>(m_sampleRate) * (m_attackMs / 1000.0f)));
        m_releaseCoeff = std::exp(-1.0f / (static_cast<float>(m_sampleRate) * (m_releaseMs / 1000.0f)));
    }

    int m_channels = 0;
    double m_sampleRate = 48000.0;

    float m_thresholdDb = -50.0f;
    float m_ratio = 1.0f; // 1:1 = inert (true no-op) by default
    float m_kneeDb = 6.0f;
    float m_rangeDb = kDefaultRangeDb;

    float m_attackMs = 5.0f;
    float m_releaseMs = 150.0f;
    float m_attackCoeff = 0.0f;
    float m_releaseCoeff = 0.0f;

    LevelDetector m_detector;
    BypassGainGlide m_bypassGlide;
    float m_envelopeDb = 0.0f; // 0 dB = fully open, i.e. the inert/idle state
};

} // namespace naikav::dsp
