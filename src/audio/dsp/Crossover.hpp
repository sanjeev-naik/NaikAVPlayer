#pragma once

#include <vector>
#include "audio/dsp/Biquad.hpp"

namespace naikav::dsp {

// Bass management for a discrete LFE/subwoofer channel of a 5.1/7.1/2.1
// layout, built from 4th-order Linkwitz-Riley filters -- two cascaded
// 2nd-order Butterworth (q = 1/sqrt2) stages per filter, giving a 24
// dB/octave slope. LR crossovers are the standard choice for bass
// management/speaker crossovers because two matched LR halves (lowpass and
// highpass at the same cutoff) sum back to a flat magnitude response,
// unlike a plain Butterworth split.
//
// Two independent behaviors, both gated by setEnabled():
//   - LFE tone control: always applied to the target (LFE) channel when
//     enabled -- a lowpass that tames a source with an unusually hot/wide
//     LFE track. Source LFE content is normally already band-limited by
//     the encoder, so this only matters for unusual sources.
//   - Bass redirect (setBassRedirectEnabled()): true bass management --
//     every *other* channel gets a matched highpass (removing content
//     below the crossover it likely can't reproduce well), and exactly the
//     content removed from each of those channels is summed into the LFE
//     channel instead of being discarded. This is what lets small
//     satellite/main speakers hand their bass off to a real subwoofer
//     rather than either losing it or trying to reproduce it themselves.
//
// Disabled by default: only meaningful when a discrete LFE channel exists.
class Crossover {
public:
    void configure(int channels, double sampleRate, int targetChannelIndex) {
        m_channels = channels;
        m_sampleRate = sampleRate;
        m_targetChannel = targetChannelIndex;
        m_mainHighpassStage1.assign(static_cast<size_t>(channels), Biquad{});
        m_mainHighpassStage2.assign(static_cast<size_t>(channels), Biquad{});
        m_mainLowpassStage1.assign(static_cast<size_t>(channels), Biquad{});
        m_mainLowpassStage2.assign(static_cast<size_t>(channels), Biquad{});
        rebuildCoefficients();
        reset();
    }

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    // See the class comment above. No effect unless setEnabled(true) too.
    void setBassRedirectEnabled(bool enabled) { m_bassRedirectEnabled = enabled; }
    bool isBassRedirectEnabled() const { return m_bassRedirectEnabled; }

    void setCutoffHz(double hz) {
        m_cutoffHz = hz;
        rebuildCoefficients();
    }

    void reset() {
        m_lfeLowpassStage1.reset();
        m_lfeLowpassStage2.reset();
        for (auto& f : m_mainHighpassStage1) f.reset();
        for (auto& f : m_mainHighpassStage2) f.reset();
        for (auto& f : m_mainLowpassStage1) f.reset();
        for (auto& f : m_mainLowpassStage2) f.reset();
    }

    // In-place processing of an interleaved float buffer. No-op unless
    // enabled and configured with a valid target (LFE) channel.
    void process(float* interleaved, int numFrames) {
        if (!m_enabled || m_targetChannel < 0 || m_targetChannel >= m_channels) {
            return;
        }
        for (int f = 0; f < numFrames; ++f) {
            float* frame = interleaved + static_cast<size_t>(f) * m_channels;

            float lfeSample = frame[m_targetChannel];
            lfeSample = m_lfeLowpassStage1.process(lfeSample);
            lfeSample = m_lfeLowpassStage2.process(lfeSample);

            float redirectedBass = 0.0f;
            if (m_bassRedirectEnabled) {
                for (int ch = 0; ch < m_channels; ++ch) {
                    if (ch == m_targetChannel) continue;
                    const float original = frame[ch];

                    float low = m_mainLowpassStage1[ch].process(original);
                    low = m_mainLowpassStage2[ch].process(low);
                    redirectedBass += low;

                    float high = m_mainHighpassStage1[ch].process(original);
                    high = m_mainHighpassStage2[ch].process(high);
                    frame[ch] = high;
                }
            }

            frame[m_targetChannel] = lfeSample + redirectedBass;
        }
    }

private:
    void rebuildCoefficients() {
        if (m_sampleRate <= 0.0) return;
        constexpr double kButterworthQ = 0.70710678118; // 1/sqrt(2)

        m_lfeLowpassStage1.setLowpass(m_cutoffHz, kButterworthQ, m_sampleRate);
        m_lfeLowpassStage2.copyCoefficientsFrom(m_lfeLowpassStage1);

        Biquad lowRef;
        lowRef.setLowpass(m_cutoffHz, kButterworthQ, m_sampleRate);
        Biquad highRef;
        highRef.setHighpass(m_cutoffHz, kButterworthQ, m_sampleRate);
        for (auto& f : m_mainLowpassStage1) f.copyCoefficientsFrom(lowRef);
        for (auto& f : m_mainLowpassStage2) f.copyCoefficientsFrom(lowRef);
        for (auto& f : m_mainHighpassStage1) f.copyCoefficientsFrom(highRef);
        for (auto& f : m_mainHighpassStage2) f.copyCoefficientsFrom(highRef);
    }

    Biquad m_lfeLowpassStage1, m_lfeLowpassStage2;
    std::vector<Biquad> m_mainHighpassStage1, m_mainHighpassStage2;
    std::vector<Biquad> m_mainLowpassStage1, m_mainLowpassStage2;

    int m_channels = 0;
    int m_targetChannel = -1;
    bool m_enabled = false;
    bool m_bassRedirectEnabled = false;
    double m_cutoffHz = 120.0;
    double m_sampleRate = 48000.0;
};

} // namespace naikav::dsp
