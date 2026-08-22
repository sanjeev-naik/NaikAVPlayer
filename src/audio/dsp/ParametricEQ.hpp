#pragma once

#include <array>
#include <vector>
#include <algorithm>
#include <cmath>
#include "audio/dsp/Biquad.hpp"
#include "audio/dsp/DspMath.hpp"

namespace naikav::dsp {

// A true 5-band parametric peaking EQ: each band's center frequency, Q
// (bandwidth), and gain are all independently adjustable at runtime (not
// just gain), applied identically to every channel. Bands start at classic
// "graphic-ish" center frequencies (bass, low-mid, mid, high-mid, treble)
// as sane defaults, but callers are free to move them. Each band has
// independent per-channel filter state so multichannel audio (5.1/7.1/etc.)
// filters correctly -- coefficients are shared across channels (same tone
// shaping for every speaker), state is not.
//
// A band at 0 dB is skipped entirely rather than run as an identity
// filter. At 0 dB the peaking coefficients satisfy b1 == a1 and b2 == a2,
// so the transfer function is algebraically unity -- but Direct Form I
// evaluates the cancelling terms as a flat accumulation rather than
// grouping them, so the identity does not hold exactly in floating point.
// Measured on the previous version: a nominally flat 5-band EQ added a
// -79 dBFS error floor, which is above the 16-bit dither floor and so
// present in the delivered output, while costing full CPU (over half the
// entire inert chain's budget at 7.1) for no effect at all. Skipping is
// both bit-exact and free.
//
// But a band may only *leave* the path once it has already ramped down to
// unity, and must be back in the path before it ramps up again. Dropping
// it the instant its gain crossed the threshold took a filter carrying
// 12 dB of boost -- and 12 dB of accumulated state -- out of the signal
// in a single sample: measured at 54x the waveform's own per-sample slope
// for a 5-band preset change, and 29x in the other direction. Note the
// click is there even though the band is algebraically unity at the
// crossing point; what steps is the filter's *state*, not its gain, which
// is exactly what Biquad's coefficient ramp exists to handle and what
// skipping went around.
//
// So an inactive band is designed at 0 dB rather than torn out, which
// lets the existing ramp glide it toward unity, and its contribution is
// additionally faded out before it leaves the path. Both are needed.
// Ramping the coefficients alone is not enough, and the reason is worth
// stating: at 0 dB the coefficients are an identity *transfer function*,
// but a filter only realises that identity in steady state. Its stored
// y[n-1]/y[n-2] still describe the boost it was applying a moment ago, so
// its output is converging toward its input rather than equal to it, and
// how long that takes is set by the pole radius -- tens of milliseconds
// for a low-frequency, high-Q band. Dropping the band at the instant the
// coefficient ramp ended therefore still stepped, by 23x the waveform's
// own per-sample slope, now at exactly kRampSamples after the change.
//
// The per-band fade bounds that: the band's contribution is crossfaded
// back to its own input over a fixed few milliseconds regardless of pole
// radius, and only then does it leave the list. Steady state is unchanged
// -- once faded out the band is dropped and the EQ is bit-exact and free
// again, exactly as above.
class ParametricEQ {
public:
    static constexpr int kNumBands = 5;

    struct BandConfig {
        double freqHz;
        double q;
        float gainDb; // 0 = flat/inert
    };

    ParametricEQ() {
        // Default center frequencies span the audible range for a classic
        // 5-band EQ. All gains start at 0 dB (flat/identity) so enabling
        // the chain doesn't change the sound until bands are adjusted.
        m_bands[0] = {60.0, 0.9, 0.0f};
        m_bands[1] = {250.0, 1.0, 0.0f};
        m_bands[2] = {1000.0, 1.0, 0.0f};
        m_bands[3] = {4000.0, 1.0, 0.0f};
        m_bands[4] = {12000.0, 0.9, 0.0f};
    }

    void configure(int channels, double sampleRate) {
        m_channels = channels;
        m_sampleRate = sampleRate;
        m_filters.assign(static_cast<size_t>(kNumBands) * channels, Biquad{});
        for (int b = 0; b < kNumBands; ++b) {
            m_bandMix[b].configure(sampleRate, bandIsActive(m_bands[b]) ? 1.0f : 0.0f);
        }
        // Re-clamp every band against the new Nyquist limit before
        // rebuilding, so a rate change cannot leave a band above it.
        for (int b = 0; b < kNumBands; ++b) {
            m_bands[b].freqHz = clampFreq(m_bands[b].freqHz);
            m_bands[b].q = std::clamp(m_bands[b].q, 0.1, 10.0);
        }
        rebuildAllCoefficients();
    }

    void setBandGainDb(int band, float gainDb) {
        if (band < 0 || band >= kNumBands) return;
        if (!std::isfinite(gainDb)) return;
        m_bands[band].gainDb = std::clamp(gainDb, -24.0f, 24.0f);
        rebuildBandCoefficients(band);
    }

    float getBandGainDb(int band) const {
        return (band >= 0 && band < kNumBands) ? m_bands[band].gainDb : 0.0f;
    }

    // Moves the band's center frequency. Clamped to a sane audible sub-range
    // (20Hz-20kHz, and below Nyquist) so a bad UI/settings value can't hand
    // the biquad cookbook formula a w0 outside its valid domain.
    void setBandFrequencyHz(int band, double freqHz) {
        if (band < 0 || band >= kNumBands || m_sampleRate <= 0.0) return;
        if (!std::isfinite(freqHz)) return;
        m_bands[band].freqHz = clampFreq(freqHz);
        rebuildBandCoefficients(band);
    }

    double getBandFrequencyHz(int band) const {
        return (band >= 0 && band < kNumBands) ? m_bands[band].freqHz : 0.0;
    }

    // Adjusts the band's Q (bandwidth) -- higher Q narrows the affected
    // band around its center frequency. Clamped to a range that stays
    // numerically well-behaved in the cookbook formula (very low Q starts
    // to resemble a shelf; very high Q approaches instability).
    void setBandQ(int band, double q) {
        if (band < 0 || band >= kNumBands) return;
        if (!std::isfinite(q)) return;
        m_bands[band].q = std::clamp(q, 0.1, 10.0);
        rebuildBandCoefficients(band);
    }

    double getBandQ(int band) const {
        return (band >= 0 && band < kNumBands) ? m_bands[band].q : 0.0;
    }

    // True when no band is doing anything and none is still ramping down,
    // so the whole stage is skipped.
    bool isInert() const { return m_activeCount == 0; }

    // In-place processing of an interleaved float buffer (numFrames *
    // m_channels samples).
    void process(float* interleaved, int numFrames) {
        if (m_channels <= 0 || numFrames <= 0) return;

        // Every filter, not only the ones about to run, and *before* the
        // flat-EQ early return below. Audio is flowing through this stage
        // even when every band sits at 0 dB, so the filters count as live
        // from here -- same reasoning as BypassCrossfade::markPrimed().
        // Priming only the bands being processed meant a flat EQ never
        // primed at all, so the first band the user touched snapped in at
        // full strength instead of ramping: 29x the waveform's own
        // per-sample slope, and the fix for the other direction did not
        // help because this path never reached it.
        for (auto& f : m_filters) {
            f.markPrimed();
        }
        for (auto& m : m_bandMix) {
            m.markPrimed();
        }
        if (m_activeCount == 0) return;

        const int active = m_activeCount;
        std::array<float, kNumBands> mix{};
        for (int f = 0; f < numFrames; ++f) {
            // Advanced once per frame, before the channel loop -- every
            // channel of a frame must see the same mix, and the glide has
            // to keep time with the audio rather than with the channel
            // count.
            for (int i = 0; i < active; ++i) {
                mix[static_cast<size_t>(i)] = m_bandMix[m_activeBands[i]].next();
            }
            float* frame = interleaved + static_cast<size_t>(f) * m_channels;
            for (int ch = 0; ch < m_channels; ++ch) {
                float sample = frame[ch];
                // Not an accumulation despite the shape: each band is a
                // stateful IIR filter whose output feeds the next, and
                // process() mutates that band's own history as a side
                // effect. std::accumulate would obscure both the ordering
                // dependency and the mutation.
                for (int i = 0; i < active; ++i) {
                    // cppcheck-suppress useStlAlgorithm
                    const float wet = filterAt(m_activeBands[i], ch).process(sample);
                    const float m = mix[static_cast<size_t>(i)];
                    // Fully wet is the overwhelmingly common case -- a band
                    // sitting at its setting -- and must stay bit-exact, so
                    // it is taken as an exact branch rather than as a blend
                    // that happens to evaluate to the same thing.
                    sample = (m == 1.0f) ? wet : (wet * m + sample * (1.0f - m));
                }
                frame[ch] = sample;
            }
        }

        // A band that has finished ramping down to unity can now leave the
        // path, which is what restores the bit-exact/zero-cost steady state.
        refreshActiveBands();
    }

    void reset() {
        for (auto& f : m_filters) f.reset();
        for (int b = 0; b < kNumBands; ++b) m_bandMix[b].reset();
        rebuildAllCoefficients();
    }

private:
    // A band whose gain rounds to zero at the resolution the UI and the
    // settings file actually carry is treated as off.
    static bool bandIsActive(const BandConfig& b) {
        return std::fabs(b.gainDb) >= 0.01f;
    }

    double clampFreq(double freqHz) const {
        const double nyquist = (m_sampleRate > 0.0) ? m_sampleRate / 2.0 : 24000.0;
        return std::clamp(freqHz, 20.0, std::min(20000.0, nyquist * 0.99));
    }

    Biquad& filterAt(int band, int channel) {
        return m_filters[static_cast<size_t>(band) * m_channels + channel];
    }

    void rebuildBandCoefficients(int band) {
        if (m_channels <= 0 || m_sampleRate <= 0.0) {
            refreshActiveBands();
            return;
        }
        // An inactive band is designed at 0 dB -- algebraically unity --
        // rather than left undesigned, so a filter that has already
        // processed audio *ramps* to unity through the same machinery a
        // gain change uses, instead of being yanked out of the path. See
        // the class comment.
        const bool activeNow = bandIsActive(m_bands[band]);
        const float gainDb = activeNow ? m_bands[band].gainDb : 0.0f;
        Biquad reference;
        reference.setPeaking(m_bands[band].freqHz, m_bands[band].q, gainDb, m_sampleRate);
        for (int ch = 0; ch < m_channels; ++ch) {
            filterAt(band, ch).copyCoefficientsFrom(reference);
        }
        m_bandMix[band].setTarget(activeNow ? 1.0f : 0.0f);
        refreshActiveBands();
    }

    void rebuildAllCoefficients() {
        for (int band = 0; band < kNumBands; ++band) {
            rebuildBandCoefficients(band);
        }
    }

    // True while any of this band's per-channel filters is still gliding
    // toward its new coefficients.
    bool bandIsRamping(int band) {
        for (int ch = 0; ch < m_channels; ++ch) {
            if (filterAt(band, ch).isRamping()) return true;
        }
        return false;
    }

    // Compacts the indices of the bands process()'s inner loop must run:
    // the non-flat ones, plus any still ramping down to unity after being
    // flattened. A band that is neither has its state cleared as it leaves
    // the path, so switching it back on later cannot resume from history
    // that is by then arbitrarily old.
    void refreshActiveBands() {
        m_activeCount = 0;
        for (int b = 0; b < kNumBands; ++b) {
            // Kept in the path until it is flat, has finished ramping, and
            // has finished fading -- all three, or the drop itself steps.
            if (bandIsActive(m_bands[b]) || bandIsRamping(b) || !m_bandMix[b].isSteady()) {
                m_activeBands[static_cast<size_t>(m_activeCount++)] = b;
            } else {
                for (int ch = 0; ch < m_channels; ++ch) {
                    filterAt(b, ch).clearState();
                }
            }
        }
    }

    std::array<BandConfig, kNumBands> m_bands;
    // Per-band wet/dry mix, so a band entering or leaving the processing
    // list does so over a few milliseconds instead of in one sample.
    std::array<SmoothedParam, kNumBands> m_bandMix;
    std::vector<Biquad> m_filters; // [band * channels + channel]
    std::array<int, kNumBands> m_activeBands{};
    int m_activeCount = 0;
    int m_channels = 0;
    double m_sampleRate = 48000.0;
};

} // namespace naikav::dsp
