#pragma once

#include <cmath>
#include <algorithm>

#include "audio/dsp/DspMath.hpp"

namespace naikav::dsp {

// Direct Form I biquad IIR filter using the Robert Bristow-Johnson "Audio
// EQ Cookbook" coefficient formulas. One instance holds the running state
// (x[n-1], x[n-2], y[n-1], y[n-2]) for a single channel -- multichannel
// callers need one instance per channel, sharing the same coefficients via
// copyCoefficientsFrom(), so each channel's history stays independent.
//
// Two properties worth knowing about this implementation:
//
// State and accumulation are double, not float. A biquad's poles sit very
// close to the unit circle at low centre frequencies (a 60Hz band at 48kHz
// has a1 ~= -1.99992), where single-precision accumulation adds audible
// roundoff noise near DC. Measured on the previous float version: a
// nominally flat 5-band EQ, which is algebraically an exact identity,
// still added a -79 dBFS error floor -- above the 16-bit dither floor, so
// present in the delivered output. Double state removes it, and on x86-64
// costs nothing measurable since the values live in SSE registers either
// way.
//
// Coefficient changes ramp rather than step. Swapping coefficients
// instantaneously while the filter keeps its old state produces a
// transient discontinuity -- the click heard when dragging an EQ slider.
// A set*() call on a filter that has already processed audio moves the
// coefficients toward their new values over kRampSamples instead; a
// set*() call on a fresh filter snaps, so a filter is fully configured
// from its very first sample.
class Biquad {
public:
    void reset() {
        m_x1 = m_x2 = m_y1 = m_y2 = 0.0;
        snapToTarget();
    }

    // Peaking/bell EQ band: boosts or cuts a band around freqHz by gainDb,
    // with bandwidth controlled by q (higher q = narrower band).
    void setPeaking(double freqHz, double q, double gainDb, double sampleRate) {
        if (!validParams(freqHz, q, sampleRate)) return;
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * freqHz / sampleRate;
        const double cosW0 = std::cos(w0);
        const double sinW0 = std::sin(w0);
        const double alpha = sinW0 / (2.0 * q);

        const double b0 = 1.0 + alpha * A;
        const double b1 = -2.0 * cosW0;
        const double b2 = 1.0 - alpha * A;
        const double a0 = 1.0 + alpha / A;
        const double a1 = -2.0 * cosW0;
        const double a2 = 1.0 - alpha / A;

        normalizeAndStore(b0, b1, b2, a0, a1, a2);
    }

    // Standard (non-shelving) 2nd-order lowpass/highpass. q = 1/sqrt(2)
    // gives a Butterworth (maximally flat) response, used by Crossover to
    // build Linkwitz-Riley slopes via cascaded stages.
    void setLowpass(double freqHz, double q, double sampleRate) {
        if (!validParams(freqHz, q, sampleRate)) return;
        const double w0 = 2.0 * kPi * freqHz / sampleRate;
        const double cosW0 = std::cos(w0);
        const double sinW0 = std::sin(w0);
        const double alpha = sinW0 / (2.0 * q);

        const double b1 = 1.0 - cosW0;
        const double b0 = b1 / 2.0;
        const double b2 = b0;
        const double a0 = 1.0 + alpha;
        const double a1 = -2.0 * cosW0;
        const double a2 = 1.0 - alpha;

        normalizeAndStore(b0, b1, b2, a0, a1, a2);
    }

    void setHighpass(double freqHz, double q, double sampleRate) {
        if (!validParams(freqHz, q, sampleRate)) return;
        const double w0 = 2.0 * kPi * freqHz / sampleRate;
        const double cosW0 = std::cos(w0);
        const double sinW0 = std::sin(w0);
        const double alpha = sinW0 / (2.0 * q);

        const double b0 = (1.0 + cosW0) / 2.0;
        const double b1 = -(1.0 + cosW0);
        const double b2 = b0;
        const double a0 = 1.0 + alpha;
        const double a1 = -2.0 * cosW0;
        const double a2 = 1.0 - alpha;

        normalizeAndStore(b0, b1, b2, a0, a1, a2);
    }

    // 2nd-order allpass: unity magnitude at every frequency, but the same
    // phase rotation a matched Linkwitz-Riley pair imposes. Used by
    // MultibandCompressor to phase-align a band that bypasses a crossover
    // with the bands that went through it -- without it, a cascaded 3-way
    // split does not sum flat.
    void setAllpass(double freqHz, double q, double sampleRate) {
        if (!validParams(freqHz, q, sampleRate)) return;
        const double w0 = 2.0 * kPi * freqHz / sampleRate;
        const double cosW0 = std::cos(w0);
        const double sinW0 = std::sin(w0);
        const double alpha = sinW0 / (2.0 * q);

        const double b0 = 1.0 - alpha;
        const double b1 = -2.0 * cosW0;
        const double b2 = 1.0 + alpha;
        const double a0 = 1.0 + alpha;
        const double a1 = -2.0 * cosW0;
        const double a2 = 1.0 - alpha;

        normalizeAndStore(b0, b1, b2, a0, a1, a2);
    }

    void copyCoefficientsFrom(const Biquad& other) {
        m_tb0 = other.m_tb0;
        m_tb1 = other.m_tb1;
        m_tb2 = other.m_tb2;
        m_ta1 = other.m_ta1;
        m_ta2 = other.m_ta2;
        beginRampOrSnap();
    }

    inline float process(float x) {
        if (m_rampRemaining > 0) {
            stepRamp();
        }
        const double xd = static_cast<double>(x);
        double y = m_b0 * xd + m_b1 * m_x1 + m_b2 * m_x2
                 - m_a1 * m_y1 - m_a2 * m_y2;
        // Explicit denormal flush so the IIR tail stays cheap even on
        // targets without MXCSR (see ScopedDenormalGuard). On x86 the
        // guard has already zeroed these, making this a predictable
        // never-taken branch. Applied before the value is both stored and
        // returned, so a denormal cannot leak into the next stage either.
        if (std::fabs(y) < 1e-300) y = 0.0;
        m_x2 = m_x1;
        m_x1 = xd;
        m_y2 = m_y1;
        m_y1 = y;
        return static_cast<float>(y);
    }

    // Clears the running state (x/y history) without touching the
    // coefficients or the primed flag.
    //
    // For a filter being taken out of the signal path: its history is
    // about to become arbitrarily old, and at the near-identity
    // coefficients such a filter is parked at, stale y[n-1]/y[n-2] in the
    // feedback terms would burst on the first sample it is switched back
    // in. reset() would do this too, but it also unprimes -- which would
    // make the next set*() snap rather than ramp, reintroducing exactly
    // the discontinuity the parking is there to avoid.
    void clearState() { m_x1 = m_x2 = m_y1 = m_y2 = 0.0; }

    // Forces any in-flight coefficient ramp to complete immediately.
    // Callers reconfiguring a stopped filter (or reset()) want the new
    // response from the very first sample, not a ramp in from the old one.
    void snapToTarget() {
        m_b0 = m_tb0; m_b1 = m_tb1; m_b2 = m_tb2;
        m_a1 = m_ta1; m_a2 = m_ta2;
        m_rampRemaining = 0;
        m_primed = false;
    }

private:
    static constexpr int kRampSamples = 480; // ~10 ms at 48 kHz

    // Rejects a parameter set that would place the poles outside the unit
    // circle or produce non-finite coefficients. A biquad is a feedback
    // structure, so a single NaN reaching m_y1/m_y2 propagates forever --
    // the filter never recovers without an explicit reset(). Callers used
    // to be able to reach this from an unvalidated settings file (see
    // AudioDspSettings::sanitize(), which is the first line of defence);
    // this is the last one, and it keeps the previous coefficients rather
    // than adopting bad ones.
    static bool validParams(double freqHz, double q, double sampleRate) {
        if (!std::isfinite(freqHz) || !std::isfinite(q) || !std::isfinite(sampleRate)) {
            return false;
        }
        if (sampleRate <= 0.0 || q <= 0.0) return false;
        // Strictly below Nyquist: at or above it, cos(w0)/sin(w0) no
        // longer describe the intended response and alpha can go
        // negative, driving a0 toward zero.
        return freqHz > 0.0 && freqHz < sampleRate * 0.5;
    }

    void normalizeAndStore(double b0, double b1, double b2, double a0, double a1, double a2) {
        if (!std::isfinite(a0) || std::fabs(a0) < 1e-12) return;
        const double nb0 = b0 / a0, nb1 = b1 / a0, nb2 = b2 / a0;
        const double na1 = a1 / a0, na2 = a2 / a0;
        if (!std::isfinite(nb0) || !std::isfinite(nb1) || !std::isfinite(nb2) ||
            !std::isfinite(na1) || !std::isfinite(na2)) {
            return;
        }
        m_tb0 = nb0; m_tb1 = nb1; m_tb2 = nb2;
        m_ta1 = na1; m_ta2 = na2;
        beginRampOrSnap();
    }

    void beginRampOrSnap() {
        if (!m_primed) {
            // Never processed a sample with these coefficients yet, so
            // there is no old response to glide away from.
            m_b0 = m_tb0; m_b1 = m_tb1; m_b2 = m_tb2;
            m_a1 = m_ta1; m_a2 = m_ta2;
            m_rampRemaining = 0;
            return;
        }
        m_rampRemaining = kRampSamples;
        const double inv = 1.0 / static_cast<double>(kRampSamples);
        m_db0 = (m_tb0 - m_b0) * inv;
        m_db1 = (m_tb1 - m_b1) * inv;
        m_db2 = (m_tb2 - m_b2) * inv;
        m_da1 = (m_ta1 - m_a1) * inv;
        m_da2 = (m_ta2 - m_a2) * inv;
    }

    inline void stepRamp() {
        if (--m_rampRemaining == 0) {
            m_b0 = m_tb0; m_b1 = m_tb1; m_b2 = m_tb2;
            m_a1 = m_ta1; m_a2 = m_ta2;
            return;
        }
        m_b0 += m_db0; m_b1 += m_db1; m_b2 += m_db2;
        m_a1 += m_da1; m_a2 += m_da2;
    }

    // Current (possibly mid-ramp) coefficients.
    double m_b0 = 1.0, m_b1 = 0.0, m_b2 = 0.0;
    double m_a1 = 0.0, m_a2 = 0.0;
    // Target coefficients the ramp is heading toward.
    double m_tb0 = 1.0, m_tb1 = 0.0, m_tb2 = 0.0;
    double m_ta1 = 0.0, m_ta2 = 0.0;
    // Per-sample ramp increments.
    double m_db0 = 0.0, m_db1 = 0.0, m_db2 = 0.0;
    double m_da1 = 0.0, m_da2 = 0.0;
    int m_rampRemaining = 0;

    double m_x1 = 0.0, m_x2 = 0.0, m_y1 = 0.0, m_y2 = 0.0;

    // False until the filter has actually run audio through these
    // coefficients, which is what distinguishes "configure me" from
    // "change me while playing".
    bool m_primed = false;

public:
    // Marks the filter as live, so subsequent coefficient changes ramp
    // instead of stepping. Owning stages call this once per process()
    // block rather than per sample.
    void markPrimed() { m_primed = true; }
    bool isRamping() const { return m_rampRemaining > 0; }
};

} // namespace naikav::dsp
