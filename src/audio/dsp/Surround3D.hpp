#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include "audio/dsp/Biquad.hpp"

namespace naikav::dsp {

// Synthesizes a wider, more enveloping "3D surround" ambience from a
// 2-channel signal, for headphones/stereo speakers that have no real
// rear channels to draw on. This is what makes the overwhelming majority
// of content -- plain stereo files, or anything already downmixed to
// stereo -- feel spatial: SpatialDownmixer only has positional
// information to synthesize from when the *source* is discrete
// multichannel; this stage works on any 2-channel signal, including one
// SpatialDownmixer already folded down (the two stack deliberately: this
// adds extra depth/ambience on top of an already-positioned downmix,
// which is a different aspect of the signal, not double-counting).
//
// Technique (deliberately simple/hand-rolled, matching the rest of this
// DSP chain -- NOT a licensed/measured technology like DTS:X, Dolby
// Atmos, or a real HRTF convolution engine, none of which this project
// has the rights or data to implement): split into mid ((L+R)/2, the
// correlated "center" content -- dialogue, lead vocals, anything panned
// center passes through completely dry) and side ((L-R)/2, the
// decorrelated/ambient content), run the side signal through two
// delay+lowpass taps (~15ms/~35ms, like a minimal two-tap early-
// reflection simulator -- longer and duller for the second tap, echoing
// how a real room's second reflection arrives later and more absorbed),
// and re-inject the result into L/R with opposite polarity. Because the
// injected ambience is added to one ear and subtracted from the other,
// summing L+R after processing exactly cancels it back out -- the mono
// downmix of this effect's output is bit-identical to the mono downmix
// of its input, so it can never make content less mono-compatible than
// it already was.
class Surround3D {
public:
    void configure(int channels, double sampleRate) {
        m_channels = channels;
        m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;

        int shortSamples = std::max(1, static_cast<int>(std::lround(kShortDelayMs / 1000.0 * m_sampleRate)));
        int longSamples = std::max(1, static_cast<int>(std::lround(kLongDelayMs / 1000.0 * m_sampleRate)));
        m_shortRing.assign(static_cast<size_t>(shortSamples), 0.0f);
        m_longRing.assign(static_cast<size_t>(longSamples), 0.0f);
        m_shortLowpass.setLowpass(kShortLowpassHz, 0.70710678118, m_sampleRate);
        m_longLowpass.setLowpass(kLongLowpassHz, 0.70710678118, m_sampleRate);
        reset();
    }

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    // 1.0 = the designed nominal strength, 0.0 = off (even if enabled),
    // >1.0 exaggerates the effect.
    void setIntensity(float intensity) { m_intensity = std::max(0.0f, intensity); }
    float getIntensity() const { return m_intensity; }

    void reset() {
        std::fill(m_shortRing.begin(), m_shortRing.end(), 0.0f);
        std::fill(m_longRing.begin(), m_longRing.end(), 0.0f);
        m_shortPos = 0;
        m_longPos = 0;
        m_shortLowpass.reset();
        m_longLowpass.reset();
    }

    // In-place processing of an interleaved float buffer. No-op unless
    // enabled, configured for exactly 2 channels, and a nonzero intensity.
    void process(float* interleaved, int numFrames) {
        if (!m_enabled || m_channels != 2 || m_intensity <= 0.0f) {
            return;
        }
        for (int f = 0; f < numFrames; ++f) {
            float* frame = interleaved + static_cast<size_t>(f) * 2;
            const float side = 0.5f * (frame[0] - frame[1]);

            float shortTap = m_shortRing[m_shortPos];
            m_shortRing[m_shortPos] = side;
            m_shortPos = (m_shortPos + 1) % m_shortRing.size();
            shortTap = m_shortLowpass.process(shortTap);

            float longTap = m_longRing[m_longPos];
            m_longRing[m_longPos] = side;
            m_longPos = (m_longPos + 1) % m_longRing.size();
            longTap = m_longLowpass.process(longTap);

            const float ambience = (shortTap * kShortGain + longTap * kLongGain) * m_intensity;
            frame[0] += ambience;
            frame[1] -= ambience;
        }
    }

private:
    static constexpr double kShortDelayMs = 15.0, kLongDelayMs = 35.0;
    static constexpr double kShortLowpassHz = 7000.0, kLongLowpassHz = 4000.0;
    static constexpr float kShortGain = 0.55f, kLongGain = 0.35f;

    std::vector<float> m_shortRing;
    std::vector<float> m_longRing;
    size_t m_shortPos = 0;
    size_t m_longPos = 0;
    Biquad m_shortLowpass;
    Biquad m_longLowpass;

    int m_channels = 0;
    double m_sampleRate = 48000.0;
    bool m_enabled = false;
    float m_intensity = 1.0f;
};

} // namespace naikav::dsp
