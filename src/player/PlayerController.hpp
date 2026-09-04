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
    // How much audio to buffer, chosen per file in openFile().
    //
    // A file with video needs enough to cover prerollVideo() without the
    // demuxer blocking on this queue and starving video of the packets the
    // preroll is waiting for -- and "enough" is a matter of time, not
    // packets: an AAC packet is ~21 ms while a TrueHD access unit is under
    // 1 ms, so 150 packets is three seconds of one and ~125 ms of the
    // other.
    //
    // An audio-only file never prerolls, and buffering that far ahead there
    // is actively wrong: the demuxer would swallow a short file whole and
    // report EOF while playback is still paused at the start.
    static constexpr size_t kAudioQueuePacketsWithVideo = 2400;
    static constexpr size_t kAudioQueuePacketsAudioOnly = 150;
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

    // Last known values of the read-only video properties the diagnostics
    // HUD shows, so those accessors never have to *wait* on the decoder.
    //
    // The video thread holds m_videoDecoderMutex across decodeNextFrame()
    // AND convertFrame(), which on 4K HDR content is tens of milliseconds.
    // A HUD that blocks on that same mutex several times per rendered frame
    // therefore stalls the render thread for far longer than a frame is
    // worth: modelled at a 27 ms conversion with five calls per frame, the
    // UI waited a median of 110 ms per frame against a 16.7 ms budget,
    // which is what made turning the HUD on visibly drop the frame rate.
    //
    // These are display-only figures that change rarely (or never) during
    // playback, so showing the previous value for one frame while the
    // decoder is busy costs nothing and cannot be seen.
    // A cached entry may only be *returned* once it holds a real reading.
    // Falling back to an empty cache would report a 0x0 video or an
    // "unknown" pixel format to a caller that simply happened to ask while
    // the decode thread held the mutex -- which is wrong rather than
    // merely stale, and is exactly what broke "Video width is populated
    // correctly". So the first read of each property waits for the real
    // value; every read after that can fall back.
    mutable std::mutex m_videoInfoCacheMutex;
    mutable ColorPipelineInfo m_cachedColorInfo;
    mutable std::string m_cachedPixelFormat{"unknown"};
    mutable bool m_cachedIsHardware = false;
    mutable int m_cachedVideoWidth = 0;
    mutable int m_cachedVideoHeight = 0;
    mutable bool m_colorInfoCached = false;
    mutable bool m_pixelFormatCached = false;
    mutable bool m_isHardwareCached = false;
    mutable bool m_videoWidthCached = false;
    mutable bool m_videoHeightCached = false;
    // Cleared whenever the decoder is replaced, so one file never reports
    // the previous file's dimensions.
    void invalidateVideoInfoCache() {
        std::lock_guard<std::mutex> lock(m_videoInfoCacheMutex);
        m_colorInfoCached = false;
        m_pixelFormatCached = false;
        m_isHardwareCached = false;
        m_videoWidthCached = false;
        m_videoHeightCached = false;
    }
    // openFile() runs on the UI thread but playlist advance calls it from
    // the playback path, so the error it leaves behind is guarded.
    mutable std::mutex m_lastOpenErrorMutex;
    std::string m_lastOpenError;
    void setLastOpenError(const std::string& reason) {
        std::lock_guard<std::mutex> lock(m_lastOpenErrorMutex);
        m_lastOpenError = reason;
    }
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

    // HDR -> SDR tone mapping. Read on the video thread once per frame and
    // written from the UI thread, hence atomic rather than mutex-guarded:
    // a toggle that lands mid-frame simply takes effect on the next one.
    std::atomic<bool> m_hdrToneMapEnabled{true};
    std::atomic<float> m_hdrTargetPeakNits{100.0f};
    // 0 = derive the source peak from the frame's own metadata.
    std::atomic<float> m_hdrSourcePeakNits{0.0f};
    std::atomic<bool> m_hdrDynamicMetadata{true};
    std::atomic<bool> m_hdrAdaptiveResolution{true};

    // Size of the area video is drawn into, in pixels. Written by the
    // render thread once per presented frame and read by the video thread
    // once per decoded frame, so atomics rather than a mutex: the render
    // thread must never block behind a conversion. 0 means the renderer
    // has not reported a size yet, which leaves the cap disabled.
    std::atomic<int> m_displayWidth{0};
    std::atomic<int> m_displayHeight{0};

    // Dimensions of the most recently converted frame. Cached here rather
    // than recomputed from the resolution selector because the two can
    // legitimately disagree -- the HDR path caps to the display size, and
    // some hardware decoders emit sizes the codec context never mentions.
    std::atomic<int> m_lastOutputWidth{0};
    std::atomic<int> m_lastOutputHeight{0};

    // Late-frame dropping. Touched only by the video thread, so plain
    // ints. See shouldDropLateFrame() for what these mean.
    int m_consecutiveLateDrops = 0;
    

    // How far behind the master clock a freshly decoded frame has to be
    // before it is dropped instead of converted. Wide enough that ordinary
    // scheduling jitter never trips it -- roughly six frames at 60 fps,
    // three at 24 -- since a frame that is merely a little late is still
    // worth showing.
    static constexpr double kLateFrameDropSeconds = 0.10;

    // Ceiling on how many frames in a row may be dropped this way. Under
    // sustained overload the drop test would otherwise keep winning and
    // the picture would freeze while the audio ran on; forcing one frame
    // through every so often keeps the video moving, just at a lower rate.
    static constexpr int kMaxConsecutiveLateDrops = 8;
    // Holds the audio clock at the starting line until the video pipeline
    // has a frame to show. See prerollVideo() for why.
    void prerollVideo();
    double m_lastPrerollSeconds = 0.0;
    // Ceiling on that wait. A file whose first frame never arrives (a
    // video stream the decoder cannot open, a corrupt leading GOP) must
    // still play its audio rather than hanging on the play button.
    static constexpr std::chrono::milliseconds kPrerollTimeout{2000};

    // Minimum number of queued video packets before a late frame may be
    // dropped -- below this the decoder is starved rather than congested,
    // and dropping discards a frame with nothing to put in its place.
    // See shouldDropLateFrame() for the measurements behind this.
    static constexpr size_t kLateDropMinQueuedPackets = 16;

    // True when the frame just decoded is so far behind the clock that
    // converting it is wasted work: the renderer would drop it on arrival
    // anyway (see the pop loop in main.cpp), and the time spent tone
    // mapping it is time the decoder is not spending catching up.
    bool shouldDropLateFrame(double framePts);

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

    // Whether the last play() had to wait for the video pipeline, and for
    // how long. Diagnostics only -- see prerollVideo().
    double getLastPrerollSeconds() const { return m_lastPrerollSeconds; }

    // Why the last openFile() returned false, ready to show a user (e.g.
    // "moov atom not found"). Empty after a successful open. Every caller
    // that can fail should surface this -- an open failure that only
    // reaches stderr is invisible in the MSVC build, which has no console.
    std::string getLastOpenError() const {
        std::lock_guard<std::mutex> lock(m_lastOpenErrorMutex);
        return m_lastOpenError;
    }
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

    // HDR -> SDR tone mapping. Takes effect on the next decoded frame, so
    // unlike setResolutionOption() these need no seek to become visible.
    bool isHdrToneMapEnabled() const { return m_hdrToneMapEnabled.load(); }
    void setHdrToneMapEnabled(bool enabled);

    // The two peak-luminance settings below apply live and do NOT write to
    // disk or disturb playback -- the decoder re-reads them for every frame
    // it converts, so a drag is visible immediately while playing at no
    // cost beyond the store. Same rule as setAudioDspSettings(): call these
    // on every change, then persistHdrSettings() once the interaction
    // settles. Persisting per drag frame is not the harmless UI-thread
    // write it is for the DSP panel -- making a change visible while
    // *paused* needs a re-decode, and one seek per drag frame tears the
    // pipeline down and refills it dozens of times a second.
    float getHdrTargetPeakNits() const { return m_hdrTargetPeakNits.load(); }
    void setHdrTargetPeakNits(float nits);

    // Peak luminance to tone map *from*. 0 means "read it from the file"
    // (mastering-display metadata reconciled with MaxCLL -- see
    // selectSourcePeakNits()), which is right for any file whose metadata
    // is honest; the override exists for the ones where it is not.
    float getHdrSourcePeakNits() const { return m_hdrSourcePeakNits.load(); }
    void setHdrSourcePeakNits(float nits);

    // Writes the HDR settings to disk and, if playback is sitting on a
    // frame, re-decodes it so the change becomes visible. Call once an
    // interaction is done (ImGui::IsItemDeactivatedAfterEdit()), not on
    // every intermediate value.
    void persistHdrSettings();

    // Follow HDR10+ / Dolby Vision per-frame metadata when the file has
    // it. Applies live, like the peak settings.
    bool isHdrDynamicMetadataEnabled() const { return m_hdrDynamicMetadata.load(); }
    void setHdrDynamicMetadataEnabled(bool enabled) { m_hdrDynamicMetadata.store(enabled); }

    // Shrink the tone-mapping target when the machine cannot hold the
    // source's frame rate at full size. See AdaptiveToneMapScale.
    bool isHdrAdaptiveResolutionEnabled() const { return m_hdrAdaptiveResolution.load(); }
    void setHdrAdaptiveResolutionEnabled(bool enabled) { m_hdrAdaptiveResolution.store(enabled); }

    // Tells the decoder how large the video is actually being drawn, so
    // HDR frames are not tone mapped at a resolution the window cannot
    // show (see capToDisplaySize). Lock-free and cheap enough to call
    // every rendered frame; pass the renderer's output size in pixels,
    // not the window size in points, or a HiDPI display gets a softer
    // picture than it asked for.
    void setDisplaySize(int width, int height) {
        m_displayWidth.store(width > 0 ? width : 0, std::memory_order_relaxed);
        m_displayHeight.store(height > 0 ? height : 0, std::memory_order_relaxed);
    }
    naikav::video::HdrToneMapSettings getHdrToneMapSettings() const {
        naikav::video::HdrToneMapSettings s;
        s.enabled = m_hdrToneMapEnabled.load();
        s.targetPeakNits = m_hdrTargetPeakNits.load();
        s.sourcePeakNits = m_hdrSourcePeakNits.load();
        s.useDynamicMetadata = m_hdrDynamicMetadata.load();
        s.adaptiveResolution = m_hdrAdaptiveResolution.load();
        return s;
    }

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
