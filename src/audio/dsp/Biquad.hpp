#pragma once

#include <cmath>

#include "audio/dsp/DspMath.hpp"

namespace naikav::dsp {

// Direct Form I biquad IIR filter using the Robert Bristow-Johnson "Audio
// EQ Cookbook" coefficient formulas. One instance holds the running state
// (x[n-1], x[n-2], y[n-1], y[n-2]) for a single channel -- multichannel
// callers need one instance per channel, sharing the same coefficients via
// copyCoefficientsFrom(), so each channel's history stays independent.
class Biquad {
public:
    void reset() {
        m_x1 = m_x2 = m_y1 = m_y2 = 0.0f;
    }

    // Peaking/bell EQ band: boosts or cuts a band around freqHz by gainDb,
    // with bandwidth controlled by q (higher q = narrower band).
    void setPeaking(double freqHz, double q, double gainDb, double sampleRate) {
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

    void copyCoefficientsFrom(const Biquad& other) {
        m_b0 = other.m_b0;
        m_b1 = other.m_b1;
        m_b2 = other.m_b2;
        m_a1 = other.m_a1;
        m_a2 = other.m_a2;
    }

    inline float process(float x) {
        const float y = m_b0 * x + m_b1 * m_x1 + m_b2 * m_x2 - m_a1 * m_y1 - m_a2 * m_y2;
        m_x2 = m_x1;
        m_x1 = x;
        m_y2 = m_y1;
        m_y1 = y;
        return y;
    }

private:
    void normalizeAndStore(double b0, double b1, double b2, double a0, double a1, double a2) {
        m_b0 = static_cast<float>(b0 / a0);
        m_b1 = static_cast<float>(b1 / a0);
        m_b2 = static_cast<float>(b2 / a0);
        m_a1 = static_cast<float>(a1 / a0);
        m_a2 = static_cast<float>(a2 / a0);
    }

    float m_b0 = 1.0f, m_b1 = 0.0f, m_b2 = 0.0f;
    float m_a1 = 0.0f, m_a2 = 0.0f;
    float m_x1 = 0.0f, m_x2 = 0.0f;
    float m_y1 = 0.0f, m_y2 = 0.0f;
};

} // namespace naikav::dsp
