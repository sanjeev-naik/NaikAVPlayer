#pragma once

#include <cmath>
#include <algorithm>

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
// Attack/release are the opposite way round from Compressor's: opening
// (moving back toward 0 dB reduction, i.e. unmuting) is the *fast* side
// so onsets aren't clipped, and closing (moving toward more reduction) is
// the *slow* side so decay tails aren't chopped off abruptly or the gate
// doesn't chatter on a signal hovering near the threshold.
//
// Default ratio (1:1) is a true no-op regardless of threshold, so
// enabling the DSP chain doesn't change the sound until configured.
class NoiseGate {
public:
    void configure(int channels, double sampleRate) {
        m_channels = channels;
        m_sampleRate = sampleRate;
        updateTimeConstants();
        updateDetectorTimeConstant();
        reset();
    }

    void setThresholdDb(float db) { m_thresholdDb = db; }
    void setRatio(float ratio) { m_ratio = std::max(1.0f, ratio); }
    void setKneeDb(float db) { m_kneeDb = std::max(0.0f, db); }
    void setAttackMs(float ms) {
        m_attackMs = std::max(0.01f, ms);
        updateTimeConstants();
    }
    void setReleaseMs(float ms) {
        m_releaseMs = std::max(0.01f, ms);
        updateTimeConstants();
    }

    void reset() {
        m_envelopeDb = 0.0f; // 0 dB = fully open/no reduction, i.e. the inert/idle state
        m_detectorLevel = 0.0f;
    }

    // In-place processing of an interleaved float buffer.
    void process(float* interleaved, int numFrames) {
        if (m_channels <= 0) return;
        for (int f = 0; f < numFrames; ++f) {
            float* frame = interleaved + static_cast<size_t>(f) * m_channels;

            float peak = 0.0f;
            for (int ch = 0; ch < m_channels; ++ch) {
                peak = std::max(peak, std::fabs(frame[ch]));
            }

            // Smooth the *detected* level itself (distinct from the gain
            // envelope's attack/release below) before comparing it to the
            // threshold. Without this, a sustained tone's instantaneous
            // sample value still passes through exactly zero twice per
            // cycle -- a raw per-sample peak detector would see that as
            // "below threshold" every single cycle and chatter the gate
            // open/closed continuously, even on loud, steady content.
            // kDetectorMs is short enough to still track a genuine
            // quiet/loud transition quickly, long enough to ride through
            // one cycle's zero-crossing dip for any normal program
            // material frequency.
            m_detectorLevel = m_detectorCoeff * m_detectorLevel + (1.0f - m_detectorCoeff) * peak;
            const float levelDb = 20.0f * std::log10(m_detectorLevel + 1e-9f);

            const float gr = staticGainReductionDb(levelDb);

            // Opening (gr > envelope, i.e. less reduction) uses the fast
            // attack coefficient; closing (gr < envelope) uses the slower
            // release -- the inverse of Compressor's coefficient choice.
            const float coeff = (gr > m_envelopeDb) ? m_attackCoeff : m_releaseCoeff;
            m_envelopeDb = coeff * m_envelopeDb + (1.0f - coeff) * gr;

            const float gainLinear = std::pow(10.0f, m_envelopeDb / 20.0f);
            for (int ch = 0; ch < m_channels; ++ch) {
                frame[ch] *= gainLinear;
            }
        }
    }

private:
    float staticGainReductionDb(float levelDb) const {
        const float under = m_thresholdDb - levelDb; // positive when below threshold
        if (2.0f * under < -m_kneeDb) {
            return 0.0f; // comfortably above threshold: fully open
        }
        if (m_kneeDb > 0.0f && 2.0f * std::fabs(under) <= m_kneeDb) {
            const float t = under + m_kneeDb / 2.0f;
            return -(m_ratio - 1.0f) * (t * t) / (2.0f * m_kneeDb);
        }
        return -(m_ratio - 1.0f) * under;
    }

    void updateTimeConstants() {
        if (m_sampleRate <= 0.0) return;
        m_attackCoeff = std::exp(-1.0f / (static_cast<float>(m_sampleRate) * (m_attackMs / 1000.0f)));
        m_releaseCoeff = std::exp(-1.0f / (static_cast<float>(m_sampleRate) * (m_releaseMs / 1000.0f)));
    }

    void updateDetectorTimeConstant() {
        if (m_sampleRate <= 0.0) return;
        m_detectorCoeff = std::exp(-1.0f / (static_cast<float>(m_sampleRate) * (kDetectorMs / 1000.0f)));
    }

    static constexpr float kDetectorMs = 8.0f;

    int m_channels = 0;
    double m_sampleRate = 48000.0;

    float m_thresholdDb = -50.0f;
    float m_ratio = 1.0f; // 1:1 = inert (true no-op) by default
    float m_kneeDb = 6.0f;

    float m_attackMs = 5.0f;
    float m_releaseMs = 150.0f;
    float m_attackCoeff = 0.0f;
    float m_releaseCoeff = 0.0f;

    float m_detectorCoeff = 0.0f;
    float m_detectorLevel = 0.0f;

    float m_envelopeDb = 0.0f; // 0 dB = fully open, i.e. the inert/idle state
};

} // namespace naikav::dsp
