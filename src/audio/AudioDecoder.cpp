#include "audio/AudioDecoder.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdint>

namespace {
#if LIBAVUTIL_VERSION_MAJOR >= 57
// Layouts this app can drive straight through to the audio device without
// any custom downmix logic of our own: SDL/most consumer audio hardware
// accepts them directly, and swr can decode straight into them (rather than
// remixing) when the source already uses one of these. Anything else
// (mono, quad, exotic/custom layouts) falls back to a stereo downmix, which
// swr's built-in remix matrix handles sensibly regardless of the source.
bool isDirectlySupportedSurroundLayout(const AVChannelLayout& layout) {
    if (layout.order != AV_CHANNEL_ORDER_NATIVE) {
        return false;
    }
    return layout.u.mask == AV_CH_LAYOUT_2POINT1 ||
           layout.u.mask == AV_CH_LAYOUT_5POINT1 ||
           layout.u.mask == AV_CH_LAYOUT_5POINT1_BACK ||
           layout.u.mask == AV_CH_LAYOUT_7POINT1;
}

// Maps a layout already confirmed by isDirectlySupportedSurroundLayout()
// to SpatialDownmixer's (FFmpeg-agnostic) SourceLayout enum, for
// AudioChannelOption::VIRTUAL_SURROUND. The final `return SEVENPOINT1`
// is exhaustive given the precondition -- isDirectlySupportedSurroundLayout
// only lets these four masks through.
naikav::dsp::SpatialDownmixer::SourceLayout spatialSourceLayoutFor(const AVChannelLayout& layout) {
    using SourceLayout = naikav::dsp::SpatialDownmixer::SourceLayout;
    if (layout.u.mask == AV_CH_LAYOUT_2POINT1) return SourceLayout::TWOPOINT1;
    if (layout.u.mask == AV_CH_LAYOUT_5POINT1) return SourceLayout::FIVEPOINT1_SIDE;
    if (layout.u.mask == AV_CH_LAYOUT_5POINT1_BACK) return SourceLayout::FIVEPOINT1_BACK;
    return SourceLayout::SEVENPOINT1;
}
#else
bool isDirectlySupportedSurroundLayout(uint64_t mask) {
    return mask == AV_CH_LAYOUT_2POINT1 ||
           mask == AV_CH_LAYOUT_5POINT1 ||
           mask == AV_CH_LAYOUT_5POINT1_BACK ||
           mask == AV_CH_LAYOUT_7POINT1;
}
#endif

// Small, fast, non-cryptographic PRNG for dither noise. A per-instance
// xorshift32 generator is plenty for this (no need for std::mt19937's
// larger state or setup cost in a per-sample hot path).
inline uint32_t nextXorshift32(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// Uniform noise in [-0.5, 0.5), one LSB unit wide.
inline float ditherUniform(uint32_t& state) {
    return (static_cast<float>(nextXorshift32(state) >> 8) / 16777216.0f) - 0.5f;
}

// Requests the SoX Resampler (libsoxr) engine instead of swresample's own
// default resampler, for meaningfully better stopband rejection/quality --
// this project's FFmpeg build has libsoxr available (confirmed via
// --enable-libsoxr in `ffmpeg -version`'s configuration string, and by
// probing the "resampler" AVOption directly against the vendored build).
// Must be called after swr_alloc_set_opts(2) but before swr_init(), since
// swr_init() is what actually builds the resampling filters from whatever
// engine is selected at that point. Best-effort: swr_init() below still
// falls back to swresample's own (still perfectly correct, just lower
// quality) resampler if this option is somehow unavailable, so a failure
// here logs a warning rather than failing audio init outright.
// bitsOfPrecision maps ResamplerQuality to soxr's "precision" AVOption
// (bits of precision soxr targets internally) -- only meaningful once the
// "resampler" option above was actually accepted, so this is also
// best-effort: av_opt_set_double() failing here just means the default
// swresample engine's own (fixed) quality is used instead.
void requestSoxrResampler(SwrContext* ctx, double bitsOfPrecision) {
    int ret = av_opt_set(ctx, "resampler", "soxr", 0);
    if (ret < 0) {
        std::cerr << "Warning: libsoxr resampler unavailable (falling back to default swresample engine)" << std::endl;
        return;
    }
    av_opt_set_double(ctx, "precision", bitsOfPrecision, 0);
}

double resamplerPrecisionBitsFor(ResamplerQuality quality) {
    switch (quality) {
        case ResamplerQuality::LOW: return 16.0;
        case ResamplerQuality::HIGH: return 28.0;
        case ResamplerQuality::VERY_HIGH: return 33.0;
        case ResamplerQuality::MEDIUM:
        default: return 20.0; // this project's original (pre-selector) soxr default
    }
}

SDL_AudioFormat sdlFormatFor(AudioOutputBitDepth depth) {
    switch (depth) {
        case AudioOutputBitDepth::BIT_32_INT: return SDL_AUDIO_S32;
        case AudioOutputBitDepth::BIT_32_FLOAT: return SDL_AUDIO_F32;
        case AudioOutputBitDepth::BIT_16:
        default: return SDL_AUDIO_S16;
    }
}

int outputBytesPerSampleFor(AudioOutputBitDepth depth) {
    return depth == AudioOutputBitDepth::BIT_16 ? 2 : 4;
}

// Resolves a device *name* (as persisted/selected via setOutputDeviceName())
// to whatever SDL_AudioDeviceID currently maps to it. Names, not IDs, are
// what's persisted/exposed in the UI, since SDL_AudioDeviceID values are
// only valid for the lifetime of one SDL_Init() session -- re-resolving by
// name on every init() call means a device unplugged-and-replugged (or a
// stale name from a previous session/machine) degrades gracefully to the
// OS default instead of silently failing to open any device at all.
SDL_AudioDeviceID resolveOutputDeviceId(const std::string& name) {
    if (name.empty()) {
        return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    }
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    SDL_AudioDeviceID found = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (devices) {
        for (int i = 0; i < count; ++i) {
            const char* devName = SDL_GetAudioDeviceName(devices[i]);
            if (devName && name == devName) {
                found = devices[i];
                break;
            }
        }
        SDL_free(devices);
    }
    return found;
}

// Converts one normalized float sample ([-1.0, 1.0]) to S16 with triangular
// (TPDF) dither: the sum of two independent uniform draws, so the combined
// noise has a triangular distribution spanning +/-1 LSB. This decorrelates
// quantization error from the signal far better than plain truncation or
// rounding, avoiding the harmonic distortion/noise modulation that shows up
// on quiet passages and fades without dither.
inline int16_t floatToS16Dithered(float sample, uint32_t& ditherState) {
    float scaled = sample * 32767.0f;
    float dither = ditherUniform(ditherState) + ditherUniform(ditherState);
    float dithered = scaled + dither;
    if (dithered > 32767.0f) dithered = 32767.0f;
    if (dithered < -32768.0f) dithered = -32768.0f;
    return static_cast<int16_t>(std::lround(dithered));
}

// Same idea as floatToS16Dithered, scaled for 32-bit integer PCM.
// ditherUniform() produces unit-scaled noise (+/-1 LSB "units"); the 65536x
// factor below is 2^31/2^15, rescaling that same unit noise from "1 LSB at
// 16-bit resolution" to "1 LSB at 32-bit resolution" so the dither is
// correctly sized for the wider output format rather than reusing the
// (relatively enormous, by 32-bit standards) 16-bit dither amplitude.
inline int32_t floatToS32Dithered(float sample, uint32_t& ditherState) {
    double scaled = static_cast<double>(sample) * 2147483647.0;
    float dither = ditherUniform(ditherState) + ditherUniform(ditherState);
    double dithered = scaled + static_cast<double>(dither) * 65536.0;
    if (dithered > 2147483647.0) dithered = 2147483647.0;
    if (dithered < -2147483648.0) dithered = -2147483648.0;
    return static_cast<int32_t>(std::llround(dithered));
}
} // namespace

AudioDecoder::AudioDecoder(AVCodecParameters* codecParams, 
                           AVRational timeBase, 
                           int64_t startTime,
                           ThreadSafeQueue<AVPacket*>& queue,
                           std::atomic<uint64_t>* decodeTimeTracker)
    : m_codecParams(codecParams),
      m_codecCtx(nullptr),
      m_swrCtx(nullptr),
      m_queue(queue),
      m_timeBase(timeBase),
      m_startTime(startTime),
      m_audioStream(nullptr),
      m_clock(0.0),
      m_clockMutex(),
      m_audioBuffer(),
      m_audioBufferIndex(0),
      m_audioBufferSize(0),
      // Seed varies per instance (via `this`) so parallel/successive
      // AudioDecoder instances (e.g. across the test suite) don't produce
      // identical dither noise sequences; the exact seed has no audible
      // consequence otherwise.
      m_ditherState(0x9E3779B9u ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this))),
      m_flushRequested(false),
      m_paused(true),
      m_startTimeSaved(false),
      m_volume(1.0f),
      m_decodedFrame(nullptr),
      m_decodeTimeTracker(decodeTimeTracker),
      m_outSampleRate(48000),
      m_outSampleFmt(AV_SAMPLE_FMT_FLT),
      m_outChannels(2),
#if LIBAVUTIL_VERSION_MAJOR >= 57
      m_outChannelLayout(AV_CHANNEL_LAYOUT_STEREO)
#else
      m_outChannelLayout(AV_CH_LAYOUT_STEREO)
#endif
{
    // Target audio configuration: 48,000Hz, Stereo, 16-bit Signed PCM
}

AudioDecoder::~AudioDecoder() {
    stop();

    if (m_decodedFrame) {
        av_frame_free(&m_decodedFrame);
    }
    if (m_swrCtx) {
        swr_free(&m_swrCtx);
    }
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_uninit(&m_outChannelLayout);
#endif
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
}

bool AudioDecoder::init() {
    const AVCodec* codec = avcodec_find_decoder(m_codecParams->codec_id);
    if (!codec) {
        std::cerr << "Error: Audio decoder not found" << std::endl;
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        std::cerr << "Error: Could not allocate audio codec context" << std::endl;
        return false;
    }

    if (avcodec_parameters_to_context(m_codecCtx, m_codecParams) < 0) {
        std::cerr << "Error: Could not copy audio parameters to codec context" << std::endl;
        return false;
    }

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        std::cerr << "Error: Could not open audio codec" << std::endl;
        return false;
    }

    // Set input channel layout fallback if missing, resolve the output
    // layout from it (preserving surround where we can drive it directly),
    // and configure resampling context accordingly.
#if LIBAVUTIL_VERSION_MAJOR >= 57
    // Zero-initialized: av_channel_layout_copy() below calls
    // av_channel_layout_uninit() on this as the destination first, which
    // frees u.map if order == AV_CHANNEL_ORDER_CUSTOM. Left
    // uninitialized, order and u.map are whatever garbage was already on
    // the stack, so it can free a garbage pointer -- an intermittent
    // access violation deep inside ucrtbase.dll (crash signature seen:
    // 0xc0000005 reading 0xFFFFFFFFFFFFFFFF, always at the same
    // ucrtbase.dll offset regardless of which build produced it) that
    // depends on whatever a prior call left on this stack slot, so it
    // doesn't reproduce reliably in short-lived single-shot repros.
    AVChannelLayout inChannelLayout = {};
    if (m_codecCtx->ch_layout.nb_channels <= 0) {
        av_channel_layout_default(&inChannelLayout, 2);
    } else {
        av_channel_layout_copy(&inChannelLayout, &m_codecCtx->ch_layout);
    }

    // AUTO and VIRTUAL_SURROUND both preserve the source's surround layout
    // internally when possible -- they differ only in what happens to it
    // afterward (sent straight to the device vs. folded down to stereo by
    // m_spatialDownmixer below).
    const bool sourceIsSupportedSurround = isDirectlySupportedSurroundLayout(inChannelLayout);
    const bool preserveSurroundInternally = sourceIsSupportedSurround &&
        (m_channelOption == AudioChannelOption::AUTO || m_channelOption == AudioChannelOption::VIRTUAL_SURROUND);

    if (preserveSurroundInternally) {
        av_channel_layout_copy(&m_outChannelLayout, &inChannelLayout);
    } else {
        av_channel_layout_uninit(&m_outChannelLayout);
        av_channel_layout_default(&m_outChannelLayout, 2);
    }
    m_dspChannels = m_outChannelLayout.nb_channels;

    // VIRTUAL_SURROUND always lands on a genuine 2-channel device stream --
    // the discrete channels resolved above are folded down by
    // m_spatialDownmixer in decodeAndResample() instead of being sent to
    // the device or flattened by swr's own downmix matrix. Falls back to
    // exactly AUTO's/FORCE_STEREO's device channel count otherwise.
    m_spatialDownmixActive = (m_channelOption == AudioChannelOption::VIRTUAL_SURROUND) && preserveSurroundInternally;
    m_outChannels = m_spatialDownmixActive ? 2 : m_dspChannels;
    if (m_spatialDownmixActive) {
        m_spatialDownmixer.configure(spatialSourceLayoutFor(m_outChannelLayout), m_outSampleRate);
    }

    // (Re)configures m_swrCtx for the current m_outChannelLayout/m_dspChannels.
    // Called again below if the device rejects a surround stream and we need
    // to fall back to stereo.
    auto initResampler = [&]() -> bool {
        if (m_swrCtx) {
            swr_free(&m_swrCtx);
        }
        int res = swr_alloc_set_opts2(
            &m_swrCtx,
            &m_outChannelLayout,
            m_outSampleFmt,
            m_outSampleRate,
            &inChannelLayout,
            m_codecCtx->sample_fmt,
            m_codecCtx->sample_rate,
            0,
            nullptr
        );
        if (res < 0 || !m_swrCtx) {
            return false;
        }
        requestSoxrResampler(m_swrCtx, resamplerPrecisionBitsFor(m_resamplerQuality));
        return swr_init(m_swrCtx) >= 0;
    };

    if (!initResampler()) {
        std::cerr << "Error: Could not initialize Audio Resampler" << std::endl;
        av_channel_layout_uninit(&inChannelLayout);
        return false;
    }
#else
    int64_t inChannelLayout = m_codecCtx->channel_layout;
    if (inChannelLayout == 0) {
        inChannelLayout = av_get_default_channel_layout(m_codecCtx->channels > 0 ? m_codecCtx->channels : 2);
    }

    // AUTO and VIRTUAL_SURROUND both preserve the source's surround layout
    // here, for parity with the >=57 branch above -- but this legacy
    // libavutil path does NOT implement SpatialDownmixer's fold-down (it
    // predates that feature and isn't reachable with the vendored FFmpeg,
    // which is >=57), so VIRTUAL_SURROUND here behaves exactly like AUTO
    // (send discrete channels straight to the device) rather than folding
    // them to stereo. m_spatialDownmixActive stays false accordingly.
    if ((m_channelOption == AudioChannelOption::AUTO || m_channelOption == AudioChannelOption::VIRTUAL_SURROUND) &&
        isDirectlySupportedSurroundLayout(static_cast<uint64_t>(inChannelLayout))) {
        m_outChannelLayout = static_cast<uint64_t>(inChannelLayout);
    } else {
        m_outChannelLayout = AV_CH_LAYOUT_STEREO;
    }
    m_outChannels = av_get_channel_layout_nb_channels(m_outChannelLayout);
    m_dspChannels = m_outChannels;
    m_spatialDownmixActive = false;

    auto initResampler = [&]() -> bool {
        if (m_swrCtx) {
            swr_free(&m_swrCtx);
        }
        m_swrCtx = swr_alloc_set_opts(
            nullptr,
            static_cast<int64_t>(m_outChannelLayout),
            m_outSampleFmt,
            m_outSampleRate,
            inChannelLayout,
            m_codecCtx->sample_fmt,
            m_codecCtx->sample_rate,
            0,
            nullptr
        );
        if (!m_swrCtx) {
            return false;
        }
        requestSoxrResampler(m_swrCtx, resamplerPrecisionBitsFor(m_resamplerQuality));
        return swr_init(m_swrCtx) >= 0;
    };

    if (!initResampler()) {
        std::cerr << "Error: Could not initialize Audio Resampler" << std::endl;
        return false;
    }
#endif

    m_decodedFrame = av_frame_alloc();
    if (!m_decodedFrame) {
        std::cerr << "Error: Could not allocate audio frame" << std::endl;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        av_channel_layout_uninit(&inChannelLayout);
#endif
        return false;
    }

    // Resolve the configured device *name* (see setOutputDeviceName()) to
    // whatever SDL_AudioDeviceID currently maps to it -- empty name (the
    // default) resolves to SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, identical to
    // this project's behavior before device selection existed.
    const SDL_AudioDeviceID targetDeviceId = resolveOutputDeviceId(m_outputDeviceName);

    // Query the target device's native channel count before opening
    // anything. This is purely diagnostic (see getDeviceNativeChannels()) --
    // SDL_OpenAudioDeviceStream below will still happily accept a
    // higher-channel request regardless, since Windows/Linux audio APIs
    // transparently downmix in shared mode. That means a successful open()
    // with N channels does NOT by itself prove N-channel hardware is
    // actually connected; this query is what lets the UI tell the user
    // when their "5.1" output is silently being downmixed by the OS to
    // whatever their real device supports.
    {
        SDL_AudioSpec deviceSpec = {};
        int sampleFrames = 0;
        if (SDL_GetAudioDeviceFormat(targetDeviceId, &deviceSpec, &sampleFrames)) {
            m_deviceNativeChannels = deviceSpec.channels;
        } else {
            m_deviceNativeChannels = 0; // unknown -- query failed (e.g. no device present)
        }
    }

    m_outputBytesPerSample = outputBytesPerSampleFor(m_outputBitDepth);

    // Configure SDL3 audio spec
    SDL_AudioSpec wantedSpec = {};
    wantedSpec.freq = m_outSampleRate;
    wantedSpec.format = sdlFormatFor(m_outputBitDepth);
    wantedSpec.channels = m_outChannels;

    m_audioStream = SDL_OpenAudioDeviceStream(targetDeviceId, &wantedSpec, sdlAudioStreamCallback, this);
    if (!m_audioStream && m_outChannels > 2) {
        // The device/driver refused the surround stream (not every sink
        // exposes >2 channels, e.g. an unconfigured WirePlumber node on
        // Linux). Fall back to a stereo downmix rather than failing audio
        // entirely -- re-run the resampler so it actually produces stereo
        // frames matching the spec we retry with.
        std::cerr << "Warning: audio device rejected " << m_outChannels
                  << "-channel output (" << SDL_GetError()
                  << "); falling back to stereo." << std::endl;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        av_channel_layout_uninit(&m_outChannelLayout);
        av_channel_layout_default(&m_outChannelLayout, 2);
#else
        m_outChannelLayout = AV_CH_LAYOUT_STEREO;
#endif
        m_outChannels = 2;
        m_dspChannels = 2; // resampler below now targets stereo too -- keep the DSP chain's channel count in sync
        if (!initResampler()) {
            std::cerr << "Error: Could not reinitialize Audio Resampler for stereo fallback" << std::endl;
#if LIBAVUTIL_VERSION_MAJOR >= 57
            av_channel_layout_uninit(&inChannelLayout);
#endif
            return false;
        }
        wantedSpec.channels = m_outChannels;
        m_audioStream = SDL_OpenAudioDeviceStream(targetDeviceId, &wantedSpec, sdlAudioStreamCallback, this);
    }

#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_uninit(&inChannelLayout);
#endif

    if (!m_audioStream) {
        std::cerr << "Error: Could not open SDL audio stream: " << SDL_GetError() << std::endl;
        return false;
    }

    m_audioBuffer.resize(0); // Initial size 0 to trigger dynamic resize on first frame

    // Resolve the LFE channel's index (if any) in the final output layout,
    // for the DSP chain's optional bass crossover. -1 if there is no
    // discrete LFE channel (e.g. plain stereo).
#if LIBAVUTIL_VERSION_MAJOR >= 57
    int lfeChannelIndex = av_channel_layout_index_from_channel(&m_outChannelLayout, AV_CHAN_LOW_FREQUENCY);
#else
    int lfeChannelIndex = -1;
    if (m_outChannelLayout & AV_CH_LOW_FREQUENCY) {
        lfeChannelIndex = static_cast<int>(av_popcount64(m_outChannelLayout & (AV_CH_LOW_FREQUENCY - 1)));
    }
#endif
    m_dsp.configure(m_dspChannels, m_outSampleRate, lfeChannelIndex);
    m_loudness.configure(m_dspChannels, m_outSampleRate);
    m_surround3d.configure(m_outChannels, m_outSampleRate);
    m_widener.configure(m_outChannels);
    m_balance.configure(m_outChannels);
    m_spectrum.configure(m_outChannels, m_outSampleRate);
    m_spectrum.setEnabled(true);
    m_finalSafetyLimiter.configure(m_outChannels, m_outSampleRate);

    if (m_playbackSpeed.load() != 1.0f) {
        SDL_SetAudioStreamFrequencyRatio(m_audioStream, m_playbackSpeed.load());
    }

    std::cout << "Audio initialized successfully. Target format: " << m_outSampleRate
              << "Hz, 16-bit PCM, " << getOutputChannelLayoutName() << std::endl;
    return true;
}

void AudioDecoder::start() {
    if (m_audioStream) {
        SDL_ResumeAudioStreamDevice(m_audioStream);
        m_paused = false;
    }
}

void AudioDecoder::pause() {
    if (m_audioStream) {
        SDL_PauseAudioStreamDevice(m_audioStream);
        m_paused = true;
    }
}

void AudioDecoder::resume() {
    if (m_audioStream) {
        SDL_ResumeAudioStreamDevice(m_audioStream);
        m_paused = false;
    }
}

void AudioDecoder::stop() {
    if (m_audioStream) {
        SDL_DestroyAudioStream(m_audioStream);
        m_audioStream = nullptr;
    }
}

void AudioDecoder::flush() {
    m_flushRequested = true;
    if (m_audioStream) {
        SDL_ClearAudioStream(m_audioStream);
    }
}

double AudioDecoder::getAudioClock() {
    int queuedBytes = 0;
    if (m_audioStream) {
        queuedBytes = SDL_GetAudioStreamQueued(m_audioStream);
    }

    std::lock_guard<std::mutex> lock(m_clockMutex);

    // Calculate precise current clock position
    // Base clock is the time at the start of the decoded frame
    double baseClock = m_clock;

    // m_audioBufferIndex counts bytes consumed from the internal float
    // buffer (4 bytes/sample), but queuedBytes comes from SDL in the S16
    // device format (2 bytes/sample) -- the two are different units since
    // Phase 2a's float-internal / dithered-truncate-to-S16 conversion.
    // Reconcile them in frames (channel-interleaved sample groups), which
    // is format-agnostic.
    const int internalBytesPerFrame = m_outChannels * static_cast<int>(sizeof(float));
    const int outputBytesPerFrame = m_outChannels * m_outputBytesPerSample;
    if (internalBytesPerFrame <= 0 || outputBytesPerFrame <= 0 || m_outSampleRate <= 0) {
        return baseClock;
    }

    int64_t playedFrames = static_cast<int64_t>(m_audioBufferIndex) / internalBytesPerFrame;
    int64_t queuedFrames = static_cast<int64_t>(queuedBytes) / outputBytesPerFrame;
    playedFrames -= queuedFrames;
    if (playedFrames < 0) {
        playedFrames = 0;
    }

    double offsetTime = static_cast<double>(playedFrames) / m_outSampleRate;

    return baseClock + offsetTime;
}

void AudioDecoder::setClock(double seconds) {
    std::lock_guard<std::mutex> lock(m_clockMutex);
    m_clock = seconds;
}

void AudioDecoder::decodeAndResample() {
    struct TimeTracker {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        std::atomic<uint64_t>* tracker;
        explicit TimeTracker(std::atomic<uint64_t>* t) : tracker(t) {}
        ~TimeTracker() {
            if (tracker) {
                auto end = std::chrono::steady_clock::now();
                uint64_t diff = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                tracker->store(diff);
            }
        }
    } tracker_guard(m_decodeTimeTracker);

    if (m_flushRequested) {
        avcodec_flush_buffers(m_codecCtx);
        if (m_swrCtx) {
            swr_init(m_swrCtx);
        }
        {
            // Locked against applyDspSettings(), which the UI thread can
            // call concurrently with this -- these reset() calls mutate
            // the same Biquad/Compressor/Limiter/Crossover/Surround3D/
            // StereoWidener state that applyDspSettings() writes to under
            // m_dspMutex, and this thread (the SDL audio callback thread,
            // via a seek-triggered flush()) is not otherwise synchronized
            // with it.
            std::lock_guard<std::mutex> dspLock(m_dspMutex);
            m_dsp.reset();
            m_loudness.reset();
            m_spatialDownmixer.reset();
            m_surround3d.reset();
            m_widener.reset();
            m_spectrum.reset();
            m_finalSafetyLimiter.reset();
        }
        m_audioBufferIndex = 0;
        m_audioBufferSize = 0;
        m_flushRequested = false;
    }

    while (true) {
        // Try to receive a decoded frame from the decoder
        int ret = avcodec_receive_frame(m_codecCtx, m_decodedFrame);
        if (ret >= 0) {
            double clockSnapshot = 0.0;
            {
                std::lock_guard<std::mutex> lock(m_clockMutex);
                clockSnapshot = m_clock;
            }

            double pts = 0.0;
            if (m_decodedFrame->pts != AV_NOPTS_VALUE) {
                pts = static_cast<double>(m_decodedFrame->pts - m_startTime) * av_q2d(m_timeBase);
            } else if (m_decodedFrame->pkt_dts != AV_NOPTS_VALUE) {
                pts = static_cast<double>(m_decodedFrame->pkt_dts - m_startTime) * av_q2d(m_timeBase);
            } else {
                pts = clockSnapshot;
            }

            if (pts < clockSnapshot - 0.050) {
                av_frame_unref(m_decodedFrame);
                continue;
            }

            // We have a frame! Resample it to the resolved output layout as
            // interleaved float (m_outSampleFmt) -- sdlAudioStreamCallback
            // handles the final dithered truncation to the S16 device format.
            int maxOutSamples = av_rescale_rnd(
                swr_get_delay(m_swrCtx, m_decodedFrame->sample_rate) + m_decodedFrame->nb_samples,
                m_outSampleRate,
                m_decodedFrame->sample_rate,
                AV_ROUND_UP
            );

            // Sized for the resampler's real (internal/DSP) channel count,
            // not m_outChannels -- the two only differ in VIRTUAL_SURROUND
            // mode, where the resampler still produces the full discrete
            // surround layout and m_spatialDownmixer folds it down to
            // m_outChannels (2) afterward, below.
            int bufferNeeded = maxOutSamples * m_dspChannels * static_cast<int>(sizeof(float));
            if (m_audioBuffer.size() < static_cast<size_t>(bufferNeeded)) {
                m_audioBuffer.resize(bufferNeeded);
            }

            uint8_t* outData[1] = { m_audioBuffer.data() };
            // Use extended_data instead of a manually-built pointer array:
            // for planar formats (e.g. fltp) each channel lives in its own
            // plane, and extended_data is always correctly populated by
            // FFmpeg for any channel count (unlike a hand-rolled 4-element
            // array, which left higher channel planes null for >4-channel
            // sources such as 5.1 audio, causing swr_convert to dereference
            // a null pointer).
            int outSamples = swr_convert(
                m_swrCtx,
                outData,
                maxOutSamples,
                const_cast<const uint8_t**>(m_decodedFrame->extended_data),
                m_decodedFrame->nb_samples
            );

            if (outSamples < 0) {
                m_audioBufferSize = 0;
                av_frame_unref(m_decodedFrame);
                return;
            }

            if (outSamples == 0) {
                // NOT an error, and not end of stream: swr_convert() legally
                // returns 0 while the resampler is still filling its internal
                // buffer. Every resampling engine has some latency before it
                // can emit its first samples, and soxr's grows with the
                // precision tier -- so the higher the user's ResamplerQuality,
                // the more often this happens.
                //
                // Falling through here would set m_audioBufferSize to
                // (0 * channels * 4) == 0, which sdlAudioStreamCallback()
                // cannot distinguish from "the queue is starved or the file
                // ended" -- so it would memset a whole block of digital
                // silence into an otherwise healthy stream. That is a hard
                // discontinuity in the output: an audible click. At 44.1kHz
                // -> 48kHz with the VERY_HIGH tier this fired on ~17% of all
                // audio callbacks, which is the crackling this fixes.
                //
                // The resampler simply needs more input, so loop and decode
                // the next frame instead of reporting silence. This cannot
                // spin forever: the loop's try_pop() returns as soon as the
                // packet queue is genuinely empty.
                av_frame_unref(m_decodedFrame);
                continue;
            }

            // DSP chain (EQ -> compressor -> limiter -> LFE crossover), then
            // loudness normalization, both run in-place here on the
            // freshly-resampled float buffer, before it's ever truncated to
            // the S16 device format. Both are no-ops when disabled (the
            // default -- see DspChain::configure() / LoudnessNormalizer).
            // cppcheck-suppress invalidPointerCast
            // m_audioBuffer is raw byte storage that m_outSampleFmt
            // (AV_SAMPLE_FMT_FLT) controls the true contents of -- swr just
            // wrote well-formed interleaved IEEE-754 float samples into it a
            // few lines above via the same uint8_t* view. Reinterpreting
            // those bytes as float* here is exactly how they're meant to be
            // read; this isn't blind reinterpretation of untrusted bytes.
            float* dspBuffer = reinterpret_cast<float*>(m_audioBuffer.data());
            {
                // Locked against applyDspSettings(), which the UI thread can
                // call concurrently with this (see m_dspMutex in the header).
                std::lock_guard<std::mutex> dspLock(m_dspMutex);
                m_dsp.process(dspBuffer, outSamples);
                m_loudness.process(dspBuffer, outSamples);

                // VIRTUAL_SURROUND: fold the discrete m_dspChannels-wide
                // buffer down to stereo with positional cues (see
                // SpatialDownmixer), then overwrite the front of
                // m_audioBuffer with the result -- the destination region
                // (outSamples * 2 floats) is always smaller than the
                // source region it replaces (outSamples * m_dspChannels
                // floats, m_dspChannels >= 3 whenever this is active), so
                // this can't clobber data still needed elsewhere.
                if (m_spatialDownmixActive) {
                    const size_t downmixSamples = static_cast<size_t>(outSamples) * 2;
                    if (m_downmixBuffer.size() < downmixSamples) {
                        m_downmixBuffer.resize(downmixSamples);
                    }
                    m_spatialDownmixer.process(dspBuffer, outSamples, m_downmixBuffer.data());
                    std::memcpy(m_audioBuffer.data(), m_downmixBuffer.data(),
                                downmixSamples * sizeof(float));
                }

                // "3D Surround" ambience synthesis, then mid-side stereo
                // widening, both on whatever ended up as the final
                // m_outChannels-wide buffer. Both are no-ops unless enabled
                // and m_outChannels == 2 (see Surround3D / StereoWidener).
                m_surround3d.process(dspBuffer, outSamples);
                m_widener.process(dspBuffer, outSamples);
                m_balance.process(dspBuffer, outSamples);
                m_spectrum.process(dspBuffer, outSamples);

                // Final safety backstop: SpatialDownmixer/Surround3D/
                // Widener above can each independently push samples past
                // +/-1.0 (see m_finalSafetyLimiter's doc comment) after
                // the DspChain's own Limiter already ran upstream on a
                // different (possibly wider) channel buffer -- this always
                // runs, regardless of dspEnabled/limiterEnabled, so no
                // combination of these stages can reach the S16 hard clamp
                // below as a raw, unlimited over.
                m_finalSafetyLimiter.process(dspBuffer, outSamples);
            }

            m_audioBufferSize = outSamples * m_outChannels * static_cast<int>(sizeof(float));
            m_audioBufferIndex = 0;

            // Set internal clock to frame start PTS relative to the start of the stream
            {
                std::lock_guard<std::mutex> lock(m_clockMutex);
                double clockForUpdate = m_clock;

                pts = 0.0;
                if (m_decodedFrame->pts != AV_NOPTS_VALUE) {
                    pts = static_cast<double>(m_decodedFrame->pts - m_startTime) * av_q2d(m_timeBase);
                } else if (m_decodedFrame->pkt_dts != AV_NOPTS_VALUE) {
                    pts = static_cast<double>(m_decodedFrame->pkt_dts - m_startTime) * av_q2d(m_timeBase);
                } else {
                    // Fallback: increment clock by the duration of the decoded audio frame
                    pts = clockForUpdate + static_cast<double>(m_decodedFrame->nb_samples) / m_decodedFrame->sample_rate;
                }

                m_clock = pts;
            }

            av_frame_unref(m_decodedFrame);
            return;
        }

        if (ret == AVERROR(EAGAIN)) {
            // Need more packets to decode a frame. Pop one from the queue.
            AVPacket* packet = nullptr;
            if (!m_queue.try_pop(packet)) {
                // Queue is empty, cannot send more packets
                m_queueEmptyCount.fetch_add(1, std::memory_order_relaxed);
                m_audioBufferSize = 0;
                m_audioBufferIndex = 0;
                return;
            }

            if (m_seekGeneration) {
                uint64_t packetGeneration = static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(packet->opaque));
                if (packetGeneration != m_seekGeneration->load(std::memory_order_relaxed)) {
                    // Stale pre-seek packet (see VideoDecoder's identical
                    // check and Demuxer.hpp's m_seekGeneration comment).
                    // Drop it and try the next one instead of decoding it.
                    av_packet_free(&packet);
                    continue;
                }
            }

            ret = avcodec_send_packet(m_codecCtx, packet);
            av_packet_free(&packet);
            if (ret < 0) {
                m_lastFailReason.store(ret, std::memory_order_relaxed);
                m_sendFailCount.fetch_add(1, std::memory_order_relaxed);
                m_audioBufferSize = 0;
                return;
            }
            continue; // Loop again to receive frame
        }

        // EOF or error
        m_lastFailReason.store(ret, std::memory_order_relaxed);
        m_receiveFailCount.fetch_add(1, std::memory_order_relaxed);
        m_audioBufferSize = 0;
        return;
    }
}

void AudioDecoder::setVolume(float volume) {
    m_volume = std::clamp(volume, 0.0f, 1.0f);
}

void AudioDecoder::setPlaybackSpeed(float speed) {
    speed = std::clamp(speed, 0.25f, 2.0f);
    m_playbackSpeed.store(speed);
    if (m_audioStream) {
        SDL_SetAudioStreamFrequencyRatio(m_audioStream, speed);
    }
}

void AudioDecoder::sdlAudioStreamCallback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount) {
    AudioDecoder* self = static_cast<AudioDecoder*>(userdata);
    (void)stream;
    (void)total_amount;
    
    self->m_callbackCount.fetch_add(1, std::memory_order_relaxed);

    int len = additional_amount;
    if (len <= 0) {
        len = 4096;
    }
    
    std::vector<uint8_t> tempBuffer(len);
    uint8_t* destPtr = tempBuffer.data();
    int bytesWritten = 0;
    
    {
        std::lock_guard<std::mutex> lock(self->m_audioMutex);
        while (len > 0) {
            if (self->m_audioBufferIndex >= self->m_audioBufferSize) {
                // Buffer is consumed, decode next frames
                self->decodeAndResample();
                if (self->m_audioBufferSize == 0) {
                    // If queues are starved or file ended, output silence
                    self->m_silenceInjections.fetch_add(1, std::memory_order_relaxed);
                    self->m_silenceBytes.fetch_add(static_cast<uint64_t>(len),
                                                   std::memory_order_relaxed);
                    std::memset(destPtr, 0, len);
                    bytesWritten += len;
                    break;
                }
            }

            // The internal buffer holds float samples (4 bytes/sample) but
            // the destination is whatever format setOutputBitDepth()
            // selected (2 or 4 bytes/sample) -- convert sample counts, not
            // raw bytes, between the two.
            constexpr int kInternalBytesPerSample = static_cast<int>(sizeof(float));
            const int outputBytesPerSample = self->m_outputBytesPerSample;

            int samplesAvailable = static_cast<int>(
                (self->m_audioBufferSize - self->m_audioBufferIndex) / kInternalBytesPerSample);
            int samplesNeeded = len / outputBytesPerSample;
            int samplesToCopy = std::min(samplesAvailable, samplesNeeded);
            if (samplesToCopy <= 0) {
                // Shouldn't happen given the refill check above, but avoid
                // spinning forever if a partial/misaligned buffer ever slips
                // through.
                break;
            }

            const float* src = reinterpret_cast<const float*>(self->m_audioBuffer.data() + self->m_audioBufferIndex);
            float volume = self->m_volume;

            switch (self->m_outputBitDepth) {
            case AudioOutputBitDepth::BIT_32_FLOAT: {
                // Float in, float out -- no truncation, so no dither needed
                // either; this is the only lossless path in the callback.
                //
                // destPtr is SDL's own output block (Uint8*), which the
                // device was opened as SDL_AUDIO_F32 in this branch, so it
                // is float data by construction and SDL guarantees its
                // alignment. The byte-typed pointer is the shape of SDL's
                // callback API, not a representation mismatch.
                // cppcheck-suppress invalidPointerCast
                float* dest = reinterpret_cast<float*>(destPtr);
                if (volume <= 0.01f) {
                    std::memset(dest, 0, samplesToCopy * sizeof(float));
                } else if (volume >= 0.99f) {
                    std::memcpy(dest, src, samplesToCopy * sizeof(float));
                } else {
                    for (int i = 0; i < samplesToCopy; ++i) {
                        dest[i] = src[i] * volume;
                    }
                }
                break;
            }
            case AudioOutputBitDepth::BIT_32_INT: {
                int32_t* dest = reinterpret_cast<int32_t*>(destPtr);
                if (volume <= 0.01f) {
                    std::memset(dest, 0, samplesToCopy * sizeof(int32_t));
                } else if (volume >= 0.99f) {
                    for (int i = 0; i < samplesToCopy; ++i) {
                        dest[i] = floatToS32Dithered(src[i], self->m_ditherState);
                    }
                } else {
                    for (int i = 0; i < samplesToCopy; ++i) {
                        dest[i] = floatToS32Dithered(src[i] * volume, self->m_ditherState);
                    }
                }
                break;
            }
            case AudioOutputBitDepth::BIT_16:
            default: {
                int16_t* dest = reinterpret_cast<int16_t*>(destPtr);
                if (volume <= 0.01f) {
                    // True digital silence for mute, rather than dithering
                    // down to (inaudible but non-zero) noise for no benefit.
                    std::memset(dest, 0, samplesToCopy * sizeof(int16_t));
                } else if (volume >= 0.99f) {
                    for (int i = 0; i < samplesToCopy; ++i) {
                        dest[i] = floatToS16Dithered(src[i], self->m_ditherState);
                    }
                } else {
                    for (int i = 0; i < samplesToCopy; ++i) {
                        dest[i] = floatToS16Dithered(src[i] * volume, self->m_ditherState);
                    }
                }
                break;
            }
            }

            int internalBytesConsumed = samplesToCopy * kInternalBytesPerSample;
            int outputBytesWritten = samplesToCopy * outputBytesPerSample;

            destPtr += outputBytesWritten;
            len -= outputBytesWritten;
            bytesWritten += outputBytesWritten;
            self->m_audioBufferIndex += internalBytesConsumed;
        }
    }
    
    if (bytesWritten > 0) {
        SDL_PutAudioStreamData(stream, tempBuffer.data(), bytesWritten);
    }
}

int AudioDecoder::getAudioStreamQueuedBytes() const {
    return m_audioStream ? SDL_GetAudioStreamQueued(m_audioStream) : 0;
}

std::vector<std::string> AudioDecoder::enumeratePlaybackDeviceNames() {
    std::vector<std::string> names;
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    if (devices) {
        for (int i = 0; i < count; ++i) {
            if (const char* name = SDL_GetAudioDeviceName(devices[i])) {
                names.emplace_back(name);
            }
        }
        SDL_free(devices);
    }
    return names;
}

std::string AudioDecoder::getOutputChannelLayoutName() const {
#if LIBAVUTIL_VERSION_MAJOR >= 57
    char buf[64];
    int len = av_channel_layout_describe(&m_outChannelLayout, buf, sizeof(buf));
    if (len > 0) {
        return std::string(buf);
    }
    return m_dspChannels == 1 ? "mono" : (m_dspChannels == 2 ? "stereo" : (std::to_string(m_dspChannels) + "ch"));
#else
    switch (m_outChannelLayout) {
        case AV_CH_LAYOUT_MONO: return "mono";
        case AV_CH_LAYOUT_STEREO: return "stereo";
        case AV_CH_LAYOUT_5POINT1: return "5.1";
        case AV_CH_LAYOUT_5POINT1_BACK: return "5.1(back)";
        case AV_CH_LAYOUT_7POINT1: return "7.1";
        default: return std::to_string(m_outChannels) + "ch";
    }
#endif
}

void AudioDecoder::applyDspSettings(const naikav::dsp::AudioDspSettings& settings) {
    std::lock_guard<std::mutex> lock(m_dspMutex);

    m_dsp.setEnabled(settings.dspEnabled);
    for (int i = 0; i < naikav::dsp::ParametricEQ::kNumBands; ++i) {
        m_dsp.eq.setBandFrequencyHz(i, settings.eqBandFreqHz[i]);
        m_dsp.eq.setBandQ(i, settings.eqBandQ[i]);
        m_dsp.eq.setBandGainDb(i, settings.eqBandGainDb[i]);
    }
    m_dsp.compressor.setThresholdDb(settings.compressorThresholdDb);
    // "Disabled" is ratio 1:1 -- a true no-op regardless of threshold (see
    // Compressor's own doc comment) -- rather than a separate enable flag.
    m_dsp.compressor.setRatio(settings.compressorEnabled ? settings.compressorRatio : 1.0f);

    // Same "ratio 1:1 = inert" convention as the compressor above.
    m_dsp.noiseGate.setThresholdDb(settings.noiseGateThresholdDb);
    m_dsp.noiseGate.setRatio(settings.noiseGateEnabled ? settings.noiseGateRatio : 1.0f);

    m_dsp.multiband.setEnabled(settings.multibandEnabled);
    m_dsp.multiband.setCrossoverFrequencies(settings.multibandLowMidHz, settings.multibandMidHighHz);
    m_dsp.multiband.low.setThresholdDb(settings.multibandLowThresholdDb);
    m_dsp.multiband.low.setRatio(settings.multibandLowRatio);
    m_dsp.multiband.mid.setThresholdDb(settings.multibandMidThresholdDb);
    m_dsp.multiband.mid.setRatio(settings.multibandMidRatio);
    m_dsp.multiband.high.setThresholdDb(settings.multibandHighThresholdDb);
    m_dsp.multiband.high.setRatio(settings.multibandHighRatio);

    // Same idea: 0dB ceiling is the Limiter's inert state.
    m_dsp.limiter.setCeilingDb(settings.limiterEnabled ? settings.limiterCeilingDb : 0.0f);
    m_dsp.crossover.setEnabled(settings.crossoverEnabled);
    m_dsp.crossover.setCutoffHz(settings.crossoverCutoffHz);
    m_dsp.crossover.setBassRedirectEnabled(settings.crossoverBassRedirectEnabled);

    m_loudness.setEnabled(settings.loudnessEnabled);
    m_loudness.setTargetLufs(settings.loudnessTargetLufs);

    m_surround3d.setEnabled(settings.surround3dEnabled);
    m_surround3d.setIntensity(settings.surround3dIntensity);

    m_widener.setEnabled(settings.widenerEnabled);
    m_widener.setWidth(settings.widenerWidth);

    m_balance.setBalance(settings.balance);

    m_spectrum.setEnabled(settings.spectrumAnalyzerEnabled);

    // Tracks the user's actual effective Limiter ceiling -- "effective"
    // meaning dspEnabled must also be true, since DspChain::process() (and
    // therefore m_dsp.limiter) is skipped entirely when dspEnabled is
    // false, same as m_dsp.setEnabled() above. Falls back to a plain
    // 0dBFS backstop otherwise; see m_finalSafetyLimiter's doc comment.
    m_finalSafetyLimiter.setCeilingDb((settings.dspEnabled && settings.limiterEnabled) ? settings.limiterCeilingDb : 0.0f);

    m_currentDspSettings = settings;
}

void AudioDecoder::primeLoudnessPrescan(double integratedLufs) {
    std::lock_guard<std::mutex> lock(m_dspMutex);
    m_loudness.primeWithPrescannedLufs(integratedLufs);
}

naikav::dsp::AudioDspSettings AudioDecoder::getDspSettings() const {
    std::lock_guard<std::mutex> lock(m_dspMutex);
    return m_currentDspSettings;
}

double AudioDecoder::getMeasuredIntegratedLufs() const {
    std::lock_guard<std::mutex> lock(m_dspMutex);
    return m_loudness.getMeasuredIntegratedLufs();
}

float AudioDecoder::getCurrentLoudnessGainDb() const {
    std::lock_guard<std::mutex> lock(m_dspMutex);
    return m_loudness.getCurrentGainDb();
}
