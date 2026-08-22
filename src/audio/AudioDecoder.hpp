#pragma once

#include <vector>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include "core/ThreadSafeQueue.hpp"
#include "audio/dsp/DspChain.hpp"
#include "audio/dsp/Limiter.hpp"
#include "audio/dsp/LoudnessNormalizer.hpp"
#include "audio/dsp/LoudnessPrescan.hpp"
#include "audio/dsp/AudioDspSettings.hpp"
#include "audio/dsp/SpatialDownmixer.hpp"
#include "audio/dsp/StereoWidener.hpp"
#include "audio/dsp/Surround3D.hpp"
#include "audio/dsp/BalanceControl.hpp"
#include "audio/dsp/SpectrumAnalyzer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
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
// getDeviceNativeChannels()). VIRTUAL_SURROUND preserves the source's
// surround layout internally (same detection as AUTO) but always opens a
// genuine 2-channel device stream, folding the discrete channels down to
// stereo with SpatialDownmixer's positional-cue downmix instead of either
// sending them to the device or letting swresample's flat downmix matrix
// blend everything into the middle -- this is what actually makes 5.1/7.1
// content sound spatial on stereo speakers/headphones. Falls back to
// exactly AUTO's behavior when the source isn't one of the directly
// supported surround layouts (nothing to virtualize).
enum class AudioChannelOption {
    AUTO = 0,
    FORCE_STEREO,
    VIRTUAL_SURROUND,
    COUNT
};

// Output device bit depth. The internal pipeline is always float
// (AV_SAMPLE_FMT_FLT) end to end -- this only controls the *final*
// truncation format handed to the SDL audio device, applied at init()
// time (see AudioDecoder::setOutputBitDepth() -- must be called before
// init(), like setChannelOption()). BIT_16 (S16) was this project's only
// option until this was added; BIT_32_FLOAT skips truncation/dither
// entirely (the device receives the same float samples the DSP chain
// already produced), and BIT_32_INT gives 32-bit integer PCM with a
// proportionally scaled dither for hardware/drivers that prefer integer
// samples but want more headroom/precision than 16-bit.
enum class AudioOutputBitDepth {
    BIT_16 = 0,
    BIT_32_INT,
    BIT_32_FLOAT,
    COUNT
};

// libsoxr resampling quality tier, applied via the "precision" AVOption
// (bits of precision soxr targets) before swr_init() -- see
// requestSoxrResampler() in AudioDecoder.cpp. Higher precision costs more
// CPU per resampled sample; MEDIUM matches this project's original
// (pre-selector) default, so leaving this unset is behavior-identical to
// before it existed.
enum class ResamplerQuality {
    LOW = 0,    // soxr ~16-bit precision -- cheapest, audibly fine for casual listening
    MEDIUM,     // soxr ~20-bit precision -- this project's original default
    HIGH,       // soxr ~28-bit precision
    VERY_HIGH,  // soxr ~33-bit precision -- effectively transparent, most CPU cost
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

    // Frames of this buffer already handed to the device at the moment
    // m_clock was last written. The clock and the buffer offset are two
    // halves of one logical position, so they are published together
    // under m_clockMutex: reading them separately let getAudioClock()
    // observe a freshly-reset offset alongside the previous frame's base
    // PTS, which showed up as up to one frame (~20ms) of A/V sync jitter
    // that was not present in the audio itself.
    int64_t m_clockFrameOffset = 0;

    // Total output latency, in frames, introduced by lookahead stages
    // downstream of the clock (the DSP chain's limiter when enabled, plus
    // the always-on safety limiter). Tracked for diagnostics only -- see
    // the long comment in getAudioClock() for why it is NOT subtracted
    // from the clock. Written on the audio thread (or init()).
    std::atomic<int> m_dspLatencyFrames{0};
    
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

    // Reusable scratch for sdlAudioStreamCallback's output block. Sized
    // once in init() to the largest block SDL can ask for, so the callback
    // never allocates -- it previously constructed a std::vector<uint8_t>
    // on every single callback, on the real-time thread.
    std::vector<uint8_t> m_callbackBuffer;

    // Volume actually applied to the last sample handed to the device.
    // A volume change is ramped from this toward m_volume across the block
    // rather than applied as a step, which is a waveform discontinuity --
    // an audible click on every volume keypress and on mute/unmute. Only
    // touched from the audio callback thread.
    float m_appliedVolume = 1.0f;

    // State for the TPDF dither applied when truncating float samples down
    // to the S16 device format. Per-instance (not global/static) so the
    // many AudioDecoder instances created across the test suite don't share
    // RNG state, and so this stays trivially thread-safe without needing a
    // shared lock (only ever touched from the SDL audio callback thread).
    uint32_t m_ditherState;
    
    // See getSilenceInjectionCount(). Written only from the SDL audio
    // callback thread, read from anywhere.
    std::atomic<uint64_t> m_callbackCount{0};
    std::atomic<uint64_t> m_silenceInjections{0};
    std::atomic<uint64_t> m_silenceBytes{0};

    // Which decodeAndResample() exit path produced a silent block, so an
    // underrun can be attributed rather than guessed at.
    std::atomic<uint64_t> m_queueEmptyCount{0};
    // Decoded frames discarded for arriving too far behind the clock (see
    // decodeAndResample). Each one is a waveform discontinuity, so it
    // belongs with the other glitch counters rather than being invisible.
    std::atomic<uint64_t> m_lateFrameDrops{0};
    std::atomic<uint64_t> m_sendFailCount{0};
    std::atomic<uint64_t> m_receiveFailCount{0};
    std::atomic<int> m_lastFailReason{0};

    std::atomic<bool> m_flushRequested;
    std::atomic<bool> m_paused;
    bool m_startTimeSaved;
    std::atomic<float> m_volume;
    
    AVFrame* m_decodedFrame;
    std::atomic<uint64_t>* m_decodeTimeTracker;
 
    // Output specs (SDL Audio configuration)
    int m_outSampleRate;
    AVSampleFormat m_outSampleFmt;
    // Channels actually sent to the SDL audio device. Equal to
    // m_dspChannels except in VIRTUAL_SURROUND mode, where the resampler/
    // DSP chain still processes the full discrete surround layout
    // (m_dspChannels) but the device only ever gets a folded-down stereo
    // stream (see m_spatialDownmixActive below).
    int m_outChannels;
    // Channels the resampler/DspChain/LoudnessNormalizer actually operate
    // on -- the resolved *internal* layout, which may differ from
    // m_outChannels (see above). Equal to m_outChannels outside
    // VIRTUAL_SURROUND mode.
    int m_dspChannels = 2;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout m_outChannelLayout;
#else
    uint64_t m_outChannelLayout;
#endif

    // The real default playback device's native channel count, queried via
    // SDL_GetAudioDeviceFormat() in init() (0 if the query failed/unknown).
    // See getDeviceNativeChannels().
    int m_deviceNativeChannels = 0;

    // The target device's native sample rate, from the same
    // SDL_GetAudioDeviceFormat() query (0 if unknown). Used by init() to
    // pick m_outSampleRate instead of forcing 48kHz -- see the comment
    // there. The query already read this field and threw it away.
    int m_deviceNativeSampleRate = 0;

    // Set via setChannelOption() before init(); see AudioChannelOption.
    AudioChannelOption m_channelOption = AudioChannelOption::AUTO;

    // Set via setOutputBitDepth() before init(); see AudioOutputBitDepth.
    AudioOutputBitDepth m_outputBitDepth = AudioOutputBitDepth::BIT_16;

    // Set via setOutputDeviceName() before init(). Empty = the OS default
    // playback device (SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK), matching this
    // project's original (pre-selector) behavior. Device *names* are used
    // rather than SDL_AudioDeviceID values because IDs are only valid for
    // the lifetime of one SDL_Init() session and aren't meaningful to
    // persist to player_settings.txt across app restarts -- see
    // resolveOutputDeviceId() in AudioDecoder.cpp, which re-resolves a
    // name to whatever ID that device currently has (falling back to the
    // default device if it's no longer present, e.g. unplugged).
    std::string m_outputDeviceName;

    // Set via setResamplerQuality() before init(); see ResamplerQuality.
    ResamplerQuality m_resamplerQuality = ResamplerQuality::MEDIUM;

    // Bytes per sample in the device's actual output format (derived from
    // m_outputBitDepth at init() time: 2 for BIT_16, 4 for BIT_32_INT/
    // BIT_32_FLOAT). Used by sdlAudioStreamCallback() and getAudioClock()
    // to convert between the internal float buffer's byte offsets and the
    // device format's, generalizing what used to be a hardcoded "2".
    int m_outputBytesPerSample = 2;

    // True when m_channelOption == VIRTUAL_SURROUND and the source is one
    // of the directly-supported surround layouts (i.e. there's actually
    // something to fold down). See m_spatialDownmixer below.
    bool m_spatialDownmixActive = false;

    // Folds m_dspChannels-wide discrete surround audio down to the 2
    // channels actually sent to the device, with positional delay/filter
    // cues instead of a flat sum. Only configured/used when
    // m_spatialDownmixActive is true. FFmpeg-agnostic (see
    // SpatialDownmixer.hpp), so AudioDecoder.cpp maps the real
    // AVChannelLayout to its SourceLayout enum.
    naikav::dsp::SpatialDownmixer m_spatialDownmixer;

    // Scratch buffer for m_spatialDownmixer's 2-channel output, copied
    // into the front of m_audioBuffer afterward. Reused across calls to
    // decodeAndResample() (like m_audioBuffer itself) to avoid a
    // per-frame allocation. Typed as float (unlike m_audioBuffer, which
    // holds whatever the device format is) because it only ever carries
    // the DSP path's native float samples.
    std::vector<float> m_downmixBuffer;

    // Parametric EQ -> compressor -> limiter -> LFE crossover, applied to
    // the freshly-decoded float buffer in decodeAndResample(), before the
    // dithered truncation to the device's S16 format. Disabled by default
    // (see DspChain::configure()), so wiring this in doesn't change
    // existing playback behavior on its own. Configured with
    // m_dspChannels, not m_outChannels -- see the comment on those above.
    naikav::dsp::DspChain m_dsp;

    // EBU R128 loudness normalization, applied after the DSP chain above
    // (matching the plan's "DSP chain -> loudness normalization" pipeline
    // order). Disabled by default; see LoudnessNormalizer.hpp. Also
    // configured with m_dspChannels.
    naikav::dsp::LoudnessNormalizer m_loudness;

    // "3D Surround" ambience synthesis -- runs on the same final
    // m_outChannels-wide buffer as m_widener, right before it (see
    // Surround3D.hpp). Unlike m_spatialDownmixer, this works on ANY
    // 2-channel output, including a plain stereo source -- it's what
    // gives ordinary stereo content a synthesized spatial feel, and adds
    // extra depth on top of m_spatialDownmixer's output when that's also
    // active. Disabled by default.
    naikav::dsp::Surround3D m_surround3d;

    // Mid-side stereo widener, run right after m_surround3d on the same
    // final m_outChannels-wide buffer (plain stereo, a FORCE_STEREO/AUTO-
    // fallback downmix, or m_spatialDownmixer's virtual surround output).
    // See StereoWidener.hpp for why this lives outside DspChain. Disabled
    // by default.
    naikav::dsp::StereoWidener m_widener;

    // Left/right output balance, run right after m_widener on the same
    // final m_outChannels-wide buffer. See BalanceControl.hpp. Disabled
    // (centered) by default.
    naikav::dsp::BalanceControl m_balance;

    // Real-time magnitude spectrum visualizer, fed on the same final
    // m_outChannels-wide buffer as m_balance, right after it (before the
    // final safety limiter, since a look-ahead limiter's tiny output
    // delay doesn't matter for a visual display, and this way the
    // spectrum reflects everything upstream of it). See
    // SpectrumAnalyzer.hpp. Disabled by default; a display feature, not
    // an audio effect -- never modifies the signal.
    naikav::dsp::SpectrumAnalyzer m_spectrum;

    // Final safety backstop, run unconditionally as the very last DSP
    // step (after m_widener), independent of m_dsp's own Limiter and of
    // whether dspEnabled is even true. m_spatialDownmixer (summing
    // multiple source channels with no headroom), m_surround3d
    // (intensity can exceed 1.0), and m_widener (width can exceed 1.0)
    // all run *after* the DspChain's Limiter and can each independently
    // push samples well past +/-1.0 -- without this, that overshoot would
    // reach floatToS16Dithered's hard int16 clamp untouched, producing
    // harsh digital clipping instead of graceful limiting. Ceiling
    // tracks the user's configured Limiter setting when that's actually
    // active (dspEnabled && limiterEnabled), else defaults to a plain
    // 0dBFS backstop -- see applyDspSettings().
    naikav::dsp::Limiter m_finalSafetyLimiter;

    // ---- Control-thread -> audio-thread settings mailbox --------------
    //
    // applyDspSettings() used to take a std::mutex that the SDL audio
    // callback also took, around the entire DSP path. That is a blocking
    // primitive on a real-time thread: if the control thread were
    // preempted while holding it -- mid biquad redesign, say, or anywhere
    // inside the filter designs a preset change runs -- the audio callback
    // would block for an unbounded time and the device would underrun.
    // Classic priority inversion, and "the critical section is short" does
    // not fix it, because the hazard is the scheduler's timing rather than
    // the section's length.
    //
    // So: no lock. The control thread publishes a plain settings snapshot
    // through a seqlock, and the audio thread picks it up at the top of
    // decodeAndResample() and applies it itself. The audio thread never
    // blocks, never waits and never allocates, and every DSP object below
    // is now touched by exactly one thread -- which is what actually makes
    // them safe, rather than a mutex making concurrent access survivable.
    //
    // Single-producer/single-consumer by construction: PlayerController
    // serializes every applyDspSettings()/primeLoudnessPrescan() call
    // behind its own m_audioDecoderMutex on the render thread, and the
    // sole consumer is the audio callback. AudioDspSettings is a flat
    // aggregate of bools and floats with no pointers or heap state, so a
    // torn read caught by the seqlock is discardable rather than dangerous.
    //
    // This is the same handshake SpectrumAnalyzer uses to publish spectrum
    // frames in the other direction; see its readSnapshot().
    naikav::dsp::AudioDspSettings m_mailboxSettings;
    std::atomic<uint32_t> m_mailboxSeq{0};

    // Control-thread-owned copy of the last published snapshot, so
    // getDspSettings() answers without reaching across to the mailbox at
    // all. The producer is also the only caller, so this needs no
    // handshake -- and keeping the seqlock's cross-thread read confined to
    // the audio side keeps the one unavoidable racy read in one place.
    naikav::dsp::AudioDspSettings m_publishedSettings;

    // Prescan priming rides the same one-way street, as its own one-shot
    // flag: it arrives on the render thread (from the background scan
    // thread's result, or from a loudness tag read inline) and it mutates
    // LoudnessNormalizer state that the audio thread owns.
    std::atomic<double> m_pendingPrescanLufs{-120.0};
    std::atomic<bool> m_pendingPrescan{false};

    // Audio-thread-owned. The last snapshot actually applied to the DSP
    // objects, kept so applyPendingDspSettings() redesigns only the
    // filters whose parameters really changed -- dragging one EQ band must
    // not cost every filter design in the chain on the callback thread.
    naikav::dsp::AudioDspSettings m_currentDspSettings;
    uint32_t m_appliedMailboxSeq = 0;
    // False until a snapshot has been applied once, so the first one
    // applies every parameter rather than diffing against a
    // default-constructed struct that may coincidentally match.
    bool m_hasAppliedDspSettings = false;

    // Seqlock read of the mailbox. False if the writer was caught
    // mid-update on every attempt, leaving *out untouched; the audio
    // thread simply retries on the next block, which is invisible against
    // a control surface that moves at UI frame rate.
    bool readMailboxSettings(naikav::dsp::AudioDspSettings* out, uint32_t* seq) const;

    // Drains the mailbox above onto the DSP objects, and applies any
    // pending prescan priming. Audio thread only; called at the top of
    // decodeAndResample(). Allocation-free: every DSP setter it reaches
    // only rewrites existing coefficient storage (the vectors are sized in
    // configure()), so the worst case is a bounded handful of
    // transcendental calls, not an unbounded wait.
    void applyPendingDspSettings();

    // Recomputes m_dspLatencyFrames from the currently-active lookahead
    // stages. Audio thread only, or init(), which runs before the device
    // is resumed and therefore before any callback can observe it.
    void updateDspLatency();

    // Live pointer to the demuxer's seek-generation counter. See
    // VideoDecoder's m_seekGeneration / Demuxer.hpp's m_seekGeneration
    // comment -- same mechanism, applied symmetrically here so a stale
    // pre-seek audio packet can't be decoded either.
    std::atomic<uint64_t>* m_seekGeneration = nullptr;

    // Dynamic playback speed rate via SDL3 audio stream frequency ratio
    std::atomic<float> m_playbackSpeed{1.0f};

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

    // Playback speed rate (pitch-preserving time stretching)
    void setPlaybackSpeed(float speed);
    float getPlaybackSpeed() const { return m_playbackSpeed.load(); }

    // Volume adjustment helper
    void setVolume(float volume); // 0.0 to 1.0

    int getAudioStreamQueuedBytes() const;

    // Underrun diagnostics. sdlAudioStreamCallback() fills the remainder of
    // its output block with digital silence whenever decodeAndResample()
    // cannot produce samples. Each such event is a hard discontinuity in
    // the output -- an audible click -- so the ratio of these two counters
    // is the direct measure of playback glitching. SDL_GetAudioStreamQueued()
    // is NOT usable for this: the callback puts exactly the amount SDL asked
    // for and SDL consumes it immediately, so that query reads ~0 at all
    // times regardless of health. Relaxed ordering -- these are monotonic
    // counters read for reporting, never used to make decisions.
    uint64_t getCallbackCount() const { return m_callbackCount.load(std::memory_order_relaxed); }
    uint64_t getSilenceInjectionCount() const { return m_silenceInjections.load(std::memory_order_relaxed); }
    uint64_t getSilenceBytes() const { return m_silenceBytes.load(std::memory_order_relaxed); }
    uint64_t getQueueEmptyCount() const { return m_queueEmptyCount.load(std::memory_order_relaxed); }
    // Decoded audio frames dropped for lateness. Non-zero means audible
    // discontinuities that the silence counters above do NOT cover.
    uint64_t getLateFrameDropCount() const { return m_lateFrameDrops.load(std::memory_order_relaxed); }
    uint64_t getSendFailCount() const { return m_sendFailCount.load(std::memory_order_relaxed); }
    uint64_t getReceiveFailCount() const { return m_receiveFailCount.load(std::memory_order_relaxed); }
    int getLastFailReason() const { return m_lastFailReason.load(std::memory_order_relaxed); }

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

    // Must be called before init(): determines the device's PCM bit depth
    // (see AudioOutputBitDepth).
    void setOutputBitDepth(AudioOutputBitDepth depth) { m_outputBitDepth = depth; }
    AudioOutputBitDepth getOutputBitDepth() const { return m_outputBitDepth; }

    // Must be called before init(): selects which playback device to open
    // by name (see m_outputDeviceName above). Empty string = OS default.
    void setOutputDeviceName(const std::string& name) { m_outputDeviceName = name; }
    const std::string& getOutputDeviceName() const { return m_outputDeviceName; }

    // Must be called before init(): selects the libsoxr resampling
    // quality tier (see ResamplerQuality).
    void setResamplerQuality(ResamplerQuality quality) { m_resamplerQuality = quality; }
    ResamplerQuality getResamplerQuality() const { return m_resamplerQuality; }

    // Enumerates currently-available playback device names, for UI
    // dropdowns (see PlayerUI's Output Device selector) and for
    // resolveOutputDeviceId()'s name->ID lookup. Safe to call at any
    // time -- purely a query against SDL's current device list, no
    // AudioDecoder state involved. Requires SDL_INIT_AUDIO to already be
    // initialized (true for the whole lifetime of this app).
    static std::vector<std::string> enumeratePlaybackDeviceNames();

    // The real default playback device's native channel count, as reported
    // by the OS (0 if unknown/query failed). Compare against
    // getOutputChannelCount(): if the output count is higher, the OS's own
    // audio mixer is silently downmixing -- the resolved layout is not
    // necessarily what's physically reproduced.
    int getDeviceNativeChannels() const { return m_deviceNativeChannels; }

    // The resolved output sample rate. No longer always 48000: init()
    // prefers the source's own rate when the device can take it, else the
    // device's native rate, so the common 44.1kHz case need not be
    // resampled at all.
    int getOutputSampleRate() const { return m_outSampleRate; }

    // Output latency in frames contributed by the lookahead limiters
    // currently active. Diagnostic only: see getAudioClock() for why the
    // audio clock deliberately does not compensate for it.
    int getDspLatencyFrames() const { return m_dspLatencyFrames.load(std::memory_order_relaxed); }

    // Number of channels actually being sent to the audio device. Resolved
    // in init() from the source layout (see chooseOutputLayout in the .cpp);
    // may fall back to 2 (stereo) if the source layout isn't a directly
    // supported surround layout, or if the device rejects the surround
    // stream, or because VIRTUAL_SURROUND always folds down to a genuine
    // stereo device stream (see isVirtualSurroundActive()). Callers must
    // not assume this is always 2.
    int getOutputChannelCount() const { return m_outChannels; }

    // True when the source's discrete surround channels are being folded
    // down to stereo by SpatialDownmixer rather than sent to the device
    // untouched. getOutputChannelCount() is still the true device channel
    // count (2) in this case; getOutputChannelLayoutName() describes the
    // internal surround layout being folded down, not the device format.
    bool isVirtualSurroundActive() const { return m_spatialDownmixActive; }

    // Human-readable name of the resolved output layout (e.g. "stereo",
    // "5.1", "7.1"), for logging/HUD display. Reflects the internal
    // resampler/DSP layout, so when isVirtualSurroundActive() is true this
    // names the surround layout being virtualized (e.g. "5.1(side)"), not
    // the 2-channel stream actually reaching the device.
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

    // Thread-safe and wait-free against the audio callback: sanitizes the
    // settings, then publishes them as one atomic snapshot that the audio
    // thread picks up at the top of its next decodeAndResample(). Takes no
    // lock, so it can never stall the callback -- see the mailbox comment
    // on m_mailboxSeq. This is the intended path for live GUI/preset
    // control during playback.
    //
    // Call from one thread only (PlayerController's render thread, which
    // already serializes it behind m_audioDecoderMutex): the mailbox is
    // single-producer.
    void applyDspSettings(const naikav::dsp::AudioDspSettings& settings);

    // Thread-safe: primes the loudness normalizer with a whole-file
    // integrated LUFS value obtained via
    // naikav::dsp::prescanIntegratedLufs() (see LoudnessPrescan.hpp),
    // switching it into two-pass mode -- see
    // LoudnessNormalizer::primeWithPrescannedLufs(). Callers should run the
    // (blocking, decode-only) prescan on whatever thread they like before
    // calling this; this final priming step just hands the value to the
    // audio thread through the same wait-free mailbox applyDspSettings()
    // uses, and the audio thread applies it to the normalizer it owns.
    void primeLoudnessPrescan(double integratedLufs);

    // Services deferred, non-real-time-safe maintenance the audio callback
    // deliberately does not do itself -- currently rebuilding the loudness
    // meter's retired filter graph after a seek swapped to its spare (see
    // LoudnessMeter::serviceRebuild()). MUST be called from a normal
    // thread; PlayerController does so once per frame. Cheap and safe to
    // call when there is nothing to do.
    void serviceDeferredMaintenance();

    // Snapshot of whatever was last passed to applyDspSettings()
    // (default-constructed AudioDspSettings if it was never called).
    // Lock-free and allocation-free. Call from the same thread that calls
    // applyDspSettings(): it reads that thread's own copy, so it is a
    // plain read rather than a cross-thread one.
    //
    // Returns by value on purpose. A const reference would alias
    // m_publishedSettings, which applyDspSettings() overwrites in place,
    // so a caller holding the reference across a later applyDspSettings()
    // would silently start reading the new settings instead of the ones it
    // asked for. AudioDspSettings is a couple of hundred bytes of PODs and
    // this is called at most once per UI frame, so the copy buys immunity
    // to that for nothing measurable.
    // cppcheck-suppress returnByReference
    naikav::dsp::AudioDspSettings getDspSettings() const;

    // Thread-safe live diagnostics, for HUD display. Lock-free: both are
    // single atomic loads of values the audio thread publishes, so the
    // Audio Processing panel polling them every UI frame cannot contend
    // with the callback.
    double getMeasuredIntegratedLufs() const;
    float getCurrentLoudnessGainDb() const;

    // Thread-safe snapshot of the current magnitude spectrum (dB per bin,
    // see SpectrumAnalyzer::kNumBins/binFrequencyHz()), for the Audio
    // Processing panel's visualizer. Self-synchronized inside
    // SpectrumAnalyzer's own seqlock -- needs no external synchronization.
    std::vector<float> getSpectrumMagnitudesDb() const { return m_spectrum.getMagnitudesDb(); }
    std::vector<float> getWaveformSamples() const { return m_spectrum.getWaveformSamples(); }
    static int getSpectrumNumBins() { return naikav::dsp::SpectrumAnalyzer::kNumBins; }
    double getSpectrumBinFrequencyHz(int bin) const { return m_spectrum.binFrequencyHz(bin); }
};
