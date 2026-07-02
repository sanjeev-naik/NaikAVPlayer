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
      m_seeking(false),
      m_allocatedWidth(0),
      m_allocatedHeight(0),
      m_allocatedFormat(AV_PIX_FMT_NONE) {}

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
    m_codecCtx->thread_type = FF_THREAD_FRAME;

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

    m_allocatedWidth = m_codecCtx->width;
    m_allocatedHeight = m_codecCtx->height;
    m_allocatedFormat = m_codecCtx->pix_fmt;

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
    if (!m_codecCtx || !m_decodedFrame || m_decodedFrame->width <= 0 || m_decodedFrame->height <= 0) {
        return false;
    }

    // Check if the decoded frame's dimensions or format changed dynamically
    if (m_decodedFrame->width != m_allocatedWidth || 
        m_decodedFrame->height != m_allocatedHeight || 
        m_decodedFrame->format != m_allocatedFormat) {
        
        std::cout << "Dynamic resolution or format change detected: " 
                  << m_allocatedWidth << "x" << m_allocatedHeight << " (fmt " << m_allocatedFormat << ") -> "
                  << m_decodedFrame->width << "x" << m_decodedFrame->height << " (fmt " << m_decodedFrame->format << ")" << std::endl;

        // Free old scaling context
        if (m_swsCtx) {
            sws_freeContext(m_swsCtx);
            m_swsCtx = nullptr;
        }

        // Only recreate YUV buffer if dimensions actually changed
        if (m_decodedFrame->width != m_allocatedWidth || m_decodedFrame->height != m_allocatedHeight) {
            if (m_yuvBuffer) {
                av_free(m_yuvBuffer);
                m_yuvBuffer = nullptr;
            }

            // Reallocate YUV buffer for the new dimensions
            m_yuvBufferSize = av_image_get_buffer_size(
                AV_PIX_FMT_YUV420P,
                m_decodedFrame->width,
                m_decodedFrame->height,
                1
            );
            m_yuvBuffer = static_cast<uint8_t*>(av_malloc(m_yuvBufferSize));
            if (!m_yuvBuffer) {
                std::cerr << "Error: Could not allocate YUV buffer after resolution change" << std::endl;
                return false;
            }

            // Associate the new buffer with the YUV frame
            int ret = av_image_fill_arrays(
                m_yuvFrame->data,
                m_yuvFrame->linesize,
                m_yuvBuffer,
                AV_PIX_FMT_YUV420P,
                m_decodedFrame->width,
                m_decodedFrame->height,
                1
            );
            if (ret < 0) {
                std::cerr << "Error: Could not associate YUV buffer on resolution change" << std::endl;
                av_free(m_yuvBuffer);
                m_yuvBuffer = nullptr;
                return false;
            }
        }

        m_allocatedWidth = m_decodedFrame->width;
        m_allocatedHeight = m_decodedFrame->height;
        m_allocatedFormat = static_cast<AVPixelFormat>(m_decodedFrame->format);
    }

    if (!m_swsCtx) {
        m_swsCtx = sws_getContext(
            m_allocatedWidth, 
            m_allocatedHeight, 
            static_cast<AVPixelFormat>(m_decodedFrame->format),
            m_allocatedWidth, 
            m_allocatedHeight, 
            AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, 
            nullptr, 
            nullptr, 
            nullptr
        );
        if (!m_swsCtx) {
            std::cerr << "Error: Could not allocate scaling context on resolution change" << std::endl;
            return false;
        }
    }

    // Optimize: If the decoded frame is already in a layout-compatible planar YUV 4:2:0 format,
    // reference it directly in m_yuvFrame to avoid costly software scaling/copying.
    if ((m_decodedFrame->format == AV_PIX_FMT_YUV420P || m_decodedFrame->format == AV_PIX_FMT_YUVJ420P)
        && m_decodedFrame->width == m_allocatedWidth 
        && m_decodedFrame->height == m_allocatedHeight) {
        
        av_frame_unref(m_yuvFrame);
        av_frame_ref(m_yuvFrame, m_decodedFrame);
        av_frame_unref(m_decodedFrame);
        return true;
    }

    // If m_yuvFrame was previously populated via reference copy, re-bind it to m_yuvBuffer
    // before running sws_scale to avoid writing to shared decoder frame buffers.
    if (m_yuvFrame->data[0] != m_yuvBuffer) {
        av_frame_unref(m_yuvFrame);
        av_image_fill_arrays(
            m_yuvFrame->data, 
            m_yuvFrame->linesize, 
            m_yuvBuffer, 
            AV_PIX_FMT_YUV420P, 
            m_allocatedWidth, 
            m_allocatedHeight, 
            1
        );
    }

    sws_scale(
        m_swsCtx,
        m_decodedFrame->data,
        m_decodedFrame->linesize,
        0,
        m_allocatedHeight,
        m_yuvFrame->data,
        m_yuvFrame->linesize
    );
    av_frame_unref(m_decodedFrame);
    return true;
}
