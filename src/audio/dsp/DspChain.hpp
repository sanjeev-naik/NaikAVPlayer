#pragma once

#include "audio/dsp/ParametricEQ.hpp"
#include "audio/dsp/NoiseGate.hpp"
#include "audio/dsp/Compressor.hpp"
#include "audio/dsp/MultibandCompressor.hpp"
#include "audio/dsp/Limiter.hpp"
#include "audio/dsp/Crossover.hpp"
#include "audio/dsp/DspMath.hpp"

#include <cstring>

namespace naikav::dsp {

// Orchestrates the full per-frame DSP signal path in order: parametric EQ
// -> noise gate -> compressor -> multiband compressor -> LFE bass
// crossover -> limiter, operating in-place on an interleaved float buffer.
// Deliberately FFmpeg-agnostic (plain channel count/sample rate/channel
// index) so this module has no dependency on libav* -- the caller
// (AudioDecoder) resolves which channel is the LFE channel, if any.
//
// The limiter is last, after the crossover, and that ordering matters.
// The crossover is a summing stage: with bass redirect on it folds every
// other channel's low end into the LFE channel. Running the limiter before
// it meant the chain's own configured ceiling did not actually bound the
// chain's output -- a user could set -1 dBFS and the chain would emit
// +17 dBFS, leaving the correction to AudioDecoder's downstream safety
// limiter, which sees a different (possibly already downmixed) buffer and
// so corrects later and more coarsely. Routing before limiting is also the
// conventionally correct order: bass management is a signal-path decision,
// and a limiter should see whatever actually leaves the chain.
//
// Master-disabled by default: process() is then a true no-op, so wiring
// this into the audio pipeline doesn't change existing playback behavior
// until a caller explicitly opts in.
class DspChain {
public:
    void configure(int channels, double sampleRate, int lfeChannelIndex = -1) {
        m_channels = channels;
        m_fade.configure(sampleRate);
        eq.configure(channels, sampleRate);
        noiseGate.configure(channels, sampleRate);
        compressor.configure(channels, sampleRate);
        multiband.configure(channels, sampleRate);
        limiter.configure(channels, sampleRate);
        crossover.configure(channels, sampleRate, lfeChannelIndex);
    }

    // The master bypass crossfades too -- it is the single biggest click
    // source in the whole chain, since it switches every stage at once.
    void setEnabled(bool enabled) { m_fade.setEnabled(enabled); }
    bool isEnabled() const { return m_fade.isEnabled(); }

    // Reserves every internal scratch buffer for a known worst-case block
    // size, so process() performs no allocation on the audio thread.
    void reserveBlock(int maxFrames) {
        multiband.reserveBlock(maxFrames);
        crossover.reserveBlock(maxFrames);
        m_dry.reserve(maxFrames, m_channels);
    }

    // Output latency this chain introduces, in frames. Only the limiter's
    // lookahead contributes; every other stage is zero-latency.
    //
    // Reported for diagnostics (AudioDecoder::getDspLatencyFrames()), not
    // for clock compensation: getAudioClock() deliberately does not
    // subtract it -- see the comment there.
    int getLatencyFrames() const {
        return m_fade.isEnabled() ? limiter.getLookaheadFrames() : 0;
    }

    void reset() {
        m_fade.reset();
        eq.reset();
        noiseGate.reset();
        compressor.reset();
        multiband.reset();
        limiter.reset();
        crossover.reset();
    }

    // In-place processing of an interleaved float buffer (numFrames *
    // channels samples, channels as passed to configure()).
    void process(float* interleaved, int numFrames) {
        if (m_channels <= 0 || numFrames <= 0) {
            return;
        }
        // Audio is flowing through this slot even while bypassed, so the
        // fade counts as live from here. Without this the first enable --
        // the one that actually clicks -- would snap instead of fading.
        m_fade.markPrimed();
        if (m_fade.isInactive()) {
            return;
        }
        const size_t total = static_cast<size_t>(numFrames) * static_cast<size_t>(m_channels);
        const bool fading = m_fade.isFading() && m_dry.fits(total);
        if (fading) {
            std::memcpy(m_dry.data(), interleaved, total * sizeof(float));
        } else if (m_fade.isFading()) {
            m_fade.snap();
        }
        eq.process(interleaved, numFrames);
        noiseGate.process(interleaved, numFrames);
        compressor.process(interleaved, numFrames);
        multiband.process(interleaved, numFrames);
        crossover.process(interleaved, numFrames);
        limiter.process(interleaved, numFrames);
        if (fading) {
            m_fade.blend(interleaved, m_dry.data(), numFrames, m_channels);
        }
    }

    ParametricEQ eq;
    NoiseGate noiseGate;
    Compressor compressor;
    MultibandCompressor multiband;
    Limiter limiter;
    Crossover crossover;

private:
    int m_channels = 0;
    BypassCrossfade m_fade;
    BypassScratch m_dry;
};

} // namespace naikav::dsp
