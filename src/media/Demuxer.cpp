#include "media/Demuxer.hpp"
#include "core/FFmpegCompat.hpp"
#include <cstdio>
#include <iostream>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstring>

namespace {
// Upper bound on how long the read loop will ever wait for room in a packet
// queue before giving up and dropping the oldest entry instead. Generous
// enough that it never engages under normal backpressure (decode keeps up
// with real-time content well within this window), but guarantees the
// single demuxer thread -- which feeds both streams -- can never be stuck
// indefinitely regardless of what stalls the consumer on the other end.
constexpr std::chrono::milliseconds kQueuePushTimeout{500};
} // namespace

Demuxer::Demuxer(const std::string& filename, 
                 ThreadSafeQueue<AVPacket*>& videoQueue, 
                 ThreadSafeQueue<AVPacket*>& audioQueue,
                 MetricRing<256>& demuxTimeRing,
                 std::atomic<bool>& profilingEnabled)
    : m_filename(filename),
      m_formatCtx(nullptr),
      m_videoStreamIdx(-1),
      m_audioStreamIdx(-1),
      m_videoCodecParams(nullptr),
      m_audioCodecParams(nullptr),
      m_videoTimeBase{0, 1},
      m_audioTimeBase{0, 1},
      m_startTimeUs(0),
      m_videoStartTime(0),
      m_audioStartTime(0),
      m_duration(0.0),
      m_videoQueue(videoQueue),
      m_audioQueue(audioQueue),
      m_running(false),
      m_seekRequested(false),
      m_seekTargetTime(0.0),
      m_eof(false),
      m_catchupMode(SeekCatchupMode::NONE),
      m_catchupTarget(0.0),
      m_demuxTimeRing(demuxTimeRing),
      m_profilingEnabled(profilingEnabled) {
}

static MetricRing<256> g_dummyDemuxRing;
static std::atomic<bool> g_dummyDemuxerProfilingEnabled{false};

Demuxer::Demuxer(const std::string& filename, 
                 ThreadSafeQueue<AVPacket*>& videoQueue, 
                 ThreadSafeQueue<AVPacket*>& audioQueue)
    : Demuxer(filename, videoQueue, audioQueue, g_dummyDemuxRing, g_dummyDemuxerProfilingEnabled) {
}

Demuxer::~Demuxer() {
    stop();
    if (m_formatCtx) {
        avformat_close_input(&m_formatCtx);
    }
}

// Recover the file's static HDR metadata by decoding a frame or two in
// software.
//
// Mastering-display luminance and MaxCLL usually reach the player as frame
// side data, put there by the decoder from the codec's SEI. A *hardware*
// decoder does not do that -- QSV hands back frames with no side data at
// all -- and not every container keeps a stream-level copy to fall back
// on: Matroska commonly keeps none, so a 4K HDR10 MKV played on the GPU
// arrives with nothing at all to tone map from and silently falls back to
// the generic 1000-nit default. On a grade mastered at 4000 nits whose
// MaxCLL is 683 that is simply the wrong curve.
//
// The metadata is static for the whole file, so one software decode at
// open time recovers it for every frame that follows, whatever decoder
// ends up doing the real work. Only runs when the stream declares an HDR
// transfer and the container offered nothing, so SDR files and
// well-populated containers pay nothing.
void Demuxer::probeHdrMetadata() {
    if (m_videoStreamIdx < 0 || !m_videoCodecParams || !m_formatCtx) {
        return;
    }
    const AVColorTransferCharacteristic trc = m_videoCodecParams->color_trc;
    if (trc != AVCOL_TRC_SMPTE2084 && trc != AVCOL_TRC_ARIB_STD_B67) {
        return;  // SDR: nothing to recover
    }
#if NAIKAV_HAVE_CODECPAR_SIDE_DATA
    // Already present at stream level? Then VideoDecoder reads it directly.
    // Where the codec parameters cannot carry side data at all (older
    // FFmpeg) there is nothing to check and the probe simply always runs,
    // which is the correct answer for those builds.
    for (int i = 0; i < m_videoCodecParams->nb_coded_side_data; ++i) {
        const AVPacketSideDataType t = m_videoCodecParams->coded_side_data[i].type;
        if (t == AV_PKT_DATA_MASTERING_DISPLAY_METADATA ||
            t == AV_PKT_DATA_CONTENT_LIGHT_LEVEL) {
            return;
        }
    }
#endif

    const AVCodec* swCodec = avcodec_find_decoder(m_videoCodecParams->codec_id);
    if (!swCodec) {
        return;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(swCodec);
    if (!ctx) {
        return;
    }
    if (avcodec_parameters_to_context(ctx, m_videoCodecParams) < 0) {
        avcodec_free_context(&ctx);
        return;
    }
    // One thread and no deep buffering: this wants the first frame out as
    // cheaply as possible, not throughput.
    ctx->thread_count = 1;
    if (avcodec_open2(ctx, swCodec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    // Bounded so a stream that never yields a decodable frame cannot turn
    // opening a file into an unbounded scan.
    constexpr int kMaxProbePackets = 48;
    int packetsRead = 0;
    bool done = false;

    while (pkt && frame && !done && packetsRead < kMaxProbePackets &&
           av_read_frame(m_formatCtx, pkt) >= 0) {
        if (pkt->stream_index != m_videoStreamIdx) {
            av_packet_unref(pkt);
            continue;
        }
        packetsRead++;
        if (avcodec_send_packet(ctx, pkt) >= 0) {
            while (avcodec_receive_frame(ctx, frame) >= 0) {
                for (int i = 0; i < frame->nb_side_data; ++i) {
                    const AVFrameSideData* sd = frame->side_data[i];
                    if (!sd || !sd->data) continue;
                    if (sd->type == AV_FRAME_DATA_MASTERING_DISPLAY_METADATA) {
                        const auto* md =
                            reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data);
                        if (md->has_luminance && md->max_luminance.den != 0) {
                            const double nits = av_q2d(md->max_luminance);
                            if (nits > 0.0) m_probedMasteringNits = static_cast<float>(nits);
                        }
                    } else if (sd->type == AV_FRAME_DATA_CONTENT_LIGHT_LEVEL) {
                        const auto* cl =
                            reinterpret_cast<const AVContentLightMetadata*>(sd->data);
                        if (cl->MaxCLL > 0) m_probedContentLightNits = static_cast<float>(cl->MaxCLL);
                    }
                }
                av_frame_unref(frame);
                // One decoded frame is enough: this metadata is static.
                done = true;
            }
        }
        av_packet_unref(pkt);
    }

    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    avcodec_free_context(&ctx);

    // Rewind: the real read loop must start at the beginning, not wherever
    // the probe left the file.
    av_seek_frame(m_formatCtx, -1, 0, AVSEEK_FLAG_BACKWARD);

    if (m_probedMasteringNits > 0.0f || m_probedContentLightNits > 0.0f) {
        std::cout << "Probed HDR metadata from bitstream: mastering peak "
                  << m_probedMasteringNits << " nits, MaxCLL "
                  << m_probedContentLightNits << " nits" << std::endl;
    }
}

bool Demuxer::open() {
    m_lastError.clear();

    // Open video file with protocol whitelist restrictions (local file and pipe only)
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file,pipe", 0);
    
    int ret = avformat_open_input(&m_formatCtx, m_filename.c_str(), nullptr, &options);
    av_dict_free(&options);

    if (ret < 0) {
        // av_strerror carries the reason that actually matters -- a
        // truncated download reports "moov atom not found", which is the
        // difference between "the player is broken" and "re-download the
        // file". AVERROR strings are short enough to put straight in front
        // of the user.
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        if (av_strerror(ret, errbuf, sizeof(errbuf)) < 0) {
            std::snprintf(errbuf, sizeof(errbuf), "error %d", ret);
        }
        m_lastError = errbuf;
        std::cerr << "Error: Could not open media file " << m_filename
                  << ": " << m_lastError << std::endl;
        return false;
    }

    // Enable fast seek using index tables instead of parsing frames
    m_formatCtx->flags |= AVFMT_FLAG_FAST_SEEK;

    // Retrieve stream info
    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        m_lastError = "could not read stream information (file may be truncated or corrupt)";
        std::cerr << "Error: Could not find stream information" << std::endl;
        return false;
    }

    // Find video, audio and subtitle streams
    m_subtitleTracks.clear();
    m_subtitleStreamIdx = -1;
    m_audioTracks.clear();
    m_audioStreamIdx = -1;

    for (unsigned int i = 0; i < m_formatCtx->nb_streams; i++) {
        AVCodecParameters* codecParams = m_formatCtx->streams[i]->codecpar;
        if (codecParams->codec_type == AVMEDIA_TYPE_VIDEO &&
            !(m_formatCtx->streams[i]->disposition & (AV_DISPOSITION_ATTACHED_PIC | AV_DISPOSITION_TIMED_THUMBNAILS)) &&
            m_videoStreamIdx < 0) {
            m_videoStreamIdx = i;
            m_videoCodecParams = codecParams;
            m_videoTimeBase = m_formatCtx->streams[i]->time_base;
            // Start times are assigned together in computeStartTimes()
            // once every stream is known -- they must share one origin.
            // avg_frame_rate is the honest average over the file;
            // r_frame_rate is the smallest rate all timestamps are
            // multiples of, which for a variable-rate stream can be a
            // large multiple of the real one. Prefer the former.
            const AVRational avg = m_formatCtx->streams[i]->avg_frame_rate;
            const AVRational rfr = m_formatCtx->streams[i]->r_frame_rate;
            if (avg.num > 0 && avg.den > 0) {
                m_videoFrameRate = av_q2d(avg);
            } else if (rfr.num > 0 && rfr.den > 0) {
                m_videoFrameRate = av_q2d(rfr);
            }
        } else if (codecParams->codec_type == AVMEDIA_TYPE_AUDIO) {
            naikav::audio::AudioTrackInfo info;
            info.id = static_cast<int>(i);

            // Extract title / handler name / description
            const char* titleTag = nullptr;
            if (const AVDictionaryEntry* e = av_dict_get(m_formatCtx->streams[i]->metadata, "title", nullptr, 0)) {
                titleTag = e->value;
            } else if (const AVDictionaryEntry* e2 = av_dict_get(m_formatCtx->streams[i]->metadata, "handler_name", nullptr, 0)) {
                titleTag = e2->value;
            } else if (const AVDictionaryEntry* e3 = av_dict_get(m_formatCtx->streams[i]->metadata, "description", nullptr, 0)) {
                titleTag = e3->value;
            }
            if (titleTag && std::strlen(titleTag) > 0) {
                info.title = titleTag;
            } else {
                info.title = "Audio Track " + std::to_string(m_audioTracks.size() + 1);
            }

            // Extract language
            const char* langTag = nullptr;
            if (const AVDictionaryEntry* e = av_dict_get(m_formatCtx->streams[i]->metadata, "language", nullptr, 0)) {
                langTag = e->value;
            } else if (const AVDictionaryEntry* e2 = av_dict_get(m_formatCtx->streams[i]->metadata, "lang", nullptr, 0)) {
                langTag = e2->value;
            }
            info.language = (langTag && std::strlen(langTag) > 0) ? langTag : "und";

            // Codec name
            const char* cname = avcodec_get_name(codecParams->codec_id);
            info.codecName = (cname && std::strlen(cname) > 0) ? cname : "unknown";

            // Channels & layout
#if LIBAVUTIL_VERSION_MAJOR >= 57
            info.channels = codecParams->ch_layout.nb_channels;
            char layoutBuf[64] = {0};
            av_channel_layout_describe(&codecParams->ch_layout, layoutBuf, sizeof(layoutBuf));
            info.channelLayout = (layoutBuf[0] != '\0') ? layoutBuf : (info.channels == 2 ? "stereo" : (info.channels == 1 ? "mono" : std::to_string(info.channels) + " ch"));
#else
            info.channels = codecParams->channels;
            char layoutBuf[64] = {0};
            av_get_channel_layout_string(layoutBuf, sizeof(layoutBuf), info.channels, codecParams->channel_layout);
            info.channelLayout = (layoutBuf[0] != '\0') ? layoutBuf : (info.channels == 2 ? "stereo" : (info.channels == 1 ? "mono" : std::to_string(info.channels) + " ch"));
#endif
            info.sampleRate = codecParams->sample_rate;
            info.bitRate = codecParams->bit_rate;
            info.isDefault = (m_formatCtx->streams[i]->disposition & AV_DISPOSITION_DEFAULT) != 0;
            info.isExternal = false;

            m_audioTracks.push_back(info);
        } else if (codecParams->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            naikav::subtitle::SubtitleTrackInfo info;
            info.id = static_cast<int>(i);

            // Extract title / handler name
            const char* titleTag = nullptr;
            if (const AVDictionaryEntry* e = av_dict_get(m_formatCtx->streams[i]->metadata, "title", nullptr, 0)) {
                titleTag = e->value;
            } else if (const AVDictionaryEntry* e2 = av_dict_get(m_formatCtx->streams[i]->metadata, "handler_name", nullptr, 0)) {
                titleTag = e2->value;
            }
            if (titleTag && std::strlen(titleTag) > 0) {
                info.title = titleTag;
            } else {
                info.title = "Subtitle Track " + std::to_string(m_subtitleTracks.size() + 1);
            }

            // Extract language
            const char* langTag = nullptr;
            if (const AVDictionaryEntry* e = av_dict_get(m_formatCtx->streams[i]->metadata, "language", nullptr, 0)) {
                langTag = e->value;
            } else if (const AVDictionaryEntry* e2 = av_dict_get(m_formatCtx->streams[i]->metadata, "lang", nullptr, 0)) {
                langTag = e2->value;
            }
            info.language = (langTag && std::strlen(langTag) > 0) ? langTag : "und";

            // Codec name
            const char* cname = avcodec_get_name(codecParams->codec_id);
            info.codecName = (cname && std::strlen(cname) > 0) ? cname : "unknown";
            info.isExternal = false;

            m_subtitleTracks.push_back(info);
        }
    }

    // Before selecting an audio stream: selectAudioStream() derives that
    // stream's origin from m_startTimeUs, which this establishes.
    computeStartTimes();

    // Select default audio stream if audio tracks were found
    if (!m_audioTracks.empty()) {
        auto it = std::find_if(m_audioTracks.begin(), m_audioTracks.end(),
                               [](const auto& track) { return track.isDefault; });
        int defaultIdx = (it != m_audioTracks.end()) ? it->id : m_audioTracks[0].id;
        selectAudioStream(defaultIdx);
    }


    if (m_videoStreamIdx < 0 && m_audioStreamIdx < 0) {
        m_lastError = "no playable video or audio streams in this file";
        std::cerr << "Error: Could not find any video or audio streams" << std::endl;
        return false;
    }

    // Calculate duration
    if (m_formatCtx->duration != AV_NOPTS_VALUE) {
        m_duration = static_cast<double>(m_formatCtx->duration) / AV_TIME_BASE;
    } else {
        m_duration = 0.0;
    }

    std::cout << "Opened media file: " << m_filename 
              << ", Duration: " << m_duration << "s" 
              << ", Video Stream: " << m_videoStreamIdx 
              << ", Audio Stream: " << m_audioStreamIdx.load()
              << ", Audio Tracks: " << m_audioTracks.size()
              << ", Subtitle Tracks: " << m_subtitleTracks.size() << std::endl;

    // After the streams are known and before the read loop starts, so the
    // rewind it performs cannot disturb playback.
    probeHdrMetadata();

    m_eof = false;
    return true;
}

bool Demuxer::selectAudioStream(int streamIdx) {
    if (streamIdx < 0) {
        m_audioStreamIdx.store(-1);
        m_audioCodecParams = nullptr;
        m_audioTimeBase = AVRational{0, 1};
        m_audioStartTime = 0;
        return true;
    }
    if (!m_formatCtx || streamIdx >= static_cast<int>(m_formatCtx->nb_streams)) {
        return false;
    }
    if (m_formatCtx->streams[streamIdx]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        return false;
    }
    m_audioStreamIdx.store(streamIdx);
    m_audioCodecParams = m_formatCtx->streams[streamIdx]->codecpar;
    m_audioTimeBase = m_formatCtx->streams[streamIdx]->time_base;
    m_audioStartTime = av_rescale_q(m_startTimeUs, AV_TIME_BASE_Q, m_audioTimeBase);
    return true;
}

AVCodecParameters* Demuxer::getAudioCodecParams(int streamIdx) const {
    if (!m_formatCtx || streamIdx < 0 || streamIdx >= static_cast<int>(m_formatCtx->nb_streams)) {
        return nullptr;
    }
    return m_formatCtx->streams[streamIdx]->codecpar;
}

AVRational Demuxer::getAudioTimeBase(int streamIdx) const {
    if (!m_formatCtx || streamIdx < 0 || streamIdx >= static_cast<int>(m_formatCtx->nb_streams)) {
        return AVRational{0, 1};
    }
    return m_formatCtx->streams[streamIdx]->time_base;
}

int64_t Demuxer::getAudioStartTime(int streamIdx) const {
    if (!m_formatCtx || streamIdx < 0 || streamIdx >= static_cast<int>(m_formatCtx->nb_streams)) {
        return 0;
    }
    int64_t st = m_formatCtx->streams[streamIdx]->start_time;
    return (st != AV_NOPTS_VALUE) ? st : 0;
}


void Demuxer::start() {
    if (m_running) return;
    m_running = true;
    m_thread = std::thread(&Demuxer::threadLoop, this);
}

void Demuxer::stop() {
    m_running = false;
    m_videoQueue.abort();
    m_audioQueue.abort();
    if (m_subtitleQueue) {
        m_subtitleQueue->abort();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void Demuxer::seek(double timeInSeconds) {
    if (timeInSeconds < 0.0) timeInSeconds = 0.0;
    if (timeInSeconds > m_duration) timeInSeconds = m_duration;
    
    m_seekTargetTime = timeInSeconds;
    m_seekRequested = true;
    m_eof = false;
}

void Demuxer::setCatchup(SeekCatchupMode mode, double targetSeconds) {
    m_catchupTarget = targetSeconds;
    m_catchupMode = mode;
}

double Demuxer::packetTimeSeconds(const AVPacket* pkt, int streamIdx) const {
    int64_t ts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
    if (ts == AV_NOPTS_VALUE) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (streamIdx == m_videoStreamIdx) {
        return (ts - m_videoStartTime) * av_q2d(m_videoTimeBase);
    }
    return (ts - m_audioStartTime) * av_q2d(m_audioTimeBase);
}

// Establishes the one origin every stream's timestamps are measured from.
// Prefers the container's own start time; falls back to the earliest stream
// start when the container declares none, and to zero when nothing does.
void Demuxer::computeStartTimes() {
    m_startTimeUs = 0;
    if (!m_formatCtx) {
        return;
    }
    if (m_formatCtx->start_time != AV_NOPTS_VALUE) {
        m_startTimeUs = m_formatCtx->start_time;
    } else {
        int64_t earliest = INT64_MAX;
        for (unsigned int i = 0; i < m_formatCtx->nb_streams; ++i) {
            const AVStream* st = m_formatCtx->streams[i];
            if (st->start_time == AV_NOPTS_VALUE) {
                continue;
            }
            const int64_t us = av_rescale_q(st->start_time, st->time_base, AV_TIME_BASE_Q);
            earliest = std::min(earliest, us);
        }
        if (earliest != INT64_MAX) {
            m_startTimeUs = earliest;
        }
    }

    if (m_videoStreamIdx >= 0) {
        m_videoStartTime = av_rescale_q(m_startTimeUs, AV_TIME_BASE_Q, m_videoTimeBase);
    }
    if (m_audioStreamIdx.load() >= 0) {
        m_audioStartTime = av_rescale_q(m_startTimeUs, AV_TIME_BASE_Q, m_audioTimeBase);
    }
}

AVCodecParameters* Demuxer::getSubtitleCodecParams(int streamIdx) const {
    if (!m_formatCtx || streamIdx < 0 || streamIdx >= static_cast<int>(m_formatCtx->nb_streams)) {
        return nullptr;
    }
    return m_formatCtx->streams[streamIdx]->codecpar;
}

AVRational Demuxer::getSubtitleTimeBase(int streamIdx) const {
    if (!m_formatCtx || streamIdx < 0 || streamIdx >= static_cast<int>(m_formatCtx->nb_streams)) {
        return AVRational{0, 1};
    }
    return m_formatCtx->streams[streamIdx]->time_base;
}

int64_t Demuxer::getSubtitleStartTime(int streamIdx) const {
    if (!m_formatCtx || streamIdx < 0 || streamIdx >= static_cast<int>(m_formatCtx->nb_streams)) {
        return 0;
    }
    int64_t st = m_formatCtx->streams[streamIdx]->start_time;
    return (st != AV_NOPTS_VALUE) ? st : 0;
}

void Demuxer::performSeek() {
    std::lock_guard<std::mutex> lock(m_seekMutex);
    
    // Capture target time and reset request flag at the start
    double targetTime = m_seekTargetTime;
    m_seekRequested = false;
    m_eof = false;
    
    // Clear queues to drop packets from the old position
    m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    if (m_subtitleQueue) {
        m_subtitleQueue->clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    }
    
    int64_t targetTs;
    int streamIdx = -1;
    
    // Convert target time to the timebase of the selected stream for seeking
    int audioIdx = m_audioStreamIdx.load();
    if (m_videoStreamIdx >= 0) {
        streamIdx = m_videoStreamIdx;
        // In FFmpeg, stream-specific seek requires the timestamp in stream timebase units
        // targetTime is normalised against m_startTimeUs (see
        // computeStartTimes()), so the origin has to be added back to land
        // on the container's own timeline. Without this, seeking to T on a
        // file whose streams do not begin at zero lands T - start instead.
        targetTs = static_cast<int64_t>(targetTime / av_q2d(m_videoTimeBase)) +
                   m_videoStartTime;
    } else if (audioIdx >= 0) {
        streamIdx = audioIdx;
        targetTs = static_cast<int64_t>(targetTime / av_q2d(m_audioTimeBase)) +
                   m_audioStartTime;
    } else {
        targetTs = static_cast<int64_t>(targetTime * AV_TIME_BASE) + m_startTimeUs;
    }
    
    // Seek to nearest keyframe around target timestamp using modern fast binary-search avformat_seek_file
    int ret = avformat_seek_file(m_formatCtx, streamIdx, INT64_MIN, targetTs, INT64_MAX, 0);
    if (ret < 0) {
        std::cerr << "Warning: Could not seek to " << targetTime << "s using stream " << streamIdx << std::endl;
        // Fallback to default seek if stream-specific seek fails
        int64_t fallbackTs =
            static_cast<int64_t>(targetTime * AV_TIME_BASE) + m_startTimeUs;
        avformat_seek_file(m_formatCtx, -1, INT64_MIN, fallbackTs, INT64_MAX, 0);
    } else {
        std::cout << "Successfully seeked format context to " << targetTime << "s" << std::endl;
    }

    // Bumped last, after the format context has actually repositioned: every
    // packet threadLoop() reads and tags from this point on genuinely comes
    // from the new position. See the m_seekGeneration comment in Demuxer.hpp.
    m_seekGeneration.fetch_add(1, std::memory_order_relaxed);
}

void Demuxer::threadLoop() {
    while (m_running) {
        if (m_seekRequested) {
            performSeek();
            // Give a tiny pause to let decoders process the flush signal before we push new packets
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        int ret;
        if (m_profilingEnabled.load(std::memory_order_relaxed)) {
            auto startTime = std::chrono::steady_clock::now();
            ret = av_read_frame(m_formatCtx, packet);
            auto end = std::chrono::steady_clock::now();
            float us = static_cast<float>(std::chrono::duration_cast<std::chrono::microseconds>(end - startTime).count());
            m_demuxTimeRing.record(us);
        } else {
            ret = av_read_frame(m_formatCtx, packet);
        }
        if (ret >= 0) {
            if (m_seekRequested.load()) {
                av_packet_free(&packet);
                continue;
            }

            // Tag with the generation this packet was actually read under,
            // so a consumer can tell -- independent of its own thread
            // timing -- whether this packet predates the most recent seek.
            // See the m_seekGeneration comment in Demuxer.hpp.
            naikavTagPacketGeneration(
                packet, m_seekGeneration.load(std::memory_order_relaxed));

            SeekCatchupMode cmode = m_catchupMode.load();
            int currentAudioIdx = m_audioStreamIdx.load();
            int currentSubIdx = m_subtitleStreamIdx.load();

            if (packet->stream_index == m_videoStreamIdx) {
                // Bounded wait, not an unconditional block: this is the only
                // thread that reads either stream, so if the video decode
                // thread were ever wedged (a hung hardware decoder, a bug
                // not yet found) an indefinite block here would silently
                // stop packet delivery to BOTH queues forever, with no way
                // to recover short of tearing down playback. Past the
                // timeout, drop the oldest queued packet and keep reading
                // instead, so the pipeline can never get permanently stuck.
                if (!m_videoQueue.push_wait_or_drop(packet, kQueuePushTimeout,
                                                     [](AVPacket*& p) { av_packet_free(&p); })) {
                    av_packet_free(&packet);
                }
            } else if (currentAudioIdx >= 0 && packet->stream_index == currentAudioIdx) {
                // During catch-up the audio device is paused: audio from
                // before the seek target is useless and would only clog the
                // queue, so drop it here.
                bool drop = false;
                if (cmode != SeekCatchupMode::NONE) {
                    double ptsSec = packetTimeSeconds(packet, currentAudioIdx);
                    if (!std::isnan(ptsSec) &&
                        ptsSec < m_catchupTarget.load() - 0.1) {
                        drop = true;
                    }
                }

                bool pushed = false;
                if (drop) {
                    pushed = false;
                } else if (m_videoStreamIdx < 0) {
                    // Audio-only media: apply full queue backpressure. When paused or queue
                    // is full, wait for the audio consumer without dropping packets.
                    pushed = m_audioQueue.push(packet);
                } else if (cmode != SeekCatchupMode::NONE) {
                    // Active video catch-up scan: audio consumer is paused, drop oldest
                    // so the demuxer does not block video keyframe scanning.
                    pushed = m_audioQueue.push_drop_oldest(packet, [](AVPacket*& p) { av_packet_free(&p); });
                } else {
                    // Video with audio in normal playback or pause: bounded-wait backstop.
                    pushed = m_audioQueue.push_wait_or_drop(packet, kQueuePushTimeout,
                                                             [](AVPacket*& p) { av_packet_free(&p); });
                }

                if (!pushed) {
                    av_packet_free(&packet);
                }
            } else if (m_subtitleQueue && currentSubIdx >= 0 && packet->stream_index == currentSubIdx) {
                if (!m_subtitleQueue->push_wait_or_drop(packet, kQueuePushTimeout,
                                                        [](AVPacket*& p) { av_packet_free(&p); })) {
                    av_packet_free(&packet);
                }
            } else {
                av_packet_free(&packet);
            }
        } else {

            // Error or EOF
            av_packet_free(&packet);
            
            if (ret == AVERROR_EOF) {
                m_eof = true;
                // At EOF, sleep a bit so we don't hog CPU. If user seeks back, loop resumes.
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
}

