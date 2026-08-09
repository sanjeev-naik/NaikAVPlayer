#include "PlayerController.hpp"
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
      m_hasAudio(false),
      m_hasVideo(false),
      m_videoClock(0.0),
      m_lastSystemTime(0.0),
      m_volume(0.05f),
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
    m_decodedFrameQueue.attachDepthMirror(&m_metrics->m_decodedFrameQueueDepth);
    loadSettings();
}

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
    m_decodedFrameQueue.reset(); // Clear aborted state set by stop()

    // Create and open the demuxer
    m_demuxer = std::make_unique<Demuxer>(
        filename, m_videoQueue, m_audioQueue,
        m_metrics->m_demuxTimePerPacketUs, m_metrics->m_profilingEnabled);
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
        m_audioDecoder = std::make_unique<AudioDecoder>(
            m_demuxer->getAudioCodecParams(),
            m_demuxer->getAudioTimeBase(),
            m_demuxer->getAudioStartTime(),
            m_audioQueue,
            &m_audioDecodeTimeUs
        );
        m_audioDecoder->setChannelOption(m_channelOption);
        m_hasAudio = m_audioDecoder->init();
        if (m_hasAudio) {
            m_audioDecoder->setVolume(m_volume);
            m_audioDecoder->applyDspSettings(m_audioDspSettings);
            // Must happen before m_demuxer->start(): the read loop is not
            // synchronized with this pointer assignment.
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
    if (m_hasAudio && m_audioDecoder) {
        m_audioDecoder->pause();
        m_audioDecoder->flush();
    }

    m_seeking.store(true);
    m_demuxer->setCatchup(SeekCatchupMode::LANDING, seconds);
    m_demuxer->seek(seconds);
    m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
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
    if (m_hasAudio && wasPlaying) {
        m_audioDecoder->pause();
    }

    // Signal demuxer to seek (clears queues and updates format context)
    m_demuxer->seek(seconds);

    // Force clear our queues immediately from this thread to speed up seek response
    m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
    m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });

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
    m_catchupEpoch.fetch_add(1);
    m_seeking.store(false);
    m_seeked.store(true);
}

void PlayerController::stop() {
    m_state = PlayerState::UNINITIALIZED;

    m_catchupMode.store(SeekCatchupMode::NONE);
    m_resumeAfterCatchup.store(false);
    m_catchupPos.store(0.0);

    // Abort all queues first so any thread waiting on push or pop is unblocked immediately
    m_decodedFrameQueue.abort();
    m_videoQueue.abort();
    m_audioQueue.abort();

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
    if (m_catchupMode.load() != SeekCatchupMode::NONE) {
        return; // clock is frozen while catching up; reset when it lands
    }
    if (m_state == PlayerState::PLAYING && !m_hasAudio) {
        double now = getSystemTimeInSeconds();
        m_videoClock.store(m_videoClock.load() + (now - m_lastSystemTime.load()));
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
        if (m_state == PlayerState::PLAYING && m_loopEnabled) {
            std::cout << "Playback reached end, looping back to start" << std::endl;
            instantSeek(0.0); // seamless wraparound, no rewind animation
            return 0.0;
        }

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
    if (m_hasAudio && m_audioDecoder) {
        return m_audioDecoder->getOutputChannelLayoutName();
    }
    return "Unknown";
}

double PlayerController::getAudioClock() {
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
    if (m_hasAudio && m_audioDecoder) {
        m_audioDecoder->setVolume(m_volume);
    }
}

void PlayerController::setAudioDspSettings(const naikav::dsp::AudioDspSettings& settings) {
    m_audioDspSettings = settings;
    if (m_hasAudio && m_audioDecoder) {
        m_audioDecoder->applyDspSettings(m_audioDspSettings);
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
        AVFrame* srcFrame = nullptr;
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
                    converted = m_videoDecoder->convertFrame(m_resolutionOption.load());
                    if (converted) {
                        srcFrame = m_videoDecoder->getYUVFrame();
                        if (srcFrame && srcFrame->data[0]) {
                            framePts = m_videoDecoder->getCurrentFramePts();
                            // Use the frame's own dimensions: hardware decoders
                            // (e.g. v4l2m2m) may output sizes that differ from
                            // the codec context, and the SDL texture must match
                            // the plane data exactly.
                            frameWidth = srcFrame->width > 0 ? srcFrame->width
                                                             : m_videoDecoder->getWidth();
                            frameHeight = srcFrame->height > 0 ? srcFrame->height
                                                               : m_videoDecoder->getHeight();
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

    if (m_hasAudio && m_audioDecoder) {
        m_audioDecoder->flush();
        m_audioDecoder->setClock(resumePts);
        if (m_resumeAfterCatchup.load() && m_state == PlayerState::PLAYING) {
            m_audioDecoder->resume();
        }
    }

    std::cout << "Seek catch-up reached " << resumePts
              << "s, resuming real-time playback" << std::endl;
}

void PlayerController::loadSettings() {
    m_resolutionOption.store(ResolutionOption::ORIGINAL);
    m_audioDspSettings = naikav::dsp::AudioDspSettings{};
    m_channelOption = AudioChannelOption::AUTO;

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
            } else if (key == "loudness_enabled") {
                m_audioDspSettings.loudnessEnabled = (std::stoi(value) != 0);
            } else if (key == "loudness_target") {
                m_audioDspSettings.loudnessTargetLufs = std::stof(value);
            } else if (key == "channel_option") {
                int v = std::stoi(value);
                if (v >= 0 && v < static_cast<int>(AudioChannelOption::COUNT)) {
                    m_channelOption = static_cast<AudioChannelOption>(v);
                }
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
    }
    f << "compressor_enabled=" << (s.compressorEnabled ? 1 : 0) << "\n";
    f << "compressor_threshold=" << s.compressorThresholdDb << "\n";
    f << "compressor_ratio=" << s.compressorRatio << "\n";
    f << "limiter_enabled=" << (s.limiterEnabled ? 1 : 0) << "\n";
    f << "limiter_ceiling=" << s.limiterCeilingDb << "\n";
    f << "crossover_enabled=" << (s.crossoverEnabled ? 1 : 0) << "\n";
    f << "crossover_cutoff=" << s.crossoverCutoffHz << "\n";
    f << "loudness_enabled=" << (s.loudnessEnabled ? 1 : 0) << "\n";
    f << "loudness_target=" << s.loudnessTargetLufs << "\n";
    f << "channel_option=" << static_cast<int>(m_channelOption) << "\n";
    std::cout << "Saved settings: ResolutionOption=" << static_cast<int>(m_resolutionOption.load()) << std::endl;
}

void PlayerController::setResolutionOption(ResolutionOption option) {
    m_resolutionOption.store(option);
    saveSettings();
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
    if (m_videoDecoder) {
        return m_videoDecoder->getColorInfo();
    }
    return ColorPipelineInfo{};
}
