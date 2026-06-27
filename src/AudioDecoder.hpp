#pragma once

#include <vector>
#include <atomic>
#include <mutex>
#include "ThreadSafeQueue.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}

#include <SDL2/SDL.h>

class AudioDecoder {
private:
    AVCodecParameters* m_codecParams;
    AVCodecContext* m_codecCtx;
    SwrContext* m_swrCtx;
    
    ThreadSafeQueue<AVPacket*>& m_queue;
    AVRational m_timeBase;
    int64_t m_startTime;
    
    SDL_AudioDeviceID m_audioDevice;
    
    // Audio Clock synchronization variables
    double m_clock; // Current audio clock (in seconds)
    std::mutex m_clockMutex;
    
    // Decoding temporary buffers
    std::vector<uint8_t> m_audioBuffer;
    size_t m_audioBufferIndex;
    size_t m_audioBufferSize;
    
    std::atomic<bool> m_flushRequested;
    std::atomic<bool> m_paused;
    bool m_startTimeSaved;
    
    AVFrame* m_decodedFrame;

    // Output specs (SDL Audio configuration)
    int m_outSampleRate;
    AVSampleFormat m_outSampleFmt;
    int m_outChannels;
    AVChannelLayout m_outChannelLayout;

    void decodeAndResample();
    static void sdlAudioCallback(void* userdata, Uint8* stream, int len);

public:
    AudioDecoder(AVCodecParameters* codecParams, 
                 AVRational timeBase, 
                 int64_t startTime,
                 ThreadSafeQueue<AVPacket*>& queue);
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
};
