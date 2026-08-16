#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include "core/ThreadSafeQueue.hpp"
#include "core/MetricRing.hpp"
#include "subtitle/SubtitleTrack.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

// Coordinates the seek catch-up phase between PlayerController and the
// Demuxer. LANDING: the player repositions immediately and decodes up to the
// target silently - only the target frame is shown. While it is active the
// demuxer drops pre-target audio.
enum class SeekCatchupMode { NONE = 0, LANDING };

class Demuxer {
private:
    std::string m_filename;
    AVFormatContext* m_formatCtx;
    
    std::atomic<int> m_videoStreamIdx;
    std::atomic<int> m_audioStreamIdx;
    std::atomic<int> m_subtitleStreamIdx{-1};
    std::vector<naikav::subtitle::SubtitleTrackInfo> m_subtitleTracks;
    
    AVCodecParameters* m_videoCodecParams;
    AVCodecParameters* m_audioCodecParams;
    
    AVRational m_videoTimeBase;
    AVRational m_audioTimeBase;
    int64_t m_videoStartTime;
    int64_t m_audioStartTime;
    
    double m_duration; // in seconds
    
    ThreadSafeQueue<AVPacket*>& m_videoQueue;
    ThreadSafeQueue<AVPacket*>& m_audioQueue;
    ThreadSafeQueue<AVPacket*>* m_subtitleQueue = nullptr;
    
    std::thread m_thread;
    std::atomic<bool> m_running;
    std::atomic<bool> m_seekRequested;
    std::atomic<double> m_seekTargetTime;
    std::mutex m_seekMutex;
    std::atomic<bool> m_eof;

    // Seek catch-up coordination: while active, audio packets from before
    // the target are dropped.
    std::atomic<SeekCatchupMode> m_catchupMode;
    std::atomic<double> m_catchupTarget;
    MetricRing<256>& m_demuxTimeRing;
    std::atomic<bool>& m_profilingEnabled;

    // Bumped once per completed seek (in performSeek(), after repositioning
    // the format context but before any further packets are read). Every
    // packet pushed afterward is tagged with this generation via its
    // opaque field (see threadLoop()). Consumers (VideoDecoder/AudioDecoder,
    // via attachSeekGeneration()) compare a packet's tag against this
    // counter's *current* live value at the moment they pop it, and drop
    // the packet if it doesn't match -- catching staleness by where the
    // packet's data actually came from, not by when the consumer thread's
    // loop iteration happened to start. That distinction matters: a packet
    // can be read from the pre-seek position and pushed to the queue in the
    // brief window between a new seek being requested and this thread
    // noticing it, surviving the caller's one-shot queue clear; a purely
    // consumer-side "epoch snapshot before decoding" check (as used
    // elsewhere for catch-up landing) can't see that, since by the time the
    // consumer picks the packet up its own epoch may already have advanced.
    std::atomic<uint64_t> m_seekGeneration{0};

    // Set once the AudioDecoder exists (see attachAudioPausedFlag). Lets the
    // read loop tell whether the audio packet queue currently has a consumer
    // draining it, so it never blocks pushing to a queue nothing is popping.
    std::atomic<bool>* m_audioPausedFlag = nullptr;

    void threadLoop();
    void performSeek();
    double packetTimeSeconds(const AVPacket* pkt, int streamIdx) const;

public:
    Demuxer(const std::string& filename, 
            ThreadSafeQueue<AVPacket*>& videoQueue, 
            ThreadSafeQueue<AVPacket*>& audioQueue,
            MetricRing<256>& demuxTimeRing,
            std::atomic<bool>& profilingEnabled);
    Demuxer(const std::string& filename, 
            ThreadSafeQueue<AVPacket*>& videoQueue, 
            ThreadSafeQueue<AVPacket*>& audioQueue);
    ~Demuxer();

    bool open();
    void start();
    void stop();
    
    // Seek to a timestamp in seconds
    void seek(double timeInSeconds);

    // Enter/leave seek catch-up mode (see SeekCatchupMode)
    void setCatchup(SeekCatchupMode mode, double targetSeconds);

    // Give the demuxer a live view of AudioDecoder's paused flag. Must be
    // called before start(), since the read loop is not synchronized with
    // this pointer assignment.
    void attachAudioPausedFlag(std::atomic<bool>* flag) { m_audioPausedFlag = flag; }

    // Attach subtitle packet queue for embedded subtitle demuxing
    void attachSubtitleQueue(ThreadSafeQueue<AVPacket*>* queue) { m_subtitleQueue = queue; }

    // Live pointer to the seek-generation counter, for VideoDecoder/
    // AudioDecoder to compare a popped packet's tag against (see
    // attachSeekGeneration() on those classes).
    std::atomic<uint64_t>* seekGenerationPtr() { return &m_seekGeneration; }

    // Getters
    int getVideoStreamIndex() const { return m_videoStreamIdx; }
    int getAudioStreamIndex() const { return m_audioStreamIdx; }
    int getSubtitleStreamIndex() const { return m_subtitleStreamIdx.load(); }
    void setSubtitleStreamIndex(int idx) { m_subtitleStreamIdx.store(idx); }
    const std::vector<naikav::subtitle::SubtitleTrackInfo>& getSubtitleTracks() const { return m_subtitleTracks; }

    AVCodecParameters* getVideoCodecParams() const { return m_videoCodecParams; }
    AVCodecParameters* getAudioCodecParams() const { return m_audioCodecParams; }
    AVCodecParameters* getSubtitleCodecParams(int streamIdx) const;
    AVRational getSubtitleTimeBase(int streamIdx) const;
    int64_t getSubtitleStartTime(int streamIdx) const;

    AVRational getVideoTimeBase() const { return m_videoTimeBase; }
    AVRational getAudioTimeBase() const { return m_audioTimeBase; }
    int64_t getVideoStartTime() const { return m_videoStartTime; }
    int64_t getAudioStartTime() const { return m_audioStartTime; }
    double getDuration() const { return m_duration; }
    bool isEOF() const { return m_eof.load(); }
    bool isSeekRequested() const { return m_seekRequested.load(); }

    // Container-level metadata tags (e.g. artist/genre/ReplayGain), for
    // callers that want to read them directly (see
    // naikav::dsp::readTaggedLoudnessAsLufs() and getGenreTag() below).
    // nullptr if no format is open.
    AVDictionary* getFormatMetadata() const { return m_formatCtx ? m_formatCtx->metadata : nullptr; }

    // Per-stream metadata tags for the audio stream specifically -- some
    // containers (e.g. FLAC with per-stream Vorbis comments) attach tags
    // there rather than at the format level. nullptr if there's no audio
    // stream open.
    AVDictionary* getAudioStreamMetadata() const {
        int idx = m_audioStreamIdx.load();
        if (!m_formatCtx || idx < 0 || idx >= static_cast<int>(m_formatCtx->nb_streams)) {
            return nullptr;
        }
        return m_formatCtx->streams[idx]->metadata;
    }

    // Convenience accessor for the free-form genre tag (container "genre"
    // key, which FFmpeg normalizes ID3 TCON/Vorbis comment "GENRE"/etc.
    // into), for naikav::dsp::presetForGenreTag(). Empty string if absent.
    std::string getGenreTag() const {
        if (const AVDictionary* d = getFormatMetadata()) {
            if (const AVDictionaryEntry* e = av_dict_get(d, "genre", nullptr, 0)) {
                return e->value;
            }
        }
        if (const AVDictionary* d = getAudioStreamMetadata()) {
            if (const AVDictionaryEntry* e = av_dict_get(d, "genre", nullptr, 0)) {
                return e->value;
            }
        }
        return "";
    }
};

