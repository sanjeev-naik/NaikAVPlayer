#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
#include "Biquad.hpp"

namespace naikav::dsp {

// Downmixes a discrete multichannel buffer (one of the layouts
// AudioDecoder can preserve: 2.1 / 5.1(side) / 5.1(back) / 7.1) to stereo
// using per-channel amplitude panning plus a lightweight "virtual
// surround" cue for surround/back channels, instead of a flat linear sum.
// That flat sum is what swresample's own downmix matrix produces -- and
// what this app's AUTO-unsupported-layout/FORCE_STEREO paths already give
// you -- and it carries no positional information at all: every channel
// just blends into the middle.
//
// Approach (deliberately simple DSP -- no HRTF convolution or measured
// impulse responses, matching the rest of this hand-rolled DSP chain):
//   1. Front left/right pass straight to their own ear, center splits
//      evenly, LFE sums to both ears at a fixed low gain (bass has no
//      useful directional information at this level of approximation).
//   2. Surround/back channels are NOT panned directly. Instead each one
//      is sent through its own short delay line and a gentle lowpass
//      ("diffuse" path), then split unevenly between the ears (louder
//      toward its own side). The delay + high-frequency rolloff
//      approximates the head-shadow/distance cue that makes a source
//      read as "behind you" rather than merely "off to the side" --
//      amplitude panning alone can't distinguish those two.
//
// FFmpeg-agnostic by design (plain enum, not an AVChannelLayout), like
// DspChain -- the caller (AudioDecoder) maps the real source layout to
// SourceLayout.
class SpatialDownmixer {
public:
    enum class SourceLayout {
        TWOPOINT1,       // FL, FR, LFE
        FIVEPOINT1_SIDE, // FL, FR, FC, LFE, SL, SR
        FIVEPOINT1_BACK, // FL, FR, FC, LFE, BL, BR
        SEVENPOINT1      // FL, FR, FC, LFE, BL, BR, SL, SR
    };

    void configure(SourceLayout layout, double sampleRate) {
        m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        m_routes = buildRoutes(layout);

        const size_t n = m_routes.size();
        m_ring.assign(n, {});
        m_writePos.assign(n, 0);
        m_lowpass.assign(n, Biquad{});
        for (size_t i = 0; i < n; ++i) {
            const auto& route = m_routes[i];
            int delaySamples = std::max(1, static_cast<int>(std::lround(route.diffuseDelayMs / 1000.0 * m_sampleRate)));
            m_ring[i].assign(static_cast<size_t>(delaySamples), 0.0f);
            if (route.diffuseLowpassHz > 0.0) {
                m_lowpass[i].setLowpass(route.diffuseLowpassHz, 0.70710678118, m_sampleRate);
            }
        }
        reset();
    }

    int numSourceChannels() const { return static_cast<int>(m_routes.size()); }

    void reset() {
        for (auto& ring : m_ring) {
            std::fill(ring.begin(), ring.end(), 0.0f);
        }
        std::fill(m_writePos.begin(), m_writePos.end(), size_t{0});
        for (auto& lp : m_lowpass) {
            lp.reset();
        }
    }

    // Reads numFrames * numSourceChannels() interleaved float samples from
    // `in` and writes numFrames * 2 interleaved float samples to `out`.
    // `in` and `out` must point to distinct, non-overlapping buffers.
    void process(const float* in, int numFrames, float* out) {
        const size_t n = m_routes.size();
        if (n == 0) {
            return;
        }
        for (int f = 0; f < numFrames; ++f) {
            const float* frame = in + static_cast<size_t>(f) * n;
            float outL = 0.0f;
            float outR = 0.0f;

            for (size_t ch = 0; ch < n; ++ch) {
                const float x = frame[ch];
                const ChannelRoute& route = m_routes[ch];

                outL += x * route.directGainL;
                outR += x * route.directGainR;

                if (route.diffuseGainL != 0.0f || route.diffuseGainR != 0.0f) {
                    std::vector<float>& ring = m_ring[ch];
                    const float delayed = ring[m_writePos[ch]];
                    ring[m_writePos[ch]] = x;
                    m_writePos[ch] = (m_writePos[ch] + 1) % ring.size();

                    float diffuse = delayed;
                    if (route.diffuseLowpassHz > 0.0) {
                        diffuse = m_lowpass[ch].process(diffuse);
                    }
                    outL += diffuse * route.diffuseGainL;
                    outR += diffuse * route.diffuseGainR;
                }
            }

            out[static_cast<size_t>(f) * 2 + 0] = outL;
            out[static_cast<size_t>(f) * 2 + 1] = outR;
        }
    }

private:
    struct ChannelRoute {
        // Straight-through contribution (no delay/filter): used for front
        // L/R (full pan to their own ear), center (split evenly), and LFE
        // (fixed low gain to both ears).
        float directGainL = 0.0f;
        float directGainR = 0.0f;
        // Delayed + lowpass-filtered "diffuse" contribution: used for
        // surround/back channels, split unevenly toward their own side.
        float diffuseGainL = 0.0f;
        float diffuseGainR = 0.0f;
        double diffuseDelayMs = 0.0;
        double diffuseLowpassHz = 0.0; // 0 = no filtering
    };

    static ChannelRoute directRoute(float gainL, float gainR) {
        ChannelRoute r;
        r.directGainL = gainL;
        r.directGainR = gainR;
        return r;
    }

    static ChannelRoute surroundRoute(float nearGain, float farGain, bool nearIsLeft,
                                       double delayMs, double lowpassHz) {
        ChannelRoute r;
        r.diffuseGainL = nearIsLeft ? nearGain : farGain;
        r.diffuseGainR = nearIsLeft ? farGain : nearGain;
        r.diffuseDelayMs = delayMs;
        r.diffuseLowpassHz = lowpassHz;
        return r;
    }

    static std::vector<ChannelRoute> buildRoutes(SourceLayout layout) {
        constexpr float kCenterGain = 0.70710678f; // -3dB, equal power split
        constexpr float kLfeGain = 0.5f;           // non-directional, modest so it doesn't dominate the mix

        // Side surrounds (~110 degrees): closer to the ears than back
        // surrounds, so a shorter delay and a brighter lowpass cutoff.
        constexpr float kSideNearGain = 0.85f, kSideFarGain = 0.40f;
        constexpr double kSideDelayMs = 8.0, kSideLowpassHz = 6500.0;

        // Back surrounds (~150 degrees): further/more occluded, so a
        // longer delay and duller lowpass cutoff for a stronger "behind
        // you" cue.
        constexpr float kBackNearGain = 0.80f, kBackFarGain = 0.35f;
        constexpr double kBackDelayMs = 14.0, kBackLowpassHz = 5000.0;

        const ChannelRoute fl = directRoute(1.0f, 0.0f);
        const ChannelRoute fr = directRoute(0.0f, 1.0f);
        const ChannelRoute fc = directRoute(kCenterGain, kCenterGain);
        const ChannelRoute lfe = directRoute(kLfeGain, kLfeGain);
        const ChannelRoute sl = surroundRoute(kSideNearGain, kSideFarGain, true, kSideDelayMs, kSideLowpassHz);
        const ChannelRoute sr = surroundRoute(kSideNearGain, kSideFarGain, false, kSideDelayMs, kSideLowpassHz);
        const ChannelRoute bl = surroundRoute(kBackNearGain, kBackFarGain, true, kBackDelayMs, kBackLowpassHz);
        const ChannelRoute br = surroundRoute(kBackNearGain, kBackFarGain, false, kBackDelayMs, kBackLowpassHz);

        switch (layout) {
            case SourceLayout::TWOPOINT1:
                return { fl, fr, lfe };
            case SourceLayout::FIVEPOINT1_SIDE:
                return { fl, fr, fc, lfe, sl, sr };
            case SourceLayout::FIVEPOINT1_BACK:
                return { fl, fr, fc, lfe, bl, br };
            case SourceLayout::SEVENPOINT1:
                return { fl, fr, fc, lfe, bl, br, sl, sr };
        }
        return {};
    }

    std::vector<ChannelRoute> m_routes;
    std::vector<std::vector<float>> m_ring;
    std::vector<size_t> m_writePos;
    std::vector<Biquad> m_lowpass;
    double m_sampleRate = 48000.0;
};

} // namespace naikav::dsp
