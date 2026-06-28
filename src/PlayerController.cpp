#include "PlayerController.hpp"
#include <chrono>
#include <iostream>
#include <algorithm>

PlayerController::PlayerController()
    : m_state(PlayerState::UNINITIALIZED),
      m_videoQueue(100), // Max capacity of 100 packets
      m_audioQueue(150), // Max capacity of 150 packets (audio packets are smaller)
      m_hasAudio(false),
      m_hasVideo(false),
      m_videoClock(0.0),
      m_lastSystemTime(0.0),
      m_volume(0.05f) {}

PlayerController::~PlayerController() {
    stop();
}

double PlayerController::getSystemTimeInSeconds() const {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration<double>(duration).count();
}

bool PlayerController::openFile(const std::string& filename) {
    // If a file is already loaded, close it first
    stop();

    m_filename = filename;
    m_videoQueue.reset();
    m_audioQueue.reset();

    // Create and open the demuxer
    m_demuxer = std::make_unique<Demuxer>(filename, m_videoQueue, m_audioQueue);
    if (!m_demuxer->open()) {
        m_state = PlayerState::ERROR_STATE;
        return false;
    }

    // Initialize Video Decoder if video stream is available
    if (m_demuxer->getVideoStreamIndex() >= 0) {
        m_videoDecoder = std::make_unique<VideoDecoder>(
            m_demuxer->getVideoCodecParams(),
            m_demuxer->getVideoTimeBase(),
            m_demuxer->getVideoStartTime(),
            m_videoQueue
        );
        m_hasVideo = m_videoDecoder->init();
    }

    // Initialize Audio Decoder if audio stream is available
    if (m_demuxer->getAudioStreamIndex() >= 0) {
        m_audioDecoder = std::make_unique<AudioDecoder>(
            m_demuxer->getAudioCodecParams(),
            m_demuxer->getAudioTimeBase(),
            m_demuxer->getAudioStartTime(),
            m_audioQueue
        );
        m_hasAudio = m_audioDecoder->init();
        if (m_hasAudio) {
            m_audioDecoder->setVolume(m_volume);
        }
    }

    if (!m_hasVideo && !m_hasAudio) {
        std::cerr << "Error: File has no playable video or audio streams" << std::endl;
        stop();
        m_state = PlayerState::ERROR_STATE;
        return false;
    }

    // Start Demuxer background reading
    m_demuxer->start();

    m_videoClock = 0.0;
    m_lastSystemTime = getSystemTimeInSeconds();
    m_state = PlayerState::OPENED;

    return true;
}

void PlayerController::play() {
    if (m_state == PlayerState::ENDED) {
        seek(0.0);
    }

    if (m_state != PlayerState::OPENED && m_state != PlayerState::PAUSED) {
        return;
    }

    if (m_hasAudio) {
        m_audioDecoder->start();
    }
    
    m_lastSystemTime = getSystemTimeInSeconds();
    m_state = PlayerState::PLAYING;
    std::cout << "Playback started" << std::endl;
}

void PlayerController::pause() {
    if (m_state != PlayerState::PLAYING) {
        return;
    }

    if (m_hasAudio) {
        m_audioDecoder->pause();
    } else {
        updateClockForVideoOnly();
    }

    m_state = PlayerState::PAUSED;
    std::cout << "Playback paused" << std::endl;
}

void PlayerController::seek(double seconds) {
    if (m_state == PlayerState::UNINITIALIZED || m_state == PlayerState::ERROR_STATE) {
        return;
    }

    bool wasPlaying = (m_state == PlayerState::PLAYING);

    // Pause audio device output during seek
    if (m_hasAudio && wasPlaying) {
        m_audioDecoder->pause();
    }

    // Signal demuxer to seek (clears queues and updates format context)
    m_demuxer->seek(seconds);

    // Force clear our queues immediately from this thread to speed up seek response
    m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });

    // Flush decoders to drop cached decoding frames
    if (m_hasVideo) {
        m_videoDecoder->flush();
    }
    if (m_hasAudio) {
        m_audioDecoder->flush();
    }

    // Reset clocks
    m_videoClock = seconds;
    m_lastSystemTime = getSystemTimeInSeconds();
    if (m_hasAudio && m_audioDecoder) {
        m_audioDecoder->setClock(seconds);
    }

    // Resume playing if we were playing before seek
    if (wasPlaying) {
        if (m_hasAudio) {
            m_audioDecoder->resume();
        }
        m_state = PlayerState::PLAYING;
    } else {
        m_state = PlayerState::OPENED; // Allow rendering first frame on seek pause
    }
}

void PlayerController::stop() {
    m_state = PlayerState::UNINITIALIZED;

    if (m_demuxer) {
        m_demuxer->stop();
    }

    if (m_audioDecoder) {
        m_audioDecoder->stop();
    }

    // Drop any packets remaining in queues
    m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });

    // Reclaim memory
    m_demuxer.reset();
    m_audioDecoder.reset();
    m_videoDecoder.reset();

    m_hasAudio = false;
    m_hasVideo = false;
    m_videoClock = 0.0;
}

void PlayerController::updateClockForVideoOnly() {
    if (m_state == PlayerState::PLAYING && !m_hasAudio) {
        double now = getSystemTimeInSeconds();
        m_videoClock += (now - m_lastSystemTime);
        m_lastSystemTime = now;
    }
}

double PlayerController::getCurrentTime() {
    if (m_state == PlayerState::UNINITIALIZED) {
        return 0.0;
    }

    if (m_state == PlayerState::ENDED) {
        return getDuration();
    }

    double currentTime;
    if (m_hasAudio) {
        currentTime = m_audioDecoder->getAudioClock();
    } else {
        updateClockForVideoOnly();
        currentTime = m_videoClock;
    }

    double duration = getDuration();
    bool reachedEnd = false;

    if (duration > 0.0 && currentTime >= duration) {
        reachedEnd = true;
    } else if (m_demuxer && m_demuxer->isEOF()) {
        bool videoQueueEmpty = !m_hasVideo || m_videoQueue.empty();
        bool audioQueueEmpty = !m_hasAudio || m_audioQueue.empty();
        if (videoQueueEmpty && audioQueueEmpty) {
            reachedEnd = true;
        }
    }

    if (reachedEnd) {
        currentTime = duration > 0.0 ? duration : currentTime;
        if (m_state == PlayerState::PLAYING) {
            m_state = PlayerState::ENDED;
            if (m_hasAudio && m_audioDecoder) {
                m_audioDecoder->pause();
            }
            std::cout << "Playback reached end, transitioned to ENDED state" << std::endl;
        }
    }

    return currentTime;
}

double PlayerController::getDuration() const {
    if (m_demuxer) {
        return m_demuxer->getDuration();
    }
    return 0.0;
}

int PlayerController::getVideoWidth() const {
    if (m_videoDecoder) {
        return m_videoDecoder->getWidth();
    }
    return 0;
}

int PlayerController::getVideoHeight() const {
    if (m_videoDecoder) {
        return m_videoDecoder->getHeight();
    }
    return 0;
}

void PlayerController::setVolume(float volume) {
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    if (m_hasAudio && m_audioDecoder) {
        m_audioDecoder->setVolume(m_volume);
    }
}

bool PlayerController::isEOF() const {
    return m_demuxer ? m_demuxer->isEOF() : false;
}
