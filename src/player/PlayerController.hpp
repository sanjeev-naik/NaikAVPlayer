#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include "core/ThreadSafeQueue.hpp"
#include "media/Demuxer.hpp"
#include "audio/AudioDecoder.hpp"
#include "audio/AudioTrack.hpp"
#include "video/VideoDecoder.hpp"
#include "subtitle/SubtitleTrack.hpp"
#include "subtitle/SubtitleDecoder.hpp"
#include "core/PipelineMetrics.hpp"
#include "playlist/Playlist.hpp"
#include <chrono>

extern "C" {
#include <libavutil/frame.h>
}

struct DecodedFrame {
    AVFrame* frame = nullptr;
    double pts = 0.0;
    int width = 0;
    int height = 0;
};

extern bool g_videoThreadEnabled;

enum class PlayerState {
    UNINITIALIZED,
    OPENED,
    PLAYING,
    PAUSED,
    ENDED,
    ERROR_STATE
};

class PlayerController {
private:
    std::string m_filename;
    // Atomic: written by the UI thread, read by the video decode thread
    std::atomic<PlayerState> m_state;
    
    // Packet queues
    ThreadSafeQueue<AVPacket*> m_videoQueue;
    ThreadSafeQueue<AVPacket*> m_audioQueue;
    ThreadSafeQueue<AVPacket*> m_subtitleQueue;
    
    // Sub-modules
    std::unique_ptr<Demuxer> m_demuxer;
    std::unique_ptr<AudioDecoder> m_audioDecoder;
    std::unique_ptr<VideoDecoder> m_videoDecoder;
    std::unique_ptr<naikav::subtitle::SubtitleDecoder> m_subtitleDecoder;

    // Subtitle state
    std::atomic<int> m_selectedSubtitleTrack{-1}; // -1 = Off, >= 0 = embedded stream index, -2 = external
    std::atomic<double> m_subtitleDelay{0.0};      // Subtitle timing offset in seconds
    mutable std::mutex m_subtitleMutex;
    std::vector<naikav::subtitle::SubtitleTrackInfo> m_cachedSubtitleTracks;
    naikav::subtitle::SubtitleTrackInfo m_externalSubtitleTrack;
    bool m_hasExternalSubtitle = false;

    // Audio track state
    std::atomic<int> m_selectedAudioTrack{-1}; // -1 = Disabled/Off, >= 0 = embedded stream index, -2 = external
    mutable std::mutex m_audioTrackMutex;
    mutable std::mutex m_audioDecoderMutex;
    std::vector<naikav::audio::AudioTrackInfo> m_cachedAudioTracks;
    naikav::audio::AudioTrackInfo m_externalAudioTrack;
    bool m_hasExternalAudio = false;
    std::unique_ptr<Demuxer> m_externalAudioDemuxer;
    ThreadSafeQueue<AVPacket*> m_dummyVideoQueue{1};
    
    bool m_hasAudio;
    bool m_hasVideo;


    // Fallback clock for video-only files
    // Atomic: also written by the video thread when a seek catch-up finishes
    std::atomic<double> m_videoClock; // in seconds
    std::atomic<double> m_lastSystemTime;
    
    float m_volume;
    std::atomic<float> m_playbackSpeed{1.0f};

    // UI-thread-only copy, mirroring m_volume's pattern: written/read only
    // from the UI thread (single-writer/single-reader), applied to the
    // audio callback thread's actual filter state via the thread-safe
    // AudioDecoder::applyDspSettings() -- see setAudioDspSettings() below.
    naikav::dsp::AudioDspSettings m_audioDspSettings;

    // Applied to AudioDecoder before init() in openFile() (see
    // AudioChannelOption). UI-thread-only, same single-writer/single-reader
    // rationale as m_volume/m_audioDspSettings above.
    AudioChannelOption m_channelOption = AudioChannelOption::AUTO;

    // Same "applied to AudioDecoder before init()" pattern as
    // m_channelOption above, for the other output-routing/quality settings.
    AudioOutputBitDepth m_outputBitDepth = AudioOutputBitDepth::BIT_16;
    std::string m_outputDeviceName;
    ResamplerQuality m_resamplerQuality = ResamplerQuality::MEDIUM;

    bool m_loopEnabled;

    // Background video decoding thread
    std::thread m_videoThread;
    std::atomic<bool> m_videoThreadRunning;
    std::atomic<bool> m_videoThreadEnabled;
    // mutable: also taken by const getters (getVideoWidth/Height,
    // isVideoHardware) that read m_videoDecoder's codec context, which the
    // video thread can free and reopen with a fresh session when recovering
    // from a hardware decode failure.
    mutable std::mutex m_videoDecoderMutex;
    std::atomic<bool> m_seeking;
    std::atomic<bool> m_seeked;
    ThreadSafeQueue<DecodedFrame> m_decodedFrameQueue;

    // Seek catch-up. Every seek LANDs: the player repositions immediately
    // and decodes from the preceding keyframe up to the target frame without
    // displaying anything in between, so the jump appears instantaneous in
    // both directions. Audio stays muted during the catch-up and resumes in
    // sync on the target.
    std::atomic<SeekCatchupMode> m_catchupMode;
    std::atomic<double> m_catchupTarget; // final seek position (seconds)
    std::atomic<double> m_catchupPos;    // position reported while catching up
    std::atomic<bool> m_resumeAfterCatchup;
    // Bumped as the last step of every seek. A frame whose decode began under
    // an older epoch belongs to the pre-seek stream and must be discarded,
    // never displayed or matched against the catch-up target.
    std::atomic<uint64_t> m_catchupEpoch;
    std::mutex m_catchupMutex;   // serializes catch-up begin/retarget/finish
    std::unique_ptr<PipelineMetrics> m_metrics;
    std::chrono::steady_clock::time_point m_seekStartTime;
    uint64_t m_seekStartEpoch = 0;

    std::atomic<ResolutionOption> m_resolutionOption;

    naikav::playlist::Playlist m_playlist;

    void loadSettings();
    void saveSettings();

    // Playlist persistence: contents saved as a sibling "playlist.m3u8"
    // file (same flat-file-in-cwd convention as player_settings.txt --
    // this codebase has no config-dir helper to hook into instead), plus
    // repeat/shuffle/current-index as three extra keys appended to
    // player_settings.txt via the existing key=value format. Called from
    // every playlist-mutating wrapper below (add/remove/move/clear/repeat/
    // shuffle/navigate), matching the "persist immediately" convention
    // already used by setAudioChannelOption() etc.
    void loadPlaylistState();
    void savePlaylistState();

    // Staging area for the three playlist_* keys loadSettings() reads out of
    // player_settings.txt: at constructor time (loadSettings() runs first),
    // m_playlist is still empty -- these are applied to m_playlist by
    // loadPlaylistState() only after it has loaded playlist.m3u8's contents.
    int m_pendingPlaylistCurrentIndex = -1;
    int m_pendingPlaylistRepeatMode = 0;
    bool m_pendingPlaylistShuffle = false;

    // Primes m_audioDecoder's loudness normalizer for the current file --
    // see LoudnessNormalizer's two-pass mode. Checks a ReplayGain/R128
    // container tag first (cheap, a couple of dictionary lookups, so
    // handled synchronously here); failing that, kicks off
    // naikav::dsp::prescanIntegratedLufs() -- a decode-only pass over the
    // *entire* audio stream -- on a background thread (see
    // m_loudnessPrescanThread below), since blocking on that would
    // otherwise freeze the whole UI thread for however long the file
    // takes to scan (openFile() is called directly from main.cpp's
    // SDL_EVENT_DROP_FILE handler on the render/event thread). No-op if
    // there's no audio decoder or no file open. Called from openFile()
    // and from setAudioDspSettings() when loudness normalization is newly
    // toggled on mid-playback.
    void prescanLoudnessForCurrentFile();

    // Background-thread state for the decode-based prescan path above.
    // Only ever touches m_pendingPrescanResult (mutex-guarded) from the
    // background thread -- never m_audioDecoder or any other
    // PlayerController state directly, so there's no race with
    // openFile()/stop() replacing m_audioDecoder mid-scan. The result is
    // only ever applied from the main thread, via
    // pollPendingLoudnessPrescan() below, and only if its generation tag
    // still matches the current file.
    std::thread m_loudnessPrescanThread;
    std::atomic<uint64_t> m_loudnessPrescanGeneration{0};
    struct PendingLoudnessPrescanResult {
        uint64_t generation = 0;
        double lufs = -120.0;
        bool valid = false;
    };
    std::mutex m_pendingPrescanMutex;
    PendingLoudnessPrescanResult m_pendingPrescanResult;

    // Joins m_loudnessPrescanThread if it's still running. Called from
    // stop() (and therefore from both openFile() and the destructor) so a
    // background scan never outlives the PlayerController instance whose
    // `this` its lambda captured.
    void joinLoudnessPrescanThread();

    // If m_audioDspSettings.autoGenrePresetEnabled is set, reads the
    // current file's genre tag (via m_demuxer) and, if it maps to a known
    // preset (see naikav::dsp::presetForGenreTag()), applies it -- called
    // from openFile() after the initial applyDspSettings() so genre-based
    // preset selection happens automatically per file, without disturbing
    // callers who never opt in.
    void applyGenrePresetIfEnabled();
    void videoThreadLoop();
    void instantSeek(double seconds);
    void finishCatchup(double resumePts);

    double getSystemTimeInSeconds() const;
    
    // Timing instrumentation (in microseconds)
    std::atomic<uint64_t> m_videoDecodeTimeUs{0};
    std::atomic<uint64_t> m_audioDecodeTimeUs{0};
    std::atomic<uint64_t> m_videoRenderTimeUs{0};
    std::atomic<uint64_t> m_presentTimeUs{0};
    std::atomic<uint64_t> m_framePacingUs{0};

public:
    PlayerController();
    ~PlayerController();

    // resetPlaylist=true (the default, used by the CLI arg / "Open File"
    // dialog / single drag-drop) collapses the playlist to just this one
    // file, matching how opening a file normally behaves outside of a
    // playlist context. Playlist-driven navigation (playlistNext() /
    // playlistPrevious() / playlistPlayIndex()) calls this with false so it
    // doesn't clobber the list it's iterating.
    bool openFile(const std::string& filename, bool resetPlaylist = true);
    void play();
    void pause();
    void seek(double seconds);
    void stop();

    // Applies a completed background loudness prescan (see
    // prescanLoudnessForCurrentFile()) if one is ready, from whichever
    // thread calls this -- intended to be called once per frame from the
    // main/render thread's event loop (main.cpp), since openFile() is
    // called directly from there and the prescan result must be applied
    // via AudioDecoder::primeLoudnessPrescan() from a thread that isn't
    // racing openFile()/stop() replacing m_audioDecoder. Cheap when
    // nothing is pending (one mutex-guarded flag check).
    void pollPendingLoudnessPrescan();

    void updateClockForVideoOnly();

    // Getters
    PlayerState getState() const { return m_state; }
    const std::string& getFilename() const { return m_filename; }
    double getCurrentTime();
    double getDuration() const;
    int getVideoWidth() const;
    int getVideoHeight() const;
    std::string getVideoCodecName() const;
    std::string getVideoPixelFormat() const;
    ColorPipelineInfo getColorInfo() const;
    bool isVideoHardware() const;
    bool isSeeking() const;
    bool isCatchingUp() const { return m_catchupMode.load() != SeekCatchupMode::NONE; }
    // Base position for relative seeks: the pending catch-up target if one is
    // active (so repeated +10s presses stack), otherwise the current time.
    double getSeekReferenceTime();
    std::string getAudioCodecName() const;
    // Resolved output channel layout name (e.g. "stereo", "5.1", "7.1"); see
    // AudioDecoder::getOutputChannelLayoutName().
    std::string getAudioChannelLayoutName() const;
    double getAudioClock();
    double getVideoClock() const;
    bool hasAudio() const { return m_hasAudio; }
    bool hasVideo() const { return m_hasVideo; }
    bool isEOF() const;
    
    VideoDecoder* getVideoDecoder() const { return m_videoDecoder.get(); }
    ThreadSafeQueue<DecodedFrame>& getDecodedFrameQueue() { return m_decodedFrameQueue; }
    
    static constexpr float kMinPlaybackSpeed = 0.25f;
    static constexpr float kMaxPlaybackSpeed = 2.0f;

    // Setters
    void setVolume(float volume);
    void setPlaybackSpeed(float speed);
    float getPlaybackSpeed() const { return m_playbackSpeed.load(); }
    void setLoopEnabled(bool enabled) { m_loopEnabled = enabled; }
    bool isLoopEnabled() const { return m_loopEnabled; }
    bool hasSeeked() const { return m_seeked.load(); }
    void clearSeeked() { m_seeked.store(false); }

    ResolutionOption getResolutionOption() const { return m_resolutionOption.load(); }
    void setResolutionOption(ResolutionOption option);
    int getPlaybackWidth() const;
    int getPlaybackHeight() const;

    // Audio DSP/loudness settings (EQ, compressor, limiter, crossover,
    // loudness target). Safe to call during playback -- see
    // AudioDecoder::applyDspSettings() for the thread-safety story. Applies
    // immediately but does NOT write to disk (see persistAudioDspSettings())
    // -- callers driving this from a continuously-changing UI control (e.g.
    // a slider being dragged) should apply on every change for live audio
    // feedback, but only persist once the interaction settles.
    void setAudioDspSettings(const naikav::dsp::AudioDspSettings& settings);
    const naikav::dsp::AudioDspSettings& getAudioDspSettings() const { return m_audioDspSettings; }
    // Writes the current settings (resolution + audio DSP) to disk. Call
    // this once an interaction is done (e.g. ImGui::IsItemDeactivatedAfterEdit()),
    // not on every intermediate value change.
    void persistAudioDspSettings() { saveSettings(); }
    double getMeasuredIntegratedLufs() const {
        std::lock_guard<std::mutex> lock(m_audioDecoderMutex);
        return (m_hasAudio && m_audioDecoder) ? m_audioDecoder->getMeasuredIntegratedLufs() : -120.0;
    }
    float getCurrentLoudnessGainDb() const {
        std::lock_guard<std::mutex> lock(m_audioDecoderMutex);
        return (m_hasAudio && m_audioDecoder) ? m_audioDecoder->getCurrentLoudnessGainDb() : 0.0f;
    }
    // Live magnitude-spectrum snapshot for the Audio Processing panel's
    // visualizer (dB per bin, empty if there's no audio decoder yet).
    // See AudioDecoder::getSpectrumMagnitudesDb().
    std::vector<float> getSpectrumMagnitudesDb() const {
        std::lock_guard<std::mutex> lock(m_audioDecoderMutex);
        return (m_hasAudio && m_audioDecoder) ? m_audioDecoder->getSpectrumMagnitudesDb() : std::vector<float>{};
    }
    std::vector<float> getWaveformSamples() const {
        std::lock_guard<std::mutex> lock(m_audioDecoderMutex);
        return (m_hasAudio && m_audioDecoder) ? m_audioDecoder->getWaveformSamples() : std::vector<float>{};
    }
    static int getSpectrumNumBins() { return AudioDecoder::getSpectrumNumBins(); }
    double getSpectrumBinFrequencyHz(int bin) const {
        std::lock_guard<std::mutex> lock(m_audioDecoderMutex);
        return (m_hasAudio && m_audioDecoder) ? m_audioDecoder->getSpectrumBinFrequencyHz(bin) : 0.0;
    }
    // Resolved output channel count (e.g. 2 for stereo, 6 for 5.1). 0 if
    // there's no audio stream. See getAudioChannelLayoutName() for the
    // human-readable name (e.g. "5.1(side)").
    int getAudioChannelCount() const {
        std::lock_guard<std::mutex> lock(m_audioDecoderMutex);
        return (m_hasAudio && m_audioDecoder) ? m_audioDecoder->getOutputChannelCount() : 0;
    }
    // The real default playback device's native channel count as reported
    // by the OS (0 if unknown). If this is lower than getAudioChannelCount(),
    // the OS's own audio mixer is silently downmixing what NaikAVPlayer
    // sends -- the resolved output layout does not guarantee that many
    // physical speakers are actually reproducing discrete channels.
    int getAudioDeviceNativeChannels() const {
        std::lock_guard<std::mutex> lock(m_audioDecoderMutex);
        return (m_hasAudio && m_audioDecoder) ? m_audioDecoder->getDeviceNativeChannels() : 0;
    }

    // True when AudioChannelOption::VIRTUAL_SURROUND is folding a discrete
    // surround source down to stereo with positional cues (see
    // AudioDecoder::isVirtualSurroundActive()), rather than either
    // preserving it untouched or downmixing it flat.
    bool isAudioVirtualSurroundActive() const {
        std::lock_guard<std::mutex> lock(m_audioDecoderMutex);
        return (m_hasAudio && m_audioDecoder) ? m_audioDecoder->isVirtualSurroundActive() : false;
    }

    // User override for output channel resolution (AUTO preserves source
    // surround layout when supported; FORCE_STEREO always downmixes;
    // VIRTUAL_SURROUND preserves it internally but folds it down to a
    // positionally-cued stereo device stream -- see AudioChannelOption).
    // Takes effect on the next openFile() call, not live during current
    // playback (changing channel count requires reopening the audio
    // device). Persisted to disk like resolution/DSP settings.
    AudioChannelOption getAudioChannelOption() const { return m_channelOption; }
    void setAudioChannelOption(AudioChannelOption option) {
        m_channelOption = option;
        saveSettings();
    }

    // Output device PCM bit depth (S16/S32/F32 -- see AudioOutputBitDepth).
    // Same "takes effect on next openFile()" rule as channel option above.
    AudioOutputBitDepth getOutputBitDepth() const { return m_outputBitDepth; }
    void setOutputBitDepth(AudioOutputBitDepth depth) {
        m_outputBitDepth = depth;
        saveSettings();
    }

    // Preferred playback device, by name (empty = OS default) -- see
    // AudioDecoder::enumeratePlaybackDeviceNames() for the list to
    // present in a UI dropdown. Same "takes effect on next openFile()"
    // rule as the other output-routing settings above.
    const std::string& getOutputDeviceName() const { return m_outputDeviceName; }
    void setOutputDeviceName(const std::string& name) {
        m_outputDeviceName = name;
        saveSettings();
    }

    // libsoxr resampling quality tier -- see ResamplerQuality. Same
    // "takes effect on next openFile()" rule as the other output/routing
    // settings above (resampler is configured once at init() time).
    ResamplerQuality getResamplerQuality() const { return m_resamplerQuality; }
    void setResamplerQuality(ResamplerQuality quality) {
        m_resamplerQuality = quality;
        saveSettings();
    }

    // Subtitle management APIs
    std::vector<naikav::subtitle::SubtitleTrackInfo> getSubtitleTracks() const;
    int getSelectedSubtitleTrack() const { return m_selectedSubtitleTrack.load(); }
    void selectSubtitleTrack(int trackId);
    bool loadExternalSubtitle(const std::string& filepath);
    std::string getCurrentSubtitleText();
    void setSubtitleDelay(double delaySeconds) { m_subtitleDelay.store(delaySeconds); }
    double getSubtitleDelay() const { return m_subtitleDelay.load(); }
    void pollSubtitlePackets();
    void autoProbeExternalSubtitles(const std::string& mediaFilename);
    bool hasExternalSubtitle() const {
        std::lock_guard<std::mutex> lock(m_subtitleMutex);
        return m_hasExternalSubtitle;
    }
    std::string getActiveSubtitleTrackName() const;

    // Audio track management APIs
    std::vector<naikav::audio::AudioTrackInfo> getAudioTracks() const;
    int getSelectedAudioTrack() const { return m_selectedAudioTrack.load(); }
    bool selectAudioTrack(int trackId);
    bool loadExternalAudio(const std::string& filepath);
    void removeExternalAudio();
    bool hasExternalAudio() const {
        std::lock_guard<std::mutex> lock(m_audioTrackMutex);
        return m_hasExternalAudio;
    }
    std::string getActiveAudioTrackName() const;

    // Playlist management APIs. UI-thread-only, single-writer/single-reader
    // like the settings members above -- PlayerUI mutates the list directly
    // through getPlaylist() (add/remove/move/repeat/shuffle are pure data
    // operations with no playback side effect), then calls one of the
    // playlistX() wrappers below when the mutation should also change what's
    // playing. Every wrapper persists via savePlaylistState().
    naikav::playlist::Playlist& getPlaylist() { return m_playlist; }
    const naikav::playlist::Playlist& getPlaylist() const { return m_playlist; }

    // Call after mutating getPlaylist() directly (add/remove/move/clear/
    // setRepeatMode/setShuffle) so the change is persisted immediately,
    // matching the rest of this class's "persist on every change" convention.
    void persistPlaylistState() { savePlaylistState(); }

    // Plays the item at the given display-order index: stop() + openFile(path,
    // false) + play(), same sequencing as opening any other file -- never
    // touches the packet queues directly.
    bool playlistPlayIndex(int index);
    bool playlistNext();
    bool playlistPrevious();

    // Called once per frame from the main loop. If playback has reached
    // PlayerState::ENDED and the playlist has a next item (per its
    // RepeatMode/shuffle state), advances to it automatically. No-op
    // otherwise -- in particular, a no-op whenever the existing per-file
    // Loop toggle (isLoopEnabled()) is on, since playback then never reaches
    // ENDED in the first place (see getCurrentTime()).
    void pollPlaylistAutoAdvance();


    // Queue depths
    size_t getVideoPacketQueueSize() const { return m_videoQueue.size(); }
    size_t getVideoPacketQueueCapacity() const { return m_videoQueue.capacity(); }
    size_t getAudioPacketQueueSize() const { return m_audioQueue.size(); }
    size_t getAudioPacketQueueCapacity() const { return m_audioQueue.capacity(); }
    size_t getSubtitlePacketQueueSize() const { return m_subtitleQueue.size(); }
    size_t getSubtitlePacketQueueCapacity() const { return m_subtitleQueue.capacity(); }
    size_t getVideoFrameQueueSize() const { return m_decodedFrameQueue.size(); }
    size_t getVideoFrameQueueCapacity() const { return m_decodedFrameQueue.capacity(); }
    size_t getAudioFrameQueueSize() const;

    // Audio underrun diagnostics -- see
    // AudioDecoder::getSilenceInjectionCount(). Zero when there's no audio.
    uint64_t getAudioCallbackCount() const {
        std::lock_guard<std::mutex> lock(m_audioDecoderMutex);
        return (m_hasAudio && m_audioDecoder) ? m_audioDecoder->getCallbackCount() : 0;
    }
    uint64_t getAudioSilenceInjectionCount() const {
        std::lock_guard<std::mutex> lock(m_audioDecoderMutex);
        return (m_hasAudio && m_audioDecoder) ? m_audioDecoder->getSilenceInjectionCount() : 0;
    }
    uint64_t getAudioSilenceBytes() const {
        std::lock_guard<std::mutex> lock(m_audioDecoderMutex);
        return (m_hasAudio && m_audioDecoder) ? m_audioDecoder->getSilenceBytes() : 0;
    }
    // Diagnostics only -- lets a smoke test attribute underruns to a
    // specific decodeAndResample() exit path. Not for playback control.
    AudioDecoder* audioDecoderForDiagnostics() const {
        std::lock_guard<std::mutex> lock(m_audioDecoderMutex);
        return m_audioDecoder.get();
    }
    static size_t getAudioFrameQueueCapacity() { return 48000; } // target scale for visual (48k samples ~ 1 sec)

    // Timing setters/getters
    void setVideoRenderTimeUs(uint64_t us) { m_videoRenderTimeUs.store(us); }
    void setPresentTimeUs(uint64_t us) { m_presentTimeUs.store(us); }
    void setFramePacingUs(uint64_t us) { m_framePacingUs.store(us); }

    double getVideoDecodeTimeMs() const {
        float latestUs = 0.0f;
        if (m_metrics && m_metrics->m_decodeTimePerFrameUs.snapshot(&latestUs, 1) > 0) {
            return latestUs / 1000.0;
        }
        return 0.0;
    }
    double getAudioDecodeTimeMs() const { return m_audioDecodeTimeUs.load() / 1000.0; }
    double getVideoRenderTimeMs() const { return m_videoRenderTimeUs.load() / 1000.0; }
    double getPresentTimeMs() const { return m_presentTimeUs.load() / 1000.0; }
    double getFramePacingMs() const { return m_framePacingUs.load() / 1000.0; }

    PipelineMetrics& getPipelineMetrics() { return *m_metrics; }
    const PipelineMetrics& getPipelineMetrics() const { return *m_metrics; }
};
