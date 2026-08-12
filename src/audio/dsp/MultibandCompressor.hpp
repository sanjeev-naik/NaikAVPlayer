#pragma once

#include <vector>
#include <algorithm>
#include "Biquad.hpp"
#include "Compressor.hpp"

namespace naikav::dsp {

// Splits the signal into three bands via two matched Linkwitz-Riley
// (4th-order, 24dB/octave) crossovers -- low/mid and mid/high -- and
// compresses each band independently with its own Compressor, summing
// the three bands back together afterward. LR crossovers sum back to a
// flat *magnitude/energy* response when every band's compressor sits at
// its inert 1:1 ratio -- an allpass identity, not sample-for-sample
// time-domain identity: the recombined signal's magnitude spectrum
// (and therefore its RMS energy) matches the original exactly, but there
// is a frequency-dependent phase rotation through each crossover point
// (a well-known LR-crossover property, not a bug -- this is also why
// real 2-way loudspeaker crossovers often wire the tweeter in reversed
// polarity relative to the woofer). Audibly and RMS-wise, enabling
// multiband compression with all three bands still at their default
// ratio doesn't change the signal.
//
// Useful for taming a specific frequency range (e.g. boomy bass, harsh
// highs) without a single full-band Compressor's "whichever frequency is
// loudest drags every frequency down with it" behavior.
//
// Disabled by default: process() is then a true no-op.
class MultibandCompressor {
public:
    void configure(int channels, double sampleRate) {
        m_channels = channels;
        m_sampleRate = sampleRate;

        m_lowLp1.assign(static_cast<size_t>(channels), Biquad{});
        m_lowLp2.assign(static_cast<size_t>(channels), Biquad{});
        m_restHp1.assign(static_cast<size_t>(channels), Biquad{});
        m_restHp2.assign(static_cast<size_t>(channels), Biquad{});
        m_midLp1.assign(static_cast<size_t>(channels), Biquad{});
        m_midLp2.assign(static_cast<size_t>(channels), Biquad{});
        m_highHp1.assign(static_cast<size_t>(channels), Biquad{});
        m_highHp2.assign(static_cast<size_t>(channels), Biquad{});

        low.configure(channels, sampleRate);
        mid.configure(channels, sampleRate);
        high.configure(channels, sampleRate);

        rebuildCrossovers();
        reset();
    }

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    void setCrossoverFrequencies(double lowMidHz, double midHighHz) {
        m_lowMidHz = std::min(lowMidHz, midHighHz - 1.0);
        m_midHighHz = std::max(midHighHz, m_lowMidHz + 1.0);
        rebuildCrossovers();
    }
    double getLowMidHz() const { return m_lowMidHz; }
    double getMidHighHz() const { return m_midHighHz; }

    void reset() {
        for (auto& f : m_lowLp1) f.reset();
        for (auto& f : m_lowLp2) f.reset();
        for (auto& f : m_restHp1) f.reset();
        for (auto& f : m_restHp2) f.reset();
        for (auto& f : m_midLp1) f.reset();
        for (auto& f : m_midLp2) f.reset();
        for (auto& f : m_highHp1) f.reset();
        for (auto& f : m_highHp2) f.reset();
        low.reset();
        mid.reset();
        high.reset();
    }

    // In-place processing of an interleaved float buffer.
    void process(float* interleaved, int numFrames) {
        if (!m_enabled || m_channels <= 0 || numFrames <= 0) return;

        const size_t total = static_cast<size_t>(numFrames) * static_cast<size_t>(m_channels);
        if (m_lowBuf.size() < total) {
            m_lowBuf.resize(total);
            m_midBuf.resize(total);
            m_highBuf.resize(total);
        }

        // First split: low band vs. everything above m_lowMidHz ("rest").
        // Second split: "rest" into a mid band and a high band. This
        // two-stage cascade is the standard way to build an N-way LR
        // crossover from 2-way splits.
        for (int f = 0; f < numFrames; ++f) {
            const float* inFrame = interleaved + static_cast<size_t>(f) * m_channels;
            for (int ch = 0; ch < m_channels; ++ch) {
                const float x = inFrame[ch];
                const float lowSample = m_lowLp2[ch].process(m_lowLp1[ch].process(x));
                const float restSample = m_restHp2[ch].process(m_restHp1[ch].process(x));
                const float midSample = m_midLp2[ch].process(m_midLp1[ch].process(restSample));
                const float highSample = m_highHp2[ch].process(m_highHp1[ch].process(restSample));

                const size_t idx = static_cast<size_t>(f) * m_channels + ch;
                m_lowBuf[idx] = lowSample;
                m_midBuf[idx] = midSample;
                m_highBuf[idx] = highSample;
            }
        }

        low.process(m_lowBuf.data(), numFrames);
        mid.process(m_midBuf.data(), numFrames);
        high.process(m_highBuf.data(), numFrames);

        for (size_t i = 0; i < total; ++i) {
            interleaved[i] = m_lowBuf[i] + m_midBuf[i] + m_highBuf[i];
        }
    }

    // Direct access to each band's compressor for configuration.
    Compressor low, mid, high;

private:
    void rebuildCrossovers() {
        if (m_sampleRate <= 0.0) return;
        constexpr double kButterworthQ = 0.70710678118; // 1/sqrt(2)

        Biquad lowLpRef;
        lowLpRef.setLowpass(m_lowMidHz, kButterworthQ, m_sampleRate);
        Biquad restHpRef;
        restHpRef.setHighpass(m_lowMidHz, kButterworthQ, m_sampleRate);
        Biquad midLpRef;
        midLpRef.setLowpass(m_midHighHz, kButterworthQ, m_sampleRate);
        Biquad highHpRef;
        highHpRef.setHighpass(m_midHighHz, kButterworthQ, m_sampleRate);

        for (auto& f : m_lowLp1) f.copyCoefficientsFrom(lowLpRef);
        for (auto& f : m_lowLp2) f.copyCoefficientsFrom(lowLpRef);
        for (auto& f : m_restHp1) f.copyCoefficientsFrom(restHpRef);
        for (auto& f : m_restHp2) f.copyCoefficientsFrom(restHpRef);
        for (auto& f : m_midLp1) f.copyCoefficientsFrom(midLpRef);
        for (auto& f : m_midLp2) f.copyCoefficientsFrom(midLpRef);
        for (auto& f : m_highHp1) f.copyCoefficientsFrom(highHpRef);
        for (auto& f : m_highHp2) f.copyCoefficientsFrom(highHpRef);
    }

    int m_channels = 0;
    double m_sampleRate = 48000.0;
    bool m_enabled = false;
    double m_lowMidHz = 250.0;
    double m_midHighHz = 4000.0;

    std::vector<Biquad> m_lowLp1, m_lowLp2;
    std::vector<Biquad> m_restHp1, m_restHp2;
    std::vector<Biquad> m_midLp1, m_midLp2;
    std::vector<Biquad> m_highHp1, m_highHp2;

    std::vector<float> m_lowBuf, m_midBuf, m_highBuf;
};

} // namespace naikav::dsp
