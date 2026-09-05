#include "video/VideoDecoder.hpp"
#include "core/FFmpegCompat.hpp"
#include <chrono>
#include <iostream>
#include <thread>


VideoDecoder::VideoDecoder(AVCodecParameters *codecParams, AVRational timeBase,
                           int64_t startTime,
                           ThreadSafeQueue<AVPacket *> &queue,
                           MetricRing<256> &decodeTimeRing,
                           MetricRing<256> &convertTimeRing,
                           std::atomic<bool> &profilingEnabled)
    : m_codecParams(codecParams), m_codecCtx(nullptr), m_swsCtx(nullptr),
      m_queue(queue), m_timeBase(timeBase), m_startTime(startTime),
      m_decodedFrame(nullptr), m_yuvFrame(nullptr), m_yuvBuffer(nullptr),
      m_yuvBufferSize(0), m_allocatedWidth(0), m_allocatedHeight(0),
      m_allocatedFormat(AV_PIX_FMT_NONE), m_allocatedTargetWidth(0),
      m_allocatedTargetHeight(0), m_currentFramePts(0.0),
      m_flushRequested(false), m_startTimeSaved(false), m_seeking(false),
      m_consecutiveEagainCount(0), m_hardwareRecoveryAttempts(0),
      m_decodeTimeRing(decodeTimeRing),
      m_convertTimeRing(convertTimeRing),
      m_profilingEnabled(profilingEnabled),
      m_hasDecodeStart(false) {}

static MetricRing<256> g_dummyDecodeRing;
static MetricRing<256> g_dummyConvertRing;
static std::atomic<bool> g_dummyVideoDecoderProfilingEnabled{false};

VideoDecoder::VideoDecoder(AVCodecParameters* codecParams, AVRational timeBase,
                           int64_t startTime, ThreadSafeQueue<AVPacket*>& queue,
                           std::atomic<uint64_t>* decodeTimeTracker)
    : VideoDecoder(codecParams, timeBase, startTime, queue,
                   g_dummyDecodeRing, g_dummyConvertRing, g_dummyVideoDecoderProfilingEnabled) {
    (void)decodeTimeTracker;
}

VideoDecoder::~VideoDecoder() {
  releaseHdrContexts();
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

bool g_disableHardwareDecoders = false;

// Pulls the mastering-display and content-light metadata off the stream
// header. These live in the container (or the codec extradata) and are
// therefore available whatever decoder is used, unlike the per-frame side
// data a hardware decoder may not produce.
// The per-frame time budget the adaptive scaler measures against. Taken
// from the stream's declared frame rate; a stream that declares none
// leaves the budget at 0, which disables adaptation rather than guessing.
void VideoDecoder::computeFrameBudget() {
  m_frameBudgetMs = 0.0;

  // The demuxer's figure first; AVCodecParameters::framerate is often
  // left unset, so it is only a fallback.
  double fps = m_sourceFrameRate;
#if NAIKAV_HAVE_CODECPAR_FRAMERATE
  if (fps <= 0.0 && m_codecParams) {
    const AVRational fr = m_codecParams->framerate;
    if (fr.num > 0 && fr.den > 0) {
      fps = av_q2d(fr);
    }
  }
#endif
  if (fps > 1.0 && fps < 1000.0) {
    m_frameBudgetMs = 1000.0 / fps;
    std::cout << "Video frame budget: " << m_frameBudgetMs << " ms (" << fps
              << " fps)" << std::endl;
  }
}

void VideoDecoder::readStreamHdrMetadata() {
  m_streamMasteringNits = 0.0f;
  m_streamContentLightNits = 0.0f;
  if (!m_codecParams) {
    return;
  }
#if NAIKAV_HAVE_CODECPAR_SIDE_DATA
  for (int i = 0; i < m_codecParams->nb_coded_side_data; ++i) {
    const AVPacketSideData &sd = m_codecParams->coded_side_data[i];
    if (!sd.data) {
      continue;
    }
    if (sd.type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA &&
        sd.size >= sizeof(AVMasteringDisplayMetadata)) {
      const auto *meta =
          reinterpret_cast<const AVMasteringDisplayMetadata *>(sd.data);
      if (meta->has_luminance && meta->max_luminance.den != 0) {
        const double nits = av_q2d(meta->max_luminance);
        if (nits > 0.0) {
          m_streamMasteringNits = static_cast<float>(nits);
        }
      }
    } else if (sd.type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL &&
               sd.size >= sizeof(AVContentLightMetadata)) {
      const auto *meta =
          reinterpret_cast<const AVContentLightMetadata *>(sd.data);
      if (meta->MaxCLL > 0) {
        m_streamContentLightNits = static_cast<float>(meta->MaxCLL);
      }
    }
  }
  if (m_streamMasteringNits > 0.0f || m_streamContentLightNits > 0.0f) {
    std::cout << "Stream HDR metadata: mastering peak "
              << m_streamMasteringNits << " nits, MaxCLL "
              << m_streamContentLightNits << " nits" << std::endl;
  }
#endif
}

bool VideoDecoder::init() {
  readStreamHdrMetadata();
  computeFrameBudget();
  AVCodecContext *codecCtx = nullptr;
  const AVCodec *codec = nullptr;

  const char **candidates = nullptr;
  size_t numCandidates = 0;

  const char *h264Candidates[] = {
#ifdef _WIN32
      "h264_d3d11va", "h264_dxva2", "h264_qsv", "h264_cuvid",
#endif
#ifdef __linux__
      "h264_vaapi", "h264_qsv", "h264_cuvid", "h264_v4l2m2m",
#endif
  };

  const char *hevcCandidates[] = {
#ifdef _WIN32
      "hevc_d3d11va", "hevc_dxva2", "hevc_qsv", "hevc_cuvid",
#endif
#ifdef __linux__
      "hevc_v4l2m2m", "hevc_vaapi", "hevc_qsv", "hevc_cuvid",
#endif
  };

  if (!g_disableHardwareDecoders) {
    if (m_codecParams->codec_id == AV_CODEC_ID_H264) {
      candidates = h264Candidates;
      numCandidates = sizeof(h264Candidates) / sizeof(h264Candidates[0]);
    } else if (m_codecParams->codec_id == AV_CODEC_ID_HEVC) {
      candidates = hevcCandidates;
      numCandidates = sizeof(hevcCandidates) / sizeof(hevcCandidates[0]);
    }
  }

  if (candidates && numCandidates > 0) {
    for (size_t i = 0; i < numCandidates; ++i) {
      const char *name = candidates[i];
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

      ctx->thread_count = 1;
      ctx->thread_type = 0;
      ctx->pkt_timebase = m_timeBase;

      // Try opening this decoder.
      if (avcodec_open2(ctx, candidate, nullptr) == 0) {
        std::string candName(name);
        bool isV4L2 = (candName.find("v4l2m2m") != std::string::npos);

        bool probeOk = true;
        // DRY-RUN CHECK: Validate hardware session initialization for non-v4l2 decoders.
        // For v4l2m2m (e.g. Raspberry Pi), sending raw extradata packets can cause
        // kernel ioctl(VIDIOC_DQBUF) to block in uninterruptible sleep (D-state).
        if (!isV4L2) {
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
          probeOk = sendOk && receiveOk;
        }

        if (probeOk) {
          codec = candidate;
          codecCtx = ctx;
          avcodec_flush_buffers(codecCtx); // reset internal state after probe
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
  m_allocatedTargetWidth = m_codecCtx->width;
  m_allocatedTargetHeight = m_codecCtx->height;

  std::cout << "Video initialized successfully. Resolution: "
            << m_codecCtx->width << "x" << m_codecCtx->height << std::endl;
  return true;
}

void VideoDecoder::flush() {
  m_flushRequested = true;
  m_currentFramePts.store(0.0, std::memory_order_relaxed);
  m_seeking = true;
  m_hasDecodeStart = false;
  // The pipeline no longer holds a converted frame, so the HUD's "tone
  // mapping active" line would otherwise keep describing the frame from
  // before the seek until a new one is converted. Cleared rather than
  // left stale so a seek into an SDR segment (or one that lands on a
  // frame the mapper rejects) does not read as still tone mapping.
  m_lastFrameToneMapped = false;
  // The frame after a seek has no relationship to the one before it, so
  // the smoothed dynamic peak must not carry across the jump.
  m_dynamicPeak.reset();
  m_lastFrameDynamicPeak = 0.0f;
  // A seek is followed by a catch-up burst whose timings say nothing
  // about steady-state playback, so the scale starts over.
  m_adaptiveScale.reset();
}

bool VideoDecoder::decodeNextFrame() {
  if (!m_codecCtx) {
    m_hasDecodeStart = false;
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
    m_currentFramePts.store(0.0, std::memory_order_relaxed);
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
      if (m_hasDecodeStart) {
        auto end = std::chrono::steady_clock::now();
        float us = static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(end - m_decodeStart).count());
        m_decodeTimeRing.record(us);
        m_hasDecodeStart = false;
      }
      // Compute the Presentation Timestamp (PTS) in seconds relative to the
      // start of the stream
      if (m_decodedFrame->pts != AV_NOPTS_VALUE) {
        m_currentFramePts.store(
            (m_decodedFrame->pts - m_startTime) * av_q2d(m_timeBase), std::memory_order_relaxed);
      } else if (m_decodedFrame->pkt_dts != AV_NOPTS_VALUE) {
        m_currentFramePts.store(
            (m_decodedFrame->pkt_dts - m_startTime) * av_q2d(m_timeBase), std::memory_order_relaxed);
      }
      return true;
    }

    if (ret == AVERROR_EOF) {
      m_hasDecodeStart = false;
      return false;
    }

    // If the decoder needs more packet data (EAGAIN), fetch next packet from
    // the queue
    if (ret == AVERROR(EAGAIN)) {
      if (!packet) {
        if (!m_queue.try_pop(packet)) {
          m_hasDecodeStart = false;
          return false; // Queue empty/aborted, do not block main thread!
        }

        if (m_seekGeneration) {
          const uint64_t currentGeneration =
              m_seekGeneration->load(std::memory_order_relaxed);
          const uint64_t packetGeneration =
              naikavPacketGeneration(packet, currentGeneration);
          if (packetGeneration != currentGeneration) {
            // This packet's data was read from before the most recent seek
            // (tagged by the demuxer at read time -- see Demuxer.hpp's
            // m_seekGeneration comment). Drop it here, before it ever
            // reaches the codec, instead of letting a stale frame come out
            // the other end. Loop back around to try the next packet.
            av_packet_free(&packet);
            packet = nullptr;
            continue;
          }
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
            m_hasDecodeStart = false;
            return false;
          }
        }
      }

      if (!m_hasDecodeStart && m_profilingEnabled.load(std::memory_order_relaxed)) {
        m_decodeStart = std::chrono::steady_clock::now();
        m_hasDecodeStart = true;
      }
      ret = avcodec_send_packet(m_codecCtx, packet);
      if (ret == AVERROR(EAGAIN)) {
        // Not a failure: the decoder's internal buffer (or, for async
        // hardware decoders, its surface pool) is full. Its own contract
        // guarantees a frame is waiting to be drained -- loop back to
        // receive_frame() and resend this same packet once space frees up.
        //
        // Do NOT sleep or trigger recovery here. Send-EAGAIN is normal
        // steady-state backpressure whenever the input queue saturates
        // (typical for hardware decoders with a 4-8 slot surface pool).
        // The genuine "stuck decoder" case -- packets going in but no
        // frames coming out -- is already tracked by m_consecutiveEagainCount
        // above, which only counts EAGAINs where we actually fed a new
        // packet, and triggers recoverHardwareDecoder() after 64 of them.
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
        m_hasDecodeStart = false;
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
      m_hasDecodeStart = false;
      return false;
    }
  }
}

naikav::video::HdrTransfer VideoDecoder::hdrTransferOf(const AVFrame *frame) {
  if (!frame) {
    return naikav::video::HdrTransfer::None;
  }
  switch (frame->color_trc) {
  case AVCOL_TRC_SMPTE2084:
    return naikav::video::HdrTransfer::PQ;
  case AVCOL_TRC_ARIB_STD_B67:
    return naikav::video::HdrTransfer::HLG;
  default:
    return naikav::video::HdrTransfer::None;
  }
}

float VideoDecoder::masteringPeakNits(const AVFrame *frame) {
  if (!frame) {
    return 0.0f;
  }
  for (int i = 0; i < frame->nb_side_data; ++i) {
    const AVFrameSideData *sd = frame->side_data[i];
    if (!sd || sd->type != AV_FRAME_DATA_MASTERING_DISPLAY_METADATA ||
        !sd->data) {
      continue;
    }
    const auto *meta =
        reinterpret_cast<const AVMasteringDisplayMetadata *>(sd->data);
    // has_luminance guards against metadata that only carries primaries;
    // a zero denominator would otherwise divide by zero in av_q2d.
    if (!meta->has_luminance || meta->max_luminance.den == 0) {
      continue;
    }
    const double nits = av_q2d(meta->max_luminance);
    if (nits > 0.0) {
      return static_cast<float>(nits);
    }
  }
  return 0.0f;
}

float VideoDecoder::dynamicPeakNits(const AVFrame *frame) {
  if (!frame) {
    return 0.0f;
  }
  for (int i = 0; i < frame->nb_side_data; ++i) {
    const AVFrameSideData *sd = frame->side_data[i];
    if (!sd || !sd->data) {
      continue;
    }

#if NAIKAV_HAVE_HDR10PLUS
    if (sd->type == AV_FRAME_DATA_DYNAMIC_HDR_PLUS) {
      const auto *hdr = reinterpret_cast<const AVDynamicHDRPlus *>(sd->data);
      if (hdr->num_windows < 1) {
        continue;
      }
      // Window 0 is the frame-wide window; the optional extra windows
      // describe sub-regions, which only a spatially varying tone curve
      // could use. maxscl is the largest linearized R, G or B in the
      // window, normalized so 1.0 is PQ's 10000-nit system peak.
      const AVHDRPlusColorTransformParams &w = hdr->params[0];
      double maxScl = 0.0;
      for (int c = 0; c < 3; ++c) {
        if (w.maxscl[c].den != 0) {
          maxScl = std::max(maxScl, av_q2d(w.maxscl[c]));
        }
      }
      if (maxScl > 0.0) {
        return static_cast<float>(std::min(maxScl, 1.0) * 10000.0);
      }
      // Some streams leave maxscl zeroed and carry only average_maxrgb.
      if (w.average_maxrgb.den != 0) {
        const double avg = av_q2d(w.average_maxrgb);
        if (avg > 0.0) {
          return static_cast<float>(std::min(avg, 1.0) * 10000.0);
        }
      }
      continue;
    }
#endif

#if NAIKAV_HAVE_DOVI_LEVELS
    if (sd->type == AV_FRAME_DATA_DOVI_METADATA) {
      const auto *dovi = reinterpret_cast<const AVDOVIMetadata *>(sd->data);
      // L1 is the per-frame luminance block: min/max/avg of the frame,
      // as 12-bit PQ code values.
      const AVDOVIDmData *dm = av_dovi_find_level(dovi, 1);
      if (!dm) {
        continue;
      }
      const double code = static_cast<double>(dm->l1.max_pq) / 4095.0;
      if (code > 0.0) {
        return static_cast<float>(naikav::video::pqEotf(code) * 10000.0);
      }
    }
#endif
  }
  return 0.0f;
}

float VideoDecoder::contentLightPeakNits(const AVFrame *frame) {
  if (!frame) {
    return 0.0f;
  }
  for (int i = 0; i < frame->nb_side_data; ++i) {
    const AVFrameSideData *sd = frame->side_data[i];
    if (!sd || sd->type != AV_FRAME_DATA_CONTENT_LIGHT_LEVEL || !sd->data) {
      continue;
    }
    const auto *meta = reinterpret_cast<const AVContentLightMetadata *>(sd->data);
    // MaxCLL is already in nits (unlike the mastering metadata's
    // rationals), and 0 is the encoder's "unknown", not a real peak.
    if (meta->MaxCLL > 0) {
      return static_cast<float>(meta->MaxCLL);
    }
  }
  return 0.0f;
}

void VideoDecoder::releaseHdrContexts() {
  if (m_hdrToRgbCtx) {
#if NAIKAV_HAVE_SWS_THREADED
    sws_free_context(&m_hdrToRgbCtx);
#else
    sws_freeContext(m_hdrToRgbCtx);
    m_hdrToRgbCtx = nullptr;
#endif
  }
  if (m_hdrRgbFrame) {
    av_frame_free(&m_hdrRgbFrame);
  }
  m_hdrTargetWidth = 0;
  m_hdrTargetHeight = 0;
}

// HDR -> SDR path. Runs instead of the plain sws_scale fallback whenever
// the source carries a PQ or HLG transfer function.
//
// Two stages, because tone mapping only means anything in linear RGB:
//   1. swscale unpacks (and rescales) the source's HDR YUV into 16-bit
//      RGB. Still HDR-encoded -- this stage only undoes the YUV packing
//      and the chroma subsampling, not the transfer function.
//   2. ToneMapper converts transfer function and gamut, writing 8-bit
//      BT.709 RGB straight into the output frame.
//
// The output stays RGB24 rather than being packed back into YUV420P.
// The renderer uploads it as an RGB texture (see main.cpp), so a third
// full-frame conversion whose only purpose was to give the GPU
// something to convert back to RGB is gone: at 4K that pass measured
// 47 ms/frame on its own.
//
// Scaling happens in stage 1, so selecting a lower playback resolution
// also shrinks the per-pixel tone mapping work rather than adding to it.
bool VideoDecoder::toneMapFrame(const AVFrame *srcFrame, int targetW,
                                int targetH,
                                const naikav::video::HdrToneMapSettings &settings) {
  if (!srcFrame || targetW <= 0 || targetH <= 0) {
    return false;
  }

  const naikav::video::HdrTransfer transfer = hdrTransferOf(srcFrame);
  if (transfer == naikav::video::HdrTransfer::None) {
    return false;
  }

  // An explicit source peak in the settings wins; otherwise follow this
  // frame's dynamic metadata if it has any, reconciled against the
  // mastering-display peak and MaxCLL, and fall back to the tone mapper's
  // default when the file carries none of it.
  // Frame side data first, stream header second -- a hardware decoder
  // often strips the former, and mapping a 4000-nit grade as though it
  // were the 1000-nit default washes the highlights out.
  float masteringNits = masteringPeakNits(srcFrame);
  if (masteringNits <= 0.0f) {
    masteringNits = m_streamMasteringNits;
  }
  if (masteringNits <= 0.0f) {
    masteringNits = m_probedMasteringNits;
  }
  float contentLightNits = contentLightPeakNits(srcFrame);
  if (contentLightNits <= 0.0f) {
    contentLightNits = m_streamContentLightNits;
  }
  if (contentLightNits <= 0.0f) {
    contentLightNits = m_probedContentLightNits;
  }

  float dynamicPeak = 0.0f;
  if (settings.useDynamicMetadata && settings.sourcePeakNits <= 0.0f) {
    dynamicPeak = m_dynamicPeak.update(dynamicPeakNits(srcFrame));
  } else {
    // Otherwise the tracker must not carry stale smoothing into a later
    // frame if the setting is turned back on mid-playback.
    m_dynamicPeak.reset();
  }
  m_lastFrameDynamicPeak = dynamicPeak;

  const float srcPeak = selectSourcePeakNits(settings.sourcePeakNits,
                                            masteringNits, contentLightNits,
                                            dynamicPeak);
  if (!m_toneMapper.configure(transfer, srcPeak, settings.targetPeakNits)) {
    return false;
  }

  // sws_alloc_context() rather than sws_getContext(): the latter has no
  // way to set `threads`, and swscale only slices a conversion across
  // cores when it has a thread count and is driven through
  // sws_scale_frame(). Single-threaded, this unpack measured 95 ms on a
  // 4K frame -- more than the tone mapper itself.
#if NAIKAV_HAVE_SWS_THREADED
  if (!m_hdrToRgbCtx) {
    m_hdrToRgbCtx = sws_alloc_context();
    if (!m_hdrToRgbCtx) {
      std::cerr << "Error: Could not allocate HDR unpack context" << std::endl;
      return false;
    }
    // 0 means "one thread per core". The tone mapper below picks its own
    // worker count the same way; the two never run at the same time, so
    // they are not competing for the machine.
    m_hdrToRgbCtx->threads = 0;
  }

  {
    // The kernel follows the direction of the resample. At native
    // resolution nothing is scaled but the chroma planes, and bilinear is
    // the standard upsample there -- nearest measured only ~8 ms/frame
    // cheaper on a 4K frame, which buys nothing at a frame rate that is
    // unwatchable either way, in exchange for visible chroma blocking on
    // saturated edges. Downscaling -- the resolution selector's whole
    // purpose on a 4K source -- wants an area average, which antialiases
    // better than bicubic and measured ~16 ms/frame cheaper than it at
    // 4K -> 1080p. Bicubic is kept for upscaling, where an area average
    // has nothing to average over.
    if (targetW == srcFrame->width && targetH == srcFrame->height) {
      m_hdrToRgbCtx->flags = SWS_BILINEAR;
    } else if (targetW <= srcFrame->width && targetH <= srcFrame->height) {
      m_hdrToRgbCtx->flags = SWS_AREA;
    } else {
      m_hdrToRgbCtx->flags = SWS_BICUBIC;
    }
  }
#else
  // Legacy swscale (pre-FFmpeg 7.1, which is what a Raspberry Pi's system
  // FFmpeg still is). The context is opaque here, so the kernel goes in
  // through sws_getCachedContext() and there is no way to ask for more
  // than one thread -- this conversion is single-threaded, and on a 4K
  // frame that is the difference between roughly 27 ms and 95 ms. The
  // picture is identical; only the speed differs.
  int legacyFlags;
  if (targetW == srcFrame->width && targetH == srcFrame->height) {
    legacyFlags = SWS_BILINEAR;
  } else if (targetW <= srcFrame->width && targetH <= srcFrame->height) {
    legacyFlags = SWS_AREA;
  } else {
    legacyFlags = SWS_BICUBIC;
  }
#endif

  // In dynamic mode swscale takes every colour property off the frames
  // rather than from the context, so an HDR stream that leaves its
  // matrix unspecified would be unpacked with BT.601 coefficients and
  // arrive at the tone mapper with every colour already skewed. A PQ or
  // HLG transfer function means BT.2020 in practice; say so explicitly
  // on a reference rather than trusting the tag to be present. (The old
  // code did the same job with sws_setColorspaceDetails, which the
  // dynamic API no longer consults.)
  const AVFrame *unpackSrc = srcFrame;
  AVFrame *taggedSrc = nullptr;
  if (srcFrame->colorspace == AVCOL_SPC_UNSPECIFIED ||
      srcFrame->color_primaries == AVCOL_PRI_UNSPECIFIED) {
    taggedSrc = av_frame_alloc();
    if (taggedSrc && av_frame_ref(taggedSrc, srcFrame) >= 0) {
      if (taggedSrc->colorspace == AVCOL_SPC_UNSPECIFIED) {
        taggedSrc->colorspace = AVCOL_SPC_BT2020_NCL;
      }
      if (taggedSrc->color_primaries == AVCOL_PRI_UNSPECIFIED) {
        taggedSrc->color_primaries = AVCOL_PRI_BT2020;
      }
      unpackSrc = taggedSrc;
    } else if (taggedSrc) {
      av_frame_free(&taggedSrc);
    }
  }

  if (m_hdrRgbFrame &&
      (m_hdrTargetWidth != targetW || m_hdrTargetHeight != targetH)) {
    av_frame_free(&m_hdrRgbFrame);
  }

  if (!m_hdrRgbFrame) {
    m_hdrRgbFrame = av_frame_alloc();
    if (!m_hdrRgbFrame) {
      av_frame_free(&taggedSrc);
      return false;
    }
    m_hdrRgbFrame->format = AV_PIX_FMT_RGB48;
    m_hdrRgbFrame->width = targetW;
    m_hdrRgbFrame->height = targetH;
    if (av_frame_get_buffer(m_hdrRgbFrame, 32) < 0) {
      std::cerr << "Error: Could not allocate HDR intermediate frame"
                << std::endl;
      av_frame_free(&m_hdrRgbFrame);
      av_frame_free(&taggedSrc);
      return false;
    }
    m_hdrTargetWidth = targetW;
    m_hdrTargetHeight = targetH;
  }

  // Tagged every frame, not once at allocation: this intermediate has to
  // describe what it actually holds -- RGB, still on the source's
  // primaries and still PQ/HLG-encoded -- or swscale will helpfully
  // insert a primaries/transfer conversion of its own on top of the one
  // the tone mapper is about to do.
  m_hdrRgbFrame->colorspace = AVCOL_SPC_RGB;
  m_hdrRgbFrame->color_range = AVCOL_RANGE_JPEG;
  m_hdrRgbFrame->color_primaries = unpackSrc->color_primaries;
  m_hdrRgbFrame->color_trc = unpackSrc->color_trc;

  // Stage 1: HDR YUV -> 16-bit RGB at the playback resolution.
#if NAIKAV_HAVE_SWS_THREADED
  const int scaled = sws_scale_frame(m_hdrToRgbCtx, m_hdrRgbFrame, unpackSrc);
  av_frame_free(&taggedSrc);
  if (scaled < 0) {
    std::cerr << "Error: HDR unpack to RGB48 failed: " << scaled << std::endl;
    return false;
  }
#else
  // Legacy path: the context carries the formats and sizes, and colour
  // handling is told to the context rather than read off the frames --
  // so the BT.2020 coefficients an unspecified-matrix HDR stream needs
  // go in through sws_setColorspaceDetails() here, which is what the
  // frame tagging above achieves on the newer API.
  m_hdrToRgbCtx = sws_getCachedContext(
      m_hdrToRgbCtx, unpackSrc->width, unpackSrc->height,
      static_cast<AVPixelFormat>(unpackSrc->format), targetW, targetH,
      AV_PIX_FMT_RGB48, legacyFlags, nullptr, nullptr, nullptr);
  if (!m_hdrToRgbCtx) {
    std::cerr << "Error: Could not allocate HDR unpack context" << std::endl;
    av_frame_free(&taggedSrc);
    return false;
  }
  {
    const int *invTable = sws_getCoefficients(SWS_CS_BT2020);
    const int *table = sws_getCoefficients(SWS_CS_ITU709);
    int srcRange = (unpackSrc->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    int dstRange = 1;  // RGB output is always full range
    int brightness = 0, contrast = 1 << 16, saturation = 1 << 16;
    sws_setColorspaceDetails(m_hdrToRgbCtx, invTable, srcRange, table,
                             dstRange, brightness, contrast, saturation);
  }
  const int scaled =
      sws_scale(m_hdrToRgbCtx, unpackSrc->data, unpackSrc->linesize, 0,
                unpackSrc->height, m_hdrRgbFrame->data, m_hdrRgbFrame->linesize);
  av_frame_free(&taggedSrc);
  if (scaled < 0) {
    std::cerr << "Error: HDR unpack to RGB48 failed: " << scaled << std::endl;
    return false;
  }
#endif

  // A fresh output frame per call, matching the plain rescale path
  // below: the decoded-frame queue holds references to what came before,
  // so the buffer the mapper writes into cannot be recycled while a
  // previously delivered frame is still in flight.
  AVFrame *outFrame = av_frame_alloc();
  if (!outFrame) {
    return false;
  }
  outFrame->format = AV_PIX_FMT_RGB24;
  outFrame->width = targetW;
  outFrame->height = targetH;
  outFrame->pts = srcFrame->pts;
  outFrame->pkt_dts = srcFrame->pkt_dts;
  // Deliberately NOT copied from the source: after tone mapping this
  // really is BT.709 SDR, and mislabeling it BT.2020/PQ here would make
  // the renderer apply a second, wrong conversion.
  outFrame->color_range = AVCOL_RANGE_JPEG;
  outFrame->colorspace = AVCOL_SPC_RGB;
  outFrame->color_primaries = AVCOL_PRI_BT709;
  outFrame->color_trc = AVCOL_TRC_BT709;

  if (av_frame_get_buffer(outFrame, 32) < 0) {
    std::cerr << "Error: Could not allocate tone mapped output frame"
              << std::endl;
    av_frame_free(&outFrame);
    return false;
  }

  // Stage 2: the actual tone map, straight into the output frame.
  m_toneMapper.process(m_hdrRgbFrame->data[0], m_hdrRgbFrame->linesize[0],
                       outFrame->data[0], outFrame->linesize[0], targetW,
                       targetH);

  av_frame_unref(m_yuvFrame);
  av_frame_move_ref(m_yuvFrame, outFrame);
  av_frame_free(&outFrame);
  return true;
}

bool VideoDecoder::convertFrame(ResolutionOption option,
                                naikav::video::HdrToneMapSettings toneMap,
                                int displayWidth, int displayHeight) {
  struct ConvertTimeTracker {
      std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      MetricRing<256>& ring;
      bool enabled;
      ConvertTimeTracker(MetricRing<256>& r, bool e) : ring(r), enabled(e) {}
      ~ConvertTimeTracker() {
          if (enabled) {
              auto end = std::chrono::steady_clock::now();
              float us = static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
              ring.record(us);
          }
      }
  } tracker_guard(m_convertTimeRing, m_profilingEnabled.load(std::memory_order_relaxed));

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

  // Calculate the aspect-ratio preserved target dimensions
  int nativeW = srcFrame->width;
  int nativeH = srcFrame->height;
  int targetW = nativeW;
  int targetH = nativeH;
  getTargetDimensions(option, nativeW, nativeH, targetW, targetH);

  bool isTargetOriginal = (targetW == nativeW && targetH == nativeH);

  // An HDR source has to go through the tone mapper regardless of pixel
  // format. This matters for the native-passthrough test below: 8-bit HLG
  // exists, and without this check such a frame would be handed straight
  // to the texture with its transfer function still applied.
  const bool wantsToneMap =
      toneMap.enabled &&
      hdrTransferOf(srcFrame) != naikav::video::HdrTransfer::None;

  bool useNative = isTargetOriginal && !wantsToneMap &&
                   (srcFrame->format == AV_PIX_FMT_YUV420P ||
                    srcFrame->format == AV_PIX_FMT_YUVJ420P ||
                    srcFrame->format == AV_PIX_FMT_NV12 ||
                    srcFrame->format == AV_PIX_FMT_NV21);

  if (wantsToneMap) {
    // Tone mapping is the most expensive stage left in the pipeline and
    // its cost is strictly per output pixel, so there is nothing to be
    // gained from mapping more pixels than the window can show -- the
    // letterbox blit would only throw them away again. Deliberately
    // scoped to this branch: the plain rescale path below has to produce
    // exactly the resolution the selector asked for, and its
    // native-passthrough decision was already made above using the
    // uncapped size.
    //
    // Known limitation: the new size only takes effect on the next
    // decoded frame, so enlarging the window while paused leaves the
    // frame on screen upscaled from the size it was mapped at until
    // playback resumes. Re-decoding on resize would mean a seek per
    // window-drag, which is the worse trade.
    capToDisplaySize(targetW, targetH, displayWidth, displayHeight);
    if (toneMap.adaptiveResolution) {
      applyAdaptiveScale(targetW, targetH, m_adaptiveScale.scale());
    } else {
      // So turning the setting back on mid-playback starts from a clean
      // slate rather than from whatever the scale had drifted to.
      m_adaptiveScale.reset();
    }

    const auto toneMapStart = std::chrono::steady_clock::now();
    if (toneMapFrame(srcFrame, targetW, targetH, toneMap)) {
      if (toneMap.adaptiveResolution) {
        const double toneMapMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - toneMapStart)
                .count();
        m_adaptiveScale.update(toneMapMs, m_frameBudgetMs);
      }
      // Report the source format, matching what the plain rescale path
      // below records -- the diagnostics HUD is describing the incoming
      // stream, not the intermediate buffers. Set only on success, since
      // the fallback path keys its context rebuild off this same field.
      m_allocatedFormat = static_cast<AVPixelFormat>(srcFrame->format);
      m_allocatedWidth = srcFrame->width;
      m_allocatedHeight = srcFrame->height;
      m_allocatedTargetWidth = targetW;
      m_allocatedTargetHeight = targetH;
      m_lastFrameToneMapped = true;
      if (tempCpuFrame) {
        av_frame_free(&tempCpuFrame);
      }
      av_frame_unref(m_decodedFrame);
      return true;
    }

    // Tone mapping failed (allocation, or an unsupported conversion).
    // Fall through to the plain rescale rather than dropping the frame:
    // a washed-out picture still beats no picture, and the HUD will say
    // tone mapping is inactive.
    std::cerr << "Warning: HDR tone mapping unavailable for this frame; "
                 "falling back to direct conversion"
              << std::endl;
  }
  m_lastFrameToneMapped = false;

  if (useNative) {
    // Keep track of the format/resolution for native frames
    m_allocatedWidth = srcFrame->width;
    m_allocatedHeight = srcFrame->height;
    m_allocatedFormat = static_cast<AVPixelFormat>(srcFrame->format);
    m_allocatedTargetWidth = targetW;
    m_allocatedTargetHeight = targetH;

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

  // Fallback / scaling path: use sws_scale to convert and resize to YUV420P
  if (srcFrame->width != m_allocatedWidth ||
      srcFrame->height != m_allocatedHeight ||
      srcFrame->format != m_allocatedFormat ||
      targetW != m_allocatedTargetWidth ||
      targetH != m_allocatedTargetHeight ||
      !m_swsCtx) {

    SwsContext *newSwsCtx =
        sws_getContext(srcFrame->width, srcFrame->height,
                       static_cast<AVPixelFormat>(srcFrame->format),
                       targetW, targetH, AV_PIX_FMT_YUV420P,
                       SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!newSwsCtx) {
      std::cerr << "Error: Could not allocate scaling context" << std::endl;
      if (tempCpuFrame) {
        av_frame_free(&tempCpuFrame);
      }
      return false;
    }

    if (m_swsCtx) {
      sws_freeContext(m_swsCtx);
    }
    m_swsCtx = newSwsCtx;
    m_allocatedWidth = srcFrame->width;
    m_allocatedHeight = srcFrame->height;
    m_allocatedFormat = static_cast<AVPixelFormat>(srcFrame->format);
    m_allocatedTargetWidth = targetW;
    m_allocatedTargetHeight = targetH;
  }

  m_yuvBufferSize = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, targetW, targetH, 1);

  AVFrame *scaledFrame = av_frame_alloc();
  if (!scaledFrame) {
    if (tempCpuFrame) {
      av_frame_free(&tempCpuFrame);
    }
    return false;
  }

  scaledFrame->format = AV_PIX_FMT_YUV420P;
  scaledFrame->width = targetW;
  scaledFrame->height = targetH;
  scaledFrame->pts = srcFrame->pts;
  scaledFrame->pkt_dts = srcFrame->pkt_dts;
  scaledFrame->color_range = srcFrame->color_range;
  scaledFrame->colorspace = srcFrame->colorspace;
  scaledFrame->color_primaries = srcFrame->color_primaries;
  scaledFrame->color_trc = srcFrame->color_trc;

  int ret = av_frame_get_buffer(scaledFrame, 32);
  if (ret < 0) {
    std::cerr << "Error: Could not allocate buffer for scaled frame: " << ret << std::endl;
    av_frame_free(&scaledFrame);
    if (tempCpuFrame) {
      av_frame_free(&tempCpuFrame);
    }
    return false;
  }

  sws_scale(m_swsCtx, srcFrame->data, srcFrame->linesize, 0, srcFrame->height,
            scaledFrame->data, scaledFrame->linesize);

  av_frame_unref(m_yuvFrame);
  av_frame_move_ref(m_yuvFrame, scaledFrame);
  av_frame_free(&scaledFrame);

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
  if (m_hardwareRecoveryAttempts < kMaxHardwareRecoveryAttempts) {
    m_hardwareRecoveryAttempts++;
    if (reopenHardwareDecoder()) {
      std::cout << "Recovered hardware decoder with a fresh session (attempt "
                << m_hardwareRecoveryAttempts << ")." << std::endl;
      return true;
    }
  }
  g_disableHardwareDecoders = true; // Permanently disable hardware decoders for session to prevent driver loops
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

ColorPipelineInfo VideoDecoder::getColorInfo() const {
  ColorPipelineInfo info;

  AVColorSpace cs = AVCOL_SPC_UNSPECIFIED;
  AVColorPrimaries cp = AVCOL_PRI_UNSPECIFIED;
  AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED;
  AVColorRange cr = AVCOL_RANGE_UNSPECIFIED;
  AVPixelFormat pixFmt = AV_PIX_FMT_NONE;

  if (m_decodedFrame && m_decodedFrame->width > 0) {
    cs = m_decodedFrame->colorspace;
    cp = m_decodedFrame->color_primaries;
    trc = m_decodedFrame->color_trc;
    cr = m_decodedFrame->color_range;
    pixFmt = static_cast<AVPixelFormat>(m_decodedFrame->format);
  } else if (m_codecCtx) {
    cs = m_codecCtx->colorspace;
    cp = m_codecCtx->color_primaries;
    trc = m_codecCtx->color_trc;
    cr = m_codecCtx->color_range;
    pixFmt = m_codecCtx->pix_fmt;
  } else if (m_codecParams) {
    cs = m_codecParams->color_space;
    cp = m_codecParams->color_primaries;
    trc = m_codecParams->color_trc;
    cr = m_codecParams->color_range;
    pixFmt = static_cast<AVPixelFormat>(m_codecParams->format);
  }

  if (m_allocatedFormat != AV_PIX_FMT_NONE) {
    pixFmt = m_allocatedFormat;
  }

  // 1. Color Space
  const char *csName = av_color_space_name(cs);
  info.colorSpace = (csName && cs != AVCOL_SPC_UNSPECIFIED) ? csName : "Unspecified";

  // 2. Color Primaries
  const char *cpName = av_color_primaries_name(cp);
  info.colorPrimaries = (cpName && cp != AVCOL_PRI_UNSPECIFIED) ? cpName : "Unspecified";

  // 3. Transfer Characteristic
  const char *trcName = av_color_transfer_name(trc);
  if (trcName && trc != AVCOL_TRC_UNSPECIFIED) {
    info.transferChar = trcName;
  } else {
    info.transferChar = "Unspecified";
  }

  // Friendly alias formatting for common TRCs
  if (trc == AVCOL_TRC_SMPTE2084) {
    info.transferChar = "PQ (ST 2084)";
  } else if (trc == AVCOL_TRC_ARIB_STD_B67) {
    info.transferChar = "HLG";
  } else if (trc == AVCOL_TRC_IEC61966_2_1) {
    info.transferChar = "sRGB";
  }

  // 4. Color Range
  if (cr == AVCOL_RANGE_MPEG) {
    info.colorRange = "Limited (16-235)";
  } else if (cr == AVCOL_RANGE_JPEG) {
    info.colorRange = "Full (0-255)";
  } else {
    info.colorRange = "Unspecified";
  }

  // 5. Pixel Format, Bit Depth & Chroma Subsampling
  if (pixFmt != AV_PIX_FMT_NONE) {
    const char *fmtName = av_get_pix_fmt_name(pixFmt);
    info.pixelFormat = fmtName ? fmtName : "Unknown";

    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(pixFmt);
    if (desc) {
      if (desc->nb_components > 0) {
        info.bitDepth = desc->comp[0].depth;
      }

      if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) {
        info.chromaSubsampling = "HW Surface";
      } else if (desc->nb_components == 1 || desc->nb_components == 2) {
        info.chromaSubsampling = "4:0:0 (Mono)";
      } else if (desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0) {
        info.chromaSubsampling = "4:4:4";
      } else if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0) {
        info.chromaSubsampling = "4:2:2";
      } else if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1) {
        info.chromaSubsampling = "4:2:0";
      } else if (desc->log2_chroma_w == 2 && desc->log2_chroma_h == 0) {
        info.chromaSubsampling = "4:1:1";
      }
    }
  }

  // 6. HDR Metadata Inspection
  info.isHDR = false;
  info.hdrType = "SDR";

  if (trc == AVCOL_TRC_SMPTE2084) {
    info.isHDR = true;
    info.hdrType = "HDR10 (PQ)";
  } else if (trc == AVCOL_TRC_ARIB_STD_B67) {
    info.isHDR = true;
    info.hdrType = "HLG";
  }

  if (m_decodedFrame) {
    for (int i = 0; i < m_decodedFrame->nb_side_data; i++) {
      const AVFrameSideData *sd = m_decodedFrame->side_data[i];
      if (!sd)
        continue;
      // The dynamic-metadata types below only exist on newer FFmpeg; on
      // an older one such a stream still decodes and tone maps, it just
      // reports as plain HDR10 rather than by name.
#if NAIKAV_HAVE_HDR10PLUS
      if (sd->type == AV_FRAME_DATA_DYNAMIC_HDR_PLUS) {
        info.isHDR = true;
        info.hdrType = "HDR10+";
      } else
#endif
#if NAIKAV_HAVE_DOVI_METADATA
      if (sd->type == AV_FRAME_DATA_DOVI_METADATA) {
        info.isHDR = true;
        info.hdrType = "Dolby Vision";
      } else
#endif
      if (sd->type == AV_FRAME_DATA_MASTERING_DISPLAY_METADATA &&
                 !info.isHDR) {
        info.isHDR = true;
        info.hdrType = "HDR10";
      }
    }
  }

  // 7. What the pipeline actually did with it. Reporting the source's HDR
  //    standard without this says nothing about whether the picture on
  //    screen was converted for an SDR display or just truncated.
  info.toneMapped = m_lastFrameToneMapped;
  if (m_lastFrameToneMapped && m_toneMapper.isReady()) {
    info.toneMapSourceNits = m_toneMapper.sourcePeakNits();
    info.toneMapTargetNits = m_toneMapper.targetPeakNits();
    info.toneMapDynamic = (m_lastFrameDynamicPeak > 0.0f);
    info.toneMapDynamicNits = m_lastFrameDynamicPeak;
  }

  return info;
}
