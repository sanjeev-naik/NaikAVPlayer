#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <vector>

#include "audio/dsp/DspMath.hpp"

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
// the output at full level, which is what a zero-lookahead design cannot
// avoid.
//
// The running window maximum comes from a monotonic deque, which makes it
// O(1) amortized instead of rescanning the window every sample. That deque
// is a fixed-capacity ring buffer, NOT std::deque: the window length is
// bounded and known at configure() time, and std::deque allocates and
// frees its map blocks as the window slides -- measured at 1,477
// allocations per second of audio, on the SDL audio callback thread,
// unconditionally (AudioDecoder runs one of these as an always-on safety
// limiter, plus a second inside DspChain). Unbounded-latency malloc is not
// something a real-time audio callback can do.
//
// The hard clamp on the final sample remains as a backstop for the case of
// the envelope still not having fully settled by the time a peak reaches
// the output.
//
// Default ceiling is 0 dBFS (1.0 in normalized float), which normal PCM
// content never exceeds, so this is gain-inert until the ceiling is
// lowered -- but not zero-latency: the lookahead delay line always costs
// exactly m_lookaheadFrames of output delay, reported by
// getLookaheadFrames().
//
// That latency is unavoidable and it is emitted honestly: after a reset
// the delay line holds zeros, so the first m_lookaheadFrames of output are
// silence. A previous version tried to avoid that silence by passing the
// input straight through while the line filled -- but it also wrote those
// same samples into the line, so once the warm-up ended the read pointer
// started back at slot 0 and replayed every one of them. The first 3ms of
// audio came out twice, with a waveform discontinuity at the seam: a click
// plus a comb artifact after every seek and at the start of playback,
// which is worse than the 3ms of silence it was avoiding. Constant,
// monotonic latency is the correct behavior for a lookahead limiter, and
// it is what the delay line naturally produces.
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
    // frames -- for callers (tests, diagnostics) that need to know it
    // exactly rather than assuming a particular m_lookaheadMs.
    //
    // Note that AudioDecoder::getAudioClock() deliberately does NOT
    // subtract this: compensating there made video drop frames, for
    // reasons specific to how that clock's per-buffer offset is built.
    // See the comment in getAudioClock() before wiring this into any
    // sync path. AudioDecoder tracks the figure via getDspLatencyFrames()
    // for diagnostics.
    int getLookaheadFrames() const { return m_lookaheadFrames; }

    void reset() {
        m_envelopeDb = 0.0f;
        std::fill(m_delayBuffer.begin(), m_delayBuffer.end(), 0.0f);
        m_writePos = 0;
        m_frameCounter = 0;
        m_head = 0;
        m_tail = 0;
    }

    void process(float* interleaved, int numFrames) {
        if (m_channels <= 0 || numFrames <= 0) return;

        const float ceilingLinear = fastDbToLinear(m_ceilingDb);

        for (int f = 0; f < numFrames; ++f) {
            float* frame = interleaved + static_cast<size_t>(f) * m_channels;

            // 1. Measure this incoming (pre-delay) frame's peak and fold
            //    it into the sliding-window maximum.
            const float incomingPeak = framePeak(frame, m_channels);
            const int64_t n = m_frameCounter++;

            // Drop every tail entry this peak dominates: they can never be
            // the window maximum again.
            while (m_head != m_tail &&
                   m_maxVal[static_cast<size_t>((m_tail - 1) & m_mask)] <= incomingPeak) {
                --m_tail;
            }
            m_maxVal[static_cast<size_t>(m_tail & m_mask)] = incomingPeak;
            m_maxIdx[static_cast<size_t>(m_tail & m_mask)] = n;
            ++m_tail;

            // Expire entries that have fallen out of the back of the
            // window. The entry just written has key n and the window
            // starts at n - m_lookaheadFrames, so it is never the one
            // expired and the ring can never empty.
            const int64_t windowStart = n - m_lookaheadFrames;
            // Bounded by m_tail - 1: the entry just written has key n,
            // and n >= windowStart always, so that entry is never expired
            // and the ring can never empty. Stating the bound structurally
            // makes the read below unconditionally safe.
            while (m_head < m_tail - 1 &&
                   m_maxIdx[static_cast<size_t>(m_head & m_mask)] < windowStart) {
                ++m_head;
            }
            const float windowPeak = m_maxVal[static_cast<size_t>(m_head & m_mask)];

            // 2. Brick-wall gain computer, driven by the future-inclusive
            //    window peak rather than only the current sample.
            const float levelDb = fastLinearToDb(windowPeak + 1e-9f);
            const float targetReductionDb = std::min(0.0f, m_ceilingDb - levelDb);
            const float coeff = (targetReductionDb < m_envelopeDb) ? m_attackCoeff : m_releaseCoeff;
            m_envelopeDb = coeff * m_envelopeDb + (1.0f - coeff) * targetReductionDb;
            // Exactly 1.0 when no reduction is called for, so an inert
            // limiter is bit-transparent rather than merely close.
            const float gainLinear = (m_envelopeDb >= -1e-6f) ? 1.0f : fastDbToLinear(m_envelopeDb);

            // 3. Delay line: emit the sample from exactly
            //    m_lookaheadFrames ago (computed against a gain envelope
            //    that already "knew" about this sample's peak in advance),
            //    then store the incoming sample for future emission.
            //
            //    Read and write indices are tracked separately rather than
            //    reading and writing one slot: the buffer is rounded up to
            //    a power of two so it can wrap with a mask, and that
            //    capacity is generally larger than the lookahead, so
            //    "delay == buffer length" would no longer hold.
            const int writeSlot = m_writePos;
            const int readSlot = (m_writePos - m_lookaheadFrames) & m_delayMask;
            m_writePos = (m_writePos + 1) & m_delayMask;

            for (int ch = 0; ch < m_channels; ++ch) {
                const float incoming = frame[ch];
                const float delayed = m_delayBuffer[static_cast<size_t>(readSlot) * m_channels + ch];
                m_delayBuffer[static_cast<size_t>(writeSlot) * m_channels + ch] = incoming;

                // Always the delayed sample -- never the incoming one. See
                // the class comment: emitting the input while the line
                // fills replays those samples again once it has filled.
                float sample = delayed * gainLinear;
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
        m_lookaheadFrames = std::max(0, m_lookaheadFrames);

        // Power-of-two lengths so both rings wrap with a mask instead of
        // an integer division -- `%` against a runtime size compiles to a
        // div, which this loop would otherwise pay once per frame.
        // Capacity must exceed the lookahead so the slot being read has
        // not yet been overwritten by the slot being written.
        const int delayLen = nextPowerOfTwo(m_lookaheadFrames + 1);
        m_delayMask = delayLen - 1;
        m_delayBuffer.assign(static_cast<size_t>(delayLen) * m_channels, 0.0f);

        // The window holds at most lookahead+1 entries.
        const int windowCap = nextPowerOfTwo(m_lookaheadFrames + 1);
        m_mask = windowCap - 1;
        m_maxVal.assign(static_cast<size_t>(windowCap), 0.0f);
        m_maxIdx.assign(static_cast<size_t>(windowCap), INT64_MIN);
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
    // output, short enough that the added output latency stays comfortably
    // inside normal AV-sync tolerance -- which matters, because the audio
    // clock does not compensate for it (see getLookaheadFrames()). Not
    // user-configurable, same rationale as m_attackMs above.
    float m_lookaheadMs = 3.0f;
    int m_lookaheadFrames = 0;

    // Delay line, power-of-two length.
    std::vector<float> m_delayBuffer;
    int m_writePos = 0;
    int m_delayMask = 0;

    // Fixed-capacity monotonic deque over [n - lookahead, n]. m_head and
    // m_tail grow monotonically and are masked on use, so no wrap
    // bookkeeping is needed beyond the mask.
    int64_t m_frameCounter = 0;
    std::vector<float> m_maxVal;
    std::vector<int64_t> m_maxIdx;
    int m_head = 0;
    int m_tail = 0;
    int m_mask = 0;
};

} // namespace naikav::dsp
