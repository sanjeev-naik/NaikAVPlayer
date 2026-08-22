#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "audio/dsp/Biquad.hpp"
#include "audio/dsp/DspMath.hpp"

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
// and re-inject the result into L/R with opposite polarity.
//
// Because the injected ambience is added to one ear and subtracted from
// the other, summing L+R after processing cancels it out exactly: the
// mono downmix of this effect's output is the mono downmix of its input,
// so it can never make content less mono-compatible than it already was.
// (Both channels are scaled by the same dry gain below, so the mono sum
// is the input's mono sum times that gain -- the cancellation is exact,
// the overall level is not unity. See the next paragraph.)
//
// The dry path passes at unity and the ambience is added on top.
//
// An earlier version mixed dry and wet at equal power, scaling dry by
// 1/sqrt(1+g^2) where g is the combined tap gain times intensity. At the
// default intensity of 1.0 that is a 2.6 dB cut to the untouched signal
// before any ambience is even added, and the cut is unconditional --
// derived from the worst case where the side signal is as loud as the
// programme, which is not how real stereo content behaves. The audible
// result was that switching the effect on read as "quieter" rather than
// "more spacious", which is precisely the judgement the control is
// supposed to let the listener make.
//
// Unscaled, enabling the effect adds ambience and nothing else: the dry
// signal is untouched, so intensity 0 and "disabled" agree exactly, and
// the mono sum is still the input's mono sum (the ambience cancels --
// see above). Where the sum pushes past full scale, AudioDecoder's
// m_finalSafetyLimiter is downstream and exists for exactly this.
class Surround3D {
public:
    void configure(int channels, double sampleRate) {
        m_channels = channels;
        m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;

        const int shortSamples = std::max(1, static_cast<int>(std::lround(kShortDelayMs / 1000.0 * m_sampleRate)));
        const int longSamples = std::max(1, static_cast<int>(std::lround(kLongDelayMs / 1000.0 * m_sampleRate)));
        m_shortDelay = shortSamples;
        m_longDelay = longSamples;

        // Power-of-two capacities so the rings wrap with a mask rather
        // than an integer division per sample. Capacity must exceed the
        // delay so the slot being read has not been overwritten yet.
        const int shortCap = nextPowerOfTwo(shortSamples + 1);
        const int longCap = nextPowerOfTwo(longSamples + 1);
        m_shortMask = shortCap - 1;
        m_longMask = longCap - 1;
        m_shortRing.assign(static_cast<size_t>(shortCap), 0.0f);
        m_longRing.assign(static_cast<size_t>(longCap), 0.0f);

        m_fade.configure(m_sampleRate);
        m_wetGlide.configure(m_sampleRate, m_intensity);
        m_shortLowpass.setLowpass(kShortLowpassHz, 0.70710678118, m_sampleRate);
        m_longLowpass.setLowpass(kLongLowpassHz, 0.70710678118, m_sampleRate);
        updateMixGains();
        reset();
    }

    // Crossfades in/out rather than switching on the next sample; see
    // BypassCrossfade.
    void setEnabled(bool enabled) { m_fade.setEnabled(enabled); }
    bool isEnabled() const { return m_fade.isEnabled(); }

    // Sizes the crossfade's dry-copy scratch so process() never allocates.
    void reserveBlock(int maxFrames) { m_dry.reserve(maxFrames, 2); }

    // 1.0 = the designed nominal strength, 0.0 = off (even if enabled),
    // >1.0 exaggerates the effect.
    void setIntensity(float intensity) {
        if (!std::isfinite(intensity)) return;
        m_intensity = std::clamp(intensity, 0.0f, 4.0f);
        updateMixGains();
    }
    float getIntensity() const { return m_intensity; }

    void reset() {
        m_fade.reset();
        m_wetGlide.reset();
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
        if (m_channels != 2 || numFrames <= 0) {
            return;
        }
        m_fade.markPrimed(); // live even while bypassed -- see DspChain
        m_wetGlide.markPrimed();
        // Zero intensity *and* settled. A glide still on its way down to
        // zero has ambience left to fade out; cutting it off here is the
        // step this glide exists to remove.
        if (m_fade.isInactive() || (m_intensity <= 0.0f && m_wetGlide.isSteady())) {
            feedDelaysOnly(interleaved, numFrames);
            return;
        }
        const size_t total = static_cast<size_t>(numFrames) * 2u;
        const bool fading = m_fade.isFading() && m_dry.fits(total);
        if (fading) {
            std::memcpy(m_dry.data(), interleaved, total * sizeof(float));
        } else if (m_fade.isFading()) {
            m_fade.snap();
        }
        const float dry = m_dryGain;
        for (int f = 0; f < numFrames; ++f) {
            // Glided per sample: intensity is a dragged slider and it
            // scales the injected ambience directly, so assigning it
            // outright stepped the output once per UI frame of the drag
            // -- measured at 9x the waveform's own per-sample slope.
            const float wet = m_wetGlide.next();
            float* frame = interleaved + static_cast<size_t>(f) * 2;
            const float side = 0.5f * (frame[0] - frame[1]);

            const int shortRead = (m_shortPos - m_shortDelay) & m_shortMask;
            float shortTap = m_shortRing[static_cast<size_t>(shortRead)];
            m_shortRing[static_cast<size_t>(m_shortPos)] = side;
            m_shortPos = (m_shortPos + 1) & m_shortMask;
            shortTap = m_shortLowpass.process(shortTap);

            const int longRead = (m_longPos - m_longDelay) & m_longMask;
            float longTap = m_longRing[static_cast<size_t>(longRead)];
            m_longRing[static_cast<size_t>(m_longPos)] = side;
            m_longPos = (m_longPos + 1) & m_longMask;
            longTap = m_longLowpass.process(longTap);

            // The tap gains are folded into m_wetGain, so this is the
            // normalized ambience directly.
            const float ambience = (shortTap * kShortGain + longTap * kLongGain) * wet;
            frame[0] = frame[0] * dry + ambience;
            frame[1] = frame[1] * dry - ambience;
        }
        if (fading) {
            m_fade.blend(interleaved, m_dry.data(), numFrames, 2);
        }
        m_shortLowpass.markPrimed();
        m_longLowpass.markPrimed();
    }

private:
    // Advances both early-reflection rings while the stage emits nothing,
    // so their contents stay contiguous with the signal actually playing.
    //
    // Returning outright instead froze the rings mid-stream. Re-enabling
    // then injected ~15ms and ~35ms of ambience built from side content
    // captured during the *previous* enabled period -- after a track
    // change, from a different file -- and then stepped to live content
    // once the taps caught up. Measured at 10x the waveform's own
    // per-sample slope, at exactly +15ms (the short tap) after each
    // re-enable: the same frozen-delay-line fault DspChain's limiter had,
    // for the same reason.
    //
    // Kept deliberately minimal: a subtract and two stores per frame, no
    // filtering and no output. The two lowpasses are left idle because
    // they resume from zero state into a correct tap signal, which is a
    // few samples of natural filter attack rather than a discontinuity --
    // and it happens while the crossfade is still near fully dry.
    void feedDelaysOnly(const float* interleaved, int numFrames) {
        for (int f = 0; f < numFrames; ++f) {
            const float* frame = interleaved + static_cast<size_t>(f) * 2;
            const float side = 0.5f * (frame[0] - frame[1]);
            m_shortRing[static_cast<size_t>(m_shortPos)] = side;
            m_shortPos = (m_shortPos + 1) & m_shortMask;
            m_longRing[static_cast<size_t>(m_longPos)] = side;
            m_longPos = (m_longPos + 1) & m_longMask;
        }
    }

    // Dry passes untouched; the ambience is added at the intensity the
    // user asked for. See the class comment for why there is no
    // compensating attenuation on the dry path.
    void updateMixGains() {
        m_dryGain = 1.0f;
        m_wetGlide.setTarget(m_intensity);
    }

    static constexpr double kShortDelayMs = 15.0, kLongDelayMs = 35.0;
    static constexpr double kShortLowpassHz = 7000.0, kLongLowpassHz = 4000.0;
    static constexpr float kShortGain = 0.55f, kLongGain = 0.35f;

    std::vector<float> m_shortRing;
    std::vector<float> m_longRing;
    int m_shortPos = 0;
    int m_longPos = 0;
    int m_shortDelay = 1;
    int m_longDelay = 1;
    int m_shortMask = 0;
    int m_longMask = 0;
    Biquad m_shortLowpass;
    Biquad m_longLowpass;

    int m_channels = 0;
    double m_sampleRate = 48000.0;
    BypassCrossfade m_fade;
    BypassScratch m_dry;
    float m_intensity = 1.0f;
    float m_dryGain = 1.0f;
    SmoothedParam m_wetGlide;
};

} // namespace naikav::dsp
