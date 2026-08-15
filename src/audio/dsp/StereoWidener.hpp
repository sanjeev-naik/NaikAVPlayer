#pragma once

#include <algorithm>

namespace naikav::dsp {

// Mid-side stereo widener: splits the signal into a mid component
// (L+R)/2 and a side component (L-R)/2, scales the side component by a
// width factor, then recombines (L' = mid + side, R' = mid - side).
// width=1.0 is the identity (mid +/- the untouched side reconstructs the
// original L/R exactly); width>1 exaggerates the difference between the
// channels for a wider/more spacious image; width=0 collapses to mono in
// both channels.
//
// Deliberately the last stage in the audio pipeline (see AudioDecoder),
// not part of DspChain: it needs to run on whatever ends up as the final
// 2-channel buffer, which may be a genuinely stereo source, a
// FORCE_STEREO/AUTO-fallback downmix, or SpatialDownmixer's virtual
// surround output -- all of which happen at different points relative to
// DspChain's fixed-channel-count EQ/compressor/limiter/crossover stages.
// Only meaningful for exactly 2-channel output; process() is a no-op for
// any other channel count (native multichannel passthrough has no single
// "side" component to widen).
class StereoWidener {
public:
    void configure(int channels) { m_channels = channels; }

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    // 1.0 = unity/no change, 0.0 = mono, >1.0 = wider than the source.
    void setWidth(float width) { m_width = std::max(0.0f, width); }
    float getWidth() const { return m_width; }

    // Intentionally empty: this stage is stateless (mid-side width is a
    // per-sample computation with no history to clear). Kept as a non-static
    // member so every DSP stage exposes the same reset() interface for
    // callers that reset the chain uniformly.
    // cppcheck-suppress functionStatic
    void reset() {}

    // In-place processing of an interleaved float buffer. No-op unless
    // enabled and configured for exactly 2 channels.
    void process(float* interleaved, int numFrames) {
        if (!m_enabled || m_channels != 2) {
            return;
        }
        for (int f = 0; f < numFrames; ++f) {
            float* frame = interleaved + static_cast<size_t>(f) * 2;
            const float mid = 0.5f * (frame[0] + frame[1]);
            const float side = 0.5f * (frame[0] - frame[1]) * m_width;
            frame[0] = mid + side;
            frame[1] = mid - side;
        }
    }

private:
    int m_channels = 0;
    bool m_enabled = false;
    float m_width = 1.0f;
};

} // namespace naikav::dsp
