#include "Demuxer.hpp"
#include <iostream>
#include <chrono>
#include <cmath>
#include <limits>

Demuxer::Demuxer(const std::string& filename, 
                 ThreadSafeQueue<AVPacket*>& videoQueue, 
                 ThreadSafeQueue<AVPacket*>& audioQueue)
    : m_filename(filename),
      m_formatCtx(nullptr),
      m_videoStreamIdx(-1),
      m_audioStreamIdx(-1),
      m_videoCodecParams(nullptr),
      m_audioCodecParams(nullptr),
      m_videoTimeBase{0, 1},
      m_audioTimeBase{0, 1},
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
      m_catchupTarget(0.0) {
}

Demuxer::~Demuxer() {
    stop();
    if (m_formatCtx) {
        avformat_close_input(&m_formatCtx);
    }
}

bool Demuxer::open() {
    // Open video file with protocol whitelist restrictions (local file and pipe only)
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file,pipe", 0);
    
    int ret = avformat_open_input(&m_formatCtx, m_filename.c_str(), nullptr, &options);
    av_dict_free(&options);

    if (ret < 0) {
        std::cerr << "Error: Could not open media file " << m_filename << std::endl;
        return false;
    }

    // Enable fast seek using index tables instead of parsing frames
    m_formatCtx->flags |= AVFMT_FLAG_FAST_SEEK;

    // Retrieve stream info
    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        std::cerr << "Error: Could not find stream information" << std::endl;
        return false;
    }

    // Find video and audio streams
    for (unsigned int i = 0; i < m_formatCtx->nb_streams; i++) {
        AVCodecParameters* codecParams = m_formatCtx->streams[i]->codecpar;
        if (codecParams->codec_type == AVMEDIA_TYPE_VIDEO && m_videoStreamIdx < 0) {
            m_videoStreamIdx = i;
            m_videoCodecParams = codecParams;
            m_videoTimeBase = m_formatCtx->streams[i]->time_base;
            m_videoStartTime = m_formatCtx->streams[i]->start_time;
            if (m_videoStartTime == AV_NOPTS_VALUE) m_videoStartTime = 0;
        } else if (codecParams->codec_type == AVMEDIA_TYPE_AUDIO && m_audioStreamIdx < 0) {
            m_audioStreamIdx = i;
            m_audioCodecParams = codecParams;
            m_audioTimeBase = m_formatCtx->streams[i]->time_base;
            m_audioStartTime = m_formatCtx->streams[i]->start_time;
            if (m_audioStartTime == AV_NOPTS_VALUE) m_audioStartTime = 0;
        }
    }

    if (m_videoStreamIdx < 0 && m_audioStreamIdx < 0) {
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
              << ", Audio Stream: " << m_audioStreamIdx << std::endl;

    m_eof = false;
    return true;
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

void Demuxer::throttleCatchupReadahead(double videoPtsSec) {
    // Called after pushing a video packet while a seek catch-up is active.
    // Once we've read slightly past the target there is no point in racing
    // further ahead of the decoder - the extra packets would only fill the
    // audio queue and stall the pipeline. Bails out as soon as a new seek is
    // requested or the catch-up ends.
    if (std::isnan(videoPtsSec)) {
        return;
    }
    while (m_running.load() && !m_seekRequested.load() &&
           m_catchupMode.load() != SeekCatchupMode::NONE &&
           videoPtsSec > m_catchupTarget.load() + 1.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void Demuxer::performSeek() {
    std::lock_guard<std::mutex> lock(m_seekMutex);
    
    // Capture target time and reset request flag at the start
    double targetTime = m_seekTargetTime;
    m_seekRequested = false;
    m_eof = false;
    
    // Clear both queues to drop packets from the old position
    m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    
    int64_t targetTs;
    int streamIdx = -1;
    
    // Convert target time to the timebase of the selected stream for seeking
    if (m_videoStreamIdx >= 0) {
        streamIdx = m_videoStreamIdx;
        // In FFmpeg, stream-specific seek requires the timestamp in stream timebase units
        targetTs = static_cast<int64_t>(targetTime / av_q2d(m_videoTimeBase));
    } else if (m_audioStreamIdx >= 0) {
        streamIdx = m_audioStreamIdx;
        targetTs = static_cast<int64_t>(targetTime / av_q2d(m_audioTimeBase));
    } else {
        targetTs = static_cast<int64_t>(targetTime * AV_TIME_BASE);
    }
    
    // Seek to nearest keyframe around target timestamp using modern fast binary-search avformat_seek_file
    int ret = avformat_seek_file(m_formatCtx, streamIdx, INT64_MIN, targetTs, INT64_MAX, 0);
    if (ret < 0) {
        std::cerr << "Warning: Could not seek to " << targetTime << "s using stream " << streamIdx << std::endl;
        // Fallback to default seek if stream-specific seek fails
        int64_t fallbackTs = static_cast<int64_t>(targetTime * AV_TIME_BASE);
        avformat_seek_file(m_formatCtx, -1, INT64_MIN, fallbackTs, INT64_MAX, 0);
    } else {
        std::cout << "Successfully seeked format context to " << targetTime << "s" << std::endl;
    }
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

        int ret = av_read_frame(m_formatCtx, packet);
        if (ret >= 0) {
            if (m_seekRequested.load()) {
                av_packet_free(&packet);
                continue;
            }

            SeekCatchupMode cmode = m_catchupMode.load();

            if (packet->stream_index == m_videoStreamIdx) {
                double ptsSec = packetTimeSeconds(packet, m_videoStreamIdx);
                if (!m_videoQueue.push(packet)) {
                    av_packet_free(&packet);
                }
                if (cmode != SeekCatchupMode::NONE) {
                    throttleCatchupReadahead(ptsSec);
                }
            } else if (packet->stream_index == m_audioStreamIdx) {
                // During catch-up the audio device is paused: audio from
                // before the seek target is useless and would only clog the
                // queue (blocking this thread), so drop it here.
                bool drop = false;
                if (cmode != SeekCatchupMode::NONE) {
                    double ptsSec = packetTimeSeconds(packet, m_audioStreamIdx);
                    if (!std::isnan(ptsSec) &&
                        ptsSec < m_catchupTarget.load() - 0.1) {
                        drop = true;
                    }
                }

                if (drop || !m_audioQueue.push(packet)) {
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
