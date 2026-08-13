#pragma once

#include "audio/dsp/ParametricEQ.hpp"
#include "audio/dsp/NoiseGate.hpp"
#include "audio/dsp/Compressor.hpp"
#include "audio/dsp/MultibandCompressor.hpp"
#include "audio/dsp/Limiter.hpp"
#include "audio/dsp/Crossover.hpp"

namespace naikav::dsp {

// Orchestrates the full per-frame DSP signal path in order: parametric EQ
// -> noise gate -> compressor -> multiband compressor -> limiter -> LFE
// bass crossover, operating in-place on an interleaved float buffer.
// Deliberately FFmpeg-agnostic (plain channel count/sample rate/channel
// index) so this module has no dependency on libav* -- the caller
// (AudioDecoder) resolves which channel is the LFE channel, if any.
//
// Master-disabled by default: process() is then a true no-op, so wiring
// this into the audio pipeline doesn't change existing playback behavior
// until a caller explicitly opts in.
class DspChain {
public:
    void configure(int channels, double sampleRate, int lfeChannelIndex = -1) {
        m_channels = channels;
        eq.configure(channels, sampleRate);
        noiseGate.configure(channels, sampleRate);
        compressor.configure(channels, sampleRate);
        multiband.configure(channels, sampleRate);
        limiter.configure(channels, sampleRate);
        crossover.configure(channels, sampleRate, lfeChannelIndex);
    }

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    void reset() {
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
        if (!m_enabled || m_channels <= 0 || numFrames <= 0) {
            return;
        }
        eq.process(interleaved, numFrames);
        noiseGate.process(interleaved, numFrames);
        compressor.process(interleaved, numFrames);
        multiband.process(interleaved, numFrames);
        limiter.process(interleaved, numFrames);
        crossover.process(interleaved, numFrames);
    }

    ParametricEQ eq;
    NoiseGate noiseGate;
    Compressor compressor;
    MultibandCompressor multiband;
    Limiter limiter;
    Crossover crossover;

private:
    int m_channels = 0;
    bool m_enabled = false;
};

} // namespace naikav::dsp
