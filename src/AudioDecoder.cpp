#include "AudioDecoder.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>

AudioDecoder::AudioDecoder(AVCodecParameters* codecParams, 
                           AVRational timeBase, 
                           int64_t startTime,
                           ThreadSafeQueue<AVPacket*>& queue)
    : m_codecParams(codecParams),
      m_codecCtx(nullptr),
      m_swrCtx(nullptr),
      m_queue(queue),
      m_timeBase(timeBase),
      m_startTime(startTime),
      m_startTimeSaved(false),
      m_audioDevice(0),
      m_clock(0.0),
      m_audioBufferIndex(0),
      m_audioBufferSize(0),
      m_flushRequested(false),
      m_paused(true),
      m_decodedFrame(nullptr) {
    m_volume = 1.0f;
    
    // Target audio configuration: 48,000Hz, Stereo, 16-bit Signed PCM
    m_outSampleRate = 48000;
    m_outSampleFmt = AV_SAMPLE_FMT_S16;
    m_outChannels = 2;
    m_outChannelLayout = AV_CHANNEL_LAYOUT_STEREO;
}

AudioDecoder::~AudioDecoder() {
    stop();
    
    if (m_decodedFrame) {
        av_frame_free(&m_decodedFrame);
    }
    if (m_swrCtx) {
        swr_free(&m_swrCtx);
    }
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

    // Set input channel layout fallback if missing
    AVChannelLayout inChannelLayout;
    if (m_codecCtx->ch_layout.nb_channels <= 0) {
        av_channel_layout_default(&inChannelLayout, 2);
    } else {
        av_channel_layout_copy(&inChannelLayout, &m_codecCtx->ch_layout);
    }

    // Configure resampling context using modern swr_alloc_set_opts2
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

    av_channel_layout_uninit(&inChannelLayout);

    if (res < 0 || !m_swrCtx || swr_init(m_swrCtx) < 0) {
        std::cerr << "Error: Could not initialize Audio Resampler" << std::endl;
        return false;
    }

    m_decodedFrame = av_frame_alloc();
    if (!m_decodedFrame) {
        std::cerr << "Error: Could not allocate audio frame" << std::endl;
        return false;
    }

    // Configure SDL audio spec
    SDL_AudioSpec wantedSpec;
    SDL_memset(&wantedSpec, 0, sizeof(wantedSpec));
    wantedSpec.freq = m_outSampleRate;
    wantedSpec.format = AUDIO_S16SYS; // Signed 16-bit native endian
    wantedSpec.channels = m_outChannels;
    wantedSpec.samples = 1024;
    wantedSpec.callback = sdlAudioCallback;
    wantedSpec.userdata = this;

    SDL_AudioSpec obtainedSpec;
    m_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &wantedSpec, &obtainedSpec, 0);
    if (m_audioDevice == 0) {
        std::cerr << "Error: Could not open SDL audio device: " << SDL_GetError() << std::endl;
        return false;
    }

    m_audioBuffer.resize(0); // Initial size 0 to trigger dynamic resize on first frame
    
    std::cout << "Audio initialized successfully. Target format: 48kHz, 16-bit PCM, Stereo" << std::endl;
    return true;
}

void AudioDecoder::start() {
    if (m_audioDevice > 0) {
        SDL_PauseAudioDevice(m_audioDevice, 0); // Start audio thread
        m_paused = false;
    }
}

void AudioDecoder::pause() {
    if (m_audioDevice > 0) {
        SDL_PauseAudioDevice(m_audioDevice, 1);
        m_paused = true;
    }
}

void AudioDecoder::resume() {
    if (m_audioDevice > 0) {
        SDL_PauseAudioDevice(m_audioDevice, 0);
        m_paused = false;
    }
}

void AudioDecoder::stop() {
    if (m_audioDevice > 0) {
        SDL_CloseAudioDevice(m_audioDevice);
        m_audioDevice = 0;
    }
}

void AudioDecoder::flush() {
    m_flushRequested = true;
}

double AudioDecoder::getAudioClock() {
    std::lock_guard<std::mutex> lock(m_clockMutex);
    
    // Calculate precise current clock position
    // Base clock is the time at the start of the decoded frame
    double baseClock = m_clock;
    
    // Sample size: 2 channels * 2 bytes (16-bit) = 4 bytes per stereo sample frame
    const int sampleSize = m_outChannels * 2;
    size_t playedBytes = m_audioBufferIndex;
    
    double offsetTime = static_cast<double>(playedBytes) / (m_outSampleRate * sampleSize);
    
    return baseClock + offsetTime;
}

void AudioDecoder::setClock(double seconds) {
    std::lock_guard<std::mutex> lock(m_clockMutex);
    m_clock = seconds;
}

void AudioDecoder::decodeAndResample() {
    if (m_flushRequested) {
        avcodec_flush_buffers(m_codecCtx);
        if (m_swrCtx) {
            swr_init(m_swrCtx);
        }
        {
            std::lock_guard<std::mutex> lock(m_clockMutex);
            m_audioBufferIndex = 0;
        }
        m_audioBufferSize = 0;
        m_flushRequested = false;
    }

    while (true) {
        // Try to receive a decoded frame from the decoder
        int ret = avcodec_receive_frame(m_codecCtx, m_decodedFrame);
        if (ret >= 0) {
            double pts = 0.0;
            if (m_decodedFrame->pts != AV_NOPTS_VALUE) {
                double frameSeconds = static_cast<double>(m_decodedFrame->pts) / m_decodedFrame->sample_rate;
                double startSeconds = static_cast<double>(m_startTime) * av_q2d(m_timeBase);
                pts = frameSeconds - startSeconds;
            } else {
                pts = m_clock;
            }

            if (pts < m_clock - 0.050) {
                av_frame_unref(m_decodedFrame);
                continue;
            }

            // We have a frame! Resample it to stereo 16-bit PCM
            int maxOutSamples = av_rescale_rnd(
                swr_get_delay(m_swrCtx, m_decodedFrame->sample_rate) + m_decodedFrame->nb_samples,
                m_outSampleRate,
                m_decodedFrame->sample_rate,
                AV_ROUND_UP
            );

            int bufferNeeded = maxOutSamples * m_outChannels * 2;
            if (m_audioBuffer.size() < static_cast<size_t>(bufferNeeded)) {
                m_audioBuffer.resize(bufferNeeded);
            }

            uint8_t* outData[1] = { m_audioBuffer.data() };
            int outSamples = swr_convert(
                m_swrCtx,
                outData,
                maxOutSamples,
                (const uint8_t**)m_decodedFrame->data,
                m_decodedFrame->nb_samples
            );

            if (outSamples < 0) {
                m_audioBufferSize = 0;
                av_frame_unref(m_decodedFrame);
                return;
            }

            m_audioBufferSize = outSamples * m_outChannels * 2;

            // Set internal clock to frame start PTS relative to the start of the stream
            pts = 0.0;
            if (m_decodedFrame->pts != AV_NOPTS_VALUE) {
                // Convert frame PTS (in codec timebase units: sample count) to seconds
                double frameSeconds = static_cast<double>(m_decodedFrame->pts) / m_decodedFrame->sample_rate;
                // Convert stream start time (in stream timebase units) to seconds
                double startSeconds = static_cast<double>(m_startTime) * av_q2d(m_timeBase);
                pts = frameSeconds - startSeconds;
            } else {
                // Fallback: increment clock by the duration of the decoded audio frame
                pts = m_clock + static_cast<double>(m_decodedFrame->nb_samples) / m_decodedFrame->sample_rate;
            }

            {
                std::lock_guard<std::mutex> lock(m_clockMutex);
                m_audioBufferIndex = 0;
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
                m_audioBufferSize = 0;
                {
                    std::lock_guard<std::mutex> lock(m_clockMutex);
                    m_audioBufferIndex = 0;
                }
                return;
            }

            ret = avcodec_send_packet(m_codecCtx, packet);
            av_packet_free(&packet);
            if (ret < 0) {
                m_audioBufferSize = 0;
                return;
            }
            continue; // Loop again to receive frame
        }

        // EOF or error
        m_audioBufferSize = 0;
        return;
    }
}

void AudioDecoder::setVolume(float volume) {
    m_volume = std::clamp(volume, 0.0f, 1.0f);
}

void AudioDecoder::sdlAudioCallback(void* userdata, Uint8* stream, int len) {
    AudioDecoder* self = static_cast<AudioDecoder*>(userdata);
    
    // Fill the audio playback device buffer
    while (len > 0) {
        if (self->m_audioBufferIndex >= self->m_audioBufferSize) {
            // Buffer is consumed, decode next frames
            self->decodeAndResample();
            if (self->m_audioBufferSize == 0) {
                // If queues are starved or file ended, output silence
                std::memset(stream, 0, len);
                break;
            }
        }

        int bytesToCopy = std::min(len, static_cast<int>(self->m_audioBufferSize - self->m_audioBufferIndex));
        
        // Software volume scaling copy
        int16_t* src = reinterpret_cast<int16_t*>(self->m_audioBuffer.data() + self->m_audioBufferIndex);
        int16_t* dest = reinterpret_cast<int16_t*>(stream);
        int samplesToCopy = bytesToCopy / 2; // 16-bit = 2 bytes per sample
        float volume = self->m_volume;

        if (volume >= 0.99f) {
            // Perfect bypass copy (faster)
            std::memcpy(dest, src, bytesToCopy);
        } else if (volume <= 0.01f) {
            // Perfect bypass for mute (faster, zero overhead)
            std::memset(dest, 0, bytesToCopy);
        } else {
            // Scale amplitude line-by-line
            for (int i = 0; i < samplesToCopy; ++i) {
                float val = src[i] * volume;
                // Clip if any overflows (not typical for attenuation, but safe practice)
                if (val > 32767.0f) val = 32767.0f;
                if (val < -32768.0f) val = -32768.0f;
                dest[i] = static_cast<int16_t>(val);
            }
        }

        stream += bytesToCopy;
        len -= bytesToCopy;
        {
            std::lock_guard<std::mutex> lock(self->m_clockMutex);
            self->m_audioBufferIndex += bytesToCopy;
        }
    }
}
