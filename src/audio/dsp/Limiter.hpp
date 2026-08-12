#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

namespace naikav::dsp {

// Lookahead peak limiter: a brick-wall gain computer (infinite ratio above
// the ceiling) with a very fast attack, plus a hard clamp on the final
// sample as a backstop.
//
// This holds a short (m_lookaheadMs) delay line and measures each
// channel-max peak as it *arrives*, before it is delayed and emitted --
// the smoothed gain envelope is therefore computed from a small window
// into the signal's future relative to what's actually being output at
// that instant. That gives the envelope time to reduce gain *ahead of* a
// fast transient instead of only reacting after it has already reached
// the output at full level, which is what a zero-lookahead design (the
// previous version of this class) cannot avoid -- a transient faster than
// the attack time constant would overshoot the smoothed envelope before it
// caught up, leaving the hard clamp to clip it rather than gain-reduce it
// smoothly. The tradeoff is a small constant output latency equal to the
// configured lookahead (a few milliseconds by default): inaudible on its
// own, and well within normal audio/video sync tolerance even with two
// Limiter instances in series (see AudioDecoder's DSP chain + final safety
// limiter).
//
// The hard clamp on the final sample remains as a backstop for the (now
// rare) case of the envelope still not having fully settled by the time a
// peak reaches the output, or of the very first lookahead window at
// startup before the delay line has filled.
//
// Default ceiling is 0 dBFS (1.0 in normalized float), which normal PCM
// content never exceeds, so this is a no-op (aside from the fixed
// lookahead delay) until the ceiling is lowered.
class Limiter {
public:
    void configure(int channels, double sampleRate) {
        m_channels = channels;
        m_sampleRate = sampleRate;
        updateTimeConstants();
        updateLookaheadBuffer();
        reset();
    }

    void setCeilingDb(float db) { m_ceilingDb = std::min(0.0f, db); }
    void setReleaseMs(float ms) {
        m_releaseMs = std::max(0.01f, ms);
        updateTimeConstants();
    }

    // Fixed output latency introduced by the lookahead delay line, in
    // frames -- for callers (tests, latency-compensation code) that need
    // to know it exactly rather than assuming a particular m_lookaheadMs.
    int getLookaheadFrames() const { return m_lookaheadFrames; }

    void reset() {
        m_envelopeDb = 0.0f; // start at "no reduction"
        std::fill(m_delayBuffer.begin(), m_delayBuffer.end(), 0.0f);
        m_writePos = 0;
        m_frameCounter = 0;
        m_windowMaxima.clear();
    }

    void process(float* interleaved, int numFrames) {
        if (m_channels <= 0) return;
        const float ceilingLinear = std::pow(10.0f, m_ceilingDb / 20.0f);

        for (int f = 0; f < numFrames; ++f) {
            float* frame = interleaved + static_cast<size_t>(f) * m_channels;

            // 1. Measure this incoming (pre-delay) frame's peak and fold it
            //    into a sliding-window maximum via a monotonic deque, so
            //    the running max over the lookahead window is O(1)
            //    amortized instead of rescanning the whole window every
            //    sample.
            float incomingPeak = 0.0f;
            for (int ch = 0; ch < m_channels; ++ch) {
                incomingPeak = std::max(incomingPeak, std::fabs(frame[ch]));
            }
            while (!m_windowMaxima.empty() && m_windowMaxima.back().second <= incomingPeak) {
                m_windowMaxima.pop_back();
            }
            const int64_t n = m_frameCounter++;
            m_windowMaxima.emplace_back(n, incomingPeak);
            const int64_t windowStart = n - m_lookaheadFrames;
            while (!m_windowMaxima.empty() && m_windowMaxima.front().first < windowStart) {
                m_windowMaxima.pop_front();
            }
            const float windowPeak = m_windowMaxima.front().second;

            // 2. Brick-wall gain computer, driven by the future-inclusive
            //    window peak rather than only the current sample.
            const float levelDb = 20.0f * std::log10(windowPeak + 1e-9f);
            const float targetReductionDb = std::min(0.0f, m_ceilingDb - levelDb);
            const float coeff = (targetReductionDb < m_envelopeDb) ? m_attackCoeff : m_releaseCoeff;
            m_envelopeDb = coeff * m_envelopeDb + (1.0f - coeff) * targetReductionDb;
            const float gainLinear = std::pow(10.0f, m_envelopeDb / 20.0f);

            // 3. Read-before-write ring buffer: emit the sample from
            //    m_lookaheadFrames ago (computed against a gain envelope
            //    that already "knew" about this sample's peak in advance),
            //    then store the incoming sample for future emission.
            const int slot = m_writePos;
            m_writePos = (m_writePos + 1) % m_delayLengthFrames;
            for (int ch = 0; ch < m_channels; ++ch) {
                const float incoming = frame[ch];
                const float delayed = m_delayBuffer[static_cast<size_t>(slot) * m_channels + ch];
                m_delayBuffer[static_cast<size_t>(slot) * m_channels + ch] = incoming;

                float sample = delayed * gainLinear;
                // Backstop clamp -- see the class comment above.
                sample = std::clamp(sample, -ceilingLinear, ceilingLinear);
                frame[ch] = sample;
            }
        }
    }

private:
    void updateTimeConstants() {
        if (m_sampleRate <= 0.0) return;
        m_attackCoeff = std::exp(-1.0f / (static_cast<float>(m_sampleRate) * (m_attackMs / 1000.0f)));
        m_releaseCoeff = std::exp(-1.0f / (static_cast<float>(m_sampleRate) * (m_releaseMs / 1000.0f)));
    }

    void updateLookaheadBuffer() {
        if (m_sampleRate <= 0.0 || m_channels <= 0) return;
        m_lookaheadFrames = static_cast<int>(std::lround(m_sampleRate * (m_lookaheadMs / 1000.0)));
        m_delayLengthFrames = std::max(1, m_lookaheadFrames);
        m_delayBuffer.assign(static_cast<size_t>(m_delayLengthFrames) * m_channels, 0.0f);
    }

    int m_channels = 0;
    double m_sampleRate = 48000.0;

    float m_ceilingDb = 0.0f; // 0 dBFS = inert by default
    float m_attackMs = 1.0f;  // deliberately very fast; not user-configurable (this is a limiter, not a compressor)
    float m_releaseMs = 50.0f;
    float m_attackCoeff = 0.0f;
    float m_releaseCoeff = 0.0f;

    float m_envelopeDb = 0.0f;

    // Fixed at a few milliseconds -- enough for the fast attack above to
    // mostly settle before a transient's delayed sample reaches the
    // output, short enough that the added output latency (up to 2x this,
    // since AudioDecoder chains two Limiter instances) stays comfortably
    // inside normal AV-sync tolerance. Not user-configurable, same
    // rationale as m_attackMs above.
    float m_lookaheadMs = 3.0f;
    int m_lookaheadFrames = 0;
    int m_delayLengthFrames = 1;
    std::vector<float> m_delayBuffer;
    int m_writePos = 0;
    int64_t m_frameCounter = 0;
    std::deque<std::pair<int64_t, float>> m_windowMaxima;
};

} // namespace naikav::dsp
