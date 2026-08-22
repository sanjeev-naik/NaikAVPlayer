#pragma once

#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "audio/dsp/Biquad.hpp"
#include "audio/dsp/Compressor.hpp"
#include "audio/dsp/DspMath.hpp"

namespace naikav::dsp {

// Splits the signal into three bands via two matched Linkwitz-Riley
// (4th-order, 24dB/octave) crossovers -- low/mid and mid/high -- and
// compresses each band independently with its own Compressor, summing
// the three bands back together afterward.
//
// The three bands sum back to a flat magnitude response when every band's
// compressor sits at its inert 1:1 ratio. Getting that right in a
// *cascaded* 3-way split takes one extra step that is easy to miss: the
// low band is produced by the first crossover and then bypasses the
// second, while the mid and high bands pass through both. A matched LR4
// pair sums to a 2nd-order allpass rather than to unity, so the mid+high
// sum carries a phase rotation the low band does not, and summing them
// directly produces a magnitude error around the lower crossover.
//
// Measured on the previous version, with all three ratios at 1:1: flat to
// 0.03 dB at the shipped 250/4000 Hz defaults (where the two crossovers
// are four octaves apart and the error is far below the low band's own
// rolloff), but -6.4 dB at 800/1200 Hz and a -54 dB notch at 1 kHz for
// (999, 1000) -- which the UI's own sliders could reach, since their
// ranges overlap at 1000 Hz.
//
// Both halves of that are fixed here: the low band now runs through a
// compensating LR4 allpass at the upper crossover so all three bands
// share the same phase, and setCrossoverFrequencies() enforces at least
// an octave of separation, below which 24 dB/octave slopes cannot
// meaningfully separate the bands anyway.
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
        m_lowAllpass.assign(static_cast<size_t>(channels), Biquad{});

        m_fade.configure(sampleRate);
        low.configure(channels, sampleRate);
        mid.configure(channels, sampleRate);
        high.configure(channels, sampleRate);

        // Re-clamp against the (possibly new) Nyquist limit.
        applyCrossoverFrequencies(m_lowMidHz, m_midHighHz);
        reset();
    }

    // Crossfades in/out rather than switching on the next sample; see
    // BypassCrossfade. Matters more here than most: the three band
    // splitters are IIR, so a hard re-enable resumes from stale state.
    void setEnabled(bool enabled) { m_fade.setEnabled(enabled); }
    bool isEnabled() const { return m_fade.isEnabled(); }

    void setCrossoverFrequencies(double lowMidHz, double midHighHz) {
        applyCrossoverFrequencies(lowMidHz, midHighHz);
    }
    double getLowMidHz() const { return m_lowMidHz; }
    double getMidHighHz() const { return m_midHighHz; }

    void reset() {
        m_fade.reset();
        for (auto& f : m_lowLp1) f.reset();
        for (auto& f : m_lowLp2) f.reset();
        for (auto& f : m_restHp1) f.reset();
        for (auto& f : m_restHp2) f.reset();
        for (auto& f : m_midLp1) f.reset();
        for (auto& f : m_midLp2) f.reset();
        for (auto& f : m_highHp1) f.reset();
        for (auto& f : m_highHp2) f.reset();
        for (auto& f : m_lowAllpass) f.reset();
        low.reset();
        mid.reset();
        high.reset();
        rebuildCrossovers();
    }

    // In-place processing of an interleaved float buffer.
    void process(float* interleaved, int numFrames) {
        if (m_channels <= 0 || numFrames <= 0) return;
        m_fade.markPrimed(); // live even while bypassed -- see DspChain
        if (m_fade.isInactive()) return;

        const size_t total = static_cast<size_t>(numFrames) * static_cast<size_t>(m_channels);
        const bool fading = m_fade.isFading() && m_dry.fits(total);
        if (fading) {
            std::memcpy(m_dry.data(), interleaved, total * sizeof(float));
        } else if (m_fade.isFading()) {
            m_fade.snap();
        }
        if (m_lowBuf.size() < total) {
            // Only ever grows, and AudioDecoder reserves the worst-case
            // block up front, so this does not allocate in steady state.
            m_lowBuf.resize(total);
            m_midBuf.resize(total);
            m_highBuf.resize(total);
        }

        // First split: low band vs. everything above m_lowMidHz ("rest").
        // Second split: "rest" into a mid band and a high band. This
        // two-stage cascade is the standard way to build an N-way LR
        // crossover from 2-way splits -- with the allpass on the low band
        // that keeps all three in phase (see the class comment).
        for (int f = 0; f < numFrames; ++f) {
            const float* inFrame = interleaved + static_cast<size_t>(f) * m_channels;
            for (int ch = 0; ch < m_channels; ++ch) {
                const float x = inFrame[ch];
                float lowSample = m_lowLp2[ch].process(m_lowLp1[ch].process(x));
                lowSample = m_lowAllpass[ch].process(lowSample);

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
        if (fading) {
            m_fade.blend(interleaved, m_dry.data(), numFrames, m_channels);
        }
        markPrimed();
    }

    // Reserves the per-band scratch buffers for a known worst-case block,
    // so process() never allocates on the audio callback thread.
    void reserveBlock(int maxFrames) {
        if (m_channels <= 0 || maxFrames <= 0) return;
        const size_t total = static_cast<size_t>(maxFrames) * static_cast<size_t>(m_channels);
        if (m_lowBuf.size() < total) {
            m_lowBuf.resize(total);
            m_midBuf.resize(total);
            m_highBuf.resize(total);
        }
        m_dry.reserve(maxFrames, m_channels);
    }

    // Direct access to each band's compressor for configuration.
    Compressor low, mid, high;

private:
    void applyCrossoverFrequencies(double lowMidHz, double midHighHz) {
        const double nyquist = (m_sampleRate > 0.0) ? m_sampleRate * 0.5 : 24000.0;
        const double maxHz = nyquist * 0.45;
        if (!std::isfinite(lowMidHz)) lowMidHz = 250.0;
        if (!std::isfinite(midHighHz)) midHighHz = 4000.0;
        lowMidHz = std::clamp(lowMidHz, 20.0, maxHz);
        midHighHz = std::clamp(midHighHz, 20.0, maxHz);

        // At least one octave apart. Closer than that and 24 dB/octave
        // slopes overlap so heavily that the "bands" stop being separate
        // bands at all -- and before the allpass compensation above, that
        // overlap is what produced the -54 dB notch.
        if (midHighHz < lowMidHz * 2.0) {
            // Preserve the geometric centre of what was asked for, then
            // open the gap symmetrically around it, so nudging either
            // slider does not slam the other to an extreme.
            const double centre = std::sqrt(lowMidHz * midHighHz);
            lowMidHz = std::max(20.0, centre / std::sqrt(2.0));
            midHighHz = std::min(maxHz, centre * std::sqrt(2.0));
            // If the clamp above collapsed the gap again (only possible
            // very close to Nyquist), give up the upper band rather than
            // the lower one.
            if (midHighHz < lowMidHz * 2.0) {
                lowMidHz = std::max(20.0, midHighHz * 0.5);
            }
        }

        m_lowMidHz = lowMidHz;
        m_midHighHz = midHighHz;
        rebuildCrossovers();
    }

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

        // A matched LR4 lowpass/highpass pair sums to exactly this
        // 2nd-order allpass, so running the low band through it puts all
        // three bands on the same phase trajectory and the three-way sum
        // is flat again.
        Biquad allpassRef;
        allpassRef.setAllpass(m_midHighHz, kButterworthQ, m_sampleRate);

        for (auto& f : m_lowLp1) f.copyCoefficientsFrom(lowLpRef);
        for (auto& f : m_lowLp2) f.copyCoefficientsFrom(lowLpRef);
        for (auto& f : m_restHp1) f.copyCoefficientsFrom(restHpRef);
        for (auto& f : m_restHp2) f.copyCoefficientsFrom(restHpRef);
        for (auto& f : m_midLp1) f.copyCoefficientsFrom(midLpRef);
        for (auto& f : m_midLp2) f.copyCoefficientsFrom(midLpRef);
        for (auto& f : m_highHp1) f.copyCoefficientsFrom(highHpRef);
        for (auto& f : m_highHp2) f.copyCoefficientsFrom(highHpRef);
        for (auto& f : m_lowAllpass) f.copyCoefficientsFrom(allpassRef);
    }

    void markPrimed() {
        for (auto& f : m_lowLp1) f.markPrimed();
        for (auto& f : m_lowLp2) f.markPrimed();
        for (auto& f : m_restHp1) f.markPrimed();
        for (auto& f : m_restHp2) f.markPrimed();
        for (auto& f : m_midLp1) f.markPrimed();
        for (auto& f : m_midLp2) f.markPrimed();
        for (auto& f : m_highHp1) f.markPrimed();
        for (auto& f : m_highHp2) f.markPrimed();
        for (auto& f : m_lowAllpass) f.markPrimed();
    }

    int m_channels = 0;
    double m_sampleRate = 48000.0;
    BypassCrossfade m_fade;
    BypassScratch m_dry;
    double m_lowMidHz = 250.0;
    double m_midHighHz = 4000.0;

    std::vector<Biquad> m_lowLp1, m_lowLp2;
    std::vector<Biquad> m_restHp1, m_restHp2;
    std::vector<Biquad> m_midLp1, m_midLp2;
    std::vector<Biquad> m_highHp1, m_highHp2;
    std::vector<Biquad> m_lowAllpass;

    std::vector<float> m_lowBuf, m_midBuf, m_highBuf;
};

} // namespace naikav::dsp
