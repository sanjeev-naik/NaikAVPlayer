#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "audio/dsp/Biquad.hpp"
#include "audio/dsp/DspMath.hpp"

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
//     below the crossover it likely can't reproduce well), and the content
//     removed from each of those channels is summed into the LFE channel
//     instead of being discarded. This is what lets small satellite/main
//     speakers hand their bass off to a real subwoofer rather than either
//     losing it or trying to reproduce it themselves.
//
// The redirected sum is at unity, because bass management is
// energy-preserving by definition: content is *removed* from N-1 speakers
// by their highpass, and the subwoofer has to receive all of it. Overall
// programme energy is conserved -- it is only relocated into one channel.
//
// A previous version divided the sum by 1/(N-1), which discarded
// (N-2)/(N-1) of the redirected bass: measured at -14.3 dB on 5.1, and
// identically so whether bass sat on two channels or all five, because
// the factor keyed off the channel *count* rather than off how many
// channels actually carried bass. Turning bass management on made bass
// quieter, which is the opposite of its purpose.
//
// That factor came from a real concern -- N fully-correlated full-scale
// channels sum to +15.3 dBFS on 5.1, and the downstream safety limiter is
// channel-linked, so an over that large drags the whole mix down. But
// "every channel simultaneously at full scale and perfectly correlated"
// is a synthetic worst case, and applying its correction unconditionally
// taxes all real content for a peak that real content does not reach.
// Headroom is handled where headroom belongs: DspChain's limiter runs
// after this stage (deliberately -- see DspChain's comment), and
// AudioDecoder's m_finalSafetyLimiter is downstream of that again.
//
// Where a trim is genuinely wanted -- a hot LFE track, or a subwoofer
// that needs matching to the mains -- setLfeGainDb() is a visible control
// the user can set, which is what the hidden 1/N should always have been.
// Contrast SpatialDownmixer, which sums full-band, only partially
// correlated channels and normalizes by power.
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
        m_fade.configure(sampleRate);
        m_lfeGlide.configure(sampleRate, m_lfeGain);
        m_redirectMix.configure(sampleRate, m_bassRedirectEnabled ? 1.0f : 0.0f);
        rebuildCoefficients();
        reset();
    }

    // Crossfades in/out over a few ms rather than switching on the next
    // sample -- see BypassCrossfade. Snaps if no audio has run yet.
    void setEnabled(bool enabled) { m_fade.setEnabled(enabled); }
    bool isEnabled() const { return m_fade.isEnabled(); }

    // Sizes the crossfade's dry-copy scratch so process() never allocates.
    void reserveBlock(int maxFrames) { m_dry.reserve(maxFrames, m_channels); }

    // See the class comment above. No effect unless setEnabled(true) too.
    //
    // Crossfaded, not switched. This is its own signal-path change on top
    // of the stage's enable flag -- every non-LFE channel swaps between
    // full-range and highpassed, and the LFE gains or loses the whole
    // redirected sum -- so flipping it outright stepped the waveform on
    // every channel at once: measured at 56x its own per-sample slope,
    // the largest single discontinuity in the folder. It got missed
    // because the stage's setEnabled() *was* crossfaded, and this reads
    // like a sub-option of it rather than a second switch.
    void setBassRedirectEnabled(bool enabled) {
        m_bassRedirectEnabled = enabled;
        m_redirectMix.setTarget(enabled ? 1.0f : 0.0f);
    }
    bool isBassRedirectEnabled() const { return m_bassRedirectEnabled; }

    // Level trim for the LFE/subwoofer channel, in dB, applied to whatever
    // this stage emits there (its own lowpassed content plus any
    // redirected bass). 0 dB -- the default -- is exactly unity, so the
    // stage is bit-transparent on that channel until the user asks for a
    // trim. This is the visible replacement for the old hidden 1/N; see
    // the class comment.
    void setLfeGainDb(float db) {
        if (!std::isfinite(db)) return;
        m_lfeGainDb = std::clamp(db, -24.0f, 12.0f);
        m_lfeGain = (m_lfeGainDb == 0.0f) ? 1.0f
                                          : std::pow(10.0f, m_lfeGainDb / 20.0f);
        // Glided rather than applied outright: this is a dragged trim and
        // it multiplies the LFE channel directly, so a bare assignment
        // stepped that channel once per UI frame of the drag -- measured
        // at 20x the waveform's own per-sample slope for a single 12 dB
        // change.
        m_lfeGlide.setTarget(m_lfeGain);
    }
    float getLfeGainDb() const { return m_lfeGainDb; }

    // Clamped to a range the cookbook formula is actually valid over.
    // Without this, a hand-edited or corrupted settings file could hand
    // the biquad a cutoff above Nyquist (alpha goes negative, a0 -> 0,
    // poles leave the unit circle) or a NaN -- and because a biquad is a
    // feedback structure, the resulting Inf/NaN propagates through its
    // state forever. Restoring a sane value would NOT recover it; only
    // reset(), which is reached only from the seek/flush path, would.
    // AudioDspSettings::sanitize() is the first line of defence; this is
    // the second, and Biquad::validParams() is the third.
    void setCutoffHz(double hz) {
        if (!std::isfinite(hz)) return;
        const double maxHz = (m_sampleRate > 0.0) ? m_sampleRate * 0.45 : 20000.0;
        m_cutoffHz = std::clamp(hz, 20.0, maxHz);
        rebuildCoefficients();
    }
    double getCutoffHz() const { return m_cutoffHz; }

    void reset() {
        m_fade.reset();
        m_lfeGlide.reset();
        m_redirectMix.reset();
        m_lfeLowpassStage1.reset();
        m_lfeLowpassStage2.reset();
        for (auto& f : m_mainHighpassStage1) f.reset();
        for (auto& f : m_mainHighpassStage2) f.reset();
        for (auto& f : m_mainLowpassStage1) f.reset();
        for (auto& f : m_mainLowpassStage2) f.reset();
        rebuildCoefficients();
    }

    // In-place processing of an interleaved float buffer. No-op unless
    // enabled and configured with a valid target (LFE) channel.
    void process(float* interleaved, int numFrames) {
        if (m_targetChannel < 0 || m_targetChannel >= m_channels || numFrames <= 0) {
            return;
        }
        m_fade.markPrimed(); // live even while bypassed -- see DspChain
        m_lfeGlide.markPrimed();
        m_redirectMix.markPrimed();
        if (m_fade.isInactive()) {
            return;
        }
        const size_t total = static_cast<size_t>(numFrames) * static_cast<size_t>(m_channels);
        const bool fading = m_fade.isFading() && m_dry.fits(total);
        if (fading) {
            std::memcpy(m_dry.data(), interleaved, total * sizeof(float));
        } else if (m_fade.isFading()) {
            m_fade.snap(); // no scratch: snap rather than allocate here
        }
        for (int f = 0; f < numFrames; ++f) {
            float* frame = interleaved + static_cast<size_t>(f) * m_channels;

            float lfeSample = frame[m_targetChannel];
            lfeSample = m_lfeLowpassStage1.process(lfeSample);
            lfeSample = m_lfeLowpassStage2.process(lfeSample);

            float redirectedBass = 0.0f;
            // Advanced every frame whether or not the redirect is running,
            // so the glide keeps time with the audio rather than with how
            // often this branch happens to be taken.
            const float redirect = m_redirectMix.next();
            if (redirect > 0.0f) {
                const float direct = 1.0f - redirect;
                for (int ch = 0; ch < m_channels; ++ch) {
                    if (ch == m_targetChannel) continue;
                    const float original = frame[ch];

                    float low = m_mainLowpassStage1[ch].process(original);
                    low = m_mainLowpassStage2[ch].process(low);
                    redirectedBass += low * redirect;

                    float high = m_mainHighpassStage1[ch].process(original);
                    high = m_mainHighpassStage2[ch].process(high);
                    // Blend toward the untouched channel rather than
                    // swapping to the highpassed one outright.
                    frame[ch] = high * redirect + original * direct;
                }
            }

            // Unity sum -- see the class comment. The glided gain is
            // exactly 1.0f unless the user has dialled in a trim.
            frame[m_targetChannel] = (lfeSample + redirectedBass) * m_lfeGlide.next();
        }
        if (fading) {
            m_fade.blend(interleaved, m_dry.data(), numFrames, m_channels);
        }
        markPrimed();
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

    // Marks every filter live, so a later cutoff change ramps its
    // coefficients rather than stepping them (see Biquad).
    void markPrimed() {
        m_lfeLowpassStage1.markPrimed();
        m_lfeLowpassStage2.markPrimed();
        for (auto& f : m_mainHighpassStage1) f.markPrimed();
        for (auto& f : m_mainHighpassStage2) f.markPrimed();
        for (auto& f : m_mainLowpassStage1) f.markPrimed();
        for (auto& f : m_mainLowpassStage2) f.markPrimed();
    }

    Biquad m_lfeLowpassStage1, m_lfeLowpassStage2;
    std::vector<Biquad> m_mainHighpassStage1, m_mainHighpassStage2;
    std::vector<Biquad> m_mainLowpassStage1, m_mainLowpassStage2;

    int m_channels = 0;
    int m_targetChannel = -1;
    BypassCrossfade m_fade;
    BypassScratch m_dry;
    bool m_bassRedirectEnabled = false;
    SmoothedParam m_redirectMix;
    double m_cutoffHz = 120.0;
    double m_sampleRate = 48000.0;
    float m_lfeGainDb = 0.0f;
    float m_lfeGain = 1.0f;
    SmoothedParam m_lfeGlide;
};

} // namespace naikav::dsp
