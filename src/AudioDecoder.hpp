#pragma once

#include <vector>
#include <atomic>
#include <mutex>
#include <string>
#include "ThreadSafeQueue.hpp"
#include "audio/dsp/DspChain.hpp"
#include "audio/dsp/LoudnessNormalizer.hpp"
#include "audio/dsp/AudioDspSettings.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>
#include <libavutil/common.h>
#include <libavutil/opt.h>
}

#include <SDL3/SDL.h>

// User-selectable override for output channel resolution, applied at
// init() time (see AudioDecoder::setChannelOption() -- must be called
// before init(), like attachSeekGeneration()). AUTO is the behavior
// described at getOutputChannelLayoutName(): preserve the source's
// surround layout when it's one of the directly-supported ones, else
// downmix to stereo. FORCE_STEREO always downmixes regardless of source,
// for cases where the automatic detection isn't what the user wants --
// e.g. a 5.1 source but genuinely stereo/headphone hardware (a successful
// device open does NOT prove multichannel hardware is connected; see
// getDeviceNativeChannels()).
enum class AudioChannelOption {
    AUTO = 0,
    FORCE_STEREO,
    COUNT
};

class AudioDecoder {
private:
    AVCodecParameters* m_codecParams;
    AVCodecContext* m_codecCtx;
    SwrContext* m_swrCtx;
    
    ThreadSafeQueue<AVPacket*>& m_queue;
    AVRational m_timeBase;
    int64_t m_startTime;
    
    SDL_AudioStream* m_audioStream;
    
    // Audio Clock synchronization variables
    double m_clock; // Current audio clock (in seconds)
    std::mutex m_clockMutex;
    
    // Decoding temporary buffer. Holds interleaved AV_SAMPLE_FMT_FLT samples
    // (raw bytes; m_outSampleFmt controls what swr actually writes here) --
    // NOT the final device format. sdlAudioStreamCallback converts from this
    // float buffer to the S16 device format sample-by-sample, with TPDF
    // dither applied only at that final truncation. m_audioBufferIndex/Size
    // are byte offsets into THIS buffer's format (4 bytes/sample), not the
    // S16 output format (2 bytes/sample) -- see getAudioClock() and
    // sdlAudioStreamCallback() for where the two are reconciled.
    std::vector<uint8_t> m_audioBuffer;
    std::atomic<size_t> m_audioBufferIndex;
    size_t m_audioBufferSize;
    std::mutex m_audioMutex;

    // State for the TPDF dither applied when truncating float samples down
    // to the S16 device format. Per-instance (not global/static) so the
    // many AudioDecoder instances created across the test suite don't share
    // RNG state, and so this stays trivially thread-safe without needing a
    // shared lock (only ever touched from the SDL audio callback thread).
    uint32_t m_ditherState;
    
    std::atomic<bool> m_flushRequested;
    std::atomic<bool> m_paused;
    bool m_startTimeSaved;
    std::atomic<float> m_volume;
    
    AVFrame* m_decodedFrame;
    std::atomic<uint64_t>* m_decodeTimeTracker;
 
    // Output specs (SDL Audio configuration)
    int m_outSampleRate;
    AVSampleFormat m_outSampleFmt;
    int m_outChannels;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout m_outChannelLayout;
#else
    uint64_t m_outChannelLayout;
#endif

    // The real default playback device's native channel count, queried via
    // SDL_GetAudioDeviceFormat() in init() (0 if the query failed/unknown).
    // See getDeviceNativeChannels().
    int m_deviceNativeChannels = 0;

    // Set via setChannelOption() before init(); see AudioChannelOption.
    AudioChannelOption m_channelOption = AudioChannelOption::AUTO;

    // Parametric EQ -> compressor -> limiter -> LFE crossover, applied to
    // the freshly-decoded float buffer in decodeAndResample(), before the
    // dithered truncation to the device's S16 format. Disabled by default
    // (see DspChain::configure()), so wiring this in doesn't change
    // existing playback behavior on its own.
    naikav::dsp::DspChain m_dsp;

    // EBU R128 loudness normalization, applied after the DSP chain above
    // (matching the plan's "DSP chain -> loudness normalization" pipeline
    // order). Disabled by default; see LoudnessNormalizer.hpp.
    naikav::dsp::LoudnessNormalizer m_loudness;

    // Guards m_dsp/m_loudness against the one real cross-thread hazard in
    // this class: applyDspSettings() is meant to be called live, from the
    // UI thread, while decodeAndResample() concurrently reads/mutates the
    // same filter/envelope state on the SDL audio callback thread. Held
    // only briefly on both sides (a handful of float writes on the UI side,
    // one process() call on the audio side), so contention is a non-issue
    // in practice. The raw dsp()/loudness() accessors below are NOT
    // protected by this -- they predate applyDspSettings() and remain
    // correct only for single-threaded use (tests, init-time-only setup).
    mutable std::mutex m_dspMutex;
    naikav::dsp::AudioDspSettings m_currentDspSettings;

    // Live pointer to the demuxer's seek-generation counter. See
    // VideoDecoder's m_seekGeneration / Demuxer.hpp's m_seekGeneration
    // comment -- same mechanism, applied symmetrically here so a stale
    // pre-seek audio packet can't be decoded either.
    std::atomic<uint64_t>* m_seekGeneration = nullptr;

    void decodeAndResample();
    static void sdlAudioStreamCallback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount);
 
public:
    AudioDecoder(AVCodecParameters* codecParams, 
                 AVRational timeBase, 
                 int64_t startTime,
                 ThreadSafeQueue<AVPacket*>& queue,
                 std::atomic<uint64_t>* decodeTimeTracker = nullptr);
    ~AudioDecoder();

    bool init();
    void start();
    void pause();
    void resume();
    void stop();
    
    // Tell the audio thread to flush codec buffers (call on seek)
    void flush();
    
    // Thread-safe access to the audio clock
    double getAudioClock();
    void setClock(double seconds);

    // Volume adjustment helper
    void setVolume(float volume); // 0.0 to 1.0

    int getAudioStreamQueuedBytes() const;

    // Live reference to the paused flag, toggled by pause()/resume()/start().
    // Lets the demuxer check whether anything is currently draining the
    // audio packet queue before deciding whether it's safe to block on it.
    std::atomic<bool>& pausedFlag() { return m_paused; }

    // Must be called before the audio thread starts consuming packets,
    // since decoding isn't synchronized with this pointer assignment. See
    // m_seekGeneration above.
    void attachSeekGeneration(std::atomic<uint64_t>* gen) { m_seekGeneration = gen; }

    // Must be called before init(): determines how init() resolves the
    // output channel layout (see AudioChannelOption).
    void setChannelOption(AudioChannelOption option) { m_channelOption = option; }

    // The real default playback device's native channel count, as reported
    // by the OS (0 if unknown/query failed). Compare against
    // getOutputChannelCount(): if the output count is higher, the OS's own
    // audio mixer is silently downmixing -- the resolved layout is not
    // necessarily what's physically reproduced.
    int getDeviceNativeChannels() const { return m_deviceNativeChannels; }

    // Number of channels actually being sent to the audio device. Resolved
    // in init() from the source layout (see chooseOutputLayout in the .cpp);
    // may fall back to 2 (stereo) if the source layout isn't a directly
    // supported surround layout, or if the device rejects the surround
    // stream. Callers must not assume this is always 2.
    int getOutputChannelCount() const { return m_outChannels; }

    // Human-readable name of the resolved output layout (e.g. "stereo",
    // "5.1", "7.1"), for logging/HUD display.
    std::string getOutputChannelLayoutName() const;

    // Direct access to the EQ/compressor/limiter/crossover chain. NOT
    // synchronized against the audio callback thread -- safe only for
    // single-threaded use (tests, or configuring before playback starts).
    // For live control during playback, use applyDspSettings() instead.
    naikav::dsp::DspChain& dsp() { return m_dsp; }

    // Direct access to the loudness normalizer. Same thread-safety caveat
    // as dsp() above -- use applyDspSettings() / getMeasuredIntegratedLufs()
    // for live control/diagnostics during playback instead.
    naikav::dsp::LoudnessNormalizer& loudness() { return m_loudness; }

    // Thread-safe: applies every DSP/loudness parameter in one locked step,
    // safe to call from the UI thread while decodeAndResample() is running
    // concurrently on the SDL audio callback thread. This is the intended
    // path for live GUI/preset control during playback.
    void applyDspSettings(const naikav::dsp::AudioDspSettings& settings);

    // Thread-safe snapshot of whatever was last passed to applyDspSettings()
    // (default-constructed AudioDspSettings if it was never called).
    naikav::dsp::AudioDspSettings getDspSettings() const;

    // Thread-safe live diagnostics, for HUD display.
    double getMeasuredIntegratedLufs() const;
    float getCurrentLoudnessGainDb() const;
};
