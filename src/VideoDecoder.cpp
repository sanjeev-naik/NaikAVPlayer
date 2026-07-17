#include "VideoDecoder.hpp"
#include <chrono>
#include <iostream>
#include <thread>


VideoDecoder::VideoDecoder(AVCodecParameters *codecParams, AVRational timeBase,
                           int64_t startTime,
                           ThreadSafeQueue<AVPacket *> &queue)
    : m_codecParams(codecParams), m_codecCtx(nullptr), m_swsCtx(nullptr),
      m_queue(queue), m_timeBase(timeBase), m_startTime(startTime),
      m_decodedFrame(nullptr), m_yuvFrame(nullptr), m_yuvBuffer(nullptr),
      m_yuvBufferSize(0), m_allocatedWidth(0), m_allocatedHeight(0),
      m_allocatedFormat(AV_PIX_FMT_NONE), m_currentFramePts(0.0),
      m_flushRequested(false), m_startTimeSaved(false), m_seeking(false),
      m_consecutiveEagainCount(0), m_hardwareRecoveryAttempts(0) {}

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
  AVCodecContext *codecCtx = nullptr;
  const AVCodec *codec = nullptr;

  if (m_codecParams->codec_id == AV_CODEC_ID_H264) {
    const char *candidates[] = {
#ifdef _WIN32
        "h264_d3d11va", "h264_dxva2", "h264_qsv", "h264_cuvid",
#endif

#ifdef __linux__
        "h264_v4l2m2m", "h264_vaapi", "h264_qsv", "h264_cuvid",
#endif
    };

    for (const char *name : candidates) {
      const AVCodec *candidate = avcodec_find_decoder_by_name(name);
      if (!candidate)
        continue;

      AVCodecContext *ctx = avcodec_alloc_context3(candidate);
      if (!ctx)
        continue;

      if (avcodec_parameters_to_context(ctx, m_codecParams) < 0) {
        avcodec_free_context(&ctx);
        continue;
      }

      // For hardware decoders, thread pool synchronization adds
      // overhead/latency. Set thread_count to 1 and disable frame-level
      // threading.
      ctx->thread_count = 1;
      ctx->thread_type = 0;
      ctx->pkt_timebase = m_timeBase;

      // Try opening this decoder.
      if (avcodec_open2(ctx, candidate, nullptr) == 0) {
        // DRY-RUN CHECK: Validate hardware session initialization using
        // extradata. This is crucial to detect cases where avcodec_open2()
        // succeeds but decoding fails (e.g. QSV on headless VMs without GPU
        // drivers).
        AVPacket *testPkt = av_packet_alloc();
        if (m_codecParams->extradata && m_codecParams->extradata_size > 0) {
          testPkt->data = m_codecParams->extradata;
          testPkt->size = m_codecParams->extradata_size;
        }
        int sendRet = avcodec_send_packet(ctx, testPkt);
        testPkt->data = nullptr;
        testPkt->size = 0;
        av_packet_free(&testPkt);

        int receiveRet = AVERROR(EAGAIN);
        if (sendRet >= 0 || sendRet == AVERROR(EAGAIN) ||
            sendRet == AVERROR_INVALIDDATA) {
          AVFrame *tempFrame = av_frame_alloc();
          if (tempFrame) {
            receiveRet = avcodec_receive_frame(ctx, tempFrame);
            av_frame_free(&tempFrame);
          }
        }

        bool sendOk =
            (sendRet >= 0 || sendRet == AVERROR(EAGAIN) ||
             sendRet == AVERROR_EOF || sendRet == AVERROR_INVALIDDATA);
        bool receiveOk = (receiveRet >= 0 || receiveRet == AVERROR(EAGAIN) ||
                          receiveRet == AVERROR_EOF);

        if (sendOk && receiveOk) {
          codec = candidate;
          codecCtx = ctx;
          avcodec_flush_buffers(
              codecCtx); // <-- reset internal state after the dry-run probe
          std::cout << "Using decoder: " << codec->name << '\n';
          break;
        }
      }

      std::cerr << "Hardware decoder " << candidate->name
                << " unavailable, trying next..." << std::endl;

      avcodec_free_context(&ctx);
    }
  }

  // Fall back to software decoder.
  if (!codecCtx) {
    codec = avcodec_find_decoder(m_codecParams->codec_id);
    if (!codec) {
      std::cerr << "Error: No decoder found." << std::endl;
      return false;
    }

    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
      std::cerr << "Error: Failed to allocate codec context." << std::endl;
      return false;
    }

    if (avcodec_parameters_to_context(codecCtx, m_codecParams) < 0) {
      avcodec_free_context(&codecCtx);
      std::cerr << "Error: Failed to copy codec parameters." << std::endl;
      return false;
    }

    // Set threads count and timebase for decoding acceleration
    codecCtx->thread_count =
        0; // FFmpeg decides automatically based on core count
    codecCtx->thread_type = FF_THREAD_FRAME;
    codecCtx->pkt_timebase = m_timeBase;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
      avcodec_free_context(&codecCtx);
      std::cerr << "Error: Failed to open software decoder." << std::endl;
      return false;
    }

    std::cout << "Using decoder: " << codec->name << " (software)" << std::endl;
  }

  m_codecCtx = codecCtx;

  m_decodedFrame = av_frame_alloc();
  m_yuvFrame = av_frame_alloc();
  if (!m_decodedFrame || !m_yuvFrame) {
    std::cerr << "Error: Could not allocate video frames" << std::endl;
    return false;
  }

  // Allocate YUV buffer space matching the video dimensions
  m_yuvBufferSize = av_image_get_buffer_size(
      AV_PIX_FMT_YUV420P, m_codecCtx->width, m_codecCtx->height, 1);

  m_yuvBuffer = static_cast<uint8_t *>(av_malloc(m_yuvBufferSize));
  if (!m_yuvBuffer) {
    std::cerr << "Error: Could not allocate memory for YUV buffer" << std::endl;
    return false;
  }

  // Bind buffer segments to YUV frame planes (Y, U, V)
  int ret = av_image_fill_arrays(m_yuvFrame->data, m_yuvFrame->linesize,
                                 m_yuvBuffer, AV_PIX_FMT_YUV420P,
                                 m_codecCtx->width, m_codecCtx->height, 1);

  if (ret < 0) {
    std::cerr << "Error: Could not associate YUV buffer to frame data planes"
              << std::endl;
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
  if (!m_codecCtx) {
    return false;
  }
  if (m_flushRequested) {
    if (m_decodedFrame) {
      // Drain the decoder before flushing to release hardware references.
      // Only AVERROR_EOF means draining is actually finished: async
      // hardware wrappers (e.g. QSV) can return EAGAIN transiently while a
      // decode is still in flight, and treating that as "done" leaves a
      // buffered frame from before the seek to leak through later, landing
      // a catch-up on the wrong target. Retry EAGAIN briefly; give up on a
      // real error so a genuinely broken decoder can't hang the seek.
      avcodec_send_packet(m_codecCtx, nullptr);
      int drainRet;
      int drainEagainRetries = 0;
      while ((drainRet = avcodec_receive_frame(m_codecCtx, m_decodedFrame)) !=
             AVERROR_EOF) {
        if (drainRet >= 0) {
          av_frame_unref(m_decodedFrame);
          drainEagainRetries = 0;
          continue;
        }
        if (drainRet == AVERROR(EAGAIN) && drainEagainRetries < 50) {
          drainEagainRetries++;
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        break; // real error, or drain never completed -- stop waiting
      }
      avcodec_flush_buffers(m_codecCtx);
    }
    m_flushRequested = false;
    m_currentFramePts = 0.0;
    // A freshly flushed decoder legitimately needs several packets before it
    // produces a frame again; don't let that look like a stuck hardware
    // decoder and trigger a needless software fallback after a seek.
    m_consecutiveEagainCount = 0;
  }

  AVPacket *packet = nullptr;
  while (true) {
    // 1. Attempt to receive a decoded frame from the codec's internal buffers
    int ret = avcodec_receive_frame(m_codecCtx, m_decodedFrame);
    if (ret >= 0) {
      m_consecutiveEagainCount = 0;
      m_hardwareRecoveryAttempts =
          0; // a real frame proves the session is healthy
      // We have successfully decoded a frame.
      // Compute the Presentation Timestamp (PTS) in seconds relative to the
      // start of the stream
      if (m_decodedFrame->pts != AV_NOPTS_VALUE) {
        m_currentFramePts =
            (m_decodedFrame->pts - m_startTime) * av_q2d(m_timeBase);
      } else if (m_decodedFrame->pkt_dts != AV_NOPTS_VALUE) {
        m_currentFramePts =
            (m_decodedFrame->pkt_dts - m_startTime) * av_q2d(m_timeBase);
      }
      return true;
    }

    if (ret == AVERROR_EOF) {
      return false;
    }

    // If the decoder needs more packet data (EAGAIN), fetch next packet from
    // the queue
    if (ret == AVERROR(EAGAIN)) {
      if (!packet) {
        if (!m_queue.try_pop(packet)) {
          return false; // Queue empty/aborted, do not block main thread!
        }

        // Only count EAGAINs where we actually feed the decoder a packet.
        // An empty packet queue (startup, EOF, seek) is not a stuck decoder.
        m_consecutiveEagainCount++;
        if (m_consecutiveEagainCount > 64 &&
            isHardwareDecoder(m_codecCtx->codec)) {
          std::cerr
              << "Hardware decoder " << m_codecCtx->codec->name
              << " stuck (no frames decoded after 64 packets). Recovering..."
              << std::endl;
          if (!recoverHardwareDecoder()) {
            av_packet_free(&packet);
            return false;
          }
        }
      }

      ret = avcodec_send_packet(m_codecCtx, packet);
      if (ret == AVERROR(EAGAIN)) {
        // Not a failure: the decoder's internal buffer (or, for async
        // hardware decoders, its surface pool) is full. Its own contract
        // guarantees a frame is waiting to be drained -- loop back to
        // receive_frame() and resend this same packet once space frees up.
        continue;
      }
      if (ret < 0 && isHardwareDecoder(m_codecCtx->codec)) {
        std::cerr << "Hardware decoder " << m_codecCtx->codec->name
                  << " failed on send. Recovering..." << std::endl;
        if (recoverHardwareDecoder()) {
          ret = avcodec_send_packet(m_codecCtx, packet);
        }
      }
      av_packet_free(&packet); // Release packet wrapper
      packet = nullptr;
      if (ret < 0) {
        return false; // Critical send error
      }
    } else {
      // Unhandled decoding error
      if (isHardwareDecoder(m_codecCtx->codec)) {
        std::cerr << "Hardware decoder " << m_codecCtx->codec->name
                  << " failed on receive (" << ret << "). Recovering..."
                  << std::endl;
        if (recoverHardwareDecoder()) {
          continue; // retry receive/send (including any pending packet) on the
                    // new context
        }
      }
      if (packet) {
        av_packet_free(&packet);
      }
      return false;
    }
  }
}

bool VideoDecoder::convertFrame() {
  if (!m_codecCtx || !m_decodedFrame || m_decodedFrame->width <= 0 ||
      m_decodedFrame->height <= 0) {
    return false;
  }

  AVFrame *srcFrame = m_decodedFrame;
  AVFrame *tempCpuFrame = nullptr;

  // Check if the decoded frame is a hardware surface frame
  if (m_decodedFrame->hw_frames_ctx ||
      isHardwarePixelFormat(
          static_cast<AVPixelFormat>(m_decodedFrame->format))) {
    tempCpuFrame = av_frame_alloc();
    if (!tempCpuFrame) {
      return false;
    }
    int err = av_hwframe_transfer_data(tempCpuFrame, m_decodedFrame, 0);
    if (err < 0) {
      av_frame_free(&tempCpuFrame);
      std::cerr << "Error: Failed to transfer hardware frame to CPU: " << err
                << std::endl;
      if (fallbackToSoftware()) {
        // Fallback succeeded, print message and let next frames decode in
        // software
      }
      return false;
    }
    av_frame_copy_props(tempCpuFrame, m_decodedFrame);
    srcFrame = tempCpuFrame;
  }

  bool useNative = (srcFrame->format == AV_PIX_FMT_YUV420P ||
                    srcFrame->format == AV_PIX_FMT_YUVJ420P ||
                    srcFrame->format == AV_PIX_FMT_NV12 ||
                    srcFrame->format == AV_PIX_FMT_NV21);

  if (useNative) {
    // Keep track of the format/resolution for native frames
    m_allocatedWidth = srcFrame->width;
    m_allocatedHeight = srcFrame->height;
    m_allocatedFormat = static_cast<AVPixelFormat>(srcFrame->format);

    av_frame_unref(m_yuvFrame);

    // Support dummy/mock frames in unit tests that have no buffers allocated
    if (!srcFrame->buf[0] && !srcFrame->data[0]) {
      m_yuvFrame->width = srcFrame->width;
      m_yuvFrame->height = srcFrame->height;
      m_yuvFrame->format = srcFrame->format;
      if (tempCpuFrame) {
        av_frame_free(&tempCpuFrame);
      }
      av_frame_unref(m_decodedFrame);
      return true;
    }

    int err = av_frame_ref(m_yuvFrame, srcFrame);
    if (err < 0) {
      std::cerr << "Error: Failed to reference native frame: " << err
                << std::endl;
      if (tempCpuFrame) {
        av_frame_free(&tempCpuFrame);
      }
      return false;
    }

    if (tempCpuFrame) {
      av_frame_free(&tempCpuFrame);
    }
    av_frame_unref(m_decodedFrame);
    return true;
  }

  // Fallback path: use sws_scale to convert to YUV420P
  // Check if the source frame's dimensions or format changed dynamically
  if (srcFrame->width != m_allocatedWidth ||
      srcFrame->height != m_allocatedHeight ||
      srcFrame->format != m_allocatedFormat) {

    std::cout << "Dynamic resolution or format change detected in fallback: "
              << m_allocatedWidth << "x" << m_allocatedHeight << " (fmt "
              << m_allocatedFormat << ") -> " << srcFrame->width << "x"
              << srcFrame->height << " (fmt " << srcFrame->format << ")"
              << std::endl;

    // Free old scaling context
    if (m_swsCtx) {
      sws_freeContext(m_swsCtx);
      m_swsCtx = nullptr;
    }

    // Only recreate YUV buffer if dimensions actually changed
    if (srcFrame->width != m_allocatedWidth ||
        srcFrame->height != m_allocatedHeight) {
      if (m_yuvBuffer) {
        av_free(m_yuvBuffer);
        m_yuvBuffer = nullptr;
      }

      // Reallocate YUV buffer for the new dimensions
      m_yuvBufferSize = av_image_get_buffer_size(
          AV_PIX_FMT_YUV420P, srcFrame->width, srcFrame->height, 1);
      m_yuvBuffer = static_cast<uint8_t *>(av_malloc(m_yuvBufferSize));
      if (!m_yuvBuffer) {
        std::cerr
            << "Error: Could not allocate YUV buffer after resolution change"
            << std::endl;
        if (tempCpuFrame) {
          av_frame_free(&tempCpuFrame);
        }
        return false;
      }

      // Associate the new buffer with the YUV frame
      int ret = av_image_fill_arrays(m_yuvFrame->data, m_yuvFrame->linesize,
                                     m_yuvBuffer, AV_PIX_FMT_YUV420P,
                                     srcFrame->width, srcFrame->height, 1);
      if (ret < 0) {
        std::cerr
            << "Error: Could not associate YUV buffer on resolution change"
            << std::endl;
        av_free(m_yuvBuffer);
        m_yuvBuffer = nullptr;
        if (tempCpuFrame) {
          av_frame_free(&tempCpuFrame);
        }
        return false;
      }
    }

    m_allocatedWidth = srcFrame->width;
    m_allocatedHeight = srcFrame->height;
    m_allocatedFormat = static_cast<AVPixelFormat>(srcFrame->format);
  }

  if (!m_swsCtx) {
    m_swsCtx =
        sws_getContext(m_allocatedWidth, m_allocatedHeight,
                       static_cast<AVPixelFormat>(srcFrame->format),
                       m_allocatedWidth, m_allocatedHeight, AV_PIX_FMT_YUV420P,
                       SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_swsCtx) {
      std::cerr
          << "Error: Could not allocate scaling context on resolution change"
          << std::endl;
      if (tempCpuFrame) {
        av_frame_free(&tempCpuFrame);
      }
      return false;
    }
  }

  // Ensure m_yuvFrame points to our allocated m_yuvBuffer and is set to
  // AV_PIX_FMT_YUV420P
  if (m_yuvFrame->data[0] != m_yuvBuffer ||
      m_yuvFrame->format != AV_PIX_FMT_YUV420P) {
    av_frame_unref(m_yuvFrame);
    m_yuvFrame->format = AV_PIX_FMT_YUV420P;
    m_yuvFrame->width = m_allocatedWidth;
    m_yuvFrame->height = m_allocatedHeight;
    av_image_fill_arrays(m_yuvFrame->data, m_yuvFrame->linesize, m_yuvBuffer,
                         AV_PIX_FMT_YUV420P, m_allocatedWidth,
                         m_allocatedHeight, 1);
  }

  sws_scale(m_swsCtx, srcFrame->data, srcFrame->linesize, 0, m_allocatedHeight,
            m_yuvFrame->data, m_yuvFrame->linesize);
  if (tempCpuFrame) {
    av_frame_free(&tempCpuFrame);
  }
  av_frame_unref(m_decodedFrame);
  return true;
}

bool VideoDecoder::isHardwareDecoder(const AVCodec *codec) noexcept {
  if (!codec || !codec->name)
    return false;
  std::string name(codec->name);
  return name.find("_qsv") != std::string::npos ||
         name.find("_cuvid") != std::string::npos ||
         name.find("_d3d11va") != std::string::npos ||
         name.find("_dxva2") != std::string::npos ||
         name.find("_vaapi") != std::string::npos ||
         name.find("_v4l2m2m") != std::string::npos ||
         name.find("_videotoolbox") != std::string::npos;
}

bool VideoDecoder::isHardwarePixelFormat(AVPixelFormat fmt) {
  return fmt == AV_PIX_FMT_VDPAU || fmt == AV_PIX_FMT_DXVA2_VLD ||
         fmt == AV_PIX_FMT_VAAPI || fmt == AV_PIX_FMT_D3D11VA_VLD ||
         fmt == AV_PIX_FMT_D3D11 || fmt == AV_PIX_FMT_DRM_PRIME ||
         fmt == AV_PIX_FMT_CUDA || fmt == AV_PIX_FMT_QSV ||
         fmt == AV_PIX_FMT_VIDEOTOOLBOX || fmt == AV_PIX_FMT_MEDIACODEC ||
         fmt == AV_PIX_FMT_OPENCL || fmt == AV_PIX_FMT_VULKAN;
}

namespace {
// A session that fails again immediately after being reopened isn't
// recovering -- cap how many times we retry before giving up on hardware
// for this playback session, so a persistently broken decoder can't loop
// forever re-opening itself instead of falling back to software.
constexpr int kMaxHardwareRecoveryAttempts = 2;
} // namespace

bool VideoDecoder::recoverHardwareDecoder() {
  // A genuine hardware decode failure was just observed (stuck, send error,
  // or receive error). Try a fresh session for the same hardware codec
  // first -- most failures of this kind (e.g. QSV's surface pool getting
  // into a bad state) are recoverable by starting over, and this lets
  // playback return to hardware instead of being stuck on software forever.
  // Only fall back to software if the hardware codec itself won't reopen,
  // or keeps failing right after being reopened.
  if (m_hardwareRecoveryAttempts < kMaxHardwareRecoveryAttempts) {
    m_hardwareRecoveryAttempts++;
    if (reopenHardwareDecoder()) {
      std::cout << "Recovered hardware decoder with a fresh session (attempt "
                << m_hardwareRecoveryAttempts << ")." << std::endl;
      return true;
    }
  }
  return fallbackToSoftware();
}

bool VideoDecoder::reopenHardwareDecoder() {
  if (!m_codecCtx || !m_codecCtx->codec || !m_codecCtx->codec->name)
    return false;

  // Re-resolve the canonical registered codec by name rather than reusing
  // m_codecCtx->codec directly: AVCodec* is meant to be treated as an opaque
  // singleton looked up from FFmpeg's codec list, not a pointer to carry
  // across a context free/realloc cycle.
  const AVCodec *codec = avcodec_find_decoder_by_name(m_codecCtx->codec->name);
  if (!codec)
    return false;

  // Free the old context first to ensure complete teardown of old threads and buffers
  avcodec_free_context(&m_codecCtx);
  if (m_decodedFrame) {
    av_frame_unref(m_decodedFrame);
  }
  if (m_yuvFrame) {
    av_frame_unref(m_yuvFrame);
  }

  AVCodecContext *ctx = avcodec_alloc_context3(codec);
  if (!ctx)
    return false;

  if (avcodec_parameters_to_context(ctx, m_codecParams) < 0) {
    avcodec_free_context(&ctx);
    return false;
  }

  ctx->thread_count = 1;
  ctx->thread_type = 0;
  ctx->pkt_timebase = m_timeBase;

  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    avcodec_free_context(&ctx);
    return false;
  }

  m_codecCtx = ctx;
  m_consecutiveEagainCount = 0; // Reset consecutive EAGAIN count on reopen
  return true;
}

bool VideoDecoder::fallbackToSoftware() {
  // Use m_codecParams->codec_id as fallback source in case m_codecCtx was already freed by reopenHardwareDecoder
  AVCodecID codecId = m_codecParams ? m_codecParams->codec_id : AV_CODEC_ID_H264;

  if (m_codecCtx) {
    avcodec_free_context(&m_codecCtx);
  }
  if (m_decodedFrame) {
    av_frame_unref(m_decodedFrame);
  }
  if (m_yuvFrame) {
    av_frame_unref(m_yuvFrame);
  }

  const AVCodec *softwareCodec = avcodec_find_decoder(codecId);
  if (!softwareCodec) {
    std::cerr << "Error: Software decoder not found during fallback."
              << std::endl;
    return false;
  }

  AVCodecContext *softwareCtx = avcodec_alloc_context3(softwareCodec);
  if (!softwareCtx) {
    std::cerr
        << "Error: Failed to allocate software codec context during fallback."
        << std::endl;
    return false;
  }

  if (avcodec_parameters_to_context(softwareCtx, m_codecParams) < 0) {
    avcodec_free_context(&softwareCtx);
    std::cerr << "Error: Failed to copy codec parameters during fallback."
              << std::endl;
    return false;
  }

  softwareCtx->thread_count = 0;
  softwareCtx->thread_type = FF_THREAD_FRAME;
  softwareCtx->pkt_timebase = m_timeBase;

  if (avcodec_open2(softwareCtx, softwareCodec, nullptr) < 0) {
    avcodec_free_context(&softwareCtx);
    std::cerr << "Error: Failed to open software decoder during fallback."
              << std::endl;
    return false;
  }

  m_codecCtx = softwareCtx;
  m_consecutiveEagainCount = 0;
  std::cout << "Successfully fell back to software decoder: "
            << softwareCodec->name << std::endl;
  return true;
}

std::string VideoDecoder::getPixelFormatName() const {
  if (m_allocatedFormat != AV_PIX_FMT_NONE) {
    const char *name = av_get_pix_fmt_name(m_allocatedFormat);
    if (name) {
      return std::string(name);
    }
  }
  return "unknown";
}
