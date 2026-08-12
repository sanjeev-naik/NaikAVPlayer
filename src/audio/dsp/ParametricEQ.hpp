#pragma once

#include <array>
#include <vector>
#include <algorithm>
#include "Biquad.hpp"

namespace naikav::dsp {

// A true 5-band parametric peaking EQ: each band's center frequency, Q
// (bandwidth), and gain are all independently adjustable at runtime (not
// just gain), applied identically to every channel. Bands start at classic
// "graphic-ish" center frequencies (bass, low-mid, mid, high-mid, treble)
// as sane defaults, but callers are free to move them. Each band has
// independent per-channel filter state so multichannel audio (5.1/7.1/etc.)
// filters correctly -- coefficients are shared across channels (same tone
// shaping for every speaker), state is not.
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
        rebuildAllCoefficients();
    }

    void setBandGainDb(int band, float gainDb) {
        if (band < 0 || band >= kNumBands) return;
        m_bands[band].gainDb = gainDb;
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
        const double nyquist = m_sampleRate / 2.0;
        m_bands[band].freqHz = std::clamp(freqHz, 20.0, std::min(20000.0, nyquist * 0.99));
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
        m_bands[band].q = std::clamp(q, 0.1, 10.0);
        rebuildBandCoefficients(band);
    }

    double getBandQ(int band) const {
        return (band >= 0 && band < kNumBands) ? m_bands[band].q : 0.0;
    }

    // In-place processing of an interleaved float buffer (numFrames *
    // m_channels samples).
    void process(float* interleaved, int numFrames) {
        if (m_channels <= 0) return;
        for (int f = 0; f < numFrames; ++f) {
            float* frame = interleaved + static_cast<size_t>(f) * m_channels;
            for (int ch = 0; ch < m_channels; ++ch) {
                float sample = frame[ch];
                for (int band = 0; band < kNumBands; ++band) {
                    sample = filterAt(band, ch).process(sample);
                }
                frame[ch] = sample;
            }
        }
    }

    void reset() {
        for (auto& f : m_filters) f.reset();
    }

private:
    Biquad& filterAt(int band, int channel) {
        return m_filters[static_cast<size_t>(band) * m_channels + channel];
    }

    void rebuildBandCoefficients(int band) {
        if (m_channels <= 0 || m_sampleRate <= 0.0) return;
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

    std::array<BandConfig, kNumBands> m_bands;
    std::vector<Biquad> m_filters; // [band * channels + channel]
    int m_channels = 0;
    double m_sampleRate = 48000.0;
};

} // namespace naikav::dsp
