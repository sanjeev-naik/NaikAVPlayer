#include "player/PlayerController.hpp"
#include "audio/dsp/ReplayGainTags.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <sstream>

bool g_videoThreadEnabled = true;

namespace {
// Seek catch-up tuning
constexpr double kMinCatchupGap = 0.35;      // below this a plain jump looks identical
} // namespace

PlayerController::PlayerController()
    : m_state(PlayerState::UNINITIALIZED),
      m_videoQueue(100), // Max capacity of 100 packets
      m_audioQueue(150), // Max capacity of 150 packets (audio packets are smaller)
      m_subtitleQueue(100), // Max capacity of 100 subtitle packets
      m_hasAudio(false),
      m_hasVideo(false),
      m_videoClock(0.0),
      m_lastSystemTime(0.0),
      m_volume(0.05f),
      m_playbackSpeed(1.0f),
      m_loopEnabled(false),
      m_videoThreadRunning(false),
      m_videoThreadEnabled(g_videoThreadEnabled),
      m_seeking(false),
      m_seeked(false),
      m_decodedFrameQueue(8),
      m_catchupMode(SeekCatchupMode::NONE),
      m_catchupTarget(0.0),
      m_catchupPos(0.0),
      m_resumeAfterCatchup(false),
      m_catchupEpoch(0),
      m_metrics(std::make_unique<PipelineMetrics>()),
      m_resolutionOption(ResolutionOption::ORIGINAL) {
    m_videoQueue.attachDepthMirror(&m_metrics->m_videoPacketQueueDepth);
    m_audioQueue.attachDepthMirror(&m_metrics->m_audioPacketQueueDepth);
    m_subtitleQueue.attachDepthMirror(&m_metrics->m_subtitlePacketQueueDepth);
    m_decodedFrameQueue.attachDepthMirror(&m_metrics->m_decodedFrameQueueDepth);
    loadSettings();
    loadPlaylistState();
}

PlayerController::~PlayerController() {
    stop();
}

double PlayerController::getSystemTimeInSeconds() const {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration<double>(duration).count();
}

bool PlayerController::openFile(const std::string& filename, bool resetPlaylist) {
    // If a file is already loaded, close it first
    stop();

    if (resetPlaylist) {
        m_playlist.clear();
        m_playlist.add(filename);
        m_playlist.setCurrentIndex(0);
        savePlaylistState();
    }

    try {
        m_filename = filename;
        m_videoQueue.reset();
        m_audioQueue.reset();
        m_subtitleQueue.reset();
        m_decodedFrameQueue.reset(); // Clear aborted state set by stop()

        // Create and open the demuxer
        m_demuxer = std::make_unique<Demuxer>(
            filename, m_videoQueue, m_audioQueue,
            m_metrics->m_demuxTimePerPacketUs, m_metrics->m_profilingEnabled);
        m_demuxer->attachSubtitleQueue(&m_subtitleQueue);

        if (!m_demuxer->open()) {
            m_state = PlayerState::ERROR_STATE;
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(m_subtitleMutex);
            m_cachedSubtitleTracks = m_demuxer->getSubtitleTracks();
            m_hasExternalSubtitle = false;
        }

        {
            std::lock_guard<std::mutex> lock(m_audioTrackMutex);
            m_cachedAudioTracks = m_demuxer->getAudioTracks();
            m_hasExternalAudio = false;
            m_selectedAudioTrack.store(m_demuxer->getAudioStreamIndex());
        }

        // Auto-probe matching external subtitle files in directory
        autoProbeExternalSubtitles(filename);


        // Initialize Video Decoder if video stream is available
        if (m_demuxer->getVideoStreamIndex() >= 0) {
            m_videoDecoder = std::make_unique<VideoDecoder>(
                m_demuxer->getVideoCodecParams(),
                m_demuxer->getVideoTimeBase(),
                m_demuxer->getVideoStartTime(),
                m_videoQueue,
                m_metrics->m_decodeTimePerFrameUs,
                m_metrics->m_convertTimeUs,
                m_metrics->m_profilingEnabled
            );
            m_hasVideo = m_videoDecoder->init();
            if (m_hasVideo) {
                // Must happen before m_demuxer->start(): the decode loop is not
                // synchronized with this pointer assignment.
                m_videoDecoder->attachSeekGeneration(m_demuxer->seekGenerationPtr());
            }
        }

        // Initialize Audio Decoder if audio stream is available
        if (m_demuxer->getAudioStreamIndex() >= 0) {
            {
                std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
                m_audioDecoder = std::make_unique<AudioDecoder>(
                    m_demuxer->getAudioCodecParams(),
                    m_demuxer->getAudioTimeBase(),
                    m_demuxer->getAudioStartTime(),
                    m_audioQueue,
                    &m_audioDecodeTimeUs
                );
                m_audioDecoder->setChannelOption(m_channelOption);
                m_audioDecoder->setOutputBitDepth(m_outputBitDepth);
                m_audioDecoder->setOutputDeviceName(m_outputDeviceName);
                m_audioDecoder->setResamplerQuality(m_resamplerQuality);
                m_hasAudio = m_audioDecoder->init();
                if (m_hasAudio) {
                    m_audioDecoder->setVolume(m_volume);
                    m_audioDecoder->setPlaybackSpeed(m_playbackSpeed.load());
                    m_audioDecoder->applyDspSettings(m_audioDspSettings);
                }
            }
            if (m_hasAudio) {
                // Released m_audioDecoderMutex above before these two calls:
                // applyGenrePresetIfEnabled() can re-enter via
                // setAudioDspSettings(), and the loudness prescan's tagged-tag
                // fast path re-enters directly -- both take
                // m_audioDecoderMutex themselves, which would self-deadlock
                // on this same (non-recursive) mutex if it were still held.
                //
                // Before the loudness prescan below: a genre-based preset swap
                // can change loudnessEnabled/loudnessTargetLufs, and the
                // prescan should reflect whatever settings actually end up
                // active for this file, not whatever was active before it.
                applyGenrePresetIfEnabled();
                if (m_audioDspSettings.loudnessEnabled) {
                    // Only blocks when a tagged loudness value short-circuits
                    // the scan (cheap dictionary lookups); otherwise this
                    // kicks off the real decode-based prescan on its own
                    // background thread and returns immediately -- see
                    // prescanLoudnessForCurrentFile()'s own comment.
                    prescanLoudnessForCurrentFile();
                }
                // Must happen before m_demuxer->start(): the read loop is not
                // synchronized with this pointer assignment.
                std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
                m_demuxer->attachAudioPausedFlag(&m_audioDecoder->pausedFlag());
                m_audioDecoder->attachSeekGeneration(m_demuxer->seekGenerationPtr());
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

        // Start video decoding background thread
        if (m_hasVideo && m_videoThreadEnabled) {
            m_videoThreadRunning = true;
            m_videoThread = std::thread(&PlayerController::videoThreadLoop, this);
        }

        m_videoClock = 0.0;
        m_lastSystemTime = getSystemTimeInSeconds();
        m_state = PlayerState::OPENED;

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error: Exception while opening file '" << filename << "': " << e.what() << std::endl;
        stop();
        m_state = PlayerState::ERROR_STATE;
        return false;
    }
}

bool PlayerController::playlistPlayIndex(int index) {
    if (index < 0 || static_cast<size_t>(index) >= m_playlist.size()) {
        return false;
    }
    std::string path = m_playlist.items()[static_cast<size_t>(index)].path;
    m_playlist.setCurrentIndex(index);
    savePlaylistState();
    bool opened = openFile(path, false);
    if (opened) {
        play();
    }
    return opened;
}

bool PlayerController::playlistNext() {
    auto item = m_playlist.next();
    savePlaylistState();
    if (!item) return false;
    bool opened = openFile(item->path, false);
    if (opened) {
        play();
    }
    return opened;
}

bool PlayerController::playlistPrevious() {
    auto item = m_playlist.previous();
    savePlaylistState();
    if (!item) return false;
    bool opened = openFile(item->path, false);
    if (opened) {
        play();
    }
    return opened;
}

void PlayerController::pollPlaylistAutoAdvance() {
    if (m_state == PlayerState::UNINITIALIZED) {
        return;
    }
    // Forces this frame's ENDED transition (see getCurrentTime()) to be
    // evaluated before checking m_state below, so this method has no
    // ordering dependency on whatever else calls getCurrentTime() this frame.
    getCurrentTime();

    if (m_state != PlayerState::ENDED) {
        return;
    }
    // The existing per-file Loop toggle already keeps playback from ever
    // reaching ENDED (see getCurrentTime()'s instantSeek(0.0) branch), so no
    // explicit check is needed here -- reaching this point already means
    // Loop is off (or there is no next item to loop to).
    auto item = m_playlist.next();
    if (!item) {
        return; // end of list under RepeatMode::Off -- stay at ENDED, as today
    }
    if (openFile(item->path, false)) {
        play();
    }
}

void PlayerController::play() {
    if (m_state == PlayerState::ENDED) {
        instantSeek(0.0); // restart from the top without a rewind animation
    }

    if (m_state != PlayerState::OPENED && m_state != PlayerState::PAUSED) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_catchupMutex);
        if (m_catchupMode.load() != SeekCatchupMode::NONE) {
            // Un-pausing mid catch-up: audio stays muted until the catch-up
            // lands on the target, then resumes in sync.
            m_resumeAfterCatchup.store(true);
            m_state = PlayerState::PLAYING;
            std::cout << "Playback started" << std::endl;
            return;
        }
    }

    {
        std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
        if (m_hasAudio && m_audioDecoder) {
            m_audioDecoder->start();
        }
    }

    m_lastSystemTime = getSystemTimeInSeconds();
    m_state = PlayerState::PLAYING;
    std::cout << "Playback started" << std::endl;
}

void PlayerController::pause() {
    if (m_state != PlayerState::PLAYING) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_catchupMutex);
        if (m_catchupMode.load() != SeekCatchupMode::NONE) {
            // Audio is already paused during catch-up; the scan keeps running
            // and will land in a paused state on the target frame.
            m_resumeAfterCatchup.store(false);
            m_state = PlayerState::PAUSED;
            std::cout << "Playback paused" << std::endl;
            return;
        }
    }

    {
        std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
        if (m_hasAudio && m_audioDecoder) {
            m_audioDecoder->pause();
        } else if (!m_hasAudio) {
            updateClockForVideoOnly();
        }
    }

    m_state = PlayerState::PAUSED;
    std::cout << "Playback paused" << std::endl;
}

void PlayerController::seek(double seconds) {
    if (m_state == PlayerState::UNINITIALIZED || m_state == PlayerState::ERROR_STATE) {
        return;
    }

    double duration = getDuration();
    if (seconds < 0.0) seconds = 0.0;
    if (duration > 0.0 && seconds > duration) seconds = duration;

    // Current on-screen position must be sampled before taking the catch-up
    // lock: getCurrentTime() can itself trigger a loop-restart seek.
    double current = isCatchingUp() ? m_catchupPos.load() : getCurrentTime();

    bool catchupActive = false;
    bool playing = false;
    double delta = 0.0;

    {
        std::lock_guard<std::mutex> lock(m_catchupMutex);
        catchupActive = (m_catchupMode.load() != SeekCatchupMode::NONE);
        if (catchupActive) {
            current = m_catchupPos.load();
        }
        playing = (m_state == PlayerState::PLAYING);
        delta = seconds - current;
    }

    // The catch-up scan is driven by the background video thread. Without it
    // (tests), without video, while not playing, or for negligible jumps,
    // the classic instant seek is the right behavior.
    if (!m_hasVideo || !m_videoThreadEnabled || (!playing && !catchupActive) ||
        std::fabs(delta) < kMinCatchupGap) {
        instantSeek(seconds);
        return;
    }

    uint64_t activeEpoch = 0;
    {
        std::lock_guard<std::mutex> lock(m_catchupMutex);
        if (!catchupActive) {
            m_resumeAfterCatchup.store(playing);
        }
        m_catchupTarget.store(seconds);
        m_catchupPos.store(seconds);
        m_catchupMode.store(SeekCatchupMode::LANDING);
        // Bump the epoch together with the new target, not later: a frame
        // already mid-decode from before this seek captures its epoch
        // snapshot up front (see threadLoop()) but only gets checked against
        // m_catchupEpoch after decode finishes, which can be well after this
        // point. If the epoch bump were delayed (as it used to be, until
        // after the demuxer seek/queue clears below), such a frame could
        // still match the old epoch, get compared against the *already
        // updated* m_catchupTarget above, and spuriously call finishCatchup()
        // with a stale pts -- stomping the target this call just set.
        activeEpoch = m_catchupEpoch.fetch_add(1) + 1;
    }

    // Mute audio for the duration of the catch-up phase; it resumes in sync
    // once the target frame is reached.
    {
        std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
        if (m_hasAudio && m_audioDecoder) {
            m_audioDecoder->pause();
            m_audioDecoder->flush();
        }
    }

    m_seeking.store(true);
    m_demuxer->setCatchup(SeekCatchupMode::LANDING, seconds);
    m_demuxer->seek(seconds);
    if (m_hasExternalAudio && m_externalAudioDemuxer) {
        m_externalAudioDemuxer->seek(seconds);
    }
    m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    {
        std::lock_guard<std::mutex> subLock(m_subtitleMutex);
        m_subtitleQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
        if (m_subtitleDecoder) {
            m_subtitleDecoder->flush();
        }
    }
    {
        std::lock_guard<std::mutex> decoderLock(m_videoDecoderMutex);
        m_decodedFrameQueue.clear([](DecodedFrame& df) {
            if (df.frame) {
                av_frame_free(&df.frame);
            }
        });
        if (m_videoDecoder) {
            m_videoDecoder->flush();
        }
    }
    m_seeking.store(false);
    if (m_metrics->m_profilingEnabled.load(std::memory_order_relaxed)) {
        m_seekStartTime = std::chrono::steady_clock::now();
        m_seekStartEpoch = activeEpoch;
    }
}

void PlayerController::instantSeek(double seconds) {
    {
        std::lock_guard<std::mutex> lock(m_catchupMutex);
        m_catchupMode.store(SeekCatchupMode::NONE);
        if (m_demuxer) {
            m_demuxer->setCatchup(SeekCatchupMode::NONE, 0.0);
        }
    }

    m_seeking.store(true);
    bool wasPlaying = (m_state == PlayerState::PLAYING);

    // Pause audio device output during seek
    {
        std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
        if (m_hasAudio && wasPlaying && m_audioDecoder) {
            m_audioDecoder->pause();
        }
    }

    // Signal demuxer to seek (clears queues and updates format context)
    m_demuxer->seek(seconds);
    if (m_hasExternalAudio && m_externalAudioDemuxer) {
        m_externalAudioDemuxer->seek(seconds);
    }

    // Force clear our queues immediately from this thread to speed up seek response
    m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    {
        std::lock_guard<std::mutex> subLock(m_subtitleMutex);
        m_subtitleQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
        if (m_subtitleDecoder) {
            m_subtitleDecoder->flush();
        }
    }

    // Flush decoders and clear decoded frame queue under lock
    {
        std::lock_guard<std::mutex> lock(m_videoDecoderMutex);
        m_decodedFrameQueue.clear([](DecodedFrame& df) {
            if (df.frame) {
                av_frame_free(&df.frame);
            }
        });
        if (m_hasVideo && m_videoDecoder) {
            m_videoDecoder->flush();
        }
    }

    {
        std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
        if (m_hasAudio && m_audioDecoder) {
            m_audioDecoder->flush();
            m_audioDecoder->setClock(seconds);
            if (wasPlaying) {
                m_audioDecoder->resume();
            }
        }
    }

    // Reset clocks
    m_videoClock = seconds;
    m_lastSystemTime = getSystemTimeInSeconds();

    // Resume playing if we were playing before seek
    if (wasPlaying) {
        m_state = PlayerState::PLAYING;
    } else {
        m_state = PlayerState::OPENED; // Allow rendering first frame on seek pause
    }
    m_catchupEpoch.fetch_add(1);
    m_seeking.store(false);
    m_seeked.store(true);
}

void PlayerController::stop() {
    m_state = PlayerState::UNINITIALIZED;

    // Must join before m_audioDecoder is reset below and before this
    // PlayerController could be destroyed -- the background thread's
    // lambda captures `this` (only to reach m_pendingPrescanMutex/
    // m_pendingPrescanResult, never m_audioDecoder directly, but it still
    // can't outlive the object it was spawned from).
    joinLoudnessPrescanThread();

    m_catchupMode.store(SeekCatchupMode::NONE);
    m_resumeAfterCatchup.store(false);
    m_catchupPos.store(0.0);

    // Abort all queues first so any thread waiting on push or pop is unblocked immediately
    m_decodedFrameQueue.abort();
    m_videoQueue.abort();
    m_audioQueue.abort();
    m_subtitleQueue.abort();
    m_dummyVideoQueue.abort();

    m_videoThreadRunning = false;
    if (m_videoThread.joinable()) {
        m_videoThread.join();
    }

    m_decodedFrameQueue.clear([](DecodedFrame& df) {
        if (df.frame) {
            av_frame_free(&df.frame);
        }
    });

    if (m_demuxer) {
        m_demuxer->stop();
    }

    {
        std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
        if (m_audioDecoder) {
            m_audioDecoder->stop();
        }
        m_audioDecoder.reset();
    }

    // Drop any packets remaining in queues
    m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    m_dummyVideoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });

    // Reclaim memory
    m_demuxer.reset();
    m_videoDecoder.reset();

    {
        std::lock_guard<std::mutex> lock(m_subtitleMutex);
        m_subtitleQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
        if (m_subtitleDecoder) {
            m_subtitleDecoder->reset();
            m_subtitleDecoder.reset();
        }
        m_cachedSubtitleTracks.clear();
        m_hasExternalSubtitle = false;
        m_selectedSubtitleTrack.store(-1);
    }

    {
        std::lock_guard<std::mutex> lock(m_audioTrackMutex);
        if (m_externalAudioDemuxer) {
            m_externalAudioDemuxer->stop();
            m_externalAudioDemuxer.reset();
        }
        m_cachedAudioTracks.clear();
        m_hasExternalAudio = false;
        m_selectedAudioTrack.store(-1);
    }

    m_hasAudio = false;
    m_hasVideo = false;
    m_videoClock = 0.0;
}

void PlayerController::updateClockForVideoOnly() {
    if (m_catchupMode.load() != SeekCatchupMode::NONE) {
        return; // clock is frozen while catching up; reset when it lands
    }
    if (m_state == PlayerState::PLAYING && !m_hasAudio) {
        double now = getSystemTimeInSeconds();
        m_videoClock.store(m_videoClock.load() + (now - m_lastSystemTime.load()) * m_playbackSpeed.load());
        m_lastSystemTime.store(now);
    }
}

double PlayerController::getCurrentTime() {
    if (m_state == PlayerState::UNINITIALIZED) {
        return 0.0;
    }

    if (m_catchupMode.load() != SeekCatchupMode::NONE) {
        // While catching up, report the seek target: the timeline jumps
        // straight there. End-of-stream handling is suspended until the
        // catch-up lands.
        return m_catchupPos.load();
    }

    if (m_state == PlayerState::ENDED) {
        return getDuration();
    }

    double currentTime;
    {
        std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
        if (m_hasAudio && m_audioDecoder) {
            currentTime = m_audioDecoder->getAudioClock();
        } else {
            updateClockForVideoOnly();
            currentTime = m_videoClock;
        }
    }

    double duration = getDuration();
    bool reachedEnd = false;

    if (duration > 0.0 && currentTime >= duration) {
        reachedEnd = true;
    } else if (m_hasVideo && m_demuxer && m_demuxer->isEOF()) {
        bool videoQueueEmpty = m_videoQueue.empty();
        bool audioQueueEmpty = !m_hasAudio || m_audioQueue.empty();
        if (videoQueueEmpty && audioQueueEmpty) {
            reachedEnd = true;
        }
    } else if (!m_hasVideo && m_demuxer && m_demuxer->isEOF()) {
        bool audioQueueEmpty = !m_hasAudio || m_audioQueue.empty();
        if ((duration <= 0.0 || currentTime >= (duration - 0.25)) && audioQueueEmpty) {
            reachedEnd = true;
        }
    }

    if (reachedEnd) {
        if (m_state == PlayerState::PLAYING && m_loopEnabled) {
            std::cout << "Playback reached end, looping back to start" << std::endl;
            instantSeek(0.0); // seamless wraparound, no rewind animation
            return 0.0;
        }

        currentTime = duration > 0.0 ? duration : currentTime;
        if (m_state == PlayerState::PLAYING) {
            m_state = PlayerState::ENDED;
            {
                std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
                if (m_hasAudio && m_audioDecoder) {
                    m_audioDecoder->pause();
                }
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
    std::lock_guard<std::mutex> lock(m_videoDecoderMutex);
    if (m_videoDecoder) {
        return m_videoDecoder->getWidth();
    }
    return 0;
}

int PlayerController::getVideoHeight() const {
    std::lock_guard<std::mutex> lock(m_videoDecoderMutex);
    if (m_videoDecoder) {
        return m_videoDecoder->getHeight();
    }
    return 0;
}

std::string PlayerController::getVideoCodecName() const {
    if (m_hasVideo && m_demuxer) {
        AVCodecParameters* params = m_demuxer->getVideoCodecParams();
        if (params) {
            const char* name = avcodec_get_name(params->codec_id);
            if (name) {
                return std::string(name);
            }
        }
    }
    return "Unknown";
}

std::string PlayerController::getAudioCodecName() const {
    if (m_hasAudio && m_demuxer) {
        AVCodecParameters* params = m_demuxer->getAudioCodecParams();
        if (params) {
            const char* name = avcodec_get_name(params->codec_id);
            if (name) {
                return std::string(name);
            }
        }
    }
    return "Unknown";
}

std::string PlayerController::getAudioChannelLayoutName() const {
    std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
    if (m_hasAudio && m_audioDecoder) {
        return m_audioDecoder->getOutputChannelLayoutName();
    }
    return "Unknown";
}

double PlayerController::getAudioClock() {
    std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
    if (m_hasAudio && m_audioDecoder) {
        return m_audioDecoder->getAudioClock();
    }
    return 0.0;
}

double PlayerController::getVideoClock() const {
    if (m_hasVideo && m_videoDecoder) {
        return m_videoDecoder->getCurrentFramePts();
    }
    return 0.0;
}

void PlayerController::setVolume(float volume) {
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
    if (m_hasAudio && m_audioDecoder) {
        m_audioDecoder->setVolume(m_volume);
    }
}

void PlayerController::setPlaybackSpeed(float speed) {
    speed = std::clamp(speed, kMinPlaybackSpeed, kMaxPlaybackSpeed);
    m_playbackSpeed.store(speed);
    std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
    if (m_hasAudio && m_audioDecoder) {
        m_audioDecoder->setPlaybackSpeed(speed);
    }
}

void PlayerController::setAudioDspSettings(const naikav::dsp::AudioDspSettings& settings) {
    const bool loudnessJustEnabled = settings.loudnessEnabled && !m_audioDspSettings.loudnessEnabled;
    m_audioDspSettings = settings;
    {
        std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
        if (m_hasAudio && m_audioDecoder) {
            m_audioDecoder->applyDspSettings(m_audioDspSettings);
        }
    }
    if (m_hasAudio && loudnessJustEnabled) {
        // Blocking on the UI thread, but only on the (0->1) toggle
        // transition, not on every subsequent slider tweak.
        prescanLoudnessForCurrentFile();
    }
}

void PlayerController::prescanLoudnessForCurrentFile() {
    if (!m_hasAudio || m_filename.empty()) {
        return;
    }

    // Prefer a ReplayGain/EBU R128 container tag when one is present: it's
    // exactly what the encoder or a tagging tool already measured against
    // the whole file, so it's at least as accurate as this project's own
    // decode-only prescan and, unlike it, needs no decoding at all. Cheap
    // (a couple of dictionary lookups), so handled synchronously.
    if (m_demuxer) {
        double taggedLufs = 0.0;
        if (naikav::dsp::readTaggedLoudnessAsLufs(m_demuxer->getFormatMetadata(),
                                                   m_demuxer->getAudioStreamMetadata(),
                                                   taggedLufs)) {
            std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
            if (m_audioDecoder) {
                m_audioDecoder->primeLoudnessPrescan(taggedLufs);
            }
            return;
        }
    }

    // No tag: fall back to decoding the whole file's audio, which can take
    // a long time for a real (non-trivial-length) file -- openFile() is
    // called directly from main.cpp's SDL_EVENT_DROP_FILE handler on the
    // render/event thread, so blocking here would freeze the entire UI
    // (no rendering, no input) for however long the scan takes, which
    // looks exactly like a crash/hang to a user opening a new file. Run it
    // on a background thread instead; pollPendingLoudnessPrescan() (called
    // once per frame from main.cpp) applies the result once ready. The
    // real-time loudness meter already runs and gives a converging-but-
    // usable reading in the meantime, so this is a pure improvement, not
    // a "loudness is briefly wrong" regression.
    joinLoudnessPrescanThread(); // previous file's scan, if one is still running

    const uint64_t generation = m_loudnessPrescanGeneration.fetch_add(1) + 1;
    const std::string filenameCopy = m_filename;
    const int audioStreamIndex = m_demuxer ? m_demuxer->getAudioStreamIndex() : -1;

    // std::thread's constructor throws std::system_error if the OS is
    // temporarily out of thread/handle capacity. Unlike the openFile() path
    // (which wraps this call in a try/catch), the other caller --
    // setAudioDspSettings(), reached live from the DSP panel whenever the
    // user flips loudness normalization on -- has no such guard, and
    // nothing between here and main.cpp's event loop would catch an
    // uncaught exception either. Losing the prescan for this toggle isn't
    // worth crashing the whole app over; the real-time loudness meter
    // still gives a usable (if slower-converging) reading without it.
    try {
        m_loudnessPrescanThread = std::thread([this, generation, filenameCopy, audioStreamIndex]() {
            double integratedLufs = naikav::dsp::prescanIntegratedLufs(filenameCopy, audioStreamIndex);
            if (integratedLufs > -70.0) {
                std::lock_guard<std::mutex> lock(m_pendingPrescanMutex);
                m_pendingPrescanResult = {generation, integratedLufs, true};
            }
        });
    } catch (const std::exception& e) {
        std::cerr << "Warning: could not start background loudness prescan thread: " << e.what() << std::endl;
    }
}

void PlayerController::pollPendingLoudnessPrescan() {
    // Service anything the audio callback deferred because it is not
    // real-time safe -- currently rebuilding the loudness meter's retired
    // filter graph after a seek swapped to its spare. This runs on the
    // render/event thread, which is exactly where multi-millisecond
    // FFmpeg graph construction belongs. Cheap when there is nothing
    // pending (one relaxed atomic load).
    {
        std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
        if (m_hasAudio && m_audioDecoder) {
            m_audioDecoder->serviceDeferredMaintenance();
        }
    }

    PendingLoudnessPrescanResult result;
    {
        std::lock_guard<std::mutex> lock(m_pendingPrescanMutex);
        if (!m_pendingPrescanResult.valid) {
            return;
        }
        result = m_pendingPrescanResult;
        m_pendingPrescanResult.valid = false;
    }
    // Only apply if this result is still for the current file -- a newer
    // openFile()/prescan since this one started bumps the generation, so
    // a slow scan finishing late for a file the user has already left
    // can't clobber the loudness state of whatever's playing now.
    if (result.generation == m_loudnessPrescanGeneration.load() && m_hasAudio) {
        std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
        if (m_audioDecoder) {
            m_audioDecoder->primeLoudnessPrescan(result.lufs);
        }
    }
}

void PlayerController::joinLoudnessPrescanThread() {
    if (m_loudnessPrescanThread.joinable()) {
        m_loudnessPrescanThread.join();
    }
}

void PlayerController::applyGenrePresetIfEnabled() {
    if (!m_audioDspSettings.autoGenrePresetEnabled || !m_demuxer) {
        return;
    }
    std::string genre = m_demuxer->getGenreTag();
    naikav::dsp::AudioDspSettings genreSettings;
    if (naikav::dsp::presetForGenreTag(genre, genreSettings)) {
        // Preserve the toggle itself -- applying a preset struct wholesale
        // would otherwise reset autoGenrePresetEnabled to that preset's
        // (always-false) default, turning the feature off after using it
        // exactly once.
        genreSettings.autoGenrePresetEnabled = true;
        setAudioDspSettings(genreSettings);
        persistAudioDspSettings();
    }
}

bool PlayerController::isEOF() const {
    return m_demuxer ? m_demuxer->isEOF() : false;
}

bool PlayerController::isSeeking() const {
    return m_demuxer && m_demuxer->isSeekRequested();
}

double PlayerController::getSeekReferenceTime() {
    if (m_catchupMode.load() != SeekCatchupMode::NONE) {
        return m_catchupTarget.load();
    }
    return getCurrentTime();
}

std::string PlayerController::getVideoPixelFormat() const {
    std::lock_guard<std::mutex> lock(m_videoDecoderMutex);
    if (m_videoDecoder) {
        return m_videoDecoder->getPixelFormatName();
    }
    return "unknown";
}

bool PlayerController::isVideoHardware() const {
    std::lock_guard<std::mutex> lock(m_videoDecoderMutex);
    if (m_videoDecoder) {
        return m_videoDecoder->isHardware();
    }
    return false;
}

void PlayerController::videoThreadLoop() {
    while (m_videoThreadRunning) {
        if (!m_videoThreadEnabled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (m_state != PlayerState::PLAYING && m_state != PlayerState::PAUSED && m_state != PlayerState::OPENED) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (m_seeking.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // While a catch-up waits for the demuxer to reposition, don't decode:
        // a leftover packet from the old position could otherwise produce a
        // frame past the target and end the catch-up on the wrong frame.
        if (m_catchupMode.load() != SeekCatchupMode::NONE &&
            m_demuxer && m_demuxer->isSeekRequested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        if (m_decodedFrameQueue.size() >= 8) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        bool decoded = false;
        bool converted = false;
        const AVFrame* srcFrame = nullptr;
        double framePts = 0.0;
        int frameWidth = 0;
        int frameHeight = 0;

        // Frames decoded under an older epoch (i.e. a seek landed while this
        // decode was in flight) belong to the pre-seek stream: discard them
        // so they are never shown or matched against the catch-up target.
        const uint64_t decodeEpoch = m_catchupEpoch.load();

        {
            std::lock_guard<std::mutex> lock(m_videoDecoderMutex);
            if (m_videoDecoder && !m_seeking.load()) {
                decoded = m_videoDecoder->decodeNextFrame();
                if (decoded) {
                    converted = m_videoDecoder->convertFrame(m_resolutionOption.load(),
                                                            getHdrToneMapSettings());
                    if (converted) {
                        srcFrame = m_videoDecoder->getYUVFrame();
                        if (srcFrame && srcFrame->data[0]) {
                            framePts = m_videoDecoder->getCurrentFramePts();
                            // Use the frame's own dimensions: hardware decoders
                            // (e.g. v4l2m2m) may output sizes that differ from
                            // the codec context, and the SDL texture must match
                            // the plane data exactly. convertFrame() never
                            // produces a frame with width/height <= 0 on
                            // success (both its native-passthrough and
                            // rescale paths guarantee positive dimensions),
                            // so there's no fallback to the codec context here.
                            frameWidth = srcFrame->width;
                            frameHeight = srcFrame->height;
                        }
                    }
                }
            }
        }

        if (decoded && m_catchupEpoch.load() != decodeEpoch) {
            // Stale pre-seek frame; drop it silently.
        } else if (decoded && converted && srcFrame && srcFrame->data[0]) {
            SeekCatchupMode mode = m_catchupMode.load();
            // A silent landing shows nothing until the target frame: frames
            // decoded on the way there are dropped, not displayed.
            bool display = !(mode != SeekCatchupMode::NONE &&
                             framePts < m_catchupTarget.load() - 0.005);

            if (display) {
                DecodedFrame df;
                df.frame = av_frame_alloc();
                if (df.frame) {
                    int err = av_frame_ref(df.frame, srcFrame);
                    if (err >= 0) {
                        df.pts = framePts;
                        df.width = frameWidth;
                        df.height = frameHeight;
                        // Bounded wait, not an unconditional block: this
                        // thread also owns draining m_videoQueue, so an
                        // indefinite block here (e.g. the render thread
                        // stalling for any reason) would silently stop this
                        // thread from consuming any more video packets,
                        // which in turn backs up the demuxer. Past the
                        // timeout, drop the oldest buffered frame and keep
                        // decoding instead of ever getting stuck.
                        if (!m_decodedFrameQueue.push_wait_or_drop(df, std::chrono::milliseconds(500),
                                                                    [](DecodedFrame& d) {
                                                                        if (d.frame) av_frame_free(&d.frame);
                                                                    })) {
                            av_frame_free(&df.frame);
                        }
                    } else {
                        av_frame_free(&df.frame);
                    }
                }
            }

            if (mode != SeekCatchupMode::NONE &&
                framePts >= m_catchupTarget.load() - 0.005) {
                finishCatchup(framePts);
            }
        } else if (!decoded) {
            // Never leave a catch-up hanging at end of stream: if the file
            // ran out before the target, land where playback actually is.
            if (m_catchupMode.load() != SeekCatchupMode::NONE &&
                m_demuxer && m_demuxer->isEOF() && m_videoQueue.empty() &&
                m_decodedFrameQueue.empty()) {
                finishCatchup(m_catchupPos.load());
            }
            // Sleep a little bit when queue is empty to avoid high CPU usage
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

// The catch-up reached the seek target (or end of stream): re-sync the clocks
// and hand control back to normal real-time playback.
void PlayerController::finishCatchup(double resumePts) {
    std::lock_guard<std::mutex> lock(m_catchupMutex);
    if (m_catchupMode.load() == SeekCatchupMode::NONE) {
        return;
    }

    // A concurrent seek may have pushed the target further out between the
    // caller's check and this lock; keep catching up unless the stream is over.
    bool atEOF = m_demuxer && m_demuxer->isEOF() && m_videoQueue.empty() &&
                 m_decodedFrameQueue.empty();
    if (resumePts < m_catchupTarget.load() - 0.005 && !atEOF) {
        return;
    }

    uint64_t currentEpoch = m_catchupEpoch.load(std::memory_order_relaxed);
    m_catchupMode.store(SeekCatchupMode::NONE);
    if (m_metrics->m_profilingEnabled.load(std::memory_order_relaxed)) {
        if (m_seekStartEpoch == currentEpoch) {
            auto end = std::chrono::steady_clock::now();
            float ms = static_cast<float>(std::chrono::duration<double, std::milli>(end - m_seekStartTime).count());
            m_metrics->recordSeekLatency(ms);
        }
    }
    if (m_demuxer) {
        m_demuxer->setCatchup(SeekCatchupMode::NONE, 0.0);
    }

    m_catchupPos.store(resumePts);
    m_videoClock.store(resumePts);
    m_lastSystemTime.store(getSystemTimeInSeconds());

    {
        std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
        if (m_hasAudio && m_audioDecoder) {
            m_audioDecoder->flush();
            m_audioDecoder->setClock(resumePts);
            if (m_resumeAfterCatchup.load() && m_state == PlayerState::PLAYING) {
                m_audioDecoder->resume();
            }
        }
    }

    std::cout << "Seek catch-up reached " << resumePts
              << "s, resuming real-time playback" << std::endl;
}

void PlayerController::loadSettings() {
    m_resolutionOption.store(ResolutionOption::ORIGINAL);
    m_hdrToneMapEnabled.store(true);
    m_hdrTargetPeakNits.store(100.0f);
    m_audioDspSettings = naikav::dsp::AudioDspSettings{};
    m_channelOption = AudioChannelOption::AUTO;
    m_outputBitDepth = AudioOutputBitDepth::BIT_16;
    m_outputDeviceName.clear();
    m_resamplerQuality = ResamplerQuality::MEDIUM;

    std::ifstream f("player_settings.txt");
    if (!f.is_open()) {
        return;
    }

    std::string firstLine;
    std::getline(f, firstLine);
    if (firstLine.find('=') == std::string::npos) {
        // Legacy format: a single bare integer (ResolutionOption ordinal),
        // written before DSP/loudness settings existed. Parse just that;
        // DSP settings stay at their just-reset defaults above.
        int optVal = 0;
        std::istringstream legacy(firstLine);
        if (legacy >> optVal && optVal >= 0 && optVal < static_cast<int>(ResolutionOption::COUNT)) {
            m_resolutionOption.store(static_cast<ResolutionOption>(optVal));
            std::cout << "Loaded settings (legacy format): ResolutionOption=" << optVal << std::endl;
        }
        return;
    }

    // Current format: one "key=value" per line. Unknown keys and
    // unparsable values are silently skipped rather than failing the
    // whole load, so a future version adding new keys stays
    // forward/backward tolerant of older/newer settings files.
    auto applyLine = [this](const std::string& line) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) return;
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        if (value.empty()) return;

        try {
            if (key == "resolution") {
                int v = std::stoi(value);
                if (v >= 0 && v < static_cast<int>(ResolutionOption::COUNT)) {
                    m_resolutionOption.store(static_cast<ResolutionOption>(v));
                }
            } else if (key == "dsp_enabled") {
                m_audioDspSettings.dspEnabled = (std::stoi(value) != 0);
            } else if (key.rfind("eq_band_freq", 0) == 0) {
                int band = std::stoi(key.substr(12));
                if (band >= 0 && band < naikav::dsp::ParametricEQ::kNumBands) {
                    m_audioDspSettings.eqBandFreqHz[band] = std::stof(value);
                }
            } else if (key.rfind("eq_band_q", 0) == 0) {
                int band = std::stoi(key.substr(9));
                if (band >= 0 && band < naikav::dsp::ParametricEQ::kNumBands) {
                    m_audioDspSettings.eqBandQ[band] = std::stof(value);
                }
            } else if (key.rfind("eq_band", 0) == 0) {
                int band = std::stoi(key.substr(7));
                if (band >= 0 && band < naikav::dsp::ParametricEQ::kNumBands) {
                    m_audioDspSettings.eqBandGainDb[band] = std::stof(value);
                }
            } else if (key == "compressor_enabled") {
                m_audioDspSettings.compressorEnabled = (std::stoi(value) != 0);
            } else if (key == "compressor_threshold") {
                m_audioDspSettings.compressorThresholdDb = std::stof(value);
            } else if (key == "compressor_ratio") {
                m_audioDspSettings.compressorRatio = std::stof(value);
            } else if (key == "limiter_enabled") {
                m_audioDspSettings.limiterEnabled = (std::stoi(value) != 0);
            } else if (key == "limiter_ceiling") {
                m_audioDspSettings.limiterCeilingDb = std::stof(value);
            } else if (key == "crossover_enabled") {
                m_audioDspSettings.crossoverEnabled = (std::stoi(value) != 0);
            } else if (key == "crossover_cutoff") {
                m_audioDspSettings.crossoverCutoffHz = std::stof(value);
            } else if (key == "crossover_bass_redirect") {
                m_audioDspSettings.crossoverBassRedirectEnabled = (std::stoi(value) != 0);
            } else if (key == "crossover_lfe_gain") {
                m_audioDspSettings.crossoverLfeGainDb = std::stof(value);
            } else if (key == "loudness_enabled") {
                m_audioDspSettings.loudnessEnabled = (std::stoi(value) != 0);
            } else if (key == "loudness_target") {
                m_audioDspSettings.loudnessTargetLufs = std::stof(value);
            } else if (key == "widener_enabled") {
                m_audioDspSettings.widenerEnabled = (std::stoi(value) != 0);
            } else if (key == "widener_width") {
                m_audioDspSettings.widenerWidth = std::stof(value);
            } else if (key == "surround3d_enabled") {
                m_audioDspSettings.surround3dEnabled = (std::stoi(value) != 0);
            } else if (key == "surround3d_intensity") {
                m_audioDspSettings.surround3dIntensity = std::stof(value);
            } else if (key == "balance") {
                m_audioDspSettings.balance = std::stof(value);
            } else if (key == "noise_gate_enabled") {
                m_audioDspSettings.noiseGateEnabled = (std::stoi(value) != 0);
            } else if (key == "noise_gate_threshold") {
                m_audioDspSettings.noiseGateThresholdDb = std::stof(value);
            } else if (key == "noise_gate_ratio") {
                m_audioDspSettings.noiseGateRatio = std::stof(value);
            } else if (key == "noise_gate_range") {
                m_audioDspSettings.noiseGateRangeDb = std::stof(value);
            } else if (key == "multiband_enabled") {
                m_audioDspSettings.multibandEnabled = (std::stoi(value) != 0);
            } else if (key == "multiband_low_mid_hz") {
                m_audioDspSettings.multibandLowMidHz = std::stof(value);
            } else if (key == "multiband_mid_high_hz") {
                m_audioDspSettings.multibandMidHighHz = std::stof(value);
            } else if (key == "multiband_low_threshold") {
                m_audioDspSettings.multibandLowThresholdDb = std::stof(value);
            } else if (key == "multiband_low_ratio") {
                m_audioDspSettings.multibandLowRatio = std::stof(value);
            } else if (key == "multiband_mid_threshold") {
                m_audioDspSettings.multibandMidThresholdDb = std::stof(value);
            } else if (key == "multiband_mid_ratio") {
                m_audioDspSettings.multibandMidRatio = std::stof(value);
            } else if (key == "multiband_high_threshold") {
                m_audioDspSettings.multibandHighThresholdDb = std::stof(value);
            } else if (key == "multiband_high_ratio") {
                m_audioDspSettings.multibandHighRatio = std::stof(value);
            } else if (key == "auto_genre_preset_enabled") {
                m_audioDspSettings.autoGenrePresetEnabled = (std::stoi(value) != 0);
            } else if (key == "spectrum_analyzer_enabled") {
                m_audioDspSettings.spectrumAnalyzerEnabled = (std::stoi(value) != 0);
            } else if (key == "channel_option") {
                int v = std::stoi(value);
                if (v >= 0 && v < static_cast<int>(AudioChannelOption::COUNT)) {
                    m_channelOption = static_cast<AudioChannelOption>(v);
                }
            } else if (key == "output_bit_depth") {
                int v = std::stoi(value);
                if (v >= 0 && v < static_cast<int>(AudioOutputBitDepth::COUNT)) {
                    m_outputBitDepth = static_cast<AudioOutputBitDepth>(v);
                }
            } else if (key == "output_device_name") {
                m_outputDeviceName = value;
            } else if (key == "resampler_quality") {
                int v = std::stoi(value);
                if (v >= 0 && v < static_cast<int>(ResamplerQuality::COUNT)) {
                    m_resamplerQuality = static_cast<ResamplerQuality>(v);
                }
            } else if (key == "hdr_tone_map_enabled") {
                m_hdrToneMapEnabled.store(std::stoi(value) != 0);
            } else if (key == "hdr_target_peak_nits") {
                float v = std::stof(value);
                if (v >= 50.0f && v <= 1000.0f) {
                    m_hdrTargetPeakNits.store(v);
                }
            } else if (key == "playlist_current_index") {
                m_pendingPlaylistCurrentIndex = std::stoi(value);
            } else if (key == "playlist_repeat_mode") {
                m_pendingPlaylistRepeatMode = std::stoi(value);
            } else if (key == "playlist_shuffle") {
                m_pendingPlaylistShuffle = (std::stoi(value) != 0);
            }
        } catch (const std::exception&) {
            // Malformed value for this key; skip it and keep going.
        }
    };

    applyLine(firstLine);
    std::string line;
    while (std::getline(f, line)) {
        applyLine(line);
    }

    // Clamp/repair everything that came off disk before it can reach a DSP
    // setter. Each value above is parsed with std::stof and stored
    // verbatim, so a hand-edited or truncated player_settings.txt could
    // otherwise put a crossover cutoff above Nyquist -- or a literal
    // "nan" -- straight into a biquad, whose feedback state then
    // propagates Inf/NaN forever. (Verified: restoring a sane slider value
    // does not recover it; only a seek does.) Sanitizing a file this app
    // itself wrote is a no-op, since every clamp matches the UI's range.
    m_audioDspSettings.sanitize();

    std::cout << "Loaded settings: ResolutionOption=" << static_cast<int>(m_resolutionOption.load())
              << ", AudioDspSettings.dspEnabled=" << m_audioDspSettings.dspEnabled
              << ", AudioDspSettings.loudnessEnabled=" << m_audioDspSettings.loudnessEnabled << std::endl;
}

void PlayerController::saveSettings() {
    std::ofstream f("player_settings.txt");
    if (!f.is_open()) {
        return;
    }
    const auto& s = m_audioDspSettings;
    f << "resolution=" << static_cast<int>(m_resolutionOption.load()) << "\n";
    f << "dsp_enabled=" << (s.dspEnabled ? 1 : 0) << "\n";
    for (int i = 0; i < naikav::dsp::ParametricEQ::kNumBands; ++i) {
        f << "eq_band" << i << "=" << s.eqBandGainDb[i] << "\n";
        f << "eq_band_freq" << i << "=" << s.eqBandFreqHz[i] << "\n";
        f << "eq_band_q" << i << "=" << s.eqBandQ[i] << "\n";
    }
    f << "compressor_enabled=" << (s.compressorEnabled ? 1 : 0) << "\n";
    f << "compressor_threshold=" << s.compressorThresholdDb << "\n";
    f << "compressor_ratio=" << s.compressorRatio << "\n";
    f << "limiter_enabled=" << (s.limiterEnabled ? 1 : 0) << "\n";
    f << "limiter_ceiling=" << s.limiterCeilingDb << "\n";
    f << "crossover_enabled=" << (s.crossoverEnabled ? 1 : 0) << "\n";
    f << "crossover_cutoff=" << s.crossoverCutoffHz << "\n";
    f << "crossover_bass_redirect=" << (s.crossoverBassRedirectEnabled ? 1 : 0) << "\n";
    f << "crossover_lfe_gain=" << s.crossoverLfeGainDb << "\n";
    f << "loudness_enabled=" << (s.loudnessEnabled ? 1 : 0) << "\n";
    f << "loudness_target=" << s.loudnessTargetLufs << "\n";
    f << "widener_enabled=" << (s.widenerEnabled ? 1 : 0) << "\n";
    f << "widener_width=" << s.widenerWidth << "\n";
    f << "surround3d_enabled=" << (s.surround3dEnabled ? 1 : 0) << "\n";
    f << "surround3d_intensity=" << s.surround3dIntensity << "\n";
    f << "balance=" << s.balance << "\n";
    f << "noise_gate_enabled=" << (s.noiseGateEnabled ? 1 : 0) << "\n";
    f << "noise_gate_threshold=" << s.noiseGateThresholdDb << "\n";
    f << "noise_gate_ratio=" << s.noiseGateRatio << "\n";
    f << "noise_gate_range=" << s.noiseGateRangeDb << "\n";
    f << "multiband_enabled=" << (s.multibandEnabled ? 1 : 0) << "\n";
    f << "multiband_low_mid_hz=" << s.multibandLowMidHz << "\n";
    f << "multiband_mid_high_hz=" << s.multibandMidHighHz << "\n";
    f << "multiband_low_threshold=" << s.multibandLowThresholdDb << "\n";
    f << "multiband_low_ratio=" << s.multibandLowRatio << "\n";
    f << "multiband_mid_threshold=" << s.multibandMidThresholdDb << "\n";
    f << "multiband_mid_ratio=" << s.multibandMidRatio << "\n";
    f << "multiband_high_threshold=" << s.multibandHighThresholdDb << "\n";
    f << "multiband_high_ratio=" << s.multibandHighRatio << "\n";
    f << "auto_genre_preset_enabled=" << (s.autoGenrePresetEnabled ? 1 : 0) << "\n";
    f << "spectrum_analyzer_enabled=" << (s.spectrumAnalyzerEnabled ? 1 : 0) << "\n";
    f << "channel_option=" << static_cast<int>(m_channelOption) << "\n";
    f << "output_bit_depth=" << static_cast<int>(m_outputBitDepth) << "\n";
    f << "output_device_name=" << m_outputDeviceName << "\n";
    f << "resampler_quality=" << static_cast<int>(m_resamplerQuality) << "\n";
    f << "hdr_tone_map_enabled=" << (m_hdrToneMapEnabled.load() ? 1 : 0) << "\n";
    f << "hdr_target_peak_nits=" << m_hdrTargetPeakNits.load() << "\n";
    f << "playlist_current_index=" << m_playlist.getCurrentIndex() << "\n";
    f << "playlist_repeat_mode=" << static_cast<int>(m_playlist.getRepeatMode()) << "\n";
    f << "playlist_shuffle=" << (m_playlist.isShuffle() ? 1 : 0) << "\n";
    std::cout << "Saved settings: ResolutionOption=" << static_cast<int>(m_resolutionOption.load()) << std::endl;
}

void PlayerController::loadPlaylistState() {
    m_playlist.loadM3U("playlist.m3u8"); // no-op (leaves an empty playlist) if missing/empty

    int repeatOrdinal = m_pendingPlaylistRepeatMode;
    if (repeatOrdinal >= 0 && repeatOrdinal <= static_cast<int>(naikav::playlist::RepeatMode::One)) {
        m_playlist.setRepeatMode(static_cast<naikav::playlist::RepeatMode>(repeatOrdinal));
    }
    m_playlist.setShuffle(m_pendingPlaylistShuffle);
    if (m_pendingPlaylistCurrentIndex >= 0) {
        m_playlist.setCurrentIndex(m_pendingPlaylistCurrentIndex);
    }
    // Deliberately does not auto-play -- matches today's "no CLI arg -> sit
    // idle" behavior; the restored playlist just sits ready to resume from.
}

void PlayerController::savePlaylistState() {
    m_playlist.saveM3U("playlist.m3u8");
    saveSettings(); // also persists playlist_current_index/repeat_mode/shuffle
}

void PlayerController::setResolutionOption(ResolutionOption option) {
    m_resolutionOption.store(option);
    saveSettings();
    if (m_hasVideo && (m_state == PlayerState::OPENED || m_state == PlayerState::PAUSED)) {
        seek(getCurrentTime());
    }
}

void PlayerController::setHdrToneMapEnabled(bool enabled) {
    m_hdrToneMapEnabled.store(enabled);
    saveSettings();
    // While paused, nothing decodes a fresh frame to make the change
    // visible, so re-decode the current position -- the same nudge
    // setResolutionOption() uses for the same reason.
    if (m_hasVideo && (m_state == PlayerState::OPENED || m_state == PlayerState::PAUSED)) {
        seek(getCurrentTime());
    }
}

void PlayerController::setHdrTargetPeakNits(float nits) {
    // Clamped to the range a display peak can sensibly take: below ~50
    // nits the roll-off crushes everything, and beyond 1000 there is
    // nothing left to map down to.
    m_hdrTargetPeakNits.store(std::clamp(nits, 50.0f, 1000.0f));
    saveSettings();
    if (m_hasVideo && (m_state == PlayerState::OPENED || m_state == PlayerState::PAUSED)) {
        seek(getCurrentTime());
    }
}

int PlayerController::getPlaybackWidth() const {
    int nativeW = getVideoWidth();
    int nativeH = getVideoHeight();
    int targetW = nativeW;
    int targetH = nativeH;
    getTargetDimensions(m_resolutionOption.load(), nativeW, nativeH, targetW, targetH);
    return targetW;
}

int PlayerController::getPlaybackHeight() const {
    int nativeW = getVideoWidth();
    int nativeH = getVideoHeight();
    int targetW = nativeW;
    int targetH = nativeH;
    getTargetDimensions(m_resolutionOption.load(), nativeW, nativeH, targetW, targetH);
    return targetH;
}

size_t PlayerController::getAudioFrameQueueSize() const {
    std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
    if (m_hasAudio && m_audioDecoder) {
        // Frame size is (channel count * 2 bytes for 16-bit PCM); channel
        // count depends on the resolved output layout (stereo downmix or
        // preserved surround), not always 2.
        int bytesPerFrame = m_audioDecoder->getOutputChannelCount() * 2;
        if (bytesPerFrame <= 0) {
            return 0;
        }
        return m_audioDecoder->getAudioStreamQueuedBytes() / bytesPerFrame;
    }
    return 0;
}

ColorPipelineInfo PlayerController::getColorInfo() const {
    std::lock_guard<std::mutex> lock(m_videoDecoderMutex);
    if (m_videoDecoder) {
        return m_videoDecoder->getColorInfo();
    }
    return ColorPipelineInfo{};
}

std::vector<naikav::subtitle::SubtitleTrackInfo> PlayerController::getSubtitleTracks() const {
    std::lock_guard<std::mutex> lock(m_subtitleMutex);
    std::vector<naikav::subtitle::SubtitleTrackInfo> result = m_cachedSubtitleTracks;
    if (m_hasExternalSubtitle) {
        result.push_back(m_externalSubtitleTrack);
    }
    return result;
}

void PlayerController::selectSubtitleTrack(int trackId) {
    std::lock_guard<std::mutex> lock(m_subtitleMutex);
    m_selectedSubtitleTrack.store(trackId);

    if (trackId == -1) {
        // Disabled / Off
        if (m_demuxer) {
            m_demuxer->setSubtitleStreamIndex(-1);
        }
        m_subtitleQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
        if (m_subtitleDecoder && !m_subtitleDecoder->isExternal()) {
            m_subtitleDecoder->reset();
        }
        std::cout << "Subtitle track disabled (Off)" << std::endl;
    } else if (trackId == -2) {
        // External subtitle selected
        if (m_demuxer) {
            m_demuxer->setSubtitleStreamIndex(-1);
        }
        m_subtitleQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
        if (!m_subtitleDecoder || !m_subtitleDecoder->isExternal()) {
            if (m_hasExternalSubtitle) {
                m_subtitleDecoder = std::make_unique<naikav::subtitle::SubtitleDecoder>();
                m_subtitleDecoder->loadExternalFile(m_externalSubtitleTrack.sourcePath);
            }
        }
        std::cout << "Selected external subtitle track: " << m_externalSubtitleTrack.sourcePath << std::endl;
    } else if (trackId >= 0) {
        // Embedded subtitle stream
        if (m_demuxer) {
            m_demuxer->setSubtitleStreamIndex(trackId);
            m_subtitleQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
            m_subtitleDecoder = std::make_unique<naikav::subtitle::SubtitleDecoder>();
            AVCodecParameters* params = m_demuxer->getSubtitleCodecParams(trackId);
            AVRational tb = m_demuxer->getSubtitleTimeBase(trackId);
            int64_t st = m_demuxer->getSubtitleStartTime(trackId);
            m_subtitleDecoder->init(params, tb, st);
            m_subtitleDecoder->attachSeekGeneration(m_demuxer->seekGenerationPtr());
            std::cout << "Selected embedded subtitle track index: " << trackId << std::endl;
        }
    }
}

bool PlayerController::loadExternalSubtitle(const std::string& filepath) {
    if (filepath.empty()) return false;

    auto decoder = std::make_unique<naikav::subtitle::SubtitleDecoder>();
    if (!decoder->loadExternalFile(filepath)) {
        std::cerr << "Failed to parse external subtitle file: " << filepath << std::endl;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_subtitleMutex);
        m_subtitleDecoder = std::move(decoder);
        m_externalSubtitleTrack.id = -2;
        m_externalSubtitleTrack.isExternal = true;
        m_externalSubtitleTrack.sourcePath = filepath;

        size_t lastSlash = filepath.find_last_of("/\\");
        std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;
        m_externalSubtitleTrack.title = filename;
        m_externalSubtitleTrack.language = "ext";
        m_externalSubtitleTrack.codecName = "external";
        m_hasExternalSubtitle = true;
    }

    selectSubtitleTrack(-2);
    return true;
}

void PlayerController::pollSubtitlePackets() {
    if (!m_subtitleDecoder || m_selectedSubtitleTrack.load() < 0) {
        return;
    }

    AVPacket* pkt = nullptr;
    while (m_subtitleQueue.try_pop(pkt)) {
        if (pkt) {
            m_subtitleDecoder->processPacket(pkt);
            av_packet_free(&pkt);
        }
    }
}

std::string PlayerController::getCurrentSubtitleText() {
    double pts = getCurrentTime();

    std::lock_guard<std::mutex> lock(m_subtitleMutex);
    pollSubtitlePackets();

    if (m_selectedSubtitleTrack.load() == -1 || !m_subtitleDecoder) {
        return "";
    }

    return m_subtitleDecoder->getActiveSubtitleText(pts, m_subtitleDelay.load());
}

void PlayerController::autoProbeExternalSubtitles(const std::string& mediaFilename) {
    if (mediaFilename.empty()) return;

    size_t lastDot = mediaFilename.find_last_of('.');
    if (lastDot == std::string::npos) return;

    std::string base = mediaFilename.substr(0, lastDot);
    std::vector<std::string> candidates = {
        base + ".srt", base + ".SRT",
        base + ".vtt", base + ".VTT",
        base + ".ass", base + ".ASS",
        base + ".ssa", base + ".SSA",
        base + ".en.srt", base + ".eng.srt",
        base + ".en.vtt", base + ".eng.vtt"
    };

    for (const auto& path : candidates) {
        std::ifstream f(path);
        if (f.good()) {
            f.close();
            if (loadExternalSubtitle(path)) {
                std::cout << "Auto-detected external subtitle file: " << path << std::endl;
                break;
            }
        }
    }
}

std::string PlayerController::getActiveSubtitleTrackName() const {
    int track = m_selectedSubtitleTrack.load();
    if (track == -1) {
        return "Off";
    }
    if (track == -2) {
        std::lock_guard<std::mutex> lock(m_subtitleMutex);
        return m_externalSubtitleTrack.title.empty() ? "External" : m_externalSubtitleTrack.title;
    }

    std::lock_guard<std::mutex> lock(m_subtitleMutex);
    auto it = std::find_if(m_cachedSubtitleTracks.begin(), m_cachedSubtitleTracks.end(),
        [track](const naikav::subtitle::SubtitleTrackInfo& t) {
            return t.id == track;
        });
    if (it != m_cachedSubtitleTracks.end()) {
        return it->title + " (" + it->language + ")";
    }
    return "Track " + std::to_string(track);
}

std::vector<naikav::audio::AudioTrackInfo> PlayerController::getAudioTracks() const {
    std::lock_guard<std::mutex> lock(m_audioTrackMutex);
    std::vector<naikav::audio::AudioTrackInfo> result = m_cachedAudioTracks;
    if (m_hasExternalAudio) {
        result.push_back(m_externalAudioTrack);
    }
    return result;
}

bool PlayerController::selectAudioTrack(int trackId) {
    std::lock_guard<std::mutex> lock(m_audioTrackMutex);

    if (trackId == m_selectedAudioTrack.load() && m_hasAudio) {
        return true;
    }

    if (m_state == PlayerState::UNINITIALIZED || m_state == PlayerState::ERROR_STATE) {
        m_selectedAudioTrack.store(trackId);
        return true;
    }

    double currentPos = getCurrentTime();
    bool wasPlaying = (m_state == PlayerState::PLAYING);

    if (trackId == -1) {
        // Disabled / Mute Audio Stream
        m_selectedAudioTrack.store(-1);
        if (m_demuxer) {
            m_demuxer->selectAudioStream(-1);
        }
        m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
        {
            std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
            if (m_audioDecoder) {
                m_audioDecoder->stop();
                m_audioDecoder.reset();
            }
            m_hasAudio = false;
        }
        std::cout << "Audio track disabled (Off)" << std::endl;
        return true;
    }

    if (trackId == -2) {
        // External Audio Track selected
        if (!m_hasExternalAudio || !m_externalAudioDemuxer) {
            std::cerr << "Error: No external audio track loaded" << std::endl;
            return false;
        }

        if (m_demuxer) {
            m_demuxer->selectAudioStream(-1); // Stop embedded audio routing
        }

        int extAudioIdx = m_externalAudioDemuxer->getAudioStreamIndex();
        if (extAudioIdx < 0) {
            std::cerr << "Error: External audio file has no valid audio stream" << std::endl;
            return false;
        }

        {
            std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
            // Stop current audio decoder and clear queue
            if (m_audioDecoder) {
                m_audioDecoder->stop();
                m_audioDecoder.reset();
            }
            m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });

            m_audioDecoder = std::make_unique<AudioDecoder>(
                m_externalAudioDemuxer->getAudioCodecParams(extAudioIdx),
                m_externalAudioDemuxer->getAudioTimeBase(extAudioIdx),
                m_externalAudioDemuxer->getAudioStartTime(extAudioIdx),
                m_audioQueue,
                &m_audioDecodeTimeUs
            );
            m_audioDecoder->setChannelOption(m_channelOption);
            m_audioDecoder->setOutputBitDepth(m_outputBitDepth);
            m_audioDecoder->setOutputDeviceName(m_outputDeviceName);
            m_audioDecoder->setResamplerQuality(m_resamplerQuality);
            m_hasAudio = m_audioDecoder->init();
            if (m_hasAudio) {
                m_audioDecoder->setVolume(m_volume);
                m_audioDecoder->setPlaybackSpeed(m_playbackSpeed.load());
                m_audioDecoder->applyDspSettings(m_audioDspSettings);
                m_externalAudioDemuxer->attachAudioPausedFlag(&m_audioDecoder->pausedFlag());
                m_audioDecoder->attachSeekGeneration(m_externalAudioDemuxer->seekGenerationPtr());
                m_audioDecoder->setClock(currentPos);
            }
        }

        m_selectedAudioTrack.store(-2);

        // Synchronize external audio to current playback position
        m_externalAudioDemuxer->seek(currentPos);
        if (m_hasVideo) {
            seek(currentPos);
        } else {
            if (wasPlaying && m_hasAudio) {
                std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
                m_audioDecoder->start();
                m_state = PlayerState::PLAYING;
            }
        }
        std::cout << "Selected external audio track: " << m_externalAudioTrack.sourcePath << std::endl;
        return true;
    }

    // Embedded audio stream (trackId >= 0)
    if (trackId >= 0 && m_demuxer) {
        if (!m_demuxer->selectAudioStream(trackId)) {
            std::cerr << "Error: Failed to select audio stream index " << trackId << std::endl;
            return false;
        }

        {
            std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
            if (m_audioDecoder) {
                m_audioDecoder->stop();
                m_audioDecoder.reset();
            }
            m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });

            m_audioDecoder = std::make_unique<AudioDecoder>(
                m_demuxer->getAudioCodecParams(trackId),
                m_demuxer->getAudioTimeBase(trackId),
                m_demuxer->getAudioStartTime(trackId),
                m_audioQueue,
                &m_audioDecodeTimeUs
            );
            m_audioDecoder->setChannelOption(m_channelOption);
            m_audioDecoder->setOutputBitDepth(m_outputBitDepth);
            m_audioDecoder->setOutputDeviceName(m_outputDeviceName);
            m_audioDecoder->setResamplerQuality(m_resamplerQuality);
            m_hasAudio = m_audioDecoder->init();
            if (m_hasAudio) {
                m_audioDecoder->setVolume(m_volume);
                m_audioDecoder->setPlaybackSpeed(m_playbackSpeed.load());
                m_audioDecoder->applyDspSettings(m_audioDspSettings);
            }
        }
        if (m_hasAudio) {
            // Released m_audioDecoderMutex above before these two calls, for
            // the same self-deadlock reason as in openFile() (see its
            // comment): both can re-enter and re-acquire the mutex.
            applyGenrePresetIfEnabled();
            if (m_audioDspSettings.loudnessEnabled) {
                prescanLoudnessForCurrentFile();
            }
            std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
            m_demuxer->attachAudioPausedFlag(&m_audioDecoder->pausedFlag());
            m_audioDecoder->attachSeekGeneration(m_demuxer->seekGenerationPtr());
            m_audioDecoder->setClock(currentPos);
        }

        m_selectedAudioTrack.store(trackId);

        // Seek format context to current position to align queues and flush buffers
        if (m_hasVideo) {
            seek(currentPos);
        } else {
            m_demuxer->seek(currentPos);
            if (wasPlaying && m_hasAudio) {
                std::lock_guard<std::mutex> audioLock(m_audioDecoderMutex);
                m_audioDecoder->start();
                m_state = PlayerState::PLAYING;
            }
        }
        std::cout << "Selected embedded audio track index: " << trackId << std::endl;
        return true;
    }

    return false;
}

bool PlayerController::loadExternalAudio(const std::string& filepath) {
    if (filepath.empty()) return false;

    // Create and open external audio demuxer
    auto extDemuxer = std::make_unique<Demuxer>(
        filepath, m_dummyVideoQueue, m_audioQueue,
        m_metrics->m_demuxTimePerPacketUs, m_metrics->m_profilingEnabled
    );

    if (!extDemuxer->open()) {
        std::cerr << "Failed to open external audio file: " << filepath << std::endl;
        return false;
    }

    if (extDemuxer->getAudioStreamIndex() < 0) {
        std::cerr << "External media file has no audio streams: " << filepath << std::endl;
        return false;
    }

    extDemuxer->start();

    {
        std::lock_guard<std::mutex> lock(m_audioTrackMutex);
        if (m_externalAudioDemuxer) {
            m_externalAudioDemuxer->stop();
        }
        m_externalAudioDemuxer = std::move(extDemuxer);

        // getAudioStreamIndex() >= 0 was already validated above, and
        // Demuxer::open() only ever sets a non-negative audio stream index
        // via selectAudioStream(), which it only calls when m_audioTracks is
        // non-empty -- so extTracks is guaranteed non-empty here.
        auto extTracks = m_externalAudioDemuxer->getAudioTracks();
        m_externalAudioTrack = extTracks[0];
        m_externalAudioTrack.id = -2;
        m_externalAudioTrack.isExternal = true;
        m_externalAudioTrack.sourcePath = filepath;

        size_t lastSlash = filepath.find_last_of("/\\");
        std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;
        m_externalAudioTrack.title = filename;
        m_hasExternalAudio = true;
    }

    return selectAudioTrack(-2);
}

void PlayerController::removeExternalAudio() {
    int fallbackTrackId = -1;
    {
        std::lock_guard<std::mutex> lock(m_audioTrackMutex);
        if (!m_hasExternalAudio) return;

        if (m_externalAudioDemuxer) {
            m_externalAudioDemuxer->stop();
            m_externalAudioDemuxer.reset();
        }
        m_hasExternalAudio = false;
        m_externalAudioTrack = naikav::audio::AudioTrackInfo{};

        if (!m_cachedAudioTracks.empty()) {
            auto it = std::find_if(m_cachedAudioTracks.begin(), m_cachedAudioTracks.end(),
                                   [](const auto& t) { return t.isDefault; });
            fallbackTrackId = (it != m_cachedAudioTracks.end()) ? it->id : m_cachedAudioTracks[0].id;
        }

    }

    selectAudioTrack(fallbackTrackId);
}

std::string PlayerController::getActiveAudioTrackName() const {
    int track = m_selectedAudioTrack.load();
    if (track == -1) {
        return "Off";
    }
    if (track == -2) {
        std::lock_guard<std::mutex> lock(m_audioTrackMutex);
        return m_externalAudioTrack.title.empty() ? "External Audio" : m_externalAudioTrack.title;
    }

    std::lock_guard<std::mutex> lock(m_audioTrackMutex);
    auto it = std::find_if(m_cachedAudioTracks.begin(), m_cachedAudioTracks.end(),
        [track](const naikav::audio::AudioTrackInfo& t) {
            return t.id == track;
        });
    if (it != m_cachedAudioTracks.end()) {
        std::string name = "[" + it->language + "] " + it->title;
        if (!it->codecName.empty() && it->codecName != "unknown") {
            name += " (" + it->codecName;
            if (!it->channelLayout.empty()) {
                name += ", " + it->channelLayout;
            }
            name += ")";
        }
        return name;
    }
    return "Track " + std::to_string(track);
}


