#include "VideoDecoder.hpp"
#include <iostream>

VideoDecoder::VideoDecoder(AVCodecParameters* codecParams, 
                           AVRational timeBase, 
                           int64_t startTime,
                           ThreadSafeQueue<AVPacket*>& queue)
    : m_codecParams(codecParams),
      m_codecCtx(nullptr),
      m_swsCtx(nullptr),
      m_queue(queue),
      m_timeBase(timeBase),
      m_startTime(startTime),
      m_startTimeSaved(false),
      m_decodedFrame(nullptr),
      m_yuvFrame(nullptr),
      m_yuvBuffer(nullptr),
      m_yuvBufferSize(0),
      m_currentFramePts(0.0),
      m_flushRequested(false),
      m_seeking(false) {}

VideoDecoder::~VideoDecoder() {
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
    }
    if (m_yuvBuffer) {
        av_free(m_yuvBuffer);
    }
    if (m_yuvFrame) {
        av_frame_free(&m_yuvFrame);
    }
    if (m_decodedFrame) {
        av_frame_free(&m_decodedFrame);
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
}

bool VideoDecoder::init() {
    const AVCodec* codec = avcodec_find_decoder(m_codecParams->codec_id);
    if (!codec) {
        std::cerr << "Error: Video decoder not found" << std::endl;
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        std::cerr << "Error: Could not allocate video codec context" << std::endl;
        return false;
    }

    if (avcodec_parameters_to_context(m_codecCtx, m_codecParams) < 0) {
        std::cerr << "Error: Could not copy video parameters to codec context" << std::endl;
        return false;
    }

    // Set threads count for decoding acceleration
    m_codecCtx->thread_count = 0; // FFmpeg decides automatically based on core count

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        std::cerr << "Error: Could not open video codec" << std::endl;
        return false;
    }

    m_decodedFrame = av_frame_alloc();
    m_yuvFrame = av_frame_alloc();
    if (!m_decodedFrame || !m_yuvFrame) {
        std::cerr << "Error: Could not allocate video frames" << std::endl;
        return false;
    }

    // Allocate YUV buffer space matching the video dimensions
    m_yuvBufferSize = av_image_get_buffer_size(
        AV_PIX_FMT_YUV420P, 
        m_codecCtx->width, 
        m_codecCtx->height, 
        1
    );
    
    m_yuvBuffer = static_cast<uint8_t*>(av_malloc(m_yuvBufferSize));
    if (!m_yuvBuffer) {
        std::cerr << "Error: Could not allocate memory for YUV buffer" << std::endl;
        return false;
    }

    // Bind buffer segments to YUV frame planes (Y, U, V)
    int ret = av_image_fill_arrays(
        m_yuvFrame->data, 
        m_yuvFrame->linesize, 
        m_yuvBuffer, 
        AV_PIX_FMT_YUV420P, 
        m_codecCtx->width, 
        m_codecCtx->height, 
        1
    );
    
    if (ret < 0) {
        std::cerr << "Error: Could not associate YUV buffer to frame data planes" << std::endl;
        return false;
    }

    std::cout << "Video initialized successfully. Resolution: " 
              << m_codecCtx->width << "x" << m_codecCtx->height << std::endl;
    return true;
}

void VideoDecoder::flush() {
    m_flushRequested = true;
    m_currentFramePts = 0.0;
    m_seeking = true;
}

bool VideoDecoder::decodeNextFrame() {
    if (m_flushRequested) {
        avcodec_flush_buffers(m_codecCtx);
        m_flushRequested = false;
        m_currentFramePts = 0.0;
    }

    AVPacket* packet = nullptr;
    while (true) {
        // 1. Attempt to receive a decoded frame from the codec's internal buffers
        int ret = avcodec_receive_frame(m_codecCtx, m_decodedFrame);
        if (ret >= 0) {
            // We have successfully decoded a frame.
            // Compute the Presentation Timestamp (PTS) in seconds relative to the start of the stream
            if (m_decodedFrame->pts != AV_NOPTS_VALUE) {
                m_currentFramePts = (m_decodedFrame->pts - m_startTime) * av_q2d(m_timeBase);
            } else if (m_decodedFrame->pkt_dts != AV_NOPTS_VALUE) {
                m_currentFramePts = (m_decodedFrame->pkt_dts - m_startTime) * av_q2d(m_timeBase);
            }
            return true;
        }

        if (ret == AVERROR_EOF) {
            return false;
        }

        // If the decoder needs more packet data (EAGAIN), fetch next packet from the queue
        if (ret == AVERROR(EAGAIN)) {
            if (!m_queue.try_pop(packet)) {
                return false; // Queue empty/aborted, do not block main thread!
            }

            ret = avcodec_send_packet(m_codecCtx, packet);
            av_packet_free(&packet); // Release packet wrapper
            if (ret < 0) {
                return false; // Critical send error
            }
        } else {
            // Unhandled decoding error
            return false;
        }
    }
}

bool VideoDecoder::convertFrame() {
    if (!m_codecCtx || !m_decodedFrame || m_decodedFrame->width <= 0) {
        return false;
    }
    if (!m_swsCtx) {
        m_swsCtx = sws_getContext(
            m_codecCtx->width, 
            m_codecCtx->height, 
            m_codecCtx->pix_fmt,
            m_codecCtx->width, 
            m_codecCtx->height, 
            AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, 
            nullptr, 
            nullptr, 
            nullptr
        );
    }
    sws_scale(
        m_swsCtx,
        m_decodedFrame->data,
        m_decodedFrame->linesize,
        0,
        m_codecCtx->height,
        m_yuvFrame->data,
        m_yuvFrame->linesize
    );
    av_frame_unref(m_decodedFrame);
    return true;
}
