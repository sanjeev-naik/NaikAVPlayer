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

    // True when every band sits at 0 dB, so the whole stage is skipped.
    bool isInert() const { return m_activeCount == 0; }

    // In-place processing of an interleaved float buffer (numFrames *
    // m_channels samples).
    void process(float* interleaved, int numFrames) {
        if (m_channels <= 0 || numFrames <= 0 || m_activeCount == 0) return;

        const int active = m_activeCount;
        for (int f = 0; f < numFrames; ++f) {
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
                    sample = filterAt(m_activeBands[i], ch).process(sample);
                }
                frame[ch] = sample;
            }
        }

        for (int i = 0; i < active; ++i) {
            for (int ch = 0; ch < m_channels; ++ch) {
                filterAt(m_activeBands[i], ch).markPrimed();
            }
        }
    }

    void reset() {
        for (auto& f : m_filters) f.reset();
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
        refreshActiveBands();
        if (m_channels <= 0 || m_sampleRate <= 0.0) return;
        if (!bandIsActive(m_bands[band])) {
            // Nothing to design: the band is skipped in process(). Clear
            // its state so re-enabling it later starts clean rather than
            // resuming from history that is now arbitrarily old.
            for (int ch = 0; ch < m_channels; ++ch) {
                filterAt(band, ch).reset();
            }
            return;
        }
        Biquad reference;
        reference.setPeaking(m_bands[band].freqHz, m_bands[band].q, m_bands[band].gainDb, m_sampleRate);
        for (int ch = 0; ch < m_channels; ++ch) {
            filterAt(band, ch).copyCoefficientsFrom(reference);
        }
    }

    void rebuildAllCoefficients() {
        for (int band = 0; band < kNumBands; ++band) {
            rebuildBandCoefficients(band);
        }
    }

    // Compacts the indices of the non-flat bands, so process()'s inner
    // loop iterates only over bands that actually do something.
    void refreshActiveBands() {
        m_activeCount = 0;
        for (int b = 0; b < kNumBands; ++b) {
            if (bandIsActive(m_bands[b])) {
                m_activeBands[static_cast<size_t>(m_activeCount++)] = b;
            }
        }
    }

    std::array<BandConfig, kNumBands> m_bands;
    std::vector<Biquad> m_filters; // [band * channels + channel]
    std::array<int, kNumBands> m_activeBands{};
    int m_activeCount = 0;
    int m_channels = 0;
    double m_sampleRate = 48000.0;
};

} // namespace naikav::dsp
