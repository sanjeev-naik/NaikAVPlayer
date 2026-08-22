#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio/dsp/DspMath.hpp"

namespace naikav::dsp {

// Mid-side stereo widener: splits the signal into a mid component
// (L+R)/2 and a side component (L-R)/2, scales the side component by a
// width factor, then recombines (L' = mid + side, R' = mid - side).
// width=1.0 is the identity (mid +/- the untouched side reconstructs the
// original L/R exactly); width>1 exaggerates the difference between the
// channels for a wider/more spacious image; width=0 collapses to mono in
// both channels.
//
// Only the side component is scaled; the output is not normalized.
//
// An earlier version divided by sqrt((1 + width^2) / 2) to hold total
// power constant. That factor is correct only for content with balanced
// mid/side energy, and it was applied unconditionally -- so near-mono
// content, which has almost no side energy for the width to act on, got
// the attenuation without any of the widening: measured at -2.1 dB for
// width 1.5, -4.0 dB for 2.0 and -9.3 dB for 4.0 on identical L/R. At
// width 0 it was worse than useless, scaling the mono sum up by 3 dB
// instead of collapsing to the mid signal exactly.
//
// Unnormalized, the transform is exactly what it says: width 1.0 is
// bit-exact identity, width 0 collapses to mid = (L+R)/2 exactly, and
// width > 1 raises the level only in proportion to side energy that
// actually exists. Where that pushes decorrelated content past full
// scale, AudioDecoder's m_finalSafetyLimiter is downstream and exists
// for exactly this -- see its comment, which names this stage.
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

    // Sample rate is only needed for the bypass crossfade's length; the
    // width transform itself is stateless.
    void configureFade(double sampleRate) {
        m_fade.configure(sampleRate);
        m_glidingWidth.configure(sampleRate, m_width);
    }

    // Sizes the crossfade's dry-copy scratch so process() never allocates.
    void reserveBlock(int maxFrames) { m_dry.reserve(maxFrames, 2); }

    // Crossfades in/out rather than switching on the next sample; see
    // BypassCrossfade.
    void setEnabled(bool enabled) { m_fade.setEnabled(enabled); }
    bool isEnabled() const { return m_fade.isEnabled(); }

    // 1.0 = unity/no change, 0.0 = mono, >1.0 = wider than the source.
    void setWidth(float width) {
        if (!std::isfinite(width)) return;
        m_width = std::clamp(width, 0.0f, 4.0f);
        // Glided, not stepped -- width is a dragged slider, and the side
        // component is scaled by it directly, so a bare assignment steps
        // the waveform once per UI frame of the drag. Measured at 16x the
        // signal's own per-sample slope across a 1.0 -> 3.0 sweep.
        m_glidingWidth.setTarget(m_width);
    }
    float getWidth() const { return m_width; }

    // The width transform itself is stateless (a per-sample computation
    // with no history), so all there is to clear is the bypass crossfade
    // and the width glide -- both snapped to their targets, so a stage
    // reset while stopped resumes at full effect from the first sample.
    void reset() {
        m_fade.reset();
        m_glidingWidth.reset();
    }

    // In-place processing of an interleaved float buffer. No-op unless
    // enabled and configured for exactly 2 channels. Also skipped at
    // unity width, where the stage is an exact identity.
    void process(float* interleaved, int numFrames) {
        if (m_channels != 2 || numFrames <= 0) {
            return;
        }
        m_fade.markPrimed(); // live even while bypassed -- see DspChain
        m_glidingWidth.markPrimed();
        // Unity *and* settled: a width glide still travelling back toward
        // 1.0 has work left to do, and abandoning it here would step.
        if (m_fade.isInactive() || (m_width == 1.0f && m_glidingWidth.isSteady())) {
            return;
        }
        const size_t total = static_cast<size_t>(numFrames) * 2u;
        const bool fading = m_fade.isFading() && m_dry.fits(total);
        if (fading) {
            std::memcpy(m_dry.data(), interleaved, total * sizeof(float));
        } else if (m_fade.isFading()) {
            m_fade.snap();
        }
        for (int f = 0; f < numFrames; ++f) {
            const float w = m_glidingWidth.next();
            float* frame = interleaved + static_cast<size_t>(f) * 2;
            const float mid = 0.5f * (frame[0] + frame[1]);
            const float side = 0.5f * (frame[0] - frame[1]) * w;
            frame[0] = mid + side;
            frame[1] = mid - side;
        }
        if (fading) {
            m_fade.blend(interleaved, m_dry.data(), numFrames, 2);
        }
    }

private:
    int m_channels = 0;
    BypassCrossfade m_fade;
    BypassScratch m_dry;
    float m_width = 1.0f;
    SmoothedParam m_glidingWidth;
};

} // namespace naikav::dsp
