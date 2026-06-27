#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include "ThreadSafeQueue.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

class Demuxer {
private:
    std::string m_filename;
    AVFormatContext* m_formatCtx;
    
    int m_videoStreamIdx;
    int m_audioStreamIdx;
    
    AVCodecParameters* m_videoCodecParams;
    AVCodecParameters* m_audioCodecParams;
    
    AVRational m_videoTimeBase;
    AVRational m_audioTimeBase;
    int64_t m_videoStartTime;
    int64_t m_audioStartTime;
    
    double m_duration; // in seconds
    
    ThreadSafeQueue<AVPacket*>& m_videoQueue;
    ThreadSafeQueue<AVPacket*>& m_audioQueue;
    
    std::thread m_thread;
    std::atomic<bool> m_running;
    std::atomic<bool> m_seekRequested;
    std::atomic<double> m_seekTargetTime;
    std::mutex m_seekMutex;
    std::atomic<bool> m_eof;

    void threadLoop();
    void performSeek();

public:
    Demuxer(const std::string& filename, 
            ThreadSafeQueue<AVPacket*>& videoQueue, 
            ThreadSafeQueue<AVPacket*>& audioQueue);
    ~Demuxer();

    bool open();
    void start();
    void stop();
    
    // Seek to a timestamp in seconds
    void seek(double timeInSeconds);

    // Getters
    int getVideoStreamIndex() const { return m_videoStreamIdx; }
    int getAudioStreamIndex() const { return m_audioStreamIdx; }
    AVCodecParameters* getVideoCodecParams() const { return m_videoCodecParams; }
    AVCodecParameters* getAudioCodecParams() const { return m_audioCodecParams; }
    AVRational getVideoTimeBase() const { return m_videoTimeBase; }
    AVRational getAudioTimeBase() const { return m_audioTimeBase; }
    int64_t getVideoStartTime() const { return m_videoStartTime; }
    int64_t getAudioStartTime() const { return m_audioStartTime; }
    double getDuration() const { return m_duration; }
    bool isRunning() const { return m_running; }
    bool isEOF() const { return m_eof.load(); }
};
