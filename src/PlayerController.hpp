#pragma once

#include <string>
#include <memory>
#include "ThreadSafeQueue.hpp"
#include "Demuxer.hpp"
#include "AudioDecoder.hpp"
#include "VideoDecoder.hpp"

enum class PlayerState {
    UNINITIALIZED,
    OPENED,
    PLAYING,
    PAUSED,
    ENDED,
    ERROR_STATE
};

class PlayerController {
private:
    std::string m_filename;
    PlayerState m_state;
    
    // Packet queues
    ThreadSafeQueue<AVPacket*> m_videoQueue;
    ThreadSafeQueue<AVPacket*> m_audioQueue;
    
    // Sub-modules
    std::unique_ptr<Demuxer> m_demuxer;
    std::unique_ptr<AudioDecoder> m_audioDecoder;
    std::unique_ptr<VideoDecoder> m_videoDecoder;
    
    bool m_hasAudio;
    bool m_hasVideo;
    
    // Fallback clock for video-only files
    double m_videoClock; // in seconds
    double m_lastSystemTime;
    
    float m_volume;

    double getSystemTimeInSeconds() const;

public:
    PlayerController();
    ~PlayerController();

    bool openFile(const std::string& filename);
    void play();
    void pause();
    void seek(double seconds);
    void stop();
    
    void updateClockForVideoOnly();

    // Getters
    PlayerState getState() const { return m_state; }
    double getCurrentTime();
    double getDuration() const;
    int getVideoWidth() const;
    int getVideoHeight() const;
    float getVolume() const { return m_volume; }
    bool hasAudio() const { return m_hasAudio; }
    bool hasVideo() const { return m_hasVideo; }
    
    VideoDecoder* getVideoDecoder() const { return m_videoDecoder.get(); }
    
    // Setters
    void setVolume(float volume);
};
