#pragma once

#include "audio/dsp/LoudnessMeter.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/version.h>
}

#include <string>
#include <vector>
#include <cstdint>

namespace naikav::dsp {

// Decode-only, no-SDL pass over a whole file's audio stream, run entirely
// through EBU R128 metering (the same LoudnessMeter used for real-time
// normalization) to obtain a stable whole-file integrated loudness figure
// before playback starts. This is what backs LoudnessNormalizer's two-pass
// mode (see primeWithPrescannedLufs()): a real-time-only meter can only
// report on however much of the file has played so far, understating (or
// overstating) the correction needed until the running measurement
// converges: it also means every seek makes the numbers wrong again while
// the newly-continuous segment reconverges. Scanning the file once, fast
// (audio-only, no video decode, no device I/O, no realtime pacing), avoids
// both problems at the cost of a one-time decode pass before playback
// begins.
//
// Returns the integrated LUFS on success, or -120.0 (below EBU R128's
// -70 LUFS silence gate, so callers already treating "no valid reading" as
// "<= -70" reject it correctly) if the file/stream couldn't be opened or
// decoded, or contained no usable audio.
inline double prescanIntegratedLufs(const std::string& filePath, int audioStreamIndex = -1) {
    constexpr double kFailureSentinel = -120.0;

    AVFormatContext* fmtCtx = nullptr;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file,pipe", 0);
    int openRet = avformat_open_input(&fmtCtx, filePath.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (openRet < 0 || !fmtCtx) {
        return kFailureSentinel;
    }

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return kFailureSentinel;
    }

    int streamIdx = audioStreamIndex;
    if (streamIdx < 0 || streamIdx >= static_cast<int>(fmtCtx->nb_streams) ||
        fmtCtx->streams[streamIdx]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        const AVCodec* dummyDecoder = nullptr;
        streamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &dummyDecoder, 0);
    }
    if (streamIdx < 0) {
        avformat_close_input(&fmtCtx);
        return kFailureSentinel;
    }

    const AVCodecParameters* codecParams = fmtCtx->streams[streamIdx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        avformat_close_input(&fmtCtx);
        return kFailureSentinel;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx || avcodec_parameters_to_context(codecCtx, codecParams) < 0 ||
        avcodec_open2(codecCtx, codec, nullptr) < 0) {
        if (codecCtx) avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return kFailureSentinel;
    }

#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout inLayout;
    if (codecCtx->ch_layout.nb_channels <= 0) {
        av_channel_layout_default(&inLayout, 2);
    } else {
        av_channel_layout_copy(&inLayout, &codecCtx->ch_layout);
    }
    const int channels = inLayout.nb_channels;
#else
    int64_t inLayout = codecCtx->channel_layout;
    if (inLayout == 0) {
        inLayout = av_get_default_channel_layout(codecCtx->channels > 0 ? codecCtx->channels : 2);
    }
    const int channels = av_get_channel_layout_nb_channels(static_cast<uint64_t>(inLayout));
#endif
    const int sampleRate = codecCtx->sample_rate > 0 ? codecCtx->sample_rate : 48000;

    // Format-only conversion (packed float, same rate/layout as the
    // decoder) -- no resampling is needed since the meter just needs
    // interleaved float at whatever rate/channel count the source has.
    SwrContext* swrCtx = nullptr;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    int swrRet = swr_alloc_set_opts2(&swrCtx, &inLayout, AV_SAMPLE_FMT_FLT, sampleRate,
                                      &inLayout, codecCtx->sample_fmt, sampleRate, 0, nullptr);
#else
    swrCtx = swr_alloc_set_opts(nullptr, inLayout, AV_SAMPLE_FMT_FLT, sampleRate,
                                 inLayout, codecCtx->sample_fmt, sampleRate, 0, nullptr);
    int swrRet = swrCtx ? 0 : -1;
#endif
    if (swrRet < 0 || !swrCtx || swr_init(swrCtx) < 0) {
        if (swrCtx) swr_free(&swrCtx);
#if LIBAVUTIL_VERSION_MAJOR >= 57
        av_channel_layout_uninit(&inLayout);
#endif
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return kFailureSentinel;
    }

    LoudnessMeter meter;
    if (!meter.configure(channels, sampleRate)) {
        swr_free(&swrCtx);
#if LIBAVUTIL_VERSION_MAJOR >= 57
        av_channel_layout_uninit(&inLayout);
#endif
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return kFailureSentinel;
    }

    std::vector<float> floatBuffer;
    auto feedFrame = [&](AVFrame* frame) {
        const int maxOutSamples = frame->nb_samples + 32; // small margin, no rate change expected
        floatBuffer.resize(static_cast<size_t>(maxOutSamples) * channels);
        uint8_t* outPtr = reinterpret_cast<uint8_t*>(floatBuffer.data());
        int converted = swr_convert(swrCtx, &outPtr, maxOutSamples,
                                     const_cast<const uint8_t**>(frame->data), frame->nb_samples);
        if (converted > 0) {
            meter.feed(floatBuffer.data(), converted);
        }
    };

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (packet && frame) {
        while (av_read_frame(fmtCtx, packet) >= 0) {
            if (packet->stream_index == streamIdx) {
                if (avcodec_send_packet(codecCtx, packet) >= 0) {
                    while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                        feedFrame(frame);
                    }
                }
            }
            av_packet_unref(packet);
        }
        // Flush the decoder's buffered frames at EOF.
        avcodec_send_packet(codecCtx, nullptr);
        while (avcodec_receive_frame(codecCtx, frame) >= 0) {
            feedFrame(frame);
        }
    }
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);

    // Push end-of-stream through the metering graph as well, not just the
    // decoder. Without this the ebur128 filter never emits its final
    // partial gating window, so the whole-file figure silently omits the
    // last fraction of a second -- a small error, but this function exists
    // precisely to be more accurate than the streaming measurement.
    meter.flush();

    const double integratedLufs = meter.getIntegratedLufs();

    swr_free(&swrCtx);
#if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_uninit(&inLayout);
#endif
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    return integratedLufs;
}

} // namespace naikav::dsp
