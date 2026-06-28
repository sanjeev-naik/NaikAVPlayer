#pragma once

#include <atomic>
#include "ThreadSafeQueue.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

class VideoDecoder {
private:
    AVCodecParameters* m_codecParams;
    AVCodecContext* m_codecCtx;
    SwsContext* m_swsCtx;
    
    ThreadSafeQueue<AVPacket*>& m_queue;
    AVRational m_timeBase;
    int64_t m_startTime;
    
    AVFrame* m_decodedFrame;
    AVFrame* m_yuvFrame;
    uint8_t* m_yuvBuffer;
    int m_yuvBufferSize;
    int m_allocatedWidth;
    int m_allocatedHeight;

    double m_currentFramePts;
    std::atomic<bool> m_flushRequested;
    bool m_startTimeSaved;
    std::atomic<bool> m_seeking;

public:
    VideoDecoder(AVCodecParameters* codecParams, 
                 AVRational timeBase, 
                 int64_t startTime,
                 ThreadSafeQueue<AVPacket*>& queue);
    ~VideoDecoder();

    bool init();
    
    // Decode the next video frame from the queue.
    // Returns true if a frame was successfully decoded and stored in m_yuvFrame.
    bool decodeNextFrame();
    
    void flush();
    bool convertFrame();

    // Getters
    AVFrame* getYUVFrame() const { return m_yuvFrame; }
    double getCurrentFramePts() const { return m_currentFramePts; }
    int getWidth() const { return m_codecCtx ? m_codecCtx->width : 0; }
    int getHeight() const { return m_codecCtx ? m_codecCtx->height : 0; }
    bool isSeeking() const { return m_seeking.load(); }
    void setSeeking(bool seeking) { m_seeking.store(seeking); }
};
