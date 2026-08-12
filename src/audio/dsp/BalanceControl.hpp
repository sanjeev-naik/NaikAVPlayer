#pragma once

#include <algorithm>
#include <cmath>

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

    void setBalance(float balance) { m_balance = std::clamp(balance, -1.0f, 1.0f); }
    float getBalance() const { return m_balance; }

    // In-place processing of an interleaved float buffer. No-op unless
    // exactly 2 channels and the balance is off-center.
    void process(float* interleaved, int numFrames) {
        if (m_channels != 2 || m_balance == 0.0f) return;

        const float leftGain = m_balance <= 0.0f ? 1.0f : 1.0f - m_balance;
        const float rightGain = m_balance >= 0.0f ? 1.0f : 1.0f + m_balance;

        for (int f = 0; f < numFrames; ++f) {
            float* frame = interleaved + static_cast<size_t>(f) * 2;
            frame[0] *= leftGain;
            frame[1] *= rightGain;
        }
    }

private:
    int m_channels = 0;
    float m_balance = 0.0f; // 0 = centered/inert by default
};

} // namespace naikav::dsp
