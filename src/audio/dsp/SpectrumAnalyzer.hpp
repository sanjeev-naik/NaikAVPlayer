#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <mutex>

#include "audio/dsp/DspMath.hpp"

namespace naikav::dsp {

// Real-time magnitude spectrum analyzer for the Audio Processing panel's
// visualizer. Hand-rolled (an in-place iterative radix-2 Cooley-Tukey FFT),
// matching this DSP folder's convention of hand-rolling standard algorithms
// rather than pulling in a dependency -- unlike EBU R128 loudness metering
// (where spec-compliance actually matters), a spectrum display has no
// "correctness" requirement beyond "looks like the right shape," so there's
// no reason to depend on FFmpeg's internal FFT/av_tx API here.
//
// Feeds on the final post-DSP-chain signal (downmixed to mono for display
// purposes -- a spectrum view doesn't need L/R separation), accumulating
// samples into a fixed-size ring buffer and computing a fresh FFT every
// kFftSize samples (~21ms at 48kHz, a non-overlapping block -- simple, and
// plenty smooth for a visual display). The result is a Hann-windowed,
// dB-scaled magnitude spectrum, exponentially smoothed frame-to-frame so
// the display doesn't flicker, exposed via a self-synchronized snapshot
// getter for the UI thread to read independently of the audio callback
// thread that's writing it.
//
// Disabled by default: process() is then a true no-op, matching the rest
// of this DSP pipeline's "disabled = truly zero cost" convention.
class SpectrumAnalyzer {
public:
    static constexpr int kFftSize = 1024; // power of 2
    static constexpr int kNumBins = kFftSize / 2;

    void configure(int channels, double sampleRate) {
        m_channels = channels;
        m_sampleRate = sampleRate;
        m_ringBuffer.assign(kFftSize, 0.0f);
        m_writePos = 0;
        buildHannWindow();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_magnitudesDb.assign(kNumBins, kFloorDb);
    }

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    void reset() {
        std::fill(m_ringBuffer.begin(), m_ringBuffer.end(), 0.0f);
        m_writePos = 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        std::fill(m_magnitudesDb.begin(), m_magnitudesDb.end(), kFloorDb);
    }

    // Feeds interleaved float samples (post-DSP-chain, pre-dither).
    // No-op while disabled.
    void process(const float* interleaved, int numFrames) {
        if (!m_enabled || m_channels <= 0) return;
        for (int f = 0; f < numFrames; ++f) {
            float mono = 0.0f;
            for (int ch = 0; ch < m_channels; ++ch) {
                mono += interleaved[static_cast<size_t>(f) * m_channels + ch];
            }
            mono /= static_cast<float>(m_channels);

            m_ringBuffer[static_cast<size_t>(m_writePos)] = mono;
            ++m_writePos;
            if (m_writePos == kFftSize) {
                // A full non-overlapping block just completed -- since
                // writes always start at index 0 and wrap only at exact
                // multiples of kFftSize, the buffer's natural index order
                // is already oldest-to-newest chronological order here, no
                // extra bookkeeping needed.
                computeSpectrum();
                m_writePos = 0;
            }
        }
    }

    // Thread-safe snapshot for the UI thread -- self-synchronized, so
    // callers don't need any of AudioDecoder's own DSP locking to use this.
    std::vector<float> getMagnitudesDb() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_magnitudesDb;
    }

    double binFrequencyHz(int bin) const {
        return static_cast<double>(bin) * m_sampleRate / kFftSize;
    }

    static constexpr float kFloorDb = -90.0f;

private:
    void buildHannWindow() {
        m_window.resize(kFftSize);
        for (int i = 0; i < kFftSize; ++i) {
            m_window[static_cast<size_t>(i)] =
                0.5f * (1.0f - std::cos(2.0f * static_cast<float>(kPi) * static_cast<float>(i) / (kFftSize - 1)));
        }
    }

    void computeSpectrum() {
        std::vector<std::complex<float>> buf(kFftSize);
        for (int i = 0; i < kFftSize; ++i) {
            buf[static_cast<size_t>(i)] = std::complex<float>(m_ringBuffer[static_cast<size_t>(i)] * m_window[static_cast<size_t>(i)], 0.0f);
        }
        fft(buf);

        // 2/N normalization: standard scaling for a one-sided magnitude
        // spectrum of a real signal (accounts for the energy folded into
        // the mirrored negative-frequency half that's being discarded).
        const float norm = 2.0f / static_cast<float>(kFftSize);
        std::vector<float> mags(kNumBins);
        for (int i = 0; i < kNumBins; ++i) {
            float mag = std::abs(buf[static_cast<size_t>(i)]) * norm;
            mags[static_cast<size_t>(i)] = 20.0f * std::log10(mag + 1e-9f);
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_magnitudesDb.size() != mags.size()) {
            m_magnitudesDb = std::move(mags);
            return;
        }
        // Exponential smoothing frame-to-frame (fixed constant, not
        // user-configurable -- same design choice as e.g. Limiter's fixed
        // attack time) so the display doesn't flicker between the ~21ms
        // FFT updates.
        for (size_t i = 0; i < mags.size(); ++i) {
            m_magnitudesDb[i] = kSmoothing * m_magnitudesDb[i] + (1.0f - kSmoothing) * mags[i];
        }
    }

    // In-place iterative radix-2 Cooley-Tukey FFT. n (a.size()) must be a
    // power of 2 -- guaranteed here since kFftSize is a compile-time
    // constant power of 2.
    static void fft(std::vector<std::complex<float>>& a) {
        const size_t n = a.size();
        if (n <= 1) return;

        for (size_t i = 1, j = 0; i < n; ++i) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) {
                j ^= bit;
            }
            j ^= bit;
            if (i < j) {
                std::swap(a[i], a[j]);
            }
        }

        for (size_t len = 2; len <= n; len <<= 1) {
            const float ang = -2.0f * static_cast<float>(kPi) / static_cast<float>(len);
            const std::complex<float> wlen(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < n; i += len) {
                std::complex<float> w(1.0f, 0.0f);
                for (size_t k = 0; k < len / 2; ++k) {
                    const std::complex<float> u = a[i + k];
                    const std::complex<float> v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wlen;
                }
            }
        }
    }

    static constexpr float kSmoothing = 0.55f;

    int m_channels = 0;
    double m_sampleRate = 48000.0;
    bool m_enabled = false;

    std::vector<float> m_ringBuffer;
    std::vector<float> m_window;
    int m_writePos = 0;

    mutable std::mutex m_mutex;
    std::vector<float> m_magnitudesDb;
};

} // namespace naikav::dsp
