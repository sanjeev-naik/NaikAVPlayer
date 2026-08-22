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
// The limiter also sits *outside* the master bypass crossfade, which
// matters just as much as where it sits in the signal order. It is the
// only stage here with latency -- its lookahead delay line, 144 frames at
// 48kHz -- so with it inside the fade the wet path carried the signal
// from n - 144 while the dry snapshot carried it from n. Blending two
// copies of the same audio 3ms apart is a comb filter, and worse, 144
// frames after every enable or disable the delay line finished emitting
// whatever it had been holding and stepped to the live signal: a
// discontinuity measured at up to 57x the waveform's own per-sample
// slope, which is exactly the click the crossfade exists to remove.
//
// Going fully bypassed also used to return before the limiter ran at all,
// freezing its delay line mid-stream. Re-enabling then replayed those 144
// frames -- audio from the previous enabled period, which after a track
// change is audio from a different file.
//
// Running the limiter unconditionally fixes both: the delay line stays
// continuously fed, the chain's latency no longer steps by 3ms on every
// toggle (see getLatencyFrames()), and the fade is left wrapping only
// zero-latency stages, which are time-aligned with the dry snapshot by
// construction.
//
// Master-disabled by default. A bypassed chain is then gain-transparent
// rather than literally a no-op: the limiter still runs, but AudioDecoder
// holds its ceiling at 0 dBFS whenever the chain is disabled, which no
// normal PCM content exceeds -- so what it costs is its delay line and
// nothing else.
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
    // Constant regardless of the master bypass, because the limiter runs
    // whether or not the chain is enabled -- see the class comment. That
    // constancy is the point: a figure that stepped by 3ms every time the
    // user toggled DSP is precisely what made the bypass crossfade click.
    //
    // Reported for diagnostics (AudioDecoder::getDspLatencyFrames()), not
    // for clock compensation: getAudioClock() deliberately does not
    // subtract it -- see the comment there.
    int getLatencyFrames() const {
        return limiter.getLookaheadFrames();
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
    //
    // Two phases: the stages the master bypass crossfade wraps, then the
    // limiter, which always runs. See the class comment for why the
    // limiter cannot be inside the fade.
    void process(float* interleaved, int numFrames) {
        if (m_channels <= 0 || numFrames <= 0) {
            return;
        }
        processBypassable(interleaved, numFrames);
        limiter.process(interleaved, numFrames);
    }

    ParametricEQ eq;
    NoiseGate noiseGate;
    Compressor compressor;
    MultibandCompressor multiband;
    Limiter limiter;
    Crossover crossover;

private:
    // Everything the master bypass crossfade covers: all zero-latency, so
    // the wet result is sample-aligned with the dry snapshot taken here
    // and the two can be blended without a comb or a seam.
    void processBypassable(float* interleaved, int numFrames) {
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
        if (fading) {
            m_fade.blend(interleaved, m_dry.data(), numFrames, m_channels);
        }
    }

    int m_channels = 0;
    BypassCrossfade m_fade;
    BypassScratch m_dry;
};

} // namespace naikav::dsp
