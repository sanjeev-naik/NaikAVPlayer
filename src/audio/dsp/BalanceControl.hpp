#pragma once

#include <algorithm>
#include <cmath>

#include "audio/dsp/DspMath.hpp"

namespace naikav::dsp {

// Left/right output balance -- attenuates whichever channel the balance is
// pulled away from, the same behavior as a typical OS/mixer "balance"
// slider (distinct from "pan", which spreads a mono source across a
// stereo field rather than adjusting the relative level of two channels
// that already exist). Only meaningful on 2-channel output; a no-op for
// any other channel count.
//
// balance = 0.0 (center, both channels at unity -- the default/inert
// state) .. -1.0 (fully left, right channel silenced) .. +1.0 (fully
// right, left channel silenced).
class BalanceControl {
public:
    void configure(int channels) { m_channels = channels; }

    // Sample rate is only needed for the parameter glide; the balance
    // transform itself is stateless. Separate call, same idiom as
    // StereoWidener::configureFade().
    // Seeded from the balance currently set, not from unity: configure()
    // is a hard reset of the glide, so seeding it at unity would silently
    // discard a setBalance() that arrived first. AudioDecoder happens to
    // call these in the safe order, but a setter that only works when
    // called second is a trap rather than a contract.
    void configureFade(double sampleRate) {
        m_leftGain.configure(sampleRate, leftGainFor(m_balance));
        m_rightGain.configure(sampleRate, rightGainFor(m_balance));
    }

    void setBalance(float balance) {
        // A non-finite value would silently multiply the whole output by
        // NaN. Unlike the IIR stages this one has no state to poison, but
        // rejecting it here keeps every DSP setter consistent about
        // refusing input it cannot use.
        if (!std::isfinite(balance)) return;
        m_balance = std::clamp(balance, -1.0f, 1.0f);
        // Glide rather than step. This is a slider the user drags, so a
        // bare assignment emits a gain discontinuity on every UI frame of
        // the drag, not just one at the end -- measured at 38x the
        // waveform's own per-sample slope for a single 0.8 change.
        m_leftGain.setTarget(leftGainFor(m_balance));
        m_rightGain.setTarget(rightGainFor(m_balance));
    }
    float getBalance() const { return m_balance; }

    // Snaps the glide to the current balance, so a stage reconfigured
    // while stopped applies it from the first sample.
    void reset() {
        m_leftGain.reset();
        m_rightGain.reset();
    }

    // In-place processing of an interleaved float buffer. No-op unless
    // exactly 2 channels and the balance is off-center.
    void process(float* interleaved, int numFrames) {
        if (m_channels != 2 || numFrames <= 0) return;
        m_leftGain.markPrimed();
        m_rightGain.markPrimed();
        // Centered *and* settled: only then is there provably nothing to
        // do. Returning on the balance value alone would abandon a glide
        // still on its way back to unity, which is the step this exists
        // to remove.
        if (m_balance == 0.0f && m_leftGain.isSteady() && m_rightGain.isSteady()) {
            return;
        }

        for (int f = 0; f < numFrames; ++f) {
            float* frame = interleaved + static_cast<size_t>(f) * 2;
            frame[0] *= m_leftGain.next();
            frame[1] *= m_rightGain.next();
        }
    }

private:
    // Pull the balance -> per-channel gain mapping out so configureFade()
    // and setBalance() cannot drift apart.
    static float leftGainFor(float balance) {
        return balance <= 0.0f ? 1.0f : 1.0f - balance;
    }
    static float rightGainFor(float balance) {
        return balance >= 0.0f ? 1.0f : 1.0f + balance;
    }

    int m_channels = 0;
    float m_balance = 0.0f; // 0 = centered/inert by default
    SmoothedParam m_leftGain, m_rightGain;
};

} // namespace naikav::dsp
