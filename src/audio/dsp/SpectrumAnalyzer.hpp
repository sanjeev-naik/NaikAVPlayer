#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <atomic>

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
// the display doesn't flicker.
//
// Three things here exist specifically because this runs on the audio
// callback thread:
//
//   - All FFT scratch is a member, not a local. The previous version
//     allocated a 1024-element complex vector and a 512-element float
//     vector on every FFT -- 94 heap allocations per second of audio, on
//     the real-time thread.
//
//   - The snapshot handshake is a seqlock, not a mutex. The getters used
//     to allocate a return vector *while holding* a mutex that the audio
//     thread also takes, so the OS-scheduled real-time callback could
//     block behind a normal-priority UI thread sitting inside the
//     allocator. Here the audio thread never blocks and never waits: it
//     bumps a generation counter, writes, and bumps it again; the reader
//     retries if it observes a torn read. That is the right trade for
//     display-only data, where a dropped frame costs nothing.
//
//   - The transform is a half-length complex FFT of the real input rather
//     than a full-length one on zero-padded imaginary parts, and the
//     twiddle factors come from a precomputed table rather than a
//     repeated complex multiply, which also removes the phase drift that
//     recurrence accumulated across each stage.
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
        buildTwiddles();
        m_scratch.assign(kNumBins, {});
        m_mags.assign(kNumBins, kFloorDb);
        m_smoothed.assign(kNumBins, kFloorDb);

        m_generation.store(0, std::memory_order_relaxed);
        m_sharedMags.assign(kNumBins, kFloorDb);
        m_sharedWaveform.assign(kFftSize, 0.0f);
    }

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    void reset() {
        std::fill(m_ringBuffer.begin(), m_ringBuffer.end(), 0.0f);
        m_writePos = 0;
        std::fill(m_smoothed.begin(), m_smoothed.end(), kFloorDb);
        publish(true);
    }

    // Feeds interleaved float samples (post-DSP-chain, pre-dither).
    // No-op while disabled.
    void process(const float* interleaved, int numFrames) {
        if (!m_enabled || m_channels <= 0 || numFrames <= 0) return;
        const float invCh = 1.0f / static_cast<float>(m_channels);
        for (int f = 0; f < numFrames; ++f) {
            const float* frame = interleaved + static_cast<size_t>(f) * m_channels;
            float mono = 0.0f;
            for (int ch = 0; ch < m_channels; ++ch) {
                mono += frame[ch];
            }
            mono *= invCh;

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

    // Thread-safe snapshot for the UI thread. Lock-free on the writer
    // side: the audio callback never blocks on this, and a reader that
    // catches a write in progress simply retries.
    std::vector<float> getMagnitudesDb() const {
        std::vector<float> out(kNumBins, kFloorDb);
        readSnapshot(&out, nullptr);
        return out;
    }

    // Thread-safe snapshot of recent time-domain audio samples [-1.0, 1.0]
    std::vector<float> getWaveformSamples() const {
        std::vector<float> out(kFftSize, 0.0f);
        readSnapshot(nullptr, &out);
        return out;
    }

    double binFrequencyHz(int bin) const {
        return static_cast<double>(bin) * m_sampleRate / kFftSize;
    }

    static constexpr float kFloorDb = -90.0f;

private:
    // Seqlock read: an odd generation, or a generation that changed
    // across the copy, means the writer was mid-update. Bounded retries
    // so a UI frame can never hang behind a pathological writer.
    void readSnapshot(std::vector<float>* mags, std::vector<float>* waveform) const {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const uint32_t before = m_generation.load(std::memory_order_acquire);
            if (before & 1u) continue; // write in progress
            if (mags) *mags = m_sharedMags;
            if (waveform) *waveform = m_sharedWaveform;
            if (m_generation.load(std::memory_order_acquire) == before) {
                return;
            }
        }
        // Fell through: leave the caller's default-filled buffers alone
        // rather than handing back a torn frame.
    }

    void publish(bool waveformOnlyZero = false) {
        m_generation.fetch_add(1, std::memory_order_release); // -> odd
        if (waveformOnlyZero) {
            std::fill(m_sharedMags.begin(), m_sharedMags.end(), kFloorDb);
            std::fill(m_sharedWaveform.begin(), m_sharedWaveform.end(), 0.0f);
        } else {
            std::copy(m_smoothed.begin(), m_smoothed.end(), m_sharedMags.begin());
            std::copy(m_ringBuffer.begin(), m_ringBuffer.end(), m_sharedWaveform.begin());
        }
        m_generation.fetch_add(1, std::memory_order_release); // -> even
    }

    void buildHannWindow() {
        m_window.resize(kFftSize);
        double sum = 0.0;
        for (int i = 0; i < kFftSize; ++i) {
            // Periodic (denominator kFftSize), not symmetric
            // (kFftSize - 1): the periodic form is the one that makes a
            // sinusoid at a bin centre land on exactly that bin, which is
            // what spectral analysis wants.
            const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * i / kFftSize));
            m_window[static_cast<size_t>(i)] = static_cast<float>(w);
            sum += w;
        }
        // Coherent gain of the window. Without dividing this out, every
        // displayed magnitude reads about 6 dB below the true level,
        // because a Hann window passes only half the amplitude of a
        // sinusoid on average.
        m_windowGain = static_cast<float>(sum / kFftSize);
    }

    void buildTwiddles() {
        // Half-length complex FFT twiddles.
        m_twiddle.resize(kNumBins / 2);
        for (int i = 0; i < kNumBins / 2; ++i) {
            const double ang = -2.0 * kPi * i / kNumBins;
            m_twiddle[static_cast<size_t>(i)] =
                std::complex<float>(static_cast<float>(std::cos(ang)),
                                    static_cast<float>(std::sin(ang)));
        }
        // Untangling twiddles that recover the real spectrum from the
        // half-length transform: exp(-j*2*pi*k/kFftSize) for every output
        // bin k.
        m_unpack.resize(kNumBins);
        for (int k = 0; k < kNumBins; ++k) {
            const double ang = -2.0 * kPi * k / kFftSize;
            m_unpack[static_cast<size_t>(k)] =
                std::complex<float>(static_cast<float>(std::cos(ang)),
                                    static_cast<float>(std::sin(ang)));
        }
    }

    void computeSpectrum() {
        // Pack the real, windowed input into a half-length complex
        // sequence: even samples become the real parts, odd samples the
        // imaginary parts.
        for (int i = 0; i < kNumBins; ++i) {
            const float even = m_ringBuffer[static_cast<size_t>(2 * i)] * m_window[static_cast<size_t>(2 * i)];
            const float odd = m_ringBuffer[static_cast<size_t>(2 * i + 1)] * m_window[static_cast<size_t>(2 * i + 1)];
            m_scratch[static_cast<size_t>(i)] = std::complex<float>(even, odd);
        }
        fft(m_scratch, m_twiddle);

        // 2/N for the one-sided spectrum of a real signal, divided by the
        // window's coherent gain so the displayed dB matches the actual
        // signal level rather than sitting ~6 dB low.
        const float norm = 2.0f / (static_cast<float>(kFftSize) * m_windowGain);

        // Untangle X[k] of the real sequence from Z[k] of the packed one.
        // With Ze and Zo the transforms of the even- and odd-indexed
        // samples:
        //   Ze[k] = (Z[k] + conj(Z[M-k])) / 2
        //   Zo[k] = (Z[k] - conj(Z[M-k])) / 2j
        //   X[k]  = Ze[k] + exp(-j*2*pi*k/N) * Zo[k]
        // which covers exactly the kNumBins bins this class reports.
        constexpr int M = kNumBins;
        for (int k = 0; k < M; ++k) {
            const std::complex<float> zk = m_scratch[static_cast<size_t>(k)];
            const std::complex<float> zm = std::conj(m_scratch[static_cast<size_t>((M - k) % M)]);
            const std::complex<float> ze = 0.5f * (zk + zm);
            const std::complex<float> zo = std::complex<float>(0.0f, -0.5f) * (zk - zm);
            const std::complex<float> x = ze + m_unpack[static_cast<size_t>(k)] * zo;
            m_mags[static_cast<size_t>(k)] = std::abs(x) * norm;
        }

        // Exponential smoothing frame-to-frame (fixed constant, not
        // user-configurable -- same design choice as e.g. Limiter's fixed
        // attack time) so the display doesn't flicker between the ~21ms
        // FFT updates.
        for (int i = 0; i < kNumBins; ++i) {
            const float db = 20.0f * std::log10(m_mags[static_cast<size_t>(i)] + 1e-9f);
            m_smoothed[static_cast<size_t>(i)] =
                kSmoothing * m_smoothed[static_cast<size_t>(i)] + (1.0f - kSmoothing) * db;
        }

        publish();
    }

    // In-place iterative radix-2 Cooley-Tukey FFT over a precomputed
    // twiddle table. n (a.size()) must be a power of 2.
    static void fft(std::vector<std::complex<float>>& a,
                    const std::vector<std::complex<float>>& twiddle) {
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
            const size_t step = n / len;
            for (size_t i = 0; i < n; i += len) {
                for (size_t k = 0; k < len / 2; ++k) {
                    const std::complex<float> u = a[i + k];
                    const std::complex<float> v = a[i + k + len / 2] * twiddle[k * step];
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
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
    float m_windowGain = 0.5f;
    int m_writePos = 0;

    // Audio-thread scratch: members, so computeSpectrum() never allocates.
    std::vector<std::complex<float>> m_scratch;
    std::vector<std::complex<float>> m_twiddle;
    std::vector<std::complex<float>> m_unpack;
    std::vector<float> m_mags;
    std::vector<float> m_smoothed;

    // Seqlock-published snapshot for the UI thread.
    mutable std::atomic<uint32_t> m_generation{0};
    std::vector<float> m_sharedMags;
    std::vector<float> m_sharedWaveform;
};

} // namespace naikav::dsp
