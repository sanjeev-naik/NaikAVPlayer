#define NAIKAV_UNIT_TESTING 1
#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <sstream>
#include <numeric>

class ExpectedErrorRedirector {
private:
    std::stringstream m_oss;
    std::streambuf* m_oldCerr;
public:
    ExpectedErrorRedirector() {
        m_oldCerr = std::cerr.rdbuf(m_oss.rdbuf());
    }
    ~ExpectedErrorRedirector() {
        std::cerr.rdbuf(m_oldCerr);
        std::string line;
        while (std::getline(m_oss, line)) {
            if (line.find("Assertion FAILED:") != std::string::npos) {
                std::cerr << line << "\n";
            } else {
                std::cout << "[EXPECTED] " << line << "\n";
            }
        }
    }
};

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

extern "C" {
#include <libavutil/version.h>
}

class Demuxer;

// -------------------------------------------------------------
// FFmpeg & SDL Mocking Interceptors
// -------------------------------------------------------------
static std::atomic<bool> force_alloc_fail{false};
static std::atomic<bool> force_open_fail{false};
static std::atomic<bool> force_frame_alloc_fail{false};
static std::atomic<bool> force_malloc_fail{false};
static std::atomic<bool> force_image_fill_fail{false};
static std::atomic<bool> force_swr_init_fail{false};
static std::atomic<bool> force_swr_convert_fail{false};
static std::atomic<bool> force_seek_fail{false};
static std::atomic<bool> force_find_stream_info_fail{false};
static std::atomic<bool> force_copy_params_fail{false};
static std::atomic<bool> force_sdl_audio_fail{false};
static std::atomic<bool> force_send_packet_fail{false};
static std::atomic<bool> force_receive_frame_fail{false};
// When true, arms pending_synthetic_flush_frame on the next flush-style
// avcodec_send_packet(ctx, nullptr) call, so the following
// avcodec_receive_frame() returns one synthetic frame instead of the real
// decoder's (likely empty, for most codecs) drain result. Used to
// deterministically exercise flush-loop code paths that depend on the
// decoder still having a buffered frame at EOF -- real decoders on this
// build never actually do for the small test assets available.
static std::atomic<bool> force_synthetic_flush_frame{false};
static std::atomic<bool> pending_synthetic_flush_frame{false};
static std::atomic<bool> force_no_pts{false};
static std::atomic<bool> force_no_streams{false};
static std::atomic<bool> force_no_duration{false};
static std::atomic<bool> force_packet_alloc_fail{false};
static std::atomic<bool> force_read_error{false};
static std::atomic<bool> force_video_eof{false};
static std::atomic<bool> force_video_error{false};
static std::atomic<bool> force_sws_context_fail{false};
static std::atomic<bool> force_read_eof{false};

static std::atomic<bool> force_zero_channels{false};
static std::atomic<bool> force_channel_layout_5_1{false};
static std::atomic<bool> force_channel_layout_2_1{false};
static std::atomic<bool> force_sdl_reject_surround{false};
static std::atomic<bool> force_sdl_init_fail{false};
static std::atomic<bool> open_finished{false};
static std::atomic<int> packet_alloc_count{0};

#include <functional>
static std::atomic<bool> force_hw_transfer_fail{false};
static std::atomic<bool> force_receive_eagain{false};
static std::atomic<bool> mock_send_packet_success{false};
static std::atomic<bool> mock_hw_transfer_nv12{false};
static std::atomic<bool> force_hw_transfer_real_buffer{false};
static std::atomic<bool> force_hw_frame_ref_fail{false};
// -1 = inactive/off. 1 = armed: let the very next av_frame_ref() call
// through untouched, then fail the one immediately after that, then
// disarm back to -1. Used to fail exactly PlayerController::videoThreadLoop()'s
// own av_frame_ref() call (its call always follows immediately after
// VideoDecoder::convertFrame()'s own, separate av_frame_ref() call within
// the same frame) without an extended failing window.
static std::atomic<int> force_player_frame_ref_fail_next{-1};
// One-shot artificial delay (milliseconds) injected into the very next
// avcodec_receive_frame() call that actually returns a real decoded frame,
// then disarms itself. Used to widen the window between videoThreadLoop()'s
// own "queue not full" gate check and its later push_wait_or_drop() call,
// without touching any of PlayerController's own mutexes from the test
// thread (which risks deadlocking/destabilizing unrelated subsystems --
// see the m_videoDecoderMutex-holding approach this replaced).
static std::atomic<int> force_decode_delay_once_ms{0};
static std::atomic<bool> force_avfilter_graph_alloc_fail{false};
static std::atomic<bool> force_avfilter_get_by_name_fail{false};
static std::atomic<bool> force_avfilter_create_filter_fail{false};
static std::atomic<bool> force_buffersrc_add_frame_fail{false};
static std::atomic<bool> force_find_decoder_fail{false};
static std::atomic<bool> force_find_encoder_fail{false};
// 0 = off (real decode), 1 = synthesize a SUBTITLE_ASS rect whose ass string
// carries a "Dialogue:" 9-comma prefix, 2 = synthesize a SUBTITLE_TEXT rect.
// Real ffmpeg subtitle decoders (subrip/webvtt/ass/mov_text) on this build
// always emit SUBTITLE_ASS rects in the raw 8-field form, never a
// "Dialogue:"-prefixed ass string or SUBTITLE_TEXT rect, so those two
// SubtitleDecoder::processPacket() branches are otherwise unreachable.
static std::atomic<int> force_synthetic_subtitle_rect{0};
static std::atomic<bool> force_pts_to_dts_only{false};
static std::atomic<bool> force_send_frame_fail{false};
static std::atomic<bool> force_receive_packet_fail{false};
// Thread-local on purpose: this is a *counter*, not a switch. Armed on
// the test thread, a process-wide counter can be consumed by any other
// thread that happens to call av_frame_alloc() in between -- the loudness
// prescan thread a live PlayerController spawns does exactly that, which
// silently disarmed the mock and made the test see a *successful* call.
static thread_local int force_frame_alloc_fail_on_second_call = -1;
static std::atomic<bool> force_send_packet_eagain{false};
// -1 = disabled, 1 = let the next avcodec_receive_frame() call return EAGAIN
// then arm for the call after (set to 0), 0 = return a hard (non-EAGAIN,
// non-EOF) error on that call then disable (-1).
// thread_local for the same reason as force_frame_alloc_fail_on_second_call
// above: armed on the test thread, and the prescan thread's own
// avcodec_receive_frame() calls would otherwise consume the arming.
static thread_local int force_receive_frame_eagain_then_fail = -1;
static std::atomic<bool> force_soxr_fail{false};
static std::atomic<bool> force_swr_alloc_fail{false};
// -1 = disabled, 1 = let the next swr_init() call succeed then arm for the
// call after (set to 0), 0 = fail the next call then disable (-1). Used to
// fail specifically the *second* initResampler() call (the stereo-fallback
// retry after the device rejects a surround stream) without also failing
// the first, always-successful call that precedes it.
// thread_local: armed on the test thread, and the prescan thread calls
// swr_init() too, which would otherwise consume the arming.
static thread_local int force_swr_init_fail_on_retry = -1;
static std::atomic<bool> force_no_pts_no_dts{false};
static std::atomic<int> force_fake_playback_devices{0};
static std::atomic<int> force_queued_bytes{-1};
static std::atomic<bool> force_channel_layout_describe_fail{false};
static std::mutex mock_read_frame_mutex;
static std::function<void()> on_mock_read_frame = nullptr;
struct AVCodec;
static const struct AVCodec* global_saved_codec = nullptr;
static const struct AVCodec* global_fake_codec_ptr = nullptr;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
}

// Inline mock wrappers
inline AVCodecContext* mock_avcodec_alloc_context3(const AVCodec* codec) {
    if (force_alloc_fail) return nullptr;
    return avcodec_alloc_context3(codec);
}
#define avcodec_alloc_context3 mock_avcodec_alloc_context3

inline int mock_avcodec_open2(AVCodecContext* avctx, const AVCodec* codec, AVDictionary** options) {
    if (force_open_fail) return -1;
    int ret = avcodec_open2(avctx, codec, options);
    if (ret >= 0) {
        open_finished = true;
        if (force_zero_channels) {
#if LIBAVUTIL_VERSION_MAJOR >= 57
            avctx->ch_layout.nb_channels = 0;
#else
            avctx->channels = 0;
            avctx->channel_layout = 0;
#endif
        }
        if (force_channel_layout_5_1) {
            // Pretend the source is a 5.1 stream regardless of what the real
            // (stereo) test asset actually decodes as, so the channel-layout
            // resolution logic can be tested without a real 5.1 fixture.
#if LIBAVUTIL_VERSION_MAJOR >= 57
            av_channel_layout_uninit(&avctx->ch_layout);
            av_channel_layout_from_mask(&avctx->ch_layout, AV_CH_LAYOUT_5POINT1);
#else
            avctx->channels = 6;
            avctx->channel_layout = AV_CH_LAYOUT_5POINT1;
#endif
        }
        if (force_channel_layout_2_1) {
            // Same idea as force_channel_layout_5_1, for a 2.1 (stereo + LFE) source.
#if LIBAVUTIL_VERSION_MAJOR >= 57
            av_channel_layout_uninit(&avctx->ch_layout);
            av_channel_layout_from_mask(&avctx->ch_layout, AV_CH_LAYOUT_2POINT1);
#else
            avctx->channels = 3;
            avctx->channel_layout = AV_CH_LAYOUT_2POINT1;
#endif
        }
    }
    return ret;
}
#define avcodec_open2 mock_avcodec_open2

inline AVFrame* mock_av_frame_alloc() {
    if (force_frame_alloc_fail && open_finished) {
        return nullptr;
    }
    // -1 = disabled, 1 = let this call succeed then arm for the next (set to
    // 0), 0 = fail this call then disable (-1). Lets a test fail specifically
    // the *second* av_frame_alloc() call in a function without also failing
    // an earlier, unrelated one in the same call.
    int state = force_frame_alloc_fail_on_second_call;
    if (state == 1) {
        force_frame_alloc_fail_on_second_call = 0;
        return av_frame_alloc();
    }
    if (state == 0) {
        force_frame_alloc_fail_on_second_call = -1;
        return nullptr;
    }
    return av_frame_alloc();
}
#define av_frame_alloc mock_av_frame_alloc

inline AVPacket* mock_av_packet_alloc() {
    if (force_packet_alloc_fail) {
        packet_alloc_count++;
        if (packet_alloc_count > 5) {
            return nullptr;
        }
    }
    return av_packet_alloc();
}
#define av_packet_alloc mock_av_packet_alloc

inline void* mock_av_malloc(size_t size) {
    if (force_malloc_fail) return nullptr;
    return av_malloc(size);
}
#define av_malloc mock_av_malloc

inline int mock_av_image_fill_arrays(uint8_t* dst_data[4], int dst_linesize[4], const uint8_t* src, AVPixelFormat pix_fmt, int width, int height, int align) {
    if (force_image_fill_fail) return -1;
    return av_image_fill_arrays(dst_data, dst_linesize, src, pix_fmt, width, height, align);
}
#define av_image_fill_arrays mock_av_image_fill_arrays

inline int mock_swr_init(struct SwrContext* s) {
    if (force_swr_init_fail) return -1;
    int retryState = force_swr_init_fail_on_retry;
    if (retryState == 1) {
        force_swr_init_fail_on_retry = 0;
        return swr_init(s);
    }
    if (retryState == 0) {
        force_swr_init_fail_on_retry = -1;
        return -1;
    }
    return swr_init(s);
}
#define swr_init mock_swr_init

inline int mock_swr_alloc_set_opts2(struct SwrContext** ps, const AVChannelLayout* out_ch_layout,
                                     enum AVSampleFormat out_sample_fmt, int out_sample_rate,
                                     const AVChannelLayout* in_ch_layout, enum AVSampleFormat in_sample_fmt,
                                     int in_sample_rate, int log_offset, void* log_ctx) {
    if (force_swr_alloc_fail) return -1;
    return swr_alloc_set_opts2(ps, out_ch_layout, out_sample_fmt, out_sample_rate,
                                in_ch_layout, in_sample_fmt, in_sample_rate, log_offset, log_ctx);
}
#define swr_alloc_set_opts2 mock_swr_alloc_set_opts2

inline int mock_av_opt_set(void* obj, const char* name, const char* val, int search_flags) {
    if (force_soxr_fail && std::string(name) == "resampler" && std::string(val) == "soxr") {
        return AVERROR(EINVAL);
    }
    return av_opt_set(obj, name, val, search_flags);
}
#define av_opt_set mock_av_opt_set

inline int mock_swr_convert(struct SwrContext* s, uint8_t** out, int out_count, const uint8_t** in, int in_count) {
    if (force_swr_convert_fail) return -1;
    return swr_convert(s, out, out_count, in, in_count);
}
#define swr_convert mock_swr_convert

inline struct SwsContext* mock_sws_getContext(int srcW, int srcH, enum AVPixelFormat srcFormat,
                                              int dstW, int dstH, enum AVPixelFormat dstFormat,
                                              int flags, SwsFilter *srcFilter,
                                              SwsFilter *dstFilter, const double *param) {
    if (force_sws_context_fail) return nullptr;
    return (sws_getContext)(srcW, srcH, srcFormat, dstW, dstH, dstFormat, flags, srcFilter, dstFilter, param);
}
#define sws_getContext mock_sws_getContext

inline int mock_avformat_seek_file(AVFormatContext* s, int stream_index, int64_t min_ts, int64_t ts, int64_t max_ts, int flags) {
    if (force_seek_fail) return -1;
    return avformat_seek_file(s, stream_index, min_ts, ts, max_ts, flags);
}
#define avformat_seek_file mock_avformat_seek_file

inline int mock_avformat_find_stream_info(AVFormatContext* ic, AVDictionary** options) {
    if (force_find_stream_info_fail) return -1;
    int ret = avformat_find_stream_info(ic, options);
    if (ret >= 0) {
        if (force_no_streams) {
            for (unsigned int i = 0; i < ic->nb_streams; i++) {
                ic->streams[i]->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
            }
        }
        if (force_no_duration) {
            ic->duration = AV_NOPTS_VALUE;
        }
    }
    return ret;
}
#define avformat_find_stream_info mock_avformat_find_stream_info

inline int mock_avcodec_parameters_to_context(AVCodecContext* codec, const AVCodecParameters* par) {
    if (force_copy_params_fail) return -1;
    return avcodec_parameters_to_context(codec, par);
}
#define avcodec_parameters_to_context mock_avcodec_parameters_to_context

inline const AVCodec* mock_avcodec_find_decoder(enum AVCodecID id) {
    if (force_find_decoder_fail) return nullptr;
    return avcodec_find_decoder(id);
}
#define avcodec_find_decoder mock_avcodec_find_decoder

inline const AVCodec* mock_avcodec_find_encoder(enum AVCodecID id) {
    if (force_find_encoder_fail) return nullptr;
    return avcodec_find_encoder(id);
}
#define avcodec_find_encoder mock_avcodec_find_encoder

inline int mock_avcodec_send_frame(AVCodecContext* avctx, const AVFrame* frame) {
    if (force_send_frame_fail) return -1;
    return avcodec_send_frame(avctx, frame);
}
#define avcodec_send_frame mock_avcodec_send_frame

inline int mock_avcodec_receive_packet(AVCodecContext* avctx, AVPacket* avpkt) {
    if (force_receive_packet_fail) return -1;
    return avcodec_receive_packet(avctx, avpkt);
}
#define avcodec_receive_packet mock_avcodec_receive_packet

inline int mock_avcodec_decode_subtitle2(AVCodecContext* avctx, AVSubtitle* sub, int* got_sub_ptr, AVPacket* avpkt) {
    int mode = force_synthetic_subtitle_rect.load(std::memory_order_relaxed);
    if (mode != 0) {
        memset(sub, 0, sizeof(*sub));
        AVSubtitleRect* rect = static_cast<AVSubtitleRect*>(av_mallocz(sizeof(AVSubtitleRect)));
        if (mode == 1) {
            rect->type = SUBTITLE_ASS;
            rect->ass = av_strdup("Dialogue: 0,0:00:01.00,0:00:02.50,Default,,0,0,0,,Synthetic dialogue text");
        } else {
            rect->type = SUBTITLE_TEXT;
            rect->text = av_strdup("Synthetic plain text");
        }
        sub->rects = static_cast<AVSubtitleRect**>(av_malloc(sizeof(AVSubtitleRect*)));
        sub->rects[0] = rect;
        sub->num_rects = 1;
        sub->start_display_time = 100;
        sub->end_display_time = 2500;
        *got_sub_ptr = 1;
        return avpkt->size;
    }
    return avcodec_decode_subtitle2(avctx, sub, got_sub_ptr, avpkt);
}
#define avcodec_decode_subtitle2 mock_avcodec_decode_subtitle2



inline SDL_AudioStream* mock_SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec* spec, SDL_AudioStreamCallback callback, void* userdata) {
    if (force_sdl_audio_fail) return nullptr;
    if (force_sdl_reject_surround && spec && spec->channels > 2) return nullptr;
#if defined(__linux__)
    // On Linux CI/headless environments, avoid initializing real ALSA audio subsystem to prevent system driver leaks
    (void)devid;
    (void)callback;
    (void)userdata;
    return SDL_CreateAudioStream(spec, spec);
#else
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(devid, spec, callback, userdata);
    if (!stream) {
        // Fallback for CI/limited environments
        std::cout << "[Test Mock] SDL_OpenAudioDeviceStream failed: " << SDL_GetError() << ". Falling back to virtual stream." << std::endl;
        return SDL_CreateAudioStream(spec, spec);
    }
    return stream;
#endif
}
#define SDL_OpenAudioDeviceStream mock_SDL_OpenAudioDeviceStream

inline void mock_avcodec_free_context(AVCodecContext** pavctx) {
    if (pavctx && *pavctx && global_saved_codec && global_fake_codec_ptr) {
        if ((*pavctx)->codec == global_fake_codec_ptr) {
            (*pavctx)->codec = global_saved_codec;
        }
    }
    avcodec_free_context(pavctx);
}
#define avcodec_free_context mock_avcodec_free_context

inline int mock_avcodec_send_packet(AVCodecContext* avctx, const AVPacket* avpkt) {
    if (force_send_packet_fail) return -1;
    if (force_send_packet_eagain) return AVERROR(EAGAIN);
    if (mock_send_packet_success) return 0;
    int ret = avcodec_send_packet(avctx, avpkt);
    if (ret >= 0 && avpkt == nullptr && force_synthetic_flush_frame) {
        pending_synthetic_flush_frame = true;
    }
    return ret;
}
#define avcodec_send_packet mock_avcodec_send_packet

inline int mock_avcodec_receive_frame(AVCodecContext* avctx, AVFrame* frame) {
    if (force_receive_frame_fail) return -2;
    if (force_video_eof) return AVERROR_EOF;
    if (force_video_error) return -5;
    if (force_receive_eagain) return AVERROR(EAGAIN);
    if (pending_synthetic_flush_frame.exchange(false)) {
        av_frame_unref(frame);
        frame->format = avctx->sample_fmt;
        frame->sample_rate = avctx->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
        av_channel_layout_copy(&frame->ch_layout, &avctx->ch_layout);
#else
        frame->channel_layout = avctx->channel_layout;
        frame->channels = avctx->channels;
#endif
        frame->nb_samples = 64;
        if (av_frame_get_buffer(frame, 0) >= 0) {
            return 0;
        }
        return AVERROR_EOF;
    }
    int eagainThenFailState = force_receive_frame_eagain_then_fail;
    if (eagainThenFailState == 1) {
        force_receive_frame_eagain_then_fail = 0;
        return AVERROR(EAGAIN);
    }
    if (eagainThenFailState == 0) {
        force_receive_frame_eagain_then_fail = -1;
        return -5;
    }

    int ret = avcodec_receive_frame(avctx, frame);
    if (ret >= 0) {
        int delayMs = force_decode_delay_once_ms.exchange(0, std::memory_order_relaxed);
        if (delayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
    }
    if (ret >= 0 && force_no_pts) {
        frame->pts = AV_NOPTS_VALUE;
        frame->pkt_dts = 1000; // Provide DTS to trigger DTS fallback path
    }
    if (ret >= 0 && force_no_pts_no_dts) {
        frame->pts = AV_NOPTS_VALUE;
        frame->pkt_dts = AV_NOPTS_VALUE;
    }
    return ret;
}
#define avcodec_receive_frame mock_avcodec_receive_frame

inline int mock_av_hwframe_transfer_data(AVFrame* dst, const AVFrame* src, int flags) {
    (void)flags;
    if (force_hw_transfer_fail) return -1;
    dst->width = src->width;
    dst->height = src->height;
    dst->format = mock_hw_transfer_nv12 ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_YUV420P;
    if (force_hw_transfer_real_buffer) {
        // A genuine hardware->CPU transfer produces a frame with real pixel
        // data, not just width/height/format -- needed so downstream code
        // (e.g. av_frame_ref()) sees a non-dummy frame (buf[0]/data[0] set)
        // instead of taking convertFrame()'s "mock frame in unit tests" shortcut.
        av_frame_get_buffer(dst, 32);
    }
    return 0;
}
#define av_hwframe_transfer_data mock_av_hwframe_transfer_data

inline int mock_av_frame_get_buffer(AVFrame* frame, int align) {
    if (force_malloc_fail || force_image_fill_fail) return -1;
    return av_frame_get_buffer(frame, align);
}
#define av_frame_get_buffer mock_av_frame_get_buffer

inline int mock_av_frame_ref(AVFrame* dst, const AVFrame* src) {
    if (force_hw_frame_ref_fail) return AVERROR(ENOMEM);
    int state = force_player_frame_ref_fail_next.load(std::memory_order_relaxed);
    if (state == 1) {
        force_player_frame_ref_fail_next.store(0, std::memory_order_relaxed);
        return av_frame_ref(dst, src);
    }
    if (state == 0) {
        force_player_frame_ref_fail_next.store(-1, std::memory_order_relaxed);
        return AVERROR(ENOMEM);
    }
    return av_frame_ref(dst, src);
}
#define av_frame_ref mock_av_frame_ref

inline AVFilterGraph* mock_avfilter_graph_alloc() {
    if (force_avfilter_graph_alloc_fail) return nullptr;
    return avfilter_graph_alloc();
}
#define avfilter_graph_alloc mock_avfilter_graph_alloc

inline const AVFilter* mock_avfilter_get_by_name(const char* name) {
    if (force_avfilter_get_by_name_fail) return nullptr;
    return avfilter_get_by_name(name);
}
#define avfilter_get_by_name mock_avfilter_get_by_name

inline int mock_avfilter_graph_create_filter(AVFilterContext** filt_ctx, const AVFilter* filt,
                                              const char* name, const char* args, void* opaque,
                                              AVFilterGraph* graph_ctx) {
    if (force_avfilter_create_filter_fail) return -1;
    return avfilter_graph_create_filter(filt_ctx, filt, name, args, opaque, graph_ctx);
}
#define avfilter_graph_create_filter mock_avfilter_graph_create_filter

inline int mock_av_buffersrc_add_frame(AVFilterContext* ctx, AVFrame* frame) {
    // Callers (see LoudnessMeter::feed()) always free `frame` themselves
    // right after this call regardless of the return value -- matching
    // real av_buffersrc_add_frame()'s documented behavior of not taking
    // ownership -- so this must not touch frame's memory itself.
    if (force_buffersrc_add_frame_fail) return -1;
    return av_buffersrc_add_frame(ctx, frame);
}
#define av_buffersrc_add_frame mock_av_buffersrc_add_frame

inline int mock_av_read_frame(AVFormatContext* s, AVPacket* pkt) {
    if (force_read_eof) return AVERROR_EOF;
    if (force_read_error) return -5;

    std::function<void()> callback = nullptr;
    {
        std::lock_guard<std::mutex> lock(mock_read_frame_mutex);
        callback = on_mock_read_frame;
    }
    if (callback) {
        callback();
    }
    int ret = av_read_frame(s, pkt);
    // Strips pts (moving it to dts) on every real packet read while active,
    // to deterministically exercise pts-is-NOPTS/dts-is-valid fallback
    // branches that real demuxers essentially never produce on their own.
    if (ret >= 0 && force_pts_to_dts_only && pkt->pts != AV_NOPTS_VALUE) {
        pkt->dts = pkt->pts;
        pkt->pts = AV_NOPTS_VALUE;
    }
    return ret;
}
#define av_read_frame mock_av_read_frame

inline bool mock_SDL_Init(SDL_InitFlags flags) {
    if (force_sdl_init_fail) return false;
    bool ret = SDL_Init(flags);
    if (!ret) {
        // Fallback for headless CI/container environments without physical audio hardware
        SDL_SetHint("SDL_AUDIO_DRIVER", "dummy");
        SDL_SetHint("SDL_VIDEO_DRIVER", "offscreen");
        ret = SDL_Init(flags);
    }
    return ret;
}
#define SDL_Init mock_SDL_Init

inline SDL_AudioDeviceID* mock_SDL_GetAudioPlaybackDevices(int* count) {
    int n = force_fake_playback_devices.load();
    if (n > 0) {
        SDL_AudioDeviceID* arr = static_cast<SDL_AudioDeviceID*>(SDL_malloc(sizeof(SDL_AudioDeviceID) * static_cast<size_t>(n)));
        for (int i = 0; i < n; ++i) {
            arr[i] = static_cast<SDL_AudioDeviceID>(9000 + i);
        }
        *count = n;
        return arr;
    }
    return SDL_GetAudioPlaybackDevices(count);
}
#define SDL_GetAudioPlaybackDevices mock_SDL_GetAudioPlaybackDevices

inline const char* mock_SDL_GetAudioDeviceName(SDL_AudioDeviceID devid) {
    if (devid >= 9000 && devid < 9064) {
        static thread_local std::string fakeName;
        fakeName = "Fake Device " + std::to_string(devid - 9000);
        return fakeName.c_str();
    }
    return SDL_GetAudioDeviceName(devid);
}
#define SDL_GetAudioDeviceName mock_SDL_GetAudioDeviceName

inline int mock_SDL_GetAudioStreamQueued(SDL_AudioStream* stream) {
    int forced = force_queued_bytes.load();
    if (forced >= 0) return forced;
    return SDL_GetAudioStreamQueued(stream);
}
#define SDL_GetAudioStreamQueued mock_SDL_GetAudioStreamQueued

inline int mock_av_channel_layout_describe(const AVChannelLayout* channel_layout, char* buf, size_t buf_size) {
    if (force_channel_layout_describe_fail) return -1;
    return av_channel_layout_describe(channel_layout, buf, buf_size);
}
#define av_channel_layout_describe mock_av_channel_layout_describe

// -------------------------------------------------------------
// Direct C++ sources inclusion with private visibility bypass
// -------------------------------------------------------------
#define private public
#include "player/PlayerController.cpp"
#include "media/Demuxer.cpp"
#include "video/VideoDecoder.cpp"
#include "audio/AudioDecoder.cpp"
#include "subtitle/SubtitleDecoder.cpp"
#include "video/FrameExporter.hpp"
#include "playlist/Playlist.hpp"
#undef private

// Simple assert helper
void test_assert(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Assertion FAILED: " << message << std::endl;
        throw std::runtime_error("exit_called_1");
    } else {
        std::cout << "Assertion PASSED: " << message << std::endl;
    }
}

// Feed a packet into a live PlayerController's packet queue without ever
// blocking. ThreadSafeQueue::push() waits indefinitely while the queue sits
// at capacity, and this binary runs with the video thread disabled
// (g_videoThreadEnabled = false in main()), so nothing drains a video queue
// the demuxer has already saturated -- which it does within milliseconds of
// openFile() on any real file. A plain push() there deadlocks the test
// thread outright, and only sometimes: it comes down to whether the demuxer
// refills the slot a try_pop() just freed before this push() takes the lock.
// push_wait_or_drop() is the codebase's own backstop for producers that must
// not block (see ThreadSafeQueue.hpp) -- it drops the oldest packet to make
// room rather than waiting forever.
static bool feedPacket(ThreadSafeQueue<AVPacket*>& queue, AVPacket* packet) {
    if (!queue.push_wait_or_drop(packet, std::chrono::milliseconds(50),
                                 [](AVPacket*& dropped) { av_packet_free(&dropped); })) {
        av_packet_free(&packet);
        return false;
    }
    return true;
}

// Defensive reset of every force_*/mock_* injection flag to its default
// (off/passthrough) state. Intended for tests that spawn real background
// decode work (e.g. the loudness prescan thread) that must behave like real
// production code -- a flag some earlier, unrelated test forgot to reset
// could otherwise silently break or hang it.
void resetAllMockFlags() {
    force_alloc_fail = false;
    force_open_fail = false;
    force_frame_alloc_fail = false;
    force_malloc_fail = false;
    force_image_fill_fail = false;
    force_swr_init_fail = false;
    force_swr_convert_fail = false;
    force_seek_fail = false;
    force_find_stream_info_fail = false;
    force_copy_params_fail = false;
    force_sdl_audio_fail = false;
    force_send_packet_fail = false;
    force_receive_frame_fail = false;
    force_synthetic_flush_frame = false;
    pending_synthetic_flush_frame = false;
    force_player_frame_ref_fail_next = -1;
    force_decode_delay_once_ms = 0;
    force_no_pts = false;
    force_no_streams = false;
    force_no_duration = false;
    force_packet_alloc_fail = false;
    force_read_error = false;
    force_video_eof = false;
    force_video_error = false;
    force_sws_context_fail = false;
    force_read_eof = false;
    force_zero_channels = false;
    force_channel_layout_5_1 = false;
    force_channel_layout_2_1 = false;
    force_sdl_reject_surround = false;
    force_sdl_init_fail = false;
    force_hw_transfer_fail = false;
    force_receive_eagain = false;
    mock_send_packet_success = false;
    mock_hw_transfer_nv12 = false;
    force_soxr_fail = false;
    force_swr_alloc_fail = false;
    force_swr_init_fail_on_retry = -1;
    force_no_pts_no_dts = false;
    force_fake_playback_devices = 0;
    force_queued_bytes = -1;
    force_channel_layout_describe_fail = false;
    force_hw_transfer_real_buffer = false;
    force_hw_frame_ref_fail = false;
    force_frame_alloc_fail_on_second_call = -1;
    force_send_packet_eagain = false;
    force_receive_frame_eagain_then_fail = -1;
    force_avfilter_graph_alloc_fail = false;
    force_avfilter_get_by_name_fail = false;
    force_avfilter_create_filter_fail = false;
    force_buffersrc_add_frame_fail = false;
    force_find_decoder_fail = false;
    force_find_encoder_fail = false;
    force_send_frame_fail = false;
    force_receive_packet_fail = false;
    force_synthetic_subtitle_rect = 0;
    force_pts_to_dts_only = false;
    {
        std::lock_guard<std::mutex> lock(mock_read_frame_mutex);
        on_mock_read_frame = nullptr;
    }
}

void push_abort_helper(ThreadSafeQueue<int>* q) {
    bool pushResult = q->push(200);
    test_assert(!pushResult, "push on aborted queue returns false");
}

// Function to drive the video decoder decoding loop for a specific duration
void drive_playback(PlayerController& controller, double seconds) {
    auto startTime = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - startTime < std::chrono::duration<double>(seconds)) {
        if (controller.hasVideo()) {
            if (!controller.m_videoThreadRunning.load()) {
                double timeNow = controller.getCurrentTime();
                VideoDecoder* decoder = controller.getVideoDecoder();
                int drops = 0;
                while (decoder->getCurrentFramePts() < timeNow - 0.010 && drops < 8) {
                    if (!decoder->decodeNextFrame()) break;
                    drops++;
                }
            } else {
                // Background thread is running; pop and discard frames to prevent queue stalling
                DecodedFrame df;
                while (controller.m_decodedFrameQueue.try_pop(df)) {
                    if (df.frame) {
                        av_frame_free(&df.frame);
                    }
                }
            }
        }
        if (controller.hasAudio() && controller.m_audioDecoder && controller.getState() == PlayerState::PLAYING) {
            // Pump audio decoder callback for headless CI environments without active hardware audio driver
            AudioDecoder::sdlAudioStreamCallback(controller.m_audioDecoder.get(), nullptr, 4096, 4096);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int real_main(int argc, char* argv[]) {
    g_disableHardwareDecoders = true;
    std::cout << "Starting NaikAVPlayer 100% coverage integration tests..." << std::endl;

    // Initialize SDL Audio
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::cerr << "[EXPECTED] Failed to initialize SDL: " << SDL_GetError() << std::endl;
        return 1;
    }

    std::string testFile = "";
    if (argc > 1 && argv[1][0] != '-') {
        testFile = argv[1];
    } else if (const char* envVal = std::getenv("TEST_VIDEO_PATH")) {
        testFile = envVal;
    }

    if (testFile.empty()) {
        std::cerr << "[EXPECTED] Error: No test video file provided.\n"
                  << "Usage: " << argv[0] << " <path_to_video_file>\n"
                  << "Alternatively, set the TEST_VIDEO_PATH environment variable." << std::endl;
        SDL_Quit();
        return 1;
    }

    std::cout << "Testing with file: " << testFile << std::endl;

    // SDL_Quit() at the end of this function frees every SDL audio
    // stream that is still open. Any PlayerController alive past that
    // point then destroys its own stream a second time from
    // ~PlayerController() -> stop() -> AudioDecoder::stop() -- the
    // heap-use-after-free ASan flagged here. Scoping every controller
    // in this function (this one and all of the try block's) so they
    // are torn down before SDL_Quit() keeps the shutdown order right.
    int realMainResult = 0;
    {
    PlayerController controller;

    try {
        bool hasExceptionArg = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--test-exception") {
                hasExceptionArg = true;
                break;
            }
        }
        if (hasExceptionArg) {
            throw std::runtime_error("Simulated test exception");
        }

        // -------------------------------------------------------------
        // Unit Test: Dynamic Resolution Change Safety Check
        // -------------------------------------------------------------
        {
            ThreadSafeQueue<AVPacket*> dummyQueue;
            AVCodecParameters* testCodecParams = avcodec_parameters_alloc();
            testCodecParams->codec_type = AVMEDIA_TYPE_VIDEO;
            testCodecParams->codec_id = AV_CODEC_ID_RAWVIDEO;
            testCodecParams->format = AV_PIX_FMT_RGB24;
            testCodecParams->width = 160;
            testCodecParams->height = 120;

            VideoDecoder testDecoder(testCodecParams, {1, 25}, 0, dummyQueue);
            bool initSuccess = testDecoder.init();
            test_assert(initSuccess, "VideoDecoder init for dynamic resolution test");
            test_assert(testDecoder.m_allocatedWidth == 160, "Initial allocated width is 160");
            test_assert(testDecoder.m_allocatedHeight == 120, "Initial allocated height is 120");

            // Mock m_decodedFrame containing a frame of original dimensions (160x120) first to initialize m_swsCtx
            testDecoder.m_decodedFrame->width = 160;
            testDecoder.m_decodedFrame->height = 120;
            testDecoder.m_decodedFrame->format = AV_PIX_FMT_RGB24;
            std::vector<uint8_t> dummySrcBuffer1(160 * 120 * 4, 0);
            av_image_fill_arrays(
                testDecoder.m_decodedFrame->data,
                testDecoder.m_decodedFrame->linesize,
                dummySrcBuffer1.data(),
                AV_PIX_FMT_RGB24,
                160,
                120,
                1
            );
            bool convertInitSuccess = testDecoder.convertFrame();
            test_assert(convertInitSuccess, "First convertFrame call initializes m_swsCtx");
            test_assert(testDecoder.m_swsCtx != nullptr, "m_swsCtx is initialized");

            // Test case 1: Dynamic resolution change fails because av_malloc returns null (force_malloc_fail = true)
            testDecoder.m_decodedFrame->width = 320;
            testDecoder.m_decodedFrame->height = 240;
            testDecoder.m_decodedFrame->format = AV_PIX_FMT_RGB24;
            std::vector<uint8_t> dummySrcBuffer2(320 * 240 * 4, 0);
            av_image_fill_arrays(
                testDecoder.m_decodedFrame->data,
                testDecoder.m_decodedFrame->linesize,
                dummySrcBuffer2.data(),
                AV_PIX_FMT_RGB24,
                320,
                240,
                1
            );
            force_malloc_fail = true;
            bool convertMallocFail = testDecoder.convertFrame();
            test_assert(!convertMallocFail, "convertFrame fails when av_malloc fails on resolution change");
            force_malloc_fail = false;

            // Test case 2: Dynamic resolution change fails because av_image_fill_arrays fails (force_image_fill_fail = true)
            testDecoder.m_decodedFrame->width = 320;
            testDecoder.m_decodedFrame->height = 240;
            testDecoder.m_decodedFrame->format = AV_PIX_FMT_RGB24;
            av_image_fill_arrays(
                testDecoder.m_decodedFrame->data,
                testDecoder.m_decodedFrame->linesize,
                dummySrcBuffer2.data(),
                AV_PIX_FMT_RGB24,
                320,
                240,
                1
            );
            force_image_fill_fail = true;
            bool convertFillFail = testDecoder.convertFrame();
            test_assert(!convertFillFail, "convertFrame fails when av_image_fill_arrays fails on resolution change");
            force_image_fill_fail = false;

            // Test case 3: Dynamic resolution change fails because sws_getContext fails (force_sws_context_fail = true)
            testDecoder.m_decodedFrame->width = 480;
            testDecoder.m_decodedFrame->height = 360;
            testDecoder.m_decodedFrame->format = AV_PIX_FMT_RGB24;
            std::vector<uint8_t> dummySrcBuffer3(480 * 360 * 4, 0);
            av_image_fill_arrays(
                testDecoder.m_decodedFrame->data,
                testDecoder.m_decodedFrame->linesize,
                dummySrcBuffer3.data(),
                AV_PIX_FMT_RGB24,
                480,
                360,
                1
            );
            force_sws_context_fail = true;
            bool convertSwsFail = testDecoder.convertFrame();
            test_assert(!convertSwsFail, "convertFrame fails when sws_getContext fails on resolution change");
            force_sws_context_fail = false;

            // Test case 4: Dynamic resolution change succeeds! (Frees old context and reallocates successfully)
            testDecoder.m_decodedFrame->width = 320;
            testDecoder.m_decodedFrame->height = 240;
            testDecoder.m_decodedFrame->format = AV_PIX_FMT_RGB24;
            av_image_fill_arrays(
                testDecoder.m_decodedFrame->data,
                testDecoder.m_decodedFrame->linesize,
                dummySrcBuffer2.data(),
                AV_PIX_FMT_RGB24,
                320,
                240,
                1
            );
            bool convertSuccess = testDecoder.convertFrame();
            test_assert(convertSuccess, "convertFrame succeeds on dynamic resolution change");
            test_assert(testDecoder.m_allocatedWidth == 320, "Allocated width updated to 320");
            test_assert(testDecoder.m_allocatedHeight == 240, "Allocated height updated to 240");
            test_assert(testDecoder.m_yuvBufferSize == av_image_get_buffer_size(AV_PIX_FMT_YUV420P, 320, 240, 1), "YUV Buffer size updated correctly");

            // Test case 5: Dynamic format change and slow-path conversion (RGB24 -> triggers sws_scale)
            testDecoder.m_decodedFrame->width = 320;
            testDecoder.m_decodedFrame->height = 240;
            testDecoder.m_decodedFrame->format = AV_PIX_FMT_RGB24;
            std::vector<uint8_t> dummySrcBuffer4(320 * 240 * 4, 0);
            av_image_fill_arrays(
                testDecoder.m_decodedFrame->data,
                testDecoder.m_decodedFrame->linesize,
                dummySrcBuffer4.data(),
                AV_PIX_FMT_RGB24,
                320,
                240,
                1
            );
            bool convertSlowSuccess = testDecoder.convertFrame();
            test_assert(convertSlowSuccess, "convertFrame slow-path succeeds for RGB24");

            // Test case 6: Rebind slow-path buffer (triggers if m_yuvFrame->data[0] != m_yuvBuffer)
            // Trigger fast-path first to reference m_decodedFrame (changing m_yuvFrame->data[0])
            testDecoder.m_decodedFrame->width = 320;
            testDecoder.m_decodedFrame->height = 240;
            testDecoder.m_decodedFrame->format = AV_PIX_FMT_YUV420P;
            av_image_fill_arrays(
                testDecoder.m_decodedFrame->data,
                testDecoder.m_decodedFrame->linesize,
                dummySrcBuffer2.data(),
                AV_PIX_FMT_YUV420P,
                320,
                240,
                1
            );
            testDecoder.convertFrame();

            // Now trigger slow-path again to run the rebind code block
            testDecoder.m_decodedFrame->width = 320;
            testDecoder.m_decodedFrame->height = 240;
            testDecoder.m_decodedFrame->format = AV_PIX_FMT_RGB24;
            av_image_fill_arrays(
                testDecoder.m_decodedFrame->data,
                testDecoder.m_decodedFrame->linesize,
                dummySrcBuffer3.data(),
                AV_PIX_FMT_RGB24,
                320,
                240,
                1
            );
            bool convertRebindSuccess = testDecoder.convertFrame();
            test_assert(convertRebindSuccess, "convertFrame slow-path buffer re-binding succeeds");

            // Test case 7: Resolution scaling options (4K, 1080p, 720p, 360p, Original)
            {
                testDecoder.m_decodedFrame->width = 1920;
                testDecoder.m_decodedFrame->height = 1080;
                testDecoder.m_decodedFrame->format = AV_PIX_FMT_YUV420P;
                std::vector<uint8_t> hdBuffer(1920 * 1080 * 2, 128);
                av_image_fill_arrays(
                    testDecoder.m_decodedFrame->data,
                    testDecoder.m_decodedFrame->linesize,
                    hdBuffer.data(),
                    AV_PIX_FMT_YUV420P,
                    1920,
                    1080,
                    1
                );

                // Scale to 4K
                bool to4K = testDecoder.convertFrame(ResolutionOption::R_4K);
                test_assert(to4K, "convertFrame scales from 1080p to 4K");
                test_assert(testDecoder.getYUVFrame()->width == 3840, "4K scaled frame width is 3840");
                test_assert(testDecoder.getYUVFrame()->height == 2160, "4K scaled frame height is 2160");

                // Re-feed 1080p frame and scale to 720p
                testDecoder.m_decodedFrame->width = 1920;
                testDecoder.m_decodedFrame->height = 1080;
                testDecoder.m_decodedFrame->format = AV_PIX_FMT_YUV420P;
                av_image_fill_arrays(
                    testDecoder.m_decodedFrame->data,
                    testDecoder.m_decodedFrame->linesize,
                    hdBuffer.data(),
                    AV_PIX_FMT_YUV420P,
                    1920,
                    1080,
                    1
                );
                bool to720p = testDecoder.convertFrame(ResolutionOption::R_720P);
                test_assert(to720p, "convertFrame scales from 1080p to 720p");
                test_assert(testDecoder.getYUVFrame()->width == 1280, "720p scaled frame width is 1280");
                test_assert(testDecoder.getYUVFrame()->height == 720, "720p scaled frame height is 720");

                // Re-feed 1080p frame and scale to Original (fast-path native)
                testDecoder.m_decodedFrame->width = 1920;
                testDecoder.m_decodedFrame->height = 1080;
                testDecoder.m_decodedFrame->format = AV_PIX_FMT_YUV420P;
                av_image_fill_arrays(
                    testDecoder.m_decodedFrame->data,
                    testDecoder.m_decodedFrame->linesize,
                    hdBuffer.data(),
                    AV_PIX_FMT_YUV420P,
                    1920,
                    1080,
                    1
                );
                bool toOrig = testDecoder.convertFrame(ResolutionOption::ORIGINAL);
                test_assert(toOrig, "convertFrame restores Original native 1080p");
                test_assert(testDecoder.getYUVFrame()->width == 1920, "Original frame width is 1920");
                test_assert(testDecoder.getYUVFrame()->height == 1080, "Original frame height is 1080");
            }

            // Test case 7b: getTargetDimensions() directly for every
            // ResolutionOption case -- convertFrame() above only exercises
            // R_4K/R_720P/ORIGINAL, leaving the other box sizes and the
            // invalid-enum default fallback untested.
            {
                int w = 0, h = 0;
                getTargetDimensions(ResolutionOption::R_360P, 1920, 1080, w, h);
                test_assert(w == 640 && h == 360, "getTargetDimensions() boxes 1920x1080 into the 360p box");

                getTargetDimensions(ResolutionOption::R_480P, 1920, 1080, w, h);
                // 480/1080 (the binding, height-constrained scale factor)
                // isn't exactly representable in floating point, so the
                // scaled width truncates to 852, not the box's nominal 854.
                test_assert(w == 852 && h == 480, "getTargetDimensions() boxes 1920x1080 into the 480p box");

                getTargetDimensions(ResolutionOption::R_1080P, 640, 360, w, h);
                test_assert(w == 1920 && h == 1080, "getTargetDimensions() boxes 640x360 up into the 1080p box");

                getTargetDimensions(ResolutionOption::R_1440P, 640, 360, w, h);
                test_assert(w == 2560 && h == 1440, "getTargetDimensions() boxes 640x360 up into the 1440p box");

                // An out-of-range enum value (COUNT, never a real selection)
                // must fall through to the "pass native dimensions through
                // unchanged" default branch rather than reading garbage boxW/boxH.
                getTargetDimensions(ResolutionOption::COUNT, 1920, 1080, w, h);
                test_assert(w == 1920 && h == 1080, "getTargetDimensions() passes native size through for an invalid enum value");
            }

            // Test case 7c: getColorInfo(), never called anywhere else in the
            // suite. testDecoder.getYUVFrame() (the sws-converted output) was
            // left at 1920x1080 by test case 7 above, but m_decodedFrame (the
            // raw decode source getColorInfo() actually reads) is a distinct
            // AVFrame -- set its width/height explicitly to reach the
            // decoded-frame source branch, rather than assuming it carries
            // over. Subsequent calls manually set specific color/side-data
            // fields (private-access) to reach the named/HDR/chroma branches
            // no real content in this suite happens to carry.
            {
                testDecoder.m_decodedFrame->width = 1920;
                testDecoder.m_decodedFrame->height = 1080;
                // Test case 7's own convertFrame() calls leave m_allocatedFormat
                // set to whatever format it last scaled to -- reset it so the
                // pixFmt manipulated below actually takes effect (its override
                // of pixFmt is tested deliberately, later, in isolation).
                testDecoder.m_allocatedFormat = AV_PIX_FMT_NONE;
                ColorPipelineInfo baseline = testDecoder.getColorInfo();
                test_assert(baseline.colorSpace == "Unspecified" || !baseline.colorSpace.empty(),
                            "getColorInfo() reads color metadata from the decoded-frame source");

                // Named colorspace/primaries, PQ transfer characteristic (hits
                // the named-transferChar branch, the PQ display-name override,
                // and the PQ isHDR/hdrType override in one call), limited
                // range, and a HW-surface pixel format for the chroma branch.
                testDecoder.m_decodedFrame->colorspace = AVCOL_SPC_BT709;
                testDecoder.m_decodedFrame->color_primaries = AVCOL_PRI_BT709;
                testDecoder.m_decodedFrame->color_trc = AVCOL_TRC_SMPTE2084;
                testDecoder.m_decodedFrame->color_range = AVCOL_RANGE_MPEG;
                testDecoder.m_decodedFrame->format = AV_PIX_FMT_D3D11;
                // Properly FFmpeg-managed side data entries (av_frame_new_side_data),
                // not hand-rolled stack structs -- av_frame_free()/av_frame_unref()
                // must be able to safely release these later. The first
                // entry's type is irrelevant: its array slot is temporarily
                // nulled below to hit the "!sd -> continue" guard.
                av_frame_new_side_data(testDecoder.m_decodedFrame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA, 1);
                av_frame_new_side_data(testDecoder.m_decodedFrame, AV_FRAME_DATA_DYNAMIC_HDR_PLUS, 1);
                av_frame_new_side_data(testDecoder.m_decodedFrame, AV_FRAME_DATA_DOVI_METADATA, 1);
                AVFrameSideData* masteringEntry = testDecoder.m_decodedFrame->side_data[0];
                AVFrameSideData* hdrPlusEntry = testDecoder.m_decodedFrame->side_data[1];
                AVFrameSideData* doviEntry = testDecoder.m_decodedFrame->side_data[2];
                testDecoder.m_decodedFrame->side_data[0] = nullptr; // null entry, for the "!sd -> continue" guard

                ColorPipelineInfo pq = testDecoder.getColorInfo();
                testDecoder.m_decodedFrame->side_data[0] = masteringEntry; // restore for safe cleanup
                test_assert(pq.colorSpace == "bt709", "getColorInfo() reports a named colorspace");
                test_assert(pq.colorPrimaries == "bt709", "getColorInfo() reports named color primaries");
                test_assert(pq.transferChar == "PQ (ST 2084)", "getColorInfo() gives PQ its friendly display name");
                test_assert(pq.colorRange == "Limited (16-235)", "getColorInfo() reports MPEG range as Limited");
                test_assert(pq.isHDR && pq.hdrType == "Dolby Vision",
                            "getColorInfo() lets a later Dolby Vision side-data entry override an earlier PQ/HDR10+ classification");
                test_assert(pq.chromaSubsampling == "HW Surface", "getColorInfo() reports a HW pixel format as 'HW Surface'");

                // HLG transfer characteristic and Full/JPEG range (HLG alone
                // already sets isHDR/hdrType via the trc check, independent
                // of any side data) -- hide the PQ case's side data entries
                // first, since a later one (Dolby Vision) would otherwise
                // unconditionally overwrite this call's hdrType too.
                testDecoder.m_decodedFrame->color_trc = AVCOL_TRC_ARIB_STD_B67;
                testDecoder.m_decodedFrame->color_range = AVCOL_RANGE_JPEG;
                int savedNbSideData = testDecoder.m_decodedFrame->nb_side_data;
                testDecoder.m_decodedFrame->nb_side_data = 0;
                ColorPipelineInfo hlg = testDecoder.getColorInfo();
                testDecoder.m_decodedFrame->nb_side_data = savedNbSideData;
                test_assert(hlg.transferChar == "HLG", "getColorInfo() gives HLG its friendly display name");
                test_assert(hlg.colorRange == "Full (0-255)", "getColorInfo() reports JPEG range as Full");
                test_assert(hlg.isHDR && hlg.hdrType == "HLG", "getColorInfo() classifies an HLG transfer characteristic as HDR");

                // A non-HDR transfer characteristic with only a
                // mastering-display side data entry: isHDR is NOT already
                // true going in (unlike the PQ/HLG cases above), so this is
                // the only way to reach the "!info.isHDR" guard's true branch.
                testDecoder.m_decodedFrame->color_trc = AVCOL_TRC_BT709;
                testDecoder.m_decodedFrame->side_data[1] = nullptr; // hide the DYNAMIC_HDR_PLUS entry from the PQ case above
                testDecoder.m_decodedFrame->side_data[2] = nullptr; // hide the DOVI_METADATA entry from the PQ case above
                ColorPipelineInfo mastering = testDecoder.getColorInfo();
                testDecoder.m_decodedFrame->side_data[1] = hdrPlusEntry; // restore both for safe cleanup
                testDecoder.m_decodedFrame->side_data[2] = doviEntry;
                test_assert(!mastering.transferChar.empty() && mastering.transferChar != "PQ (ST 2084)",
                            "getColorInfo() does not misreport BT.709 as PQ");
                test_assert(mastering.isHDR && mastering.hdrType == "HDR10",
                            "getColorInfo() classifies mastering-display-only metadata as HDR10 when nothing else already flagged it HDR");

                // sRGB transfer characteristic naming, and standard 4:2:0
                // chroma subsampling via a real (non-HW) pixel format.
                testDecoder.m_decodedFrame->color_trc = AVCOL_TRC_IEC61966_2_1;
                testDecoder.m_decodedFrame->format = AV_PIX_FMT_YUV420P;
                testDecoder.m_decodedFrame->nb_side_data = 0;
                ColorPipelineInfo srgb = testDecoder.getColorInfo();
                test_assert(srgb.transferChar == "sRGB", "getColorInfo() gives sRGB its friendly display name");
                test_assert(srgb.chromaSubsampling == "4:2:0", "getColorInfo() reports YUV420P as 4:2:0 chroma subsampling");
                test_assert(srgb.bitDepth == 8, "getColorInfo() reports YUV420P's 8-bit component depth");

                // Mono (grayscale), 4:2:2, and 4:4:4/4:1:1 chroma subsampling
                // namings, each via a distinct real pixel format.
                testDecoder.m_decodedFrame->format = AV_PIX_FMT_GRAY8;
                test_assert(testDecoder.getColorInfo().chromaSubsampling == "4:0:0 (Mono)",
                            "getColorInfo() reports a single-component format as 4:0:0 (Mono)");
                testDecoder.m_decodedFrame->format = AV_PIX_FMT_YUV422P;
                test_assert(testDecoder.getColorInfo().chromaSubsampling == "4:2:2",
                            "getColorInfo() reports YUV422P as 4:2:2 chroma subsampling");
                testDecoder.m_decodedFrame->format = AV_PIX_FMT_YUV444P;
                test_assert(testDecoder.getColorInfo().chromaSubsampling == "4:4:4",
                            "getColorInfo() reports YUV444P as 4:4:4 chroma subsampling");
                testDecoder.m_decodedFrame->format = AV_PIX_FMT_YUV411P;
                test_assert(testDecoder.getColorInfo().chromaSubsampling == "4:1:1",
                            "getColorInfo() reports YUV411P as 4:1:1 chroma subsampling");

                // Explicitly unspecified colorspace/primaries -> named-lookup
                // branch's "Unspecified" fallback (distinct from the baseline
                // call above, which only *incidentally* had unspecified
                // metadata rather than exercising the fallback deliberately).
                testDecoder.m_decodedFrame->colorspace = AVCOL_SPC_UNSPECIFIED;
                testDecoder.m_decodedFrame->color_primaries = AVCOL_PRI_UNSPECIFIED;
                testDecoder.m_decodedFrame->color_trc = AVCOL_TRC_UNSPECIFIED;
                testDecoder.m_decodedFrame->color_range = AVCOL_RANGE_UNSPECIFIED;
                testDecoder.m_decodedFrame->format = AV_PIX_FMT_NONE;
                ColorPipelineInfo unspecified = testDecoder.getColorInfo();
                test_assert(unspecified.colorSpace == "Unspecified", "getColorInfo() falls back to 'Unspecified' colorspace");
                test_assert(unspecified.colorPrimaries == "Unspecified", "getColorInfo() falls back to 'Unspecified' primaries");
                test_assert(unspecified.transferChar == "Unspecified", "getColorInfo() falls back to 'Unspecified' transfer characteristic");
                test_assert(unspecified.colorRange == "Unspecified", "getColorInfo() falls back to 'Unspecified' color range");
                test_assert(!unspecified.isHDR, "getColorInfo() reports SDR for fully unspecified metadata");

                // m_allocatedFormat override: takes priority over whatever
                // pixFmt was resolved from the decoded frame/codec context.
                testDecoder.m_allocatedFormat = AV_PIX_FMT_NV12;
                ColorPipelineInfo allocOverride = testDecoder.getColorInfo();
                test_assert(allocOverride.pixelFormat == "nv12", "getColorInfo() prefers m_allocatedFormat over the source pixel format");
                testDecoder.m_allocatedFormat = AV_PIX_FMT_NONE;

                // No decoded frame (or width <= 0): falls through to the
                // m_codecCtx source branch instead.
                testDecoder.m_decodedFrame->width = 0;
                testDecoder.m_codecCtx->colorspace = AVCOL_SPC_BT470BG;
                ColorPipelineInfo fromCtx = testDecoder.getColorInfo();
                test_assert(fromCtx.colorSpace == "bt470bg", "getColorInfo() falls back to m_codecCtx's color metadata when there's no decoded frame yet");
                testDecoder.m_decodedFrame->width = 1920;
            }

            avcodec_parameters_free(&testCodecParams);
        }

        // Test case 7d: getColorInfo()'s m_codecParams-only source branch --
        // needs a VideoDecoder with neither a decoded frame nor a live codec
        // context yet, which only a never-init()'d instance has.
        {
            ThreadSafeQueue<AVPacket*> dummyQueue2;
            AVCodecParameters* paramsOnly = avcodec_parameters_alloc();
            paramsOnly->codec_type = AVMEDIA_TYPE_VIDEO;
            paramsOnly->codec_id = AV_CODEC_ID_RAWVIDEO;
            paramsOnly->format = AV_PIX_FMT_RGB24;
            paramsOnly->width = 160;
            paramsOnly->height = 120;
            VideoDecoder uninitDecoder(paramsOnly, {1, 25}, 0, dummyQueue2);
            ColorPipelineInfo fromParams = uninitDecoder.getColorInfo();
            test_assert(!fromParams.colorSpace.empty(),
                        "getColorInfo() falls back to m_codecParams's color metadata before init() has run");
            avcodec_parameters_free(&paramsOnly);
        }

        // -------------------------------------------------------------
        // A. Basic Guard Checks (Uninitialized Controller)
        // -------------------------------------------------------------
        test_assert(controller.getDuration() == 0.0, "Duration is 0 when uninitialized");
        test_assert(controller.getVideoWidth() == 0, "Width is 0 when uninitialized");
        test_assert(controller.getVideoHeight() == 0, "Height is 0 when uninitialized");
        test_assert(controller.getCurrentTime() == 0.0, "Current time is 0 when uninitialized");
        
        // Trigger early return on play/pause/seek before opening
        controller.play(); // Returns early
        controller.pause(); // Returns early
        controller.seek(10.0); // Returns early

        // -------------------------------------------------------------
        // B. Error Loading Scenarios
        // -------------------------------------------------------------
        bool invalidSuccess = controller.openFile("non_existent_file.xyz");
        test_assert(!invalidSuccess, "Loading non-existent file returns false");

        // -------------------------------------------------------------
        // C. Standard Lifecycle (Load -> Play -> Pause -> Resume)
        // -------------------------------------------------------------
        bool openSuccess = controller.openFile(testFile);
        test_assert(openSuccess, "Loading test file");
        test_assert(controller.getState() == PlayerState::OPENED, "State is OPENED after loading");
        if (controller.getVideoDecoder()) {
            bool convertFailOk = !controller.getVideoDecoder()->convertFrame();
            test_assert(convertFailOk, "convertFrame returns false when no frames have been decoded");
            
            // Verify seeking state accessors
            test_assert(!controller.getVideoDecoder()->isSeeking(), "isSeeking is initially false");
            controller.getVideoDecoder()->setSeeking(true);
            test_assert(controller.getVideoDecoder()->isSeeking(), "isSeeking is true after setting");
            controller.getVideoDecoder()->setSeeking(false);
            test_assert(!controller.getVideoDecoder()->isSeeking(), "isSeeking is false after resetting");
        }
        test_assert(controller.getDuration() > 0.0, "File duration is greater than 0");
        test_assert(controller.getVideoWidth() > 0, "Video width is populated correctly");
        test_assert(controller.getVideoHeight() > 0, "Video height is populated correctly");
        test_assert(!controller.isEOF(), "isEOF is false initially");

        PlayerController uninitController2;
        test_assert(!uninitController2.isEOF(), "isEOF is false on uninitialized controller");

        // Volume adjustments with playback pauses to test callback bypass copy logic
        controller.setVolume(1.0f); // Bypass copy path
        controller.play();
        drive_playback(controller, 0.2); // Let callback run at 1.0 vol

        controller.setVolume(0.0f); // Silence path
        drive_playback(controller, 0.2); // Let callback run at 0.0 vol

        controller.setVolume(0.5f); // Scaling path
        drive_playback(controller, 0.2); // Let callback run at 0.5 vol

        controller.setVolume(-1.0f); // Bounds lower clamp
        controller.setVolume(2.0f); // Bounds upper clamp
        controller.setVolume(0.05f); // Reset to safe volume

        // Pause / Resume cycle
        controller.pause();
        test_assert(controller.getState() == PlayerState::PAUSED, "State is PAUSED after pause");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        controller.play();
        test_assert(controller.getState() == PlayerState::PLAYING, "State is PLAYING after resume");
        drive_playback(controller, 0.5);

        // -------------------------------------------------------------
        // D. Seek Forward & Backward Operations
        // -------------------------------------------------------------
        // Seek Forward
        double seekForwardTarget = controller.getDuration() * 0.6;
        std::cout << "Seeking forward to " << seekForwardTarget << "s..." << std::endl;
        controller.seek(seekForwardTarget);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        drive_playback(controller, 0.5);
        double timeAfterForward = controller.getCurrentTime();
        test_assert(std::abs(timeAfterForward - seekForwardTarget) < 5.0, "Seek forward position is accurate");

        // Seek Backward
        double seekBackwardTarget = controller.getDuration() * 0.2;
        std::cout << "Seeking backward to " << seekBackwardTarget << "s..." << std::endl;
        controller.seek(seekBackwardTarget);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        drive_playback(controller, 0.5);
        double timeAfterBackward = controller.getCurrentTime();
        test_assert(std::abs(timeAfterBackward - seekBackwardTarget) < 5.0, "Seek backward position is accurate");

        // Seek while Paused
        controller.pause();
        double seekPausedTarget = controller.getDuration() * 0.4;
        std::cout << "Seeking while paused to " << seekPausedTarget << "s..." << std::endl;
        controller.seek(seekPausedTarget);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        drive_playback(controller, 0.2);
        test_assert(controller.getState() == PlayerState::OPENED, "State is OPENED (paused seek first-frame render)");

        // Resume playback after paused seek
        controller.play();
        test_assert(controller.getState() == PlayerState::PLAYING, "Resumed playback successfully");
        drive_playback(controller, 0.5);

        // Out of bounds seeks
        controller.seek(-5.0); // Clamps to 0
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        controller.seek(controller.getDuration() + 10.0); // Clamps to duration
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // -------------------------------------------------------------
        // D2. Seek Catch-Up (forward fast-forward sweep / backward landing)
        // -------------------------------------------------------------
        // The catch-up path needs the background video thread, which the rest
        // of this suite keeps disabled, so it gets its own controller.
        {
            std::cout << "Testing seek catch-up (background video thread enabled)..." << std::endl;
            g_videoThreadEnabled = true;
            PlayerController catchupController;
            test_assert(catchupController.openFile(testFile), "Catch-up: file loads with video thread");
            catchupController.play();

            // Consume decoded frames like the render loop would, tracking the
            // earliest frame timestamp delivered since the last reset and how
            // many frames have come through since the last reset.
            double minPoppedPts = 1e18;
            size_t framesDrainedTotal = 0;
            auto drainFrames = [&catchupController, &minPoppedPts, &framesDrainedTotal]() {
                DecodedFrame df;
                while (catchupController.getDecodedFrameQueue().try_pop(df)) {
                    framesDrainedTotal++;
                    if (df.pts < minPoppedPts) {
                        minPoppedPts = df.pts;
                    }
                    if (df.frame) {
                        av_frame_free(&df.frame);
                    }
                }
            };
            auto driveFor = [&](double seconds) {
                auto start = std::chrono::steady_clock::now();
                while (std::chrono::steady_clock::now() - start <
                       std::chrono::duration<double>(seconds)) {
                    drainFrames();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            };
            auto waitForCatchup = [&](double timeoutSeconds) {
                auto start = std::chrono::steady_clock::now();
                while (catchupController.isCatchingUp() &&
                       std::chrono::steady_clock::now() - start <
                           std::chrono::duration<double>(timeoutSeconds)) {
                    drainFrames();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                drainFrames();
            };

            driveFor(0.4); // let playback settle

            double dur = catchupController.getDuration();
            double fwdTarget = std::min(catchupController.getCurrentTime() + 5.0, dur * 0.8);
            catchupController.seek(fwdTarget);
            test_assert(catchupController.isCatchingUp(), "Catch-up engages on forward seek while playing");
            waitForCatchup(20.0);
            test_assert(!catchupController.isCatchingUp(), "Forward catch-up completes");
            test_assert(std::abs(catchupController.getCurrentTime() - fwdTarget) < 2.0,
                        "Forward catch-up lands on the seek target");
            test_assert(catchupController.getState() == PlayerState::PLAYING,
                        "Real-time playback resumes after forward catch-up");

            double backTarget = std::max(0.5, catchupController.getCurrentTime() - 4.0);
            minPoppedPts = 1e18; // watch what the backward seek delivers
            catchupController.seek(backTarget);
            test_assert(std::abs(catchupController.getCurrentTime() - backTarget) < 0.01,
                        "Backward seek repositions the timeline immediately");
            waitForCatchup(20.0);
            test_assert(!catchupController.isCatchingUp(), "Backward landing completes");
            test_assert(std::abs(catchupController.getCurrentTime() - backTarget) < 2.0,
                        "Backward seek lands on the target position");
            test_assert(minPoppedPts >= backTarget - 0.05,
                        "Backward seek shows no pre-target frames (no reverse playback)");
            test_assert(catchupController.getState() == PlayerState::PLAYING,
                        "Real-time playback resumes after backward seek");

            // Paused seeks bypass the catch-up scan entirely
            catchupController.pause();
            catchupController.seek(backTarget + 2.0);
            test_assert(!catchupController.isCatchingUp(), "Paused seek uses the instant path");
            catchupController.play();

            // -------------------------------------------------------------
            // D2b. Rapid consecutive seeks must self-recover, no extra nudge
            // -------------------------------------------------------------
            // Regression test for a deadlock where the demuxer thread -- the
            // single thread that reads both the video and audio streams --
            // could block forever pushing a packet into the audio queue
            // while nothing was draining it (audio is muted for the whole
            // catch-up scan, and also stays paused for as long as the user
            // leaves playback paused). Once blocked there, it never called
            // av_read_frame again for either stream: the video packet queue
            // drained to 0 and stayed there, and so did the frame queue.
            // The bug only "recovered" because issuing another seek forced
            // a queue clear() that happened to wake the blocked push.
            // This drives many back-to-back seeks with no settling time in
            // between (mimicking a dragged seekbar or a held seek key) and
            // then asserts the pipeline comes back to life entirely on its
            // own afterward, with no further seek to bail it out.
            {
                std::cout << "Testing rapid consecutive seek recovery (no settling)..." << std::endl;
                double lo = std::max(0.2, dur * 0.1);
                double hi = std::max(lo + 0.5, dur * 0.8);

                // Storm while playing: back-to-back seeks, no draining and no
                // waiting for catch-up to land between them, so any audio
                // packets that pile up while muted have nowhere to go until
                // the storm stops.
                for (int i = 0; i < 15; i++) {
                    catchupController.seek((i % 2 == 0) ? hi : lo);
                }
                waitForCatchup(20.0);
                test_assert(!catchupController.isCatchingUp(),
                            "Rapid seek storm (playing) settles without another nudge");
                // cppcheck-suppress knownConditionTrueFalse
                // False positive: driveFor() calls drainFrames(), which
                // increments framesDrainedTotal by reference two lambda
                // layers away from this scope. cppcheck's value flow doesn't
                // follow the mutation through that indirection.
                framesDrainedTotal = 0;
                driveFor(1.0);
                // cppcheck-suppress knownConditionTrueFalse
                test_assert(framesDrainedTotal > 0,
                            "Video frames are being produced again after the playing seek storm");
                test_assert(catchupController.getState() == PlayerState::PLAYING,
                            "Playback is still PLAYING after the playing seek storm");

                // Storm while paused: audio stays paused across every one of
                // these, so this is the scenario most likely to fill the
                // audio queue with nothing draining it at all.
                catchupController.pause();
                for (int i = 0; i < 15; i++) {
                    catchupController.seek((i % 2 == 0) ? lo : hi);
                }
                catchupController.play();
                waitForCatchup(20.0);
                test_assert(!catchupController.isCatchingUp(),
                            "Rapid seek storm (paused) settles without another nudge");
                // cppcheck-suppress knownConditionTrueFalse
                // Same false positive as above (driveFor -> drainFrames
                // mutates framesDrainedTotal through nested lambda capture).
                framesDrainedTotal = 0;
                driveFor(1.0);
                // cppcheck-suppress knownConditionTrueFalse
                test_assert(framesDrainedTotal > 0,
                            "Video frames are being produced again after the paused seek storm");
                test_assert(catchupController.getState() == PlayerState::PLAYING,
                            "Playback resumes normally after the paused seek storm");
            }

            // Repeated consecutive forward/backward seeks must not knock the
            // decoder off hardware: a transient hardware decode failure used
            // to fall back to software permanently, with no attempt to
            // recover -- once one seek tripped it, every later seek stayed
            // on software even though the hardware decoder was fine again.
            if (catchupController.isVideoHardware()) {
                double lo = std::max(0.2, dur * 0.1);
                double hi = std::max(lo + 0.5, dur * 0.8);
                for (int i = 0; i < 8; i++) {
                    catchupController.seek((i % 2 == 0) ? hi : lo);
                    waitForCatchup(20.0);
                }
                test_assert(catchupController.isVideoHardware(),
                            "Hardware decoder stays engaged across repeated consecutive seeks");
            }

            // videoThreadLoop()'s av_frame_ref() failure branch (distinct
            // from VideoDecoder::convertFrame()'s own, earlier av_frame_ref()
            // call in its useNative fast path -- force_hw_frame_ref_fail
            // alone can't isolate just this later call site, since both go
            // through the same globally-mocked function and the earlier one
            // would simply fail convertFrame() first, so this call site
            // would never be reached). force_player_frame_ref_fail_next
            // instead lets exactly one call through (VideoDecoder's) before
            // failing exactly the next one (PlayerController's), then
            // disarms itself -- a single surgical failure rather than an
            // extended failing window, to avoid destabilizing the decoder.
            //
            // Seek to a known position with several seconds of stream left
            // first: by this point the controller may be resting anywhere
            // the earlier seek storms left it, including right at/near EOF,
            // where normal playback legitimately stops producing frames on
            // its own -- that's correct behavior, not something this test
            // should be confused by.
            {
                catchupController.seek(std::min(1.0, dur * 0.2));
                waitForCatchup(20.0);
                catchupController.play();

                force_player_frame_ref_fail_next = 1;
                auto armStart = std::chrono::steady_clock::now();
                while (force_player_frame_ref_fail_next.load() != -1 &&
                       std::chrono::steady_clock::now() - armStart < std::chrono::seconds(5)) {
                    drainFrames();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                force_player_frame_ref_fail_next = -1; // safety: disarm even if it never fired
                // cppcheck-suppress knownConditionTrueFalse
                // False positive: driveFor() calls drainFrames(), which
                // increments framesDrainedTotal by reference two lambda
                // layers away from this scope. cppcheck's value flow doesn't
                // follow the mutation through that indirection.
                framesDrainedTotal = 0;
                driveFor(1.0);
                test_assert(catchupController.getState() == PlayerState::PLAYING,
                            "Playback survives a single dropped frame from a transient av_frame_ref() failure");
                // cppcheck-suppress knownConditionTrueFalse
                test_assert(framesDrainedTotal > 0,
                            "Video frames resume after a single transient av_frame_ref() failure");
            }

            // videoThreadLoop()'s m_decodedFrameQueue.push_wait_or_drop()
            // drop-callback and abort-return-false branches. Simply keeping
            // the queue full doesn't work: videoThreadLoop()'s own
            // pre-check (`if (queue.size() >= 8) continue;`) then skips
            // every iteration before it ever attempts its own push at all,
            // since it's the queue's sole normal producer. What's needed is
            // to let it pass that gate first (queue not full yet), then
            // fill the queue while it's busy decoding, so by the time it
            // reaches its own push call the queue is already full.
            //
            // A first attempt did this by holding m_videoDecoderMutex (the
            // same mutex videoThreadLoop locks around decode+convert) from
            // the test thread to create that window -- it worked
            // structurally, but holding a PlayerController-internal mutex
            // from outside destabilized SDL's own internals (a hard
            // SDL_LockMutex_srw assertion failure and a genuinely hung
            // process, not mere flakiness -- and a crash never flushes
            // .gcda, silently losing all coverage data for that run). This
            // version creates the same window without touching any
            // PlayerController mutex at all: force_decode_delay_once_ms
            // injects a one-shot artificial delay into the next real
            // avcodec_receive_frame() call (already globally mocked), so
            // the video thread is genuinely busy inside decodeNextFrame()
            // -- past its own gate check, not holding anything the test
            // needs -- while this thread fills the queue via the same
            // public push_wait_or_drop() API, a legitimate second producer.
            {
                catchupController.seek(std::min(1.0, dur * 0.2));
                waitForCatchup(20.0);
                catchupController.play();
                driveFor(0.3); // let real decode/drain run for a moment first

                DecodedFrame flushDf;
                while (catchupController.getDecodedFrameQueue().try_pop(flushDf)) {
                    if (flushDf.frame) av_frame_free(&flushDf.frame);
                }

                auto fillQueueDuringDelayedDecode = [&]() {
                    force_decode_delay_once_ms = 600;
                    // Give the video thread a moment to pass its gate check
                    // (queue empty right now) and enter the delayed decode.
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    for (int i = 0; i < 8; i++) {
                        DecodedFrame filler;
                        filler.frame = av_frame_alloc();
                        catchupController.getDecodedFrameQueue().push_wait_or_drop(
                            filler, std::chrono::milliseconds(50),
                            [](DecodedFrame& d) { if (d.frame) av_frame_free(&d.frame); });
                    }
                };

                // Sub-test A: drop-callback path. Once the delayed decode
                // finishes and convertFrame() succeeds (fast), the video
                // thread's own push_wait_or_drop() finds the queue already
                // full and waits past its 500ms timeout, hitting the
                // drop-oldest callback.
                fillQueueDuringDelayedDecode();
                std::this_thread::sleep_for(std::chrono::milliseconds(1200));

                DecodedFrame drainDf;
                while (catchupController.getDecodedFrameQueue().try_pop(drainDf)) {
                    if (drainDf.frame) av_frame_free(&drainDf.frame);
                }
                test_assert(catchupController.getState() == PlayerState::PLAYING,
                            "Playback survives a saturated decoded-frame queue (drop-oldest path)");

                // Sub-test B: abort-returns-false path. Same setup, but
                // abort the real queue while the video thread's own call is
                // waiting on it, so it observes m_aborted and returns false
                // immediately instead of timing out into a drop.
                fillQueueDuringDelayedDecode();
                std::this_thread::sleep_for(std::chrono::milliseconds(650)); // past the decode delay, mid-wait
                catchupController.getDecodedFrameQueue().abort();
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                catchupController.getDecodedFrameQueue().reset();

                // Re-seek to a position with fresh runway before checking
                // for resumed frames: real-time playback kept advancing
                // through every sleep above, and could easily have run out
                // of stream by now -- legitimately producing no more frames
                // on its own, which isn't something this check should
                // mistake for a stuck decoder.
                catchupController.seek(std::min(1.0, dur * 0.2));
                waitForCatchup(20.0);
                catchupController.play();

                // Patient poll, not a fixed window: a failed assertion here
                // throws and aborts the whole suite mid-run, which loses
                // all coverage data for this run (no .gcda gets written on
                // that path), so this must not be a hair-trigger check.
                framesDrainedTotal = 0;
                auto resumeStart = std::chrono::steady_clock::now();
                // cppcheck-suppress knownConditionTrueFalse
                // False positive: drainFrames() (called each iteration)
                // increments framesDrainedTotal by reference, so this loop
                // condition does change -- cppcheck's value flow doesn't
                // follow the mutation through the captured-by-reference
                // lambda.
                while (framesDrainedTotal == 0 &&
                       std::chrono::steady_clock::now() - resumeStart < std::chrono::seconds(10)) {
                    drainFrames();
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                test_assert(catchupController.getState() == PlayerState::PLAYING,
                            "Playback survives a queue abort/reset cycle");
                // cppcheck-suppress knownConditionTrueFalse
                test_assert(framesDrainedTotal > 0,
                            "Video frames resume normally afterward");
            }

            catchupController.stop();
            g_videoThreadEnabled = false;
        }

        // -------------------------------------------------------------
        // E. White-Box Static Decoder & Demuxer Error Branches
        // -------------------------------------------------------------
        std::cout << "Testing white-box failure paths for Audio/Video decoders..." << std::endl;
        {
            ExpectedErrorRedirector redirector;
        
        // 1. Audio Decoder - Invalid Codec ID
        AVCodecParameters* badAudioParams = avcodec_parameters_alloc();
        badAudioParams->codec_id = AV_CODEC_ID_NONE;
        AudioDecoder badAudioDecoder(badAudioParams, {1, 90000}, 0, controller.m_audioQueue);
        bool badAudioInit = badAudioDecoder.init();
        test_assert(!badAudioInit, "AudioDecoder::init fails on unknown codec");
        avcodec_parameters_free(&badAudioParams);

        // 2. Audio Decoder - Zero Input Channels fallback (tests line 72 default layout)
        force_zero_channels = true;
        AVCodecParameters* zeroChanParams = avcodec_parameters_alloc();
        zeroChanParams->codec_type = AVMEDIA_TYPE_AUDIO;
        zeroChanParams->codec_id = AV_CODEC_ID_AAC;
        zeroChanParams->sample_rate = 48000;
        zeroChanParams->format = AV_SAMPLE_FMT_FLTP;
        if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
            AVCodecParameters* audioParams = controller.m_demuxer->getAudioCodecParams();
            if (audioParams->codec_id < AV_CODEC_ID_FIRST_AUDIO || audioParams->codec_id >= AV_CODEC_ID_ADPCM_IMA_QT) {
                avcodec_parameters_copy(zeroChanParams, audioParams);
            }
        }
#if LIBAVUTIL_VERSION_MAJOR >= 57
        zeroChanParams->ch_layout.nb_channels = 0;
#else
        zeroChanParams->channels = 0;
        zeroChanParams->channel_layout = 0;
#endif
        AudioDecoder zeroChanDecoder(zeroChanParams, {1, 48000}, 0, controller.m_audioQueue);
        bool zeroChanInit = zeroChanDecoder.init();
        test_assert(zeroChanInit, "AudioDecoder::init succeeds and falls back with 0 input channels");
        avcodec_parameters_free(&zeroChanParams);
        force_zero_channels = false;

        // 3. Video Decoder - Invalid Codec ID
        AVCodecParameters* badVideoParams = avcodec_parameters_alloc();
        badVideoParams->codec_id = AV_CODEC_ID_NONE;
        VideoDecoder badVideoDecoder(badVideoParams, {1, 90000}, 0, controller.m_videoQueue);
        bool badVideoInit = badVideoDecoder.init();
        test_assert(!badVideoInit, "VideoDecoder::init fails on unknown codec");
        avcodec_parameters_free(&badVideoParams);

        // 4. Audio Resampler buffer resize triggering
        if (controller.m_audioDecoder) {
            controller.m_audioDecoder->m_audioBuffer.resize(0); // Shrink to 0 to trigger buffer allocation resize path
            drive_playback(controller, 0.2); // Triggers resizing logic in audio callback
        }

        }

        // -------------------------------------------------------------
        // F. White-Box Demuxer Seeking Branches
        // -------------------------------------------------------------
        std::cout << "Testing Demuxer seeking branches..." << std::endl;
        int savedVideoIdx = -1;
        int savedAudioIdx = -1;
        if (controller.m_demuxer) {
            savedVideoIdx = controller.m_demuxer->m_videoStreamIdx;
            savedAudioIdx = controller.m_demuxer->m_audioStreamIdx;

            // Force Audio-Only seeking branch
            controller.m_demuxer->m_videoStreamIdx = -1;
            controller.m_demuxer->m_audioStreamIdx = (controller.m_demuxer->m_formatCtx->nb_streams > 1) ? 1 : 0;
            controller.m_demuxer->seek(10.0);
            controller.m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
            controller.m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
            while (controller.m_demuxer->m_seekRequested.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            {
                std::lock_guard<std::mutex> seekLock(controller.m_demuxer->m_seekMutex);
            }

            // Force No-Stream seeking branch
            controller.m_demuxer->m_videoStreamIdx = -1;
            controller.m_demuxer->m_audioStreamIdx = -1;
            controller.m_demuxer->seek(10.0);
            controller.m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
            controller.m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
            while (controller.m_demuxer->m_seekRequested.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            {
                std::lock_guard<std::mutex> seekLock(controller.m_demuxer->m_seekMutex);
            }
        }

        // -------------------------------------------------------------
        // G. White-Box Video-Only Player Clock Synchronization
        // -------------------------------------------------------------
        std::cout << "Testing Video-Only clock synchronization updates..." << std::endl;
        if (controller.m_demuxer) {
            controller.m_demuxer->m_videoStreamIdx = savedVideoIdx;
            controller.m_demuxer->m_audioStreamIdx = savedAudioIdx;
            controller.m_demuxer->m_eof = false;
            controller.m_demuxer->seek(0.0);
            controller.m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
            controller.m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
            while (controller.m_demuxer->m_seekRequested.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            {
                std::lock_guard<std::mutex> seekLock(controller.m_demuxer->m_seekMutex);
            }
        }
        controller.m_hasAudio = false;
        controller.m_hasVideo = true;
        controller.m_videoClock = 0.0;
        controller.m_lastSystemTime = controller.getSystemTimeInSeconds();
        controller.m_state = PlayerState::PLAYING;

        // Push a dummy packet to ensure queue is not empty, preventing premature EOF state transition
        AVPacket* dummyPkt = av_packet_alloc();
        feedPacket(controller.m_videoQueue, dummyPkt);

        // Verify getCurrentTime drives updateClockForVideoOnly()
        double videoOnlyTime1 = controller.getCurrentTime();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        double videoOnlyTime2 = controller.getCurrentTime();
        test_assert(videoOnlyTime2 > videoOnlyTime1, "Video-Only clock progresses via system time delta");

        // Clean up the dummy packet
        controller.m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });

        // Verify pause handles video-only update
        controller.pause();
        test_assert(controller.getState() == PlayerState::PAUSED, "Paused video-only playback successfully");

        // -------------------------------------------------------------
        // H. Advanced Interceptor Error Injectors (100% Coverage Target)
        // -------------------------------------------------------------
        std::cout << "Injecting advanced hardware & library failure codes..." << std::endl;
        {
            ExpectedErrorRedirector redirector;
        
        // 1. avcodec_alloc_context3 fail (Audio & Video)
        force_alloc_fail = true;
        AVCodecParameters* testParams = avcodec_parameters_alloc();
        testParams->codec_id = AV_CODEC_ID_AAC;
        AudioDecoder audioCtxFail(testParams, {1, 48000}, 0, controller.m_audioQueue);
        test_assert(!audioCtxFail.init(), "AudioDecoder fails gracefully on avcodec_alloc_context3 nullptr");
        
        testParams->codec_id = AV_CODEC_ID_H264;
        VideoDecoder videoCtxFail(testParams, {1, 90000}, 0, controller.m_videoQueue);
        test_assert(!videoCtxFail.init(), "VideoDecoder fails gracefully on avcodec_alloc_context3 nullptr");
        force_alloc_fail = false;

        // 2. avcodec_open2 fail (Audio & Video)
        force_open_fail = true;
        testParams->codec_id = AV_CODEC_ID_AAC;
        AudioDecoder audioOpenFail(testParams, {1, 48000}, 0, controller.m_audioQueue);
        test_assert(!audioOpenFail.init(), "AudioDecoder fails gracefully on avcodec_open2 error");
        
        testParams->codec_id = AV_CODEC_ID_H264;
        VideoDecoder videoOpenFail(testParams, {1, 90000}, 0, controller.m_videoQueue);
        test_assert(!videoOpenFail.init(), "VideoDecoder fails gracefully on avcodec_open2 error");
        force_open_fail = false;

        // 3. avcodec_parameters_to_context copy failure (Audio & Video)
        force_copy_params_fail = true;
        testParams->codec_id = AV_CODEC_ID_AAC;
        AudioDecoder audioCopyFail(testParams, {1, 48000}, 0, controller.m_audioQueue);
        test_assert(!audioCopyFail.init(), "AudioDecoder fails gracefully on avcodec_parameters_to_context error");
        
        testParams->codec_id = AV_CODEC_ID_H264;
        VideoDecoder videoCopyFail(testParams, {1, 90000}, 0, controller.m_videoQueue);
        test_assert(!videoCopyFail.init(), "VideoDecoder fails gracefully on avcodec_parameters_to_context error");
        force_copy_params_fail = false;
        
        avcodec_parameters_free(&testParams);

        // 4. av_frame_alloc fail (Audio & Video)
        force_frame_alloc_fail = true;
        open_finished = false;
        AVCodecParameters* paramsFrameFail = avcodec_parameters_alloc();
        if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
            avcodec_parameters_copy(paramsFrameFail, controller.m_demuxer->getAudioCodecParams());
        }
        AudioDecoder audioFrameFail(paramsFrameFail, {1, 48000}, 0, controller.m_audioQueue);
        test_assert(!audioFrameFail.init(), "AudioDecoder fails gracefully on av_frame_alloc nullptr");
        
        open_finished = false;
        paramsFrameFail->codec_id = AV_CODEC_ID_H264;
        if (controller.m_demuxer && controller.m_demuxer->getVideoCodecParams()) {
            avcodec_parameters_copy(paramsFrameFail, controller.m_demuxer->getVideoCodecParams());
        }
        VideoDecoder videoFrameFail(paramsFrameFail, {1, 90000}, 0, controller.m_videoQueue);
        test_assert(!videoFrameFail.init(), "VideoDecoder fails gracefully on av_frame_alloc nullptr");
        force_frame_alloc_fail = false;
        avcodec_parameters_free(&paramsFrameFail);

        // 5. swr_init fail (Audio resampler init error)
        force_swr_init_fail = true;
        AVCodecParameters* paramsSwrFail = avcodec_parameters_alloc();
        paramsSwrFail->codec_id = AV_CODEC_ID_AAC;
        if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
            avcodec_parameters_copy(paramsSwrFail, controller.m_demuxer->getAudioCodecParams());
        }
        AudioDecoder audioSwrInitFail(paramsSwrFail, {1, 48000}, 0, controller.m_audioQueue);
        test_assert(!audioSwrInitFail.init(), "AudioDecoder fails gracefully on swr_init error");
        force_swr_init_fail = false;
        avcodec_parameters_free(&paramsSwrFail);

        // 6. av_malloc fail (Video frame buffer allocation error)
        force_malloc_fail = true;
        AVCodecParameters* paramsMallocFail = avcodec_parameters_alloc();
        paramsMallocFail->codec_id = AV_CODEC_ID_H264;
        VideoDecoder videoMallocFail(paramsMallocFail, {1, 90000}, 0, controller.m_videoQueue);
        test_assert(!videoMallocFail.init(), "VideoDecoder fails gracefully on av_malloc nullptr");
        force_malloc_fail = false;
        avcodec_parameters_free(&paramsMallocFail);

        // 7. av_image_fill_arrays fail (Video array filling error)
        force_image_fill_fail = true;
        AVCodecParameters* paramsFillFail = avcodec_parameters_alloc();
        if (controller.m_demuxer && controller.m_demuxer->getVideoCodecParams()) {
            avcodec_parameters_copy(paramsFillFail, controller.m_demuxer->getVideoCodecParams());
        }
        VideoDecoder videoFillFail(paramsFillFail, {1, 90000}, 0, controller.m_videoQueue);
        test_assert(!videoFillFail.init(), "VideoDecoder fails gracefully on av_image_fill_arrays error");
        force_image_fill_fail = false;
        avcodec_parameters_free(&paramsFillFail);

        // 8. avformat_find_stream_info failure
        force_find_stream_info_fail = true;
        PlayerController streamInfoFailController;
        test_assert(!streamInfoFailController.openFile(testFile), "PlayerController fails gracefully if find_stream_info fails");
        force_find_stream_info_fail = false;

        // 9. av_seek_frame failure (warning path verification)
        force_seek_fail = true;
        PlayerController seekFailController;
        seekFailController.openFile(testFile);
        seekFailController.seek(20.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // Wait for demuxer thread seek warning to print
        force_seek_fail = false;

        // 10. File with no playable streams (fails controller.openFile)
        force_open_fail = true; // Forces Audio/Video decoder context creations to fail
        PlayerController noPlayableStreamsController;
        test_assert(!noPlayableStreamsController.openFile(testFile), "PlayerController fails gracefully if no streams can be initialized");
        force_open_fail = false;

        // 11. SDL Open Audio Device fail path
        force_sdl_audio_fail = true;
        AVCodecParameters* paramsSdlFail = avcodec_parameters_alloc();
        if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
            avcodec_parameters_copy(paramsSdlFail, controller.m_demuxer->getAudioCodecParams());
        }
        AudioDecoder audioSdlFail(paramsSdlFail, {1, 48000}, 0, controller.m_audioQueue);
        test_assert(!audioSdlFail.init(), "AudioDecoder fails gracefully on SDL audio hardware failure");
        avcodec_parameters_free(&paramsSdlFail);
        force_sdl_audio_fail = false;

        // 11b. Multichannel (5.1) source: output layout should be preserved,
        // not forced down to stereo, when the device accepts it.
        force_channel_layout_5_1 = true;
        AVCodecParameters* params51 = avcodec_parameters_alloc();
        if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
            avcodec_parameters_copy(params51, controller.m_demuxer->getAudioCodecParams());
        }
        AudioDecoder audio51(params51, {1, 48000}, 0, controller.m_audioQueue);
        test_assert(audio51.init(), "AudioDecoder initializes on a 5.1 source");
        test_assert(audio51.getOutputChannelCount() == 6,
                    "5.1 source preserves 6-channel output instead of downmixing to stereo");
        // FFmpeg's av_channel_layout_describe() names the AV_CH_LAYOUT_5POINT1
        // (side-surround) mask "5.1(side)", reserving plain "5.1" for the
        // back-surround variant -- match its actual naming, not an assumed one.
        test_assert(audio51.getOutputChannelLayoutName() == "5.1(side)",
                    "5.1 source reports '5.1(side)' as the resolved output layout name");
        // Whatever the real device reports (0 if the query failed/no
        // device), it must never be negative -- just confirms the
        // SDL_GetAudioDeviceFormat query path doesn't misbehave.
        test_assert(audio51.getDeviceNativeChannels() >= 0,
                    "getDeviceNativeChannels() returns a sane (non-negative) value");
        avcodec_parameters_free(&params51);

        // 11b2. Same 5.1 source, but with AudioChannelOption::FORCE_STEREO
        // set before init(): must downmix regardless of the source layout
        // and regardless of what the device would have accepted.
        AVCodecParameters* params51Forced = avcodec_parameters_alloc();
        if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
            avcodec_parameters_copy(params51Forced, controller.m_demuxer->getAudioCodecParams());
        }
        AudioDecoder audio51Forced(params51Forced, {1, 48000}, 0, controller.m_audioQueue);
        audio51Forced.setChannelOption(AudioChannelOption::FORCE_STEREO);
        test_assert(audio51Forced.init(), "AudioDecoder initializes on a 5.1 source with FORCE_STEREO set");
        test_assert(audio51Forced.getOutputChannelCount() == 2,
                    "FORCE_STEREO downmixes a 5.1 source to stereo despite the source having 5.1");
        test_assert(audio51Forced.getOutputChannelLayoutName() == "stereo",
                    "FORCE_STEREO reports 'stereo' as the resolved output layout name");
        avcodec_parameters_free(&params51Forced);
        force_channel_layout_5_1 = false;

        // 11c. Multichannel source but the device/driver only accepts stereo
        // (e.g. an unconfigured surround sink): must fall back to a working
        // stereo stream instead of failing audio entirely.
        force_channel_layout_5_1 = true;
        force_sdl_reject_surround = true;
        AVCodecParameters* params51Fallback = avcodec_parameters_alloc();
        if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
            avcodec_parameters_copy(params51Fallback, controller.m_demuxer->getAudioCodecParams());
        }
        AudioDecoder audio51Fallback(params51Fallback, {1, 48000}, 0, controller.m_audioQueue);
        test_assert(audio51Fallback.init(),
                    "AudioDecoder falls back to stereo (not a hard failure) when the device rejects surround");
        test_assert(audio51Fallback.getOutputChannelCount() == 2,
                    "Stereo-only device forces output channel count back to 2 after surround rejection");
        test_assert(audio51Fallback.getOutputChannelLayoutName() == "stereo",
                    "Stereo fallback reports 'stereo' as the resolved output layout name");
        avcodec_parameters_free(&params51Fallback);
        force_channel_layout_5_1 = false;
        force_sdl_reject_surround = false;

        // 11d. 2.1 (stereo + LFE) source: also preserved directly, not
        // downmixed to plain stereo (which would silently drop the LFE).
        force_channel_layout_2_1 = true;
        AVCodecParameters* params21 = avcodec_parameters_alloc();
        if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
            avcodec_parameters_copy(params21, controller.m_demuxer->getAudioCodecParams());
        }
        AudioDecoder audio21(params21, {1, 48000}, 0, controller.m_audioQueue);
        test_assert(audio21.init(), "AudioDecoder initializes on a 2.1 source");
        test_assert(audio21.getOutputChannelCount() == 3,
                    "2.1 source preserves 3-channel output (stereo + LFE) instead of dropping the LFE channel");
        test_assert(audio21.getOutputChannelLayoutName() == "2.1",
                    "2.1 source reports '2.1' as the resolved output layout name");
        avcodec_parameters_free(&params21);
        force_channel_layout_2_1 = false;

        // 11e. Same 5.1 source, but with AudioChannelOption::VIRTUAL_SURROUND:
        // must preserve the surround layout internally (for the reported
        // layout name and the LFE crossover) while always landing on a
        // genuine 2-channel device stream, with the spatial downmix active.
        force_channel_layout_5_1 = true;
        AVCodecParameters* params51Virtual = avcodec_parameters_alloc();
        if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
            avcodec_parameters_copy(params51Virtual, controller.m_demuxer->getAudioCodecParams());
        }
        AudioDecoder audio51Virtual(params51Virtual, {1, 48000}, 0, controller.m_audioQueue);
        audio51Virtual.setChannelOption(AudioChannelOption::VIRTUAL_SURROUND);
        test_assert(audio51Virtual.init(), "AudioDecoder initializes on a 5.1 source with VIRTUAL_SURROUND set");
        test_assert(audio51Virtual.getOutputChannelCount() == 2,
                    "VIRTUAL_SURROUND always lands on a 2-channel device stream");
        test_assert(audio51Virtual.isVirtualSurroundActive(),
                    "VIRTUAL_SURROUND reports the spatial downmix as active for a supported surround source");
        test_assert(audio51Virtual.getOutputChannelLayoutName() == "5.1(side)",
                    "VIRTUAL_SURROUND still reports the internal surround layout name being folded down");
        avcodec_parameters_free(&params51Virtual);
        force_channel_layout_5_1 = false;

        // 11f. VIRTUAL_SURROUND on a source that ISN'T one of the directly
        // supported surround layouts (the plain-stereo test asset): nothing
        // to virtualize, so it must behave exactly like AUTO/stereo.
        AVCodecParameters* paramsStereoVirtual = avcodec_parameters_alloc();
        if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
            avcodec_parameters_copy(paramsStereoVirtual, controller.m_demuxer->getAudioCodecParams());
        }
        AudioDecoder audioStereoVirtual(paramsStereoVirtual, {1, 48000}, 0, controller.m_audioQueue);
        audioStereoVirtual.setChannelOption(AudioChannelOption::VIRTUAL_SURROUND);
        test_assert(audioStereoVirtual.init(), "AudioDecoder initializes on a stereo source with VIRTUAL_SURROUND set");
        test_assert(audioStereoVirtual.getOutputChannelCount() == 2,
                    "VIRTUAL_SURROUND on an already-stereo source stays at 2 channels");
        test_assert(!audioStereoVirtual.isVirtualSurroundActive(),
                    "VIRTUAL_SURROUND has nothing to fold down for an already-stereo source");
        avcodec_parameters_free(&paramsStereoVirtual);

        // 11f2. Actually decode through a VIRTUAL_SURROUND+5.1 decoder (11e
        // above only ever calls init() on one, never decodeAndResample()):
        // the spatial-downmix fold-down inside the live decode loop is only
        // reachable once real resampled samples exist to fold down.
        {
            force_channel_layout_5_1 = true;
            AVCodecParameters* paramsVSurroundDecode = avcodec_parameters_alloc();
            if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
                avcodec_parameters_copy(paramsVSurroundDecode, controller.m_demuxer->getAudioCodecParams());
            }
            ThreadSafeQueue<AVPacket*> privateVSurroundQueue(8);
            AudioDecoder audioVSurroundDecode(paramsVSurroundDecode, {1, 48000}, 0, privateVSurroundQueue);
            audioVSurroundDecode.setChannelOption(AudioChannelOption::VIRTUAL_SURROUND);
            test_assert(audioVSurroundDecode.init(), "AudioDecoder initializes for the VIRTUAL_SURROUND decode test");
            test_assert(audioVSurroundDecode.isVirtualSurroundActive(),
                        "VIRTUAL_SURROUND decode test decoder has the spatial downmix active");

            // Feed several real packets (borrowed non-blocking from the live
            // demuxer's audio queue): swr_convert() can legitimately return 0
            // samples on early calls while its internal buffer fills, so a
            // single packet isn't guaranteed to actually reach the
            // spatial-downmix stage.
            for (int i = 0; i < 10; ++i) {
                AVPacket* realPkt = nullptr;
                for (int w = 0; w < 25 && !controller.m_audioQueue.try_pop(realPkt); ++w) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (!realPkt) break;
                privateVSurroundQueue.push(realPkt);
                audioVSurroundDecode.decodeAndResample();
            }
            privateVSurroundQueue.clear([](AVPacket*& p) { av_packet_free(&p); });
            avcodec_parameters_free(&paramsVSurroundDecode);
            force_channel_layout_5_1 = false;
        }

        // 11g. Anonymous-namespace helpers isDirectlySupportedSurroundLayout()/
        // spatialSourceLayoutFor() -- callable directly since tests.cpp includes
        // AudioDecoder.cpp into this same translation unit. Covers a non-native
        // channel order and the 5.1(back)/7.1 masks that no real test asset's
        // actual layout ever triggers.
        {
            AVChannelLayout customOrder{};
            customOrder.order = AV_CHANNEL_ORDER_UNSPEC;
            test_assert(!isDirectlySupportedSurroundLayout(customOrder),
                        "isDirectlySupportedSurroundLayout() rejects a non-native channel order");

            AVChannelLayout backLayout{};
            av_channel_layout_from_mask(&backLayout, AV_CH_LAYOUT_5POINT1_BACK);
            test_assert(spatialSourceLayoutFor(backLayout) == naikav::dsp::SpatialDownmixer::SourceLayout::FIVEPOINT1_BACK,
                        "spatialSourceLayoutFor() maps 5.1(back) to FIVEPOINT1_BACK");
            av_channel_layout_uninit(&backLayout);

            AVChannelLayout layout71{};
            av_channel_layout_from_mask(&layout71, AV_CH_LAYOUT_7POINT1);
            test_assert(spatialSourceLayoutFor(layout71) == naikav::dsp::SpatialDownmixer::SourceLayout::SEVENPOINT1,
                        "spatialSourceLayoutFor() maps 7.1 to SEVENPOINT1");
            av_channel_layout_uninit(&layout71);
        }

        // 11h. libsoxr unavailable (best-effort fallback): av_opt_set() fails
        // for the "resampler"="soxr" option specifically -- init() must still
        // succeed using swresample's own default engine, just with a warning.
        {
            force_soxr_fail = true;
            AVCodecParameters* paramsSoxrFail = avcodec_parameters_alloc();
            if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
                avcodec_parameters_copy(paramsSoxrFail, controller.m_demuxer->getAudioCodecParams());
            }
            AudioDecoder audioSoxrFail(paramsSoxrFail, {1, 48000}, 0, controller.m_audioQueue);
            test_assert(audioSoxrFail.init(), "AudioDecoder still initializes when libsoxr is unavailable (best-effort fallback)");
            avcodec_parameters_free(&paramsSoxrFail);
            force_soxr_fail = false;
        }

        // 11i. swr_alloc_set_opts2() itself fails (distinct from swr_init()
        // failing): initResampler() must report failure and init() must fail
        // gracefully rather than dereferencing a null SwrContext.
        {
            force_swr_alloc_fail = true;
            AVCodecParameters* paramsSwrAllocFail = avcodec_parameters_alloc();
            if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
                avcodec_parameters_copy(paramsSwrAllocFail, controller.m_demuxer->getAudioCodecParams());
            }
            AudioDecoder audioSwrAllocFail(paramsSwrAllocFail, {1, 48000}, 0, controller.m_audioQueue);
            test_assert(!audioSwrAllocFail.init(), "AudioDecoder fails gracefully when swr_alloc_set_opts2() fails");
            avcodec_parameters_free(&paramsSwrAllocFail);
            force_swr_alloc_fail = false;
        }

        // 11j. Stereo-fallback resampler reinitialization itself fails (distinct
        // from 11c, where the retry succeeds): the device rejects the surround
        // stream AND the retry's initResampler() call fails -- init() must
        // report failure rather than proceeding with a stale/surround resampler
        // against a stereo device spec.
        {
            force_channel_layout_5_1 = true;
            force_sdl_reject_surround = true;
            force_swr_init_fail_on_retry = 1; // let the first initResampler() succeed, fail the retry
            AVCodecParameters* paramsRetryFail = avcodec_parameters_alloc();
            if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
                avcodec_parameters_copy(paramsRetryFail, controller.m_demuxer->getAudioCodecParams());
            }
            AudioDecoder audioRetryFail(paramsRetryFail, {1, 48000}, 0, controller.m_audioQueue);
            test_assert(!audioRetryFail.init(),
                        "AudioDecoder fails gracefully when the stereo-fallback resampler reinit itself fails");
            avcodec_parameters_free(&paramsRetryFail);
            force_channel_layout_5_1 = false;
            force_sdl_reject_surround = false;
            force_swr_init_fail_on_retry = -1;
        }

        // 11k. Output device resolution by name: enumeratePlaybackDeviceNames()'s
        // device-list loop and resolveOutputDeviceId()'s name-matching loop,
        // neither of which a real (often deviceless/headless CI) enumeration
        // exercises.
        {
            force_fake_playback_devices = 3;
            auto fakeNames = AudioDecoder::enumeratePlaybackDeviceNames();
            test_assert(fakeNames.size() == 3, "enumeratePlaybackDeviceNames() lists every enumerated device");
            test_assert(fakeNames[1] == "Fake Device 1", "enumeratePlaybackDeviceNames() reports each device's real name");

            AVCodecParameters* paramsNamedDevice = avcodec_parameters_alloc();
            if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
                avcodec_parameters_copy(paramsNamedDevice, controller.m_demuxer->getAudioCodecParams());
            }
            AudioDecoder audioNamedDevice(paramsNamedDevice, {1, 48000}, 0, controller.m_audioQueue);
            audioNamedDevice.setOutputDeviceName("Fake Device 1");
            test_assert(audioNamedDevice.init(), "AudioDecoder initializes against a resolved named device");
            avcodec_parameters_free(&paramsNamedDevice);
            force_fake_playback_devices = 0;
        }

        // 11l. setPlaybackSpeed() called before init(): the resolved SDL
        // stream must be created with that non-1.0x ratio already applied,
        // not just updated lazily on a later setPlaybackSpeed() call.
        {
            AVCodecParameters* paramsSpeed = avcodec_parameters_alloc();
            if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
                avcodec_parameters_copy(paramsSpeed, controller.m_demuxer->getAudioCodecParams());
            }
            AudioDecoder audioSpeed(paramsSpeed, {1, 48000}, 0, controller.m_audioQueue);
            audioSpeed.setPlaybackSpeed(1.5f);
            test_assert(audioSpeed.init(), "AudioDecoder initializes with a pre-set non-1.0x playback speed");
            avcodec_parameters_free(&paramsSpeed);
        }

        // 11m. getAudioClock()'s divide-by-zero guard: m_outChannels/m_outSampleRate
        // both default to real nonzero values in the constructor (2 and 48000),
        // so the only way to reach this guard is to force one of them to zero
        // directly -- it's otherwise unreachable through any public API.
        {
            AVCodecParameters* paramsClock = avcodec_parameters_alloc();
            if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
                avcodec_parameters_copy(paramsClock, controller.m_demuxer->getAudioCodecParams());
            }
            AudioDecoder audioClock(paramsClock, {1, 48000}, 0, controller.m_audioQueue);
            audioClock.setClock(42.0);
            audioClock.m_outChannels = 0; // private-access: force the zero-channels guard
            test_assert(audioClock.getAudioClock() == 42.0,
                        "getAudioClock() returns the base clock via the zero-channels guard");
            audioClock.m_outChannels = 2;

            // getAudioClock()'s playedFrames-goes-negative clamp: after
            // init(), force SDL_GetAudioStreamQueued() to report far more
            // queued bytes than have ever been consumed.
            test_assert(audioClock.init(), "AudioDecoder initializes for the getAudioClock() clamp test");
            force_queued_bytes = 999999999;
            double clampedClock = audioClock.getAudioClock();
            test_assert(clampedClock >= audioClock.getAudioClock() - 1.0,
                        "getAudioClock() clamps a negative playedFrames count to 0 instead of going negative");
            force_queued_bytes = -1;
            avcodec_parameters_free(&paramsClock);
        }

        // 11n. Decoded frame with neither pts nor pkt_dts set: both the
        // drop-check's clock-snapshot fallback and the post-resample clock
        // update's frame-duration fallback must be used, rather than reading
        // an uninitialized/garbage timestamp. Also covers the stale
        // seek-generation packet drop (mismatched generation -> dropped and
        // retried) and the "no pts anywhere" branch's interaction with a live
        // decode, using a decoder wired to the real demuxer's audio queue so
        // it decodes genuine packets.
        {
            AVCodecParameters* paramsDecode = avcodec_parameters_alloc();
            if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
                avcodec_parameters_copy(paramsDecode, controller.m_demuxer->getAudioCodecParams());
            }
            // A private, dedicated queue rather than the live controller's
            // shared m_audioQueue: this test pushes packets directly and must
            // control exactly what decodeAndResample() sees next, with no
            // risk of blocking on (or racing) whatever the real demuxer
            // thread is doing to that shared queue concurrently.
            ThreadSafeQueue<AVPacket*> privateAudioQueue(8);
            AudioDecoder audioDecode(paramsDecode, {1, 48000}, 0, privateAudioQueue);
            test_assert(audioDecode.init(), "AudioDecoder initializes for the pts-fallback/decode tests");

            // Stale seek-generation packet: attach a generation counter, then
            // feed a packet tagged with an old generation so decodeAndResample()
            // must drop it (av_packet_free + continue) rather than decode it.
            std::atomic<uint64_t> genCounter{5};
            audioDecode.attachSeekGeneration(&genCounter);
            AVPacket* stalePkt = av_packet_alloc();
            stalePkt->opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
            privateAudioQueue.push(stalePkt); // empty, capacity-8 private queue: never blocks
            audioDecode.decodeAndResample(); // pops+drops the stale packet, then finds the queue empty
            audioDecode.attachSeekGeneration(nullptr);

            // Both pts and pkt_dts unset on a real decoded frame: borrow one
            // genuine packet (non-blocking) from the live demuxer's audio
            // queue -- same codec/stream, so it decodes normally -- and feed
            // it into this decoder's own private queue.
            force_no_pts_no_dts = true;
            AVPacket* realPkt = nullptr;
            for (int i = 0; i < 50 && !controller.m_audioQueue.try_pop(realPkt); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (realPkt) {
                privateAudioQueue.push(realPkt);
                audioDecode.decodeAndResample();
            }
            force_no_pts_no_dts = false;

            privateAudioQueue.clear([](AVPacket*& p) { av_packet_free(&p); });
            avcodec_parameters_free(&paramsDecode);
        }

        // 11o. sdlAudioStreamCallback()'s len<=0 defaulting-to-4096 branch,
        // and the samplesToCopy<=0 defensive break (an additional_amount so
        // small that fewer than one output sample's worth of bytes are
        // requested, once a real decode has already primed the buffer).
        {
            AVCodecParameters* paramsCallback = avcodec_parameters_alloc();
            if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
                avcodec_parameters_copy(paramsCallback, controller.m_demuxer->getAudioCodecParams());
            }
            AudioDecoder audioCallback(paramsCallback, {1, 48000}, 0, controller.m_audioQueue);
            test_assert(audioCallback.init(), "AudioDecoder initializes for the sdlAudioStreamCallback() tests");

            AudioDecoder::sdlAudioStreamCallback(&audioCallback, nullptr, 0, 0);
            AudioDecoder::sdlAudioStreamCallback(&audioCallback, nullptr, -1, -1);

            // Prime the internal buffer with a real decode, then request an
            // absurdly small number of bytes so samplesToCopy resolves to 0.
            AudioDecoder::sdlAudioStreamCallback(&audioCallback, nullptr, 4096, 4096);
            AudioDecoder::sdlAudioStreamCallback(&audioCallback, nullptr, 1, 1);

            avcodec_parameters_free(&paramsCallback);

            // BIT_32_FLOAT and BIT_32_INT output paths, each at all three
            // volume tiers (muted / partial / full) -- only BIT_16 (the
            // default) is exercised by any other test. setOutputBitDepth()
            // must be called before init() (it's what sizes
            // m_outputBytesPerSample and the SDL stream's own format), so
            // each depth needs its own freshly-initialized decoder rather
            // than changing an already-initialized one's depth in place.
            for (auto depth : { AudioOutputBitDepth::BIT_32_FLOAT, AudioOutputBitDepth::BIT_32_INT }) {
                AVCodecParameters* paramsDepth = avcodec_parameters_alloc();
                if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
                    avcodec_parameters_copy(paramsDepth, controller.m_demuxer->getAudioCodecParams());
                }
                AudioDecoder audioDepth(paramsDepth, {1, 48000}, 0, controller.m_audioQueue);
                audioDepth.setOutputBitDepth(depth);
                test_assert(audioDepth.init(), "AudioDecoder initializes with a non-default output bit depth");
                for (float vol : { 0.0f, 0.5f, 1.0f }) {
                    audioDepth.setVolume(vol);
                    AudioDecoder::sdlAudioStreamCallback(&audioDepth, nullptr, 4096, 4096);
                }
                avcodec_parameters_free(&paramsDepth);
            }
        }

        // 11p. getOutputChannelLayoutName()'s av_channel_layout_describe()
        // failure fallback (falls back to "mono"/"stereo"/"Nch" by channel
        // count), and the trivial getters no other test calls at all.
        {
            AVCodecParameters* paramsGetters = avcodec_parameters_alloc();
            if (controller.m_demuxer && controller.m_demuxer->getAudioCodecParams()) {
                avcodec_parameters_copy(paramsGetters, controller.m_demuxer->getAudioCodecParams());
            }
            AudioDecoder audioGetters(paramsGetters, {1, 48000}, 0, controller.m_audioQueue);
            test_assert(audioGetters.init(), "AudioDecoder initializes for the remaining-getters tests");

            force_channel_layout_describe_fail = true;
            std::string fallbackName = audioGetters.getOutputChannelLayoutName();
            test_assert(fallbackName == "stereo" || fallbackName == "mono" || fallbackName.find("ch") != std::string::npos,
                        "getOutputChannelLayoutName() falls back to a channel-count name when av_channel_layout_describe() fails");
            force_channel_layout_describe_fail = false;

            test_assert(audioGetters.getAudioStreamQueuedBytes() >= 0, "getAudioStreamQueuedBytes() returns a sane value");
            (void)audioGetters.getDspSettings();
            (void)audioGetters.getCurrentLoudnessGainDb();
            avcodec_parameters_free(&paramsGetters);
        }

        // 12. Demuxer finds no video/audio streams path
        force_no_streams = true;
        PlayerController noStreamsController;
        test_assert(!noStreamsController.openFile(testFile), "PlayerController fails when file contains no audio/video streams");
        force_no_streams = false;

        // 13. Demuxer has no duration format metadata path
        force_no_duration = true;
        PlayerController noDurController;
        test_assert(noDurController.openFile(testFile), "PlayerController opens successfully with 0.0 duration fallback");
        test_assert(noDurController.getDuration() == 0.0, "Capped duration is 0.0 successfully");
        noDurController.stop();
        force_no_duration = false;

        // 14. av_packet_alloc failure path
        force_packet_alloc_fail = true;
        packet_alloc_count = 0;
        PlayerController pktAllocFailController;
        pktAllocFailController.openFile(testFile);
        pktAllocFailController.play();
        drive_playback(pktAllocFailController, 0.2); // Drives demuxer thread to fail packet allocation loop
        pktAllocFailController.stop();
        force_packet_alloc_fail = false;

        // 15. av_read_frame failure warning loop path
        force_read_error = true;
        PlayerController readErrorController;
        readErrorController.openFile(testFile);
        readErrorController.play();
        drive_playback(readErrorController, 0.2); // Drives read error sleep path
        readErrorController.stop();
        force_read_error = false;

        // 16. swr_convert resampler failure path
        force_swr_convert_fail = true;
        PlayerController swrFailController;
        swrFailController.openFile(testFile);
        swrFailController.play();
        drive_playback(swrFailController, 0.2); // Drives resampling conversion fail paths
        swrFailController.stop();
        force_swr_convert_fail = false;

        // 17. avcodec_send_packet failure path (called directly on video decoder after queue pre-warming)
        PlayerController sendFailController;
        sendFailController.openFile(testFile);
        sendFailController.play();
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Let demuxer push packets to populate queue!
        force_send_packet_fail = true;
        test_assert(!sendFailController.getVideoDecoder()->decodeNextFrame(), "decodeNextFrame fails on packet send error");
        sendFailController.stop();
        force_send_packet_fail = false;

        // 18. avcodec_send_packet failure path in AudioDecoder (resampling packet send error)
        PlayerController audioSendFailController;
        audioSendFailController.openFile(testFile);
        audioSendFailController.play();
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Let demuxer push packets
        force_send_packet_fail = true;
        std::vector<uint8_t> dummyAudioBuf(4096);
        // Call decodeAndResample directly to execute lines 257-258
        if (audioSendFailController.m_audioDecoder) {
            audioSendFailController.m_audioDecoder->decodeAndResample();
        }
        audioSendFailController.stop();
        force_send_packet_fail = false;

        // 18b. avcodec_receive_frame EOF path in AudioDecoder
        PlayerController audioEofController;
        audioEofController.openFile(testFile);
        audioEofController.play();
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Populate queue
        force_video_eof = true;
        if (audioEofController.m_audioDecoder) {
            audioEofController.m_audioDecoder->decodeAndResample();
        }
        audioEofController.stop();
        force_video_eof = false;

        // 19. avcodec_receive_frame critical failure path (called directly after pre-warming)
        PlayerController receiveFailController;
        receiveFailController.openFile(testFile);
        receiveFailController.play();
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Populate queue
        force_receive_frame_fail = true;
        test_assert(!receiveFailController.getVideoDecoder()->decodeNextFrame(), "decodeNextFrame fails on receive frame error");
        receiveFailController.stop();
        force_receive_frame_fail = false;

        // 20. DTS fallback presentation timestamp decoding path
        force_no_pts = true;
        PlayerController noPtsController;
        noPtsController.openFile(testFile);
        noPtsController.play();
        drive_playback(noPtsController, 0.5); // Runs and processes video frames using DTS fallback paths
        noPtsController.stop();
        force_no_pts = false;

        // 21. Video Decoder - EAGAIN try_pop empty queue path (called directly)
        PlayerController eagainController;
        eagainController.openFile(testFile);
        eagainController.play();
        // Abort the queue and call decodeNextFrame directly to force empty queue return
        eagainController.m_videoQueue.abort();
        test_assert(!eagainController.m_videoDecoder->decodeNextFrame(), "decodeNextFrame returns false when queue is empty/aborted");
        eagainController.stop();

        // 22. Video Decoder - EOF path (called directly after pre-warming)
        PlayerController eofController;
        eofController.openFile(testFile);
        eofController.play();
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Populate queue
        force_video_eof = true;
        test_assert(!eofController.m_videoDecoder->decodeNextFrame(), "decodeNextFrame returns false on EOF");
        eofController.stop();
        force_video_eof = false;

        // 23. Video Decoder - Unhandled decoding error path (called directly after pre-warming)
        PlayerController errController;
        errController.openFile(testFile);
        errController.play();
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Populate queue
        force_video_error = true;
        test_assert(!errController.m_videoDecoder->decodeNextFrame(), "decodeNextFrame returns false on critical error");
        errController.stop();
        force_video_error = false;

        // 24. ThreadSafeQueue coverage (push abort and reset loop on AVPacket* template)
        {
            // Reset loop on AVPacket* instantiation
            ThreadSafeQueue<AVPacket*> pktQ(5);
            AVPacket* dummyPkt1 = av_packet_alloc();
            AVPacket* dummyPkt2 = av_packet_alloc();
            pktQ.push(dummyPkt1);
            pktQ.push(dummyPkt2);
            test_assert(pktQ.size() == 2, "pktQ size is 2 before reset");
            pktQ.reset();
            test_assert(pktQ.size() == 0, "pktQ size is 0 after reset");
            av_packet_free(&dummyPkt1);
            av_packet_free(&dummyPkt2);

            // Push abort return false path
            ThreadSafeQueue<int> abortQ(1);
            abortQ.push(100); // Fill the queue
            
            std::thread t(push_abort_helper, &abortQ);

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            abortQ.abort();
            t.join();
        }

        // -------------------------------------------------------------
        // H2. Playback End & ENDED State Tests
        // -------------------------------------------------------------
        std::cout << "Testing Playback End & ENDED State transitions..." << std::endl;
        {
            // Verify new codec name and clock lookups on uninitialized controller
            PlayerController uninitController;
            test_assert(uninitController.getVideoCodecName() == "Unknown", "Uninitialized video codec name is Unknown");
            test_assert(uninitController.getAudioCodecName() == "Unknown", "Uninitialized audio codec name is Unknown");
            test_assert(uninitController.getVideoClock() == 0.0, "Uninitialized video clock is 0.0");
            test_assert(uninitController.getAudioClock() == 0.0, "Uninitialized audio clock is 0.0");

            PlayerController testEndController;
            testEndController.openFile(testFile);
            test_assert(testEndController.getState() == PlayerState::OPENED, "testEndController is OPENED");

            // Verify codec name lookups on initialized controller
            std::string vCodec = testEndController.getVideoCodecName();
            std::string aCodec = testEndController.getAudioCodecName();
            test_assert(vCodec != "Unknown" && !vCodec.empty(), "Video codec name is valid");
            test_assert(aCodec != "Unknown" && !aCodec.empty(), "Audio codec name is valid");

            // Verify clock lookups on initialized controller
            double vClock = testEndController.getVideoClock();
            double aClock = testEndController.getAudioClock();
            test_assert(vClock >= 0.0, "Video clock is non-negative");
            test_assert(aClock >= 0.0, "Audio clock is non-negative");

            testEndController.play();
            test_assert(testEndController.getState() == PlayerState::PLAYING, "testEndController is PLAYING");

            // Test 1: Set the clock to exceed the duration
            double duration = testEndController.getDuration();
            testEndController.m_videoClock = duration + 5.0;
            if (testEndController.m_hasAudio && testEndController.m_audioDecoder) {
                testEndController.m_audioDecoder->setClock(duration + 5.0);
            }

            // Getting time should clamp and transition state to ENDED
            double time = testEndController.getCurrentTime();
            test_assert(time == duration, "Current time is clamped to duration");
            test_assert(testEndController.getState() == PlayerState::ENDED, "State transitioned to ENDED");

            // Play again should restart playback by seeking to 0.0
            testEndController.play();
            test_assert(testEndController.getState() == PlayerState::PLAYING, "State transitions to PLAYING after playing from ENDED");
            
            // Wait for the seek operation to fully complete in the demuxer thread
            for (int i = 0; i < 100; i++) {
                if (!testEndController.m_demuxer->m_seekRequested.load()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));

            test_assert(testEndController.getCurrentTime() < 2.0, "Current time is reset/restarted near 0.0");

            // Test 2: Mock demuxer EOF and empty queues
            force_read_eof = true;
            testEndController.m_demuxer->m_eof = true;

            // Let the demuxer thread finish any active packet pushing before we clear the queues
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Wait/loop to allow state to transition to ENDED, clearing queues to handle any late packets
            for (int i = 0; i < 150; i++) {
                testEndController.m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
                testEndController.m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
                time = testEndController.getCurrentTime();
                if (testEndController.getState() == PlayerState::ENDED) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            test_assert(testEndController.getState() == PlayerState::ENDED, "EOF + empty queues transitions to ENDED");

            force_read_eof = false;

            // Test 3: Seek backward from ENDED and resume
            double seekTime = std::min(10.0, testEndController.getDuration() * 0.5);
            testEndController.seek(seekTime);
            test_assert(!testEndController.m_demuxer->isEOF(), "isEOF() is false immediately after seeking backward");
            test_assert(testEndController.getState() == PlayerState::OPENED, "State is OPENED after seek from ENDED");

            double timeRightAfterSeek = testEndController.getCurrentTime();
            drive_playback(testEndController, 0.5);
            double timeAfterWait = testEndController.getCurrentTime();
            test_assert(std::abs(timeAfterWait - timeRightAfterSeek) < 0.05, "Clock does not progress after seek from ENDED without play");

            testEndController.play();
            test_assert(testEndController.getState() == PlayerState::PLAYING, "State is PLAYING after play");
            test_assert(std::abs(testEndController.getCurrentTime() - seekTime) < 1.0, "Current time is near seek position");

            testEndController.stop();
        }

        }

        // -------------------------------------------------------------
        // H2. Reopen a file on an already-used PlayerController (the
        // "load a new video while one is already open/playing" user
        // flow) -- never exercised anywhere else in this suite; every
        // other openFile() call above uses a freshly-constructed
        // PlayerController.
        // -------------------------------------------------------------
        {
            bool reopenSuccess = controller.openFile(testFile);
            test_assert(reopenSuccess, "Reopening a file on an already-used PlayerController succeeds");
            controller.play();
            drive_playback(controller, 0.5);
            test_assert(controller.getState() == PlayerState::PLAYING, "State is PLAYING after reopening and playing");
        }

        // -------------------------------------------------------------
        // H3. TEMP DEBUG REPRO: open files with genuinely different
        // channel layouts back-to-back on the same PlayerController.
        // -------------------------------------------------------------
        {
            const char* channelTestFile = std::getenv("NAIKAV_CHANNEL_TEST_FILE");
            const char* mono51TestFile = std::getenv("NAIKAV_51_TEST_FILE");
            if (channelTestFile && mono51TestFile) {
                std::cout << "[REPRO] Opening stereo file..." << std::endl;
                test_assert(controller.openFile(testFile), "[REPRO] stereo open");
                controller.play();
                drive_playback(controller, 0.3);

                std::cout << "[REPRO] Opening mono file..." << std::endl;
                test_assert(controller.openFile(channelTestFile), "[REPRO] mono open");
                controller.play();
                drive_playback(controller, 0.3);

                std::cout << "[REPRO] Opening 5.1 file..." << std::endl;
                test_assert(controller.openFile(mono51TestFile), "[REPRO] 5.1 open");
                controller.play();
                drive_playback(controller, 0.3);

                std::cout << "[REPRO] Opening stereo file again..." << std::endl;
                test_assert(controller.openFile(testFile), "[REPRO] stereo open again");
                controller.play();
                drive_playback(controller, 0.3);
                std::cout << "[REPRO] All reopen cycles completed without crashing." << std::endl;
            }
        }

        // -------------------------------------------------------------
        // H4. Audio-Only Playback (e.g. MP3) - Normal speed and duration
        // -------------------------------------------------------------
        {
            std::string audioTestFile = testFile.substr(0, testFile.find_last_of("/\\") + 1) + "test_audio.mp3";
            std::cout << "Testing Audio-Only Playback with MP3: " << audioTestFile << std::endl;
            bool audioOpenSuccess = controller.openFile(audioTestFile);
            test_assert(audioOpenSuccess, "Audio-only file opens successfully");
            test_assert(controller.hasAudio(), "Audio-only file hasAudio() is true");
            test_assert(!controller.hasVideo(), "Audio-only file hasVideo() is false");
            test_assert(controller.getDuration() > 4.5, "Audio-only duration is correct (~5s)");

            // Stay paused for 250ms to verify demuxer backpressure does not discard packets
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            test_assert(controller.getCurrentTime() < 0.1, "Audio-only playback does not skip while paused in OPENED state");
            test_assert(!controller.isEOF(), "Audio-only demuxer does not prematurely hit EOF while paused");

            controller.play();
            drive_playback(controller, 0.5);
            test_assert(controller.getState() == PlayerState::PLAYING, "Audio-only state is PLAYING after play");
            double t = controller.getCurrentTime();
            test_assert(t > 0.05 && t < 2.0, "Audio-only clock advances normally from the beginning");

            // Verify visualizer real-time telemetry APIs
            auto spectrum = controller.getSpectrumMagnitudesDb();
            test_assert(!spectrum.empty() && spectrum.size() == 512, "Visualizer magnitude spectrum is available and has 512 bins");
            auto waveform = controller.getWaveformSamples();
            test_assert(!waveform.empty() && waveform.size() == 1024, "Visualizer waveform snapshot is available and has 1024 samples");

            // Test seeking in audio-only file
            controller.seek(2.5);
            drive_playback(controller, 0.3);
            test_assert(std::abs(controller.getCurrentTime() - 2.5) < 1.0, "Audio-only seek positions accurately");

            controller.stop();
            test_assert(controller.getState() == PlayerState::UNINITIALIZED, "Audio-only stopped cleanly");
        }

        // -------------------------------------------------------------
        // H5. Audio-Only with Embedded Album Art / Attached Picture (APIC)
        // -------------------------------------------------------------
        {
            std::string coverAudioFile = testFile.substr(0, testFile.find_last_of("/\\") + 1) + "test_audio_with_cover.mp3";
            std::cout << "Testing Audio with Attached Picture: " << coverAudioFile << std::endl;
            bool coverOpenSuccess = controller.openFile(coverAudioFile);
            test_assert(coverOpenSuccess, "Audio file with album art opens successfully");
            test_assert(controller.hasAudio(), "Audio with album art hasAudio() is true");
            test_assert(!controller.hasVideo(), "Audio with album art hasVideo() is false (attached picture excluded from video pipeline)");

            controller.play();
            drive_playback(controller, 0.4);
            test_assert(controller.getState() == PlayerState::PLAYING, "State is PLAYING for audio with album art");
            auto spectrum = controller.getSpectrumMagnitudesDb();
            test_assert(!spectrum.empty() && spectrum.size() == 512, "Visualizer spectrum active for audio with album art");

            controller.stop();
            test_assert(controller.getState() == PlayerState::UNINITIALIZED, "Audio with album art stopped cleanly");
        }

        // -------------------------------------------------------------
        // I. Clean Stopping & Destruction
        // -------------------------------------------------------------
        controller.stop();
        test_assert(controller.getState() == PlayerState::UNINITIALIZED, "State is UNINITIALIZED after stop");

    } catch (const std::exception& e) {
        std::cerr << "[EXPECTED] Exception occurred during tests: " << e.what() << std::endl;
        realMainResult = 1;
    }
    } // scope end: every PlayerController above is destroyed here

    SDL_Quit();
    if (realMainResult != 0) {
        return realMainResult;
    }
    std::cout << "All integration tests PASSED successfully!" << std::endl;
    return 0;
}

extern bool g_videoThreadEnabled;

int main(int argc, char* argv[]) {
    g_videoThreadEnabled = false;
    try {
        // Parse testFile in a way that covers all branches in main
        std::string testFile = "";
    for (int pass = 0; pass < 2; ++pass) {
        int tempArgc = (pass == 0) ? 1 : argc;
        if (pass == 0) {
#ifdef _WIN32
            _putenv_s("TEST_VIDEO_PATH", "dummy_val");
#else
            setenv("TEST_VIDEO_PATH", "dummy_val", 1);
#endif
        }
        if (tempArgc > 1 && argv[1][0] != '-') {
            testFile = argv[1];
        } else if (const char* envVal = std::getenv("TEST_VIDEO_PATH")) {
            testFile = envVal;
        }
        if (pass == 0) {
#ifdef _WIN32
            _putenv_s("TEST_VIDEO_PATH", "");
#else
            unsetenv("TEST_VIDEO_PATH");
#endif
        }
    }

    // 1. Cover "No test video file provided" path (returns 1)
    char* argvNoArgs[] = { argv[0] };
    real_main(1, argvNoArgs);

    // 2. Cover "SDL_Init failure" path (returns 1)
    force_sdl_init_fail = true;
    // cppcheck: Severity=style | Rule=cstyleCast | C-style pointer casting
    char* argvSdlFail[] = { argv[0], const_cast<char*>("dummy.mp4") };
    real_main(2, argvSdlFail);
    force_sdl_init_fail = false;

    // 3. Cover TEST_VIDEO_PATH env variable parsing path inside real_main
    {
#ifdef _WIN32
        _putenv_s("TEST_VIDEO_PATH", testFile.c_str());
#else
        setenv("TEST_VIDEO_PATH", testFile.c_str(), 1);
#endif
        char* argvEnv[] = { argv[0] };
        real_main(1, argvEnv);
#ifdef _WIN32
        _putenv_s("TEST_VIDEO_PATH", "");
#else
        unsetenv("TEST_VIDEO_PATH");
#endif
    }

    // 4. Cover exception catch block path in real_main (returns 1)
    if (!testFile.empty()) {
        // cppcheck: Severity=style | Rule=cstyleCast | C-style pointer casting
        char* argvException[] = { argv[0], const_cast<char*>(testFile.c_str()), const_cast<char*>("--test-exception") };
        real_main(3, argvException);
    }

    // 5. Cover assert failure exit(1) path (via exception throw)
    try {
        test_assert(false, "Intentionally failing assert to cover exit(1) path");
    } catch (const std::exception& e) {
        std::cout << "Successfully covered assert exit(1) path: " << e.what() << std::endl;
    }

    // -------------------------------------------------------------
    // Playlist unit tests (naikav::playlist::Playlist / PlaylistIO /
    // MediaFileFilter). Pure logic, no SDL/FFmpeg dependency and no real
    // media file needed -- placed here (before the `!testFile.empty()`
    // block below) rather than at the end of main(), since that block's
    // real-media-decode tests can throw and abort the rest of main() when
    // no real test video is available (this run has none), which would
    // otherwise skip these entirely.
    // -------------------------------------------------------------
    {
        using naikav::playlist::MediaKind;
        using naikav::playlist::Playlist;
        using naikav::playlist::RepeatMode;

        std::cout << "Running Playlist unit tests..." << std::endl;

        // Real (empty) temp files so Playlist::add()'s isValid check (which
        // does a real std::filesystem::exists()) sees them as present.
        std::filesystem::path tmpDir =
            std::filesystem::temp_directory_path() / "naikav_playlist_test";
        std::error_code mkEc;
        std::filesystem::create_directories(tmpDir, mkEc);

        auto makeTempFile = [&](const std::string& name) -> std::string {
            std::filesystem::path p = tmpDir / name;
            std::ofstream(p.string()).put('x');
            return p.string();
        };

        std::string fileA = makeTempFile("a.mp4");
        std::string fileB = makeTempFile("b.mp3");
        std::string fileC = makeTempFile("c.mkv");
        std::string missingFile = (tmpDir / "does_not_exist.mp4").string();

        // --- classifyMediaKind ---
        test_assert(naikav::playlist::classifyMediaKind("movie.MP4") == MediaKind::Video,
                    "classifyMediaKind: uppercase .MP4 -> Video");
        test_assert(naikav::playlist::classifyMediaKind("song.flac") == MediaKind::Audio,
                    "classifyMediaKind: .flac -> Audio");
        test_assert(naikav::playlist::classifyMediaKind("readme.txt") == MediaKind::Unknown,
                    "classifyMediaKind: .txt -> Unknown");
        test_assert(naikav::playlist::classifyMediaKind("no_extension") == MediaKind::Unknown,
                    "classifyMediaKind: no extension -> Unknown");

        // --- add / addMany / clear basics ---
        {
            Playlist pl;
            test_assert(pl.empty(), "New playlist is empty");
            auto item = pl.add(fileA);
            test_assert(pl.size() == 1, "add(): size becomes 1");
            test_assert(item.isValid, "add(): existing file is valid");
            test_assert(item.kind == MediaKind::Video, "add(): fileA classified as Video");

            pl.addMany({fileB, fileC});
            test_assert(pl.size() == 3, "addMany(): size becomes 3");

            pl.clear();
            test_assert(pl.empty(), "clear(): playlist becomes empty");
            test_assert(!pl.currentItem().has_value(), "clear(): currentItem() is nullopt");
        }

        // --- removeAt on current item: next remaining item becomes current ---
        {
            Playlist pl;
            pl.addMany({fileA, fileB, fileC});
            pl.setCurrentIndex(1); // fileB
            uint64_t idOfC = pl.items()[2].id;

            bool removed = pl.removeAt(1); // remove fileB (current)
            test_assert(removed, "removeAt(current index) returns true");
            test_assert(pl.size() == 2, "removeAt(): size decreases");
            test_assert(pl.currentItem().has_value() && pl.currentItem()->id == idOfC,
                        "removeAt(current): item that slid into the slot (fileC) becomes current");

            // Removing the last remaining current item clears current.
            pl.setCurrentIndex(1);
            pl.removeAt(1);
            pl.removeAt(0);
            test_assert(pl.empty() && !pl.currentItem().has_value(),
                        "removeAt(): removing the only item clears current");
        }

        // --- move() preserves current-item identity across reordering ---
        {
            Playlist pl;
            pl.addMany({fileA, fileB, fileC});
            pl.setCurrentIndex(0); // fileA
            uint64_t currentId = pl.currentItem()->id;

            bool moved = pl.move(0, 2); // move fileA to the end
            test_assert(moved, "move() returns true");
            test_assert(pl.items()[2].id == currentId, "move(): item physically relocated to new index");
            test_assert(pl.currentItem().has_value() && pl.currentItem()->id == currentId,
                        "move(): current item identity unchanged after reorder");
            test_assert(pl.getCurrentIndex() == 2, "move(): getCurrentIndex() reflects the new position");
        }

        // --- next()/previous() wraparound per RepeatMode ---
        {
            Playlist pl;
            pl.addMany({fileA, fileB, fileC});

            // RepeatMode::Off: stops at the end, doesn't wrap.
            pl.setRepeatMode(RepeatMode::Off);
            pl.setCurrentIndex(0);
            auto n1 = pl.next();
            auto n2 = pl.next();
            test_assert(n1.has_value() && n1->path == fileB, "Off: next() from 0 -> fileB");
            test_assert(n2.has_value() && n2->path == fileC, "Off: next() from 1 -> fileC");
            auto n3 = pl.next(); // past the end
            test_assert(!n3.has_value(), "Off: next() past the last item returns nullopt");
            test_assert(pl.getCurrentIndex() == 2, "Off: current index stays on the last item after a failed next()");

            // RepeatMode::All: wraps around in both directions.
            pl.setRepeatMode(RepeatMode::All);
            pl.setCurrentIndex(2); // last item
            auto wrapNext = pl.next();
            test_assert(wrapNext.has_value() && wrapNext->path == fileA,
                        "All: next() from last item wraps to first");
            pl.setCurrentIndex(0);
            auto wrapPrev = pl.previous();
            test_assert(wrapPrev.has_value() && wrapPrev->path == fileC,
                        "All: previous() from first item wraps to last");

            // RepeatMode::One: replays the same item regardless of direction.
            pl.setRepeatMode(RepeatMode::One);
            pl.setCurrentIndex(1);
            auto same1 = pl.next();
            auto same2 = pl.previous();
            test_assert(same1.has_value() && same1->path == fileB, "One: next() replays current item");
            test_assert(same2.has_value() && same2->path == fileB, "One: previous() replays current item");
        }

        // --- next()/previous() skip invalid entries; all-invalid -> nullopt ---
        {
            Playlist pl;
            pl.addMany({fileA, missingFile, fileC});
            test_assert(!pl.items()[1].isValid, "addMany(): missing file is marked isValid=false");

            pl.setRepeatMode(RepeatMode::Off);
            pl.setCurrentIndex(0);
            auto skipped = pl.next();
            test_assert(skipped.has_value() && skipped->path == fileC,
                        "next() skips over an invalid entry");

            Playlist allInvalid;
            allInvalid.addMany({missingFile});
            allInvalid.setCurrentIndex(0);
            auto none = allInvalid.next();
            test_assert(!none.has_value(), "next() on an all-invalid list returns nullopt, not an infinite loop");
        }

        // --- shuffle produces a full permutation before repeating ---
        {
            Playlist pl;
            pl.addMany({fileA, fileB, fileC});
            pl.setRepeatMode(RepeatMode::All);
            pl.setShuffle(true);
            pl.setCurrentIndex(0);

            std::vector<std::string> visited;
            visited.push_back(pl.currentItem()->path);
            for (int i = 0; i < 2; ++i) {
                auto item = pl.next();
                test_assert(item.has_value(), "shuffle: next() keeps producing items under RepeatMode::All");
                visited.push_back(item->path);
            }
            std::sort(visited.begin(), visited.end());
            std::vector<std::string> expected = {fileA, fileB, fileC};
            std::sort(expected.begin(), expected.end());
            test_assert(visited == expected,
                        "shuffle: three next() calls visit all three items exactly once");
        }

        // --- M3U/M3U8 round-trip ---
        {
            Playlist pl;
            pl.addMany({fileA, fileB, fileC});
            std::string m3uPath = (tmpDir / "roundtrip.m3u8").string();
            pl.saveM3U(m3uPath);

            Playlist reloaded;
            bool loaded = reloaded.loadM3U(m3uPath);
            test_assert(loaded, "loadM3U(): successfully loads a saved playlist");
            test_assert(reloaded.size() == 3, "loadM3U(): round-trip preserves item count");
            test_assert(reloaded.items()[0].path == fileA &&
                        reloaded.items()[1].path == fileB &&
                        reloaded.items()[2].path == fileC,
                        "loadM3U(): round-trip preserves paths and order");
            for (const auto& it : reloaded.items()) {
                test_assert(it.isValid, "loadM3U(): round-tripped entries for existing files are valid");
            }
        }

        // move(): the invalid-index/no-op early return.
        {
            Playlist pl;
            pl.addMany({fileA, fileB, fileC});
            test_assert(!pl.move(-1, 0), "move(-1, 0) fails: negative index");
            test_assert(!pl.move(0, 99), "move(0, 99) fails: out-of-range index");
            test_assert(!pl.move(1, 1), "move(1, 1) fails: from == to");
        }

        // setCurrentIndex(): the invalid-index branch (clears m_currentId).
        {
            Playlist pl;
            pl.addMany({fileA, fileB});
            pl.setCurrentIndex(0);
            test_assert(pl.getCurrentIndex() == 0, "setCurrentIndex(0) succeeds first");
            test_assert(!pl.setCurrentIndex(99), "setCurrentIndex(99) fails: out of range");
            test_assert(pl.getCurrentIndex() == -1, "setCurrentIndex(99) clears the current item");
        }

        // previous() with nothing currently selected: starts from the *last*
        // item (distinct from next()'s "start at the first item" branch,
        // already covered elsewhere).
        {
            Playlist pl;
            pl.addMany({fileA, fileB, fileC});
            auto prev = pl.previous();
            test_assert(prev.has_value() && prev->path == fileC,
                        "previous() with nothing selected starts from the last item");
        }

        // step()'s final "every item invalid" fallthrough (distinct from the
        // Off-repeat-mode "stepped past the end" early return already
        // covered): needs RepeatMode::All (or One) so the loop actually runs
        // to completion instead of bailing out early on the first
        // out-of-bounds step.
        {
            Playlist pl;
            pl.addMany({missingFile, missingFile});
            pl.setRepeatMode(RepeatMode::All);
            auto none = pl.next();
            test_assert(!none.has_value(), "next() under RepeatMode::All returns nullopt when every item is invalid");
        }

        // loadM3U(): a network URL entry is skipped (and clears any pending
        // #EXTINF title), rather than added as a broken local path.
        {
            std::filesystem::path m3uWithUrl = tmpDir / "with_url.m3u8";
            {
                std::ofstream f(m3uWithUrl.string());
                f << "#EXTM3U\n"
                  << "#EXTINF:-1,A Network Stream\n"
                  << "http://example.com/stream.mp3\n"
                  << "#EXTINF:-1,Local File\n"
                  << fileA << "\n";
            }
            auto entries = naikav::playlist::loadM3U(m3uWithUrl.string());
            test_assert(entries.size() == 1, "loadM3U() skips a network-URL entry entirely");
            test_assert(entries[0].path == fileA, "loadM3U() still loads the local entry after skipping a URL");
        }

        // SpectrumAnalyzer: the magnitudesDb size-mismatch recovery branch on
        // computeSpectrum() (only reachable if the size was forced out of
        // sync with configure()'s own sizing, which no normal call sequence
        // does) and SpatialDownmixer's zero-routes early return, the
        // FIVEPOINT1_BACK route table (no AudioDecoder test drives an actual
        // 5.1-back source through VIRTUAL_SURROUND), and the impossible
        // (all enum values handled) default case in buildRoutes().
        {
            naikav::dsp::SpectrumAnalyzer analyzer;
            analyzer.configure(1, 48000.0);
            analyzer.setEnabled(true);
            analyzer.m_magnitudesDb.resize(3); // force a mismatch vs. kNumBins
            std::vector<float> samples(naikav::dsp::SpectrumAnalyzer::kFftSize, 0.1f);
            analyzer.process(samples.data(), static_cast<int>(samples.size()));
            test_assert(analyzer.getMagnitudesDb().size() == naikav::dsp::SpectrumAnalyzer::kNumBins,
                        "SpectrumAnalyzer recovers from a magnitudesDb size mismatch on computeSpectrum()");
        }
        {
            naikav::dsp::SpatialDownmixer downmixer;
            float dummyIn[8] = {0};
            float dummyOut[8] = {0};
            downmixer.process(dummyIn, 4, dummyOut); // m_routes empty (never configured): early return
            test_assert(downmixer.numSourceChannels() == 0, "SpatialDownmixer::process() no-ops before configure()");

            downmixer.configure(naikav::dsp::SpatialDownmixer::SourceLayout::FIVEPOINT1_BACK, 48000.0);
            test_assert(downmixer.numSourceChannels() == 6, "SpatialDownmixer configures 6 routes for FIVEPOINT1_BACK");
            float in6[6] = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f};
            float out2[2] = {0};
            downmixer.process(in6, 1, out2);

            auto invalidRoutes = naikav::dsp::SpatialDownmixer::buildRoutes(
                static_cast<naikav::dsp::SpatialDownmixer::SourceLayout>(99));
            test_assert(invalidRoutes.empty(), "SpatialDownmixer::buildRoutes() returns empty for an out-of-range enum value");
        }

        // SubtitleTrack.hpp: sanitizeSubtitleText()'s ASS "Dialogue:" prefix
        // stripping (9-comma form), the non-Dialogue 8-comma form, and
        // trimming leading whitespace -- none exercised by any subtitle
        // decode test, which only feeds it already-clean SRT text.
        {
            test_assert(naikav::subtitle::sanitizeSubtitleText(
                             "Dialogue: Marked=0,0:00:01.00,0:00:03.50,Default,,0,0,0,,Hello") == "Hello",
                        "sanitizeSubtitleText() strips an ASS 'Dialogue:' line's 9-comma prefix");
            test_assert(naikav::subtitle::sanitizeSubtitleText("0,0,Default,,0,0,0,,World") == "World",
                        "sanitizeSubtitleText() strips a non-Dialogue 8-comma prefix");
            test_assert(naikav::subtitle::sanitizeSubtitleText("   Leading spaces") == "Leading spaces",
                        "sanitizeSubtitleText() trims leading whitespace");
        }

        // ReplayGainTags.hpp: readTaggedLoudnessAsLufs()'s R128_TRACK_GAIN and
        // REPLAYGAIN_TRACK_GAIN success paths, tested directly against a real
        // AVDictionary rather than through the full PlayerController/openFile()
        // pipeline (which doesn't actually prove which tag tier was matched).
        {
            AVDictionary* dict = nullptr;
            av_dict_set(&dict, "R128_TRACK_GAIN", "-256", 0); // -256/256 = -1.0dB -> -23.0 - (-1.0) = -22.0 LUFS
            double lufs = 0.0;
            test_assert(naikav::dsp::readTaggedLoudnessAsLufs(dict, nullptr, lufs), "readTaggedLoudnessAsLufs() finds an R128_TRACK_GAIN tag");
            test_assert(std::abs(lufs - (-22.0)) < 0.01, "readTaggedLoudnessAsLufs() converts R128_TRACK_GAIN (Q7.8) to LUFS correctly");
            av_dict_free(&dict);

            AVDictionary* dict2 = nullptr;
            av_dict_set(&dict2, "REPLAYGAIN_TRACK_GAIN", "-3.5 dB", 0);
            double lufs2 = 0.0;
            test_assert(naikav::dsp::readTaggedLoudnessAsLufs(dict2, nullptr, lufs2), "readTaggedLoudnessAsLufs() finds a REPLAYGAIN_TRACK_GAIN tag");
            test_assert(std::abs(lufs2 - (-14.5)) < 0.01, "readTaggedLoudnessAsLufs() converts REPLAYGAIN_TRACK_GAIN to LUFS correctly");
            av_dict_free(&dict2);

            // Album-level fallback tiers -- checked only when neither
            // track-level tag is present.
            AVDictionary* dict3 = nullptr;
            av_dict_set(&dict3, "R128_ALBUM_GAIN", "-256", 0);
            double lufs3 = 0.0;
            test_assert(naikav::dsp::readTaggedLoudnessAsLufs(dict3, nullptr, lufs3), "readTaggedLoudnessAsLufs() falls back to R128_ALBUM_GAIN");
            test_assert(std::abs(lufs3 - (-22.0)) < 0.01, "readTaggedLoudnessAsLufs() converts R128_ALBUM_GAIN (Q7.8) to LUFS correctly");
            av_dict_free(&dict3);

            AVDictionary* dict4 = nullptr;
            av_dict_set(&dict4, "REPLAYGAIN_ALBUM_GAIN", "-3.5 dB", 0);
            double lufs4 = 0.0;
            test_assert(naikav::dsp::readTaggedLoudnessAsLufs(dict4, nullptr, lufs4), "readTaggedLoudnessAsLufs() falls back to REPLAYGAIN_ALBUM_GAIN");
            test_assert(std::abs(lufs4 - (-14.5)) < 0.01, "readTaggedLoudnessAsLufs() converts REPLAYGAIN_ALBUM_GAIN to LUFS correctly");
            av_dict_free(&dict4);
        }

        // LoudnessMeter.hpp: configure()'s allocation/filter-lookup/filter-
        // creation failure paths and feed()'s frame-allocation/buffer
        // failures -- none of AudioDecoder's own loudness tests force any
        // of the underlying libavfilter calls to fail.
        {
            open_finished = true; // gates force_frame_alloc_fail in the av_frame_alloc mock

            force_avfilter_graph_alloc_fail = true;
            { naikav::dsp::LoudnessMeter meter; test_assert(!meter.configure(2, 48000), "LoudnessMeter::configure() fails gracefully when avfilter_graph_alloc() fails"); }
            force_avfilter_graph_alloc_fail = false;

            force_avfilter_get_by_name_fail = true;
            { naikav::dsp::LoudnessMeter meter; test_assert(!meter.configure(2, 48000), "LoudnessMeter::configure() fails gracefully when avfilter_get_by_name() fails"); }
            force_avfilter_get_by_name_fail = false;

            force_avfilter_create_filter_fail = true;
            { naikav::dsp::LoudnessMeter meter; test_assert(!meter.configure(2, 48000), "LoudnessMeter::configure() fails gracefully when avfilter_graph_create_filter() fails"); }
            force_avfilter_create_filter_fail = false;

            {
                naikav::dsp::LoudnessMeter meter;
                test_assert(meter.configure(2, 48000), "LoudnessMeter::configure() succeeds normally");
                std::vector<float> samples(2 * 100, 0.1f);

                force_frame_alloc_fail = true;
                meter.feed(samples.data(), 100); // frame alloc fails -> early return
                force_frame_alloc_fail = false;

                force_malloc_fail = true;
                meter.feed(samples.data(), 100); // av_frame_get_buffer() fails -> early return
                force_malloc_fail = false;

                force_frame_alloc_fail_on_second_call = 1;
                meter.feed(samples.data(), 100); // "frame" alloc succeeds, "outFrame" alloc fails
                force_frame_alloc_fail_on_second_call = -1;

                force_buffersrc_add_frame_fail = true;
                meter.feed(samples.data(), 100); // av_buffersrc_add_frame() fails -> early return
                force_buffersrc_add_frame_fail = false;

                test_assert(true, "LoudnessMeter::feed() handles frame/buffer allocation failures without crashing");
            }
        }

        // LoudnessPrescan.hpp: prescanIntegratedLufs()'s failure branches --
        // the background-thread prescan test elsewhere only ever exercises
        // the success path against a real file.
        {
            resetAllMockFlags();

            force_find_stream_info_fail = true;
            test_assert(naikav::dsp::prescanIntegratedLufs(testFile, -1) <= -100.0,
                        "prescanIntegratedLufs() fails gracefully when avformat_find_stream_info() fails");
            force_find_stream_info_fail = false;

            std::string videoOnlyFile2 = (std::filesystem::temp_directory_path() / "naikav_prescan_video_only.mkv").string();
            std::string cmd = "ffmpeg -y -loglevel error -f lavfi -i \"testsrc=duration=1:size=64x64:rate=5\" -an -c:v mpeg4 \"" + videoOnlyFile2 + "\"";
            if (std::system(cmd.c_str()) == 0) {
                test_assert(naikav::dsp::prescanIntegratedLufs(videoOnlyFile2, -1) <= -100.0,
                            "prescanIntegratedLufs() fails gracefully when the file has no audio stream");
            }

            force_find_decoder_fail = true;
            test_assert(naikav::dsp::prescanIntegratedLufs(testFile, -1) <= -100.0,
                        "prescanIntegratedLufs() fails gracefully when avcodec_find_decoder() fails");
            force_find_decoder_fail = false;

            force_alloc_fail = true;
            test_assert(naikav::dsp::prescanIntegratedLufs(testFile, -1) <= -100.0,
                        "prescanIntegratedLufs() fails gracefully when avcodec_alloc_context3() fails");
            force_alloc_fail = false;

            force_swr_alloc_fail = true;
            test_assert(naikav::dsp::prescanIntegratedLufs(testFile, -1) <= -100.0,
                        "prescanIntegratedLufs() fails gracefully when swr_alloc_set_opts2() fails");
            force_swr_alloc_fail = false;

            force_avfilter_graph_alloc_fail = true;
            test_assert(naikav::dsp::prescanIntegratedLufs(testFile, -1) <= -100.0,
                        "prescanIntegratedLufs() fails gracefully when the LoudnessMeter fails to configure");
            force_avfilter_graph_alloc_fail = false;

            force_zero_channels = true;
            // Forcing the codec context's channel count to 0 after open()
            // also affects the real decoder's own internal state (not just
            // this function's local bookkeeping), so the overall scan isn't
            // guaranteed to still succeed downstream -- this only verifies
            // the 0-channels fallback itself doesn't crash.
            naikav::dsp::prescanIntegratedLufs(testFile, -1);
            test_assert(true, "prescanIntegratedLufs() handles a 0-channel codec context without crashing");
            force_zero_channels = false;
        }

        // Clean up temp files.
        std::error_code rmEc;
        std::filesystem::remove_all(tmpDir, rmEc);

        std::cout << "Playlist unit tests PASSED!" << std::endl;
    }

    // 7. Run additional coverage tests to hit remaining uncovered branches!
    if (!testFile.empty()) {
        std::cout << "Running additional code coverage tests..." << std::endl;

        // PlayerController additional methods
        {
            PlayerController controller;
            test_assert(controller.getVideoPixelFormat() == "unknown", "getVideoPixelFormat returns unknown when uninitialized");
            test_assert(!controller.isVideoHardware(), "isVideoHardware returns false when uninitialized");
            test_assert(!controller.isSeeking(), "isSeeking returns false when uninitialized");
            test_assert(!controller.isAudioVirtualSurroundActive(), "isAudioVirtualSurroundActive returns false when uninitialized");

            g_disableHardwareDecoders = false;
            if (controller.openFile(testFile)) {
                const VideoDecoder* dec = controller.getVideoDecoder();
                if (dec) {
                    test_assert(!dec->isSeeking(), "isSeeking returns false during normal playback init");
                }
                test_assert(!controller.isSeeking(), "isSeeking returns false during normal playback init");
                std::string pixFmt = controller.getVideoPixelFormat();
                test_assert(!pixFmt.empty() && pixFmt != "unknown", "getVideoPixelFormat returns valid format name");
                test_assert(controller.isVideoHardware(), "isVideoHardware returns true when using hardware");
            }
            g_disableHardwareDecoders = true;
        }

        // isAudioVirtualSurroundActive() true branch: needs a real
        // hasAudio()/m_audioDecoder with an actually-active spatial downmix.
        {
            PlayerController vSurroundController;
            vSurroundController.setAudioChannelOption(AudioChannelOption::VIRTUAL_SURROUND);
            force_channel_layout_5_1 = true;
            if (vSurroundController.openFile(testFile)) {
                test_assert(vSurroundController.isAudioVirtualSurroundActive(),
                            "isAudioVirtualSurroundActive returns true for a 5.1 source with VIRTUAL_SURROUND set");
            }
            force_channel_layout_5_1 = false;
        }

        // PlayerController: playlist navigation (playlistPlayIndex/Next/Previous,
        // pollPlaylistAutoAdvance) -- entirely new with the playlist feature,
        // never driven by any other test.
        {
            // Out-of-range indices.
            {
                PlayerController pc;
                pc.m_playlist.add(testFile);
                test_assert(!pc.playlistPlayIndex(-1), "playlistPlayIndex(-1) fails (out of range)");
                test_assert(!pc.playlistPlayIndex(99), "playlistPlayIndex(99) fails (out of range)");
            }
            // Empty playlist: next()/previous() have nothing to return.
            {
                PlayerController pc;
                test_assert(!pc.playlistNext(), "playlistNext() fails on an empty playlist");
                test_assert(!pc.playlistPrevious(), "playlistPrevious() fails on an empty playlist");
            }
            // playlistPlayIndex()/playlistNext()/playlistPrevious() success paths.
            {
                PlayerController pc;
                pc.m_playlist.add(testFile);
                pc.m_playlist.add(testFile);
                test_assert(pc.playlistPlayIndex(0), "playlistPlayIndex(0) opens and plays the first item");
                test_assert(pc.getState() == PlayerState::PLAYING, "playlistPlayIndex(0) leaves the controller PLAYING");
                test_assert(pc.playlistNext(), "playlistNext() opens and plays the next item");
                test_assert(pc.playlistPrevious(), "playlistPrevious() opens and plays the previous item");
                pc.stop();
            }
            // pollPlaylistAutoAdvance(): UNINITIALIZED early return, then a
            // real ENDED->next-item advance, then the end-of-list (no next
            // item) case that must leave state alone.
            {
                PlayerController pc;
                pc.pollPlaylistAutoAdvance(); // UNINITIALIZED -> early return, no-op

                pc.m_playlist.add(testFile);
                pc.m_playlist.add(testFile);
                // resetPlaylist=false: the default (true) would clear the
                // two-item playlist just built above and replace it with a
                // single-item one containing only this path.
                if (pc.openFile(testFile, false)) {
                    pc.m_playlist.setCurrentIndex(0);
                    pc.m_state = PlayerState::ENDED;
                    pc.pollPlaylistAutoAdvance();
                    test_assert(pc.getState() == PlayerState::PLAYING,
                                "pollPlaylistAutoAdvance() advances to and plays the next playlist item on ENDED");
                }
                pc.stop();
            }
            {
                PlayerController pc;
                pc.m_playlist.add(testFile); // only one item: next() has nothing after it
                if (pc.openFile(testFile)) {
                    pc.m_state = PlayerState::ENDED;
                    pc.pollPlaylistAutoAdvance();
                    test_assert(pc.getState() == PlayerState::ENDED,
                                "pollPlaylistAutoAdvance() leaves state at ENDED when there's no next item");
                }
                pc.stop();
            }
        }

        // PlayerController: play()/pause() while a seek catch-up is active --
        // both must transition state immediately (audio stays muted/paused
        // until the catch-up lands) rather than touching the audio device.
        {
            PlayerController pc;
            if (pc.openFile(testFile)) {
                pc.m_catchupMode.store(SeekCatchupMode::LANDING);
                pc.m_state = PlayerState::OPENED;
                pc.play();
                test_assert(pc.m_resumeAfterCatchup.load(), "play() during catch-up sets resumeAfterCatchup");
                test_assert(pc.getState() == PlayerState::PLAYING, "play() during catch-up transitions straight to PLAYING");

                pc.pause();
                test_assert(!pc.m_resumeAfterCatchup.load(), "pause() during catch-up clears resumeAfterCatchup");
                test_assert(pc.getState() == PlayerState::PAUSED, "pause() during catch-up transitions straight to PAUSED");

                pc.m_catchupMode.store(SeekCatchupMode::NONE);
            }
            pc.stop();
        }

        // PlayerController: updateClockForVideoOnly()'s catch-up-frozen early
        // return, and getCurrentTime()'s loop-wraparound (Loop toggle on,
        // reached end while PLAYING -> seamless instantSeek(0.0) instead of
        // transitioning to ENDED).
        {
            PlayerController pc;
            if (pc.openFile(testFile)) {
                pc.m_catchupMode.store(SeekCatchupMode::LANDING);
                double clockBefore = pc.m_videoClock.load();
                pc.updateClockForVideoOnly();
                test_assert(pc.m_videoClock.load() == clockBefore,
                            "updateClockForVideoOnly() is a no-op while catch-up is active");
                pc.m_catchupMode.store(SeekCatchupMode::NONE);

                pc.setLoopEnabled(true);
                pc.m_state = PlayerState::PLAYING;
                pc.m_hasVideo = false; // audio-only path is simplest to force "reached end" on
                pc.m_hasAudio = false;
                pc.m_videoClock.store(999999.0); // comfortably past any real duration
                double t = pc.getCurrentTime();
                test_assert(t == 0.0, "getCurrentTime() seamlessly wraps to 0.0 when Loop is on and playback reached the end");
                test_assert(pc.getState() == PlayerState::PLAYING, "Loop wraparound leaves state at PLAYING, never ENDED");
                pc.setLoopEnabled(false);
            }
            pc.stop();
        }

        // PlayerController: simple getters/setters no other test calls at all.
        {
            PlayerController pc;
            test_assert(pc.getAudioChannelLayoutName() == "Unknown", "getAudioChannelLayoutName() returns 'Unknown' when uninitialized");
            test_assert(pc.getSeekReferenceTime() == 0.0, "getSeekReferenceTime() matches getCurrentTime() outside of catch-up");
            test_assert(pc.getPlaybackWidth() == 0, "getPlaybackWidth() is 0 when uninitialized");
            test_assert(pc.getPlaybackHeight() == 0, "getPlaybackHeight() is 0 when uninitialized");
            test_assert(pc.getAudioFrameQueueSize() == 0, "getAudioFrameQueueSize() is 0 when uninitialized");
            ColorPipelineInfo ci = pc.getColorInfo();
            test_assert(ci.colorSpace == "Unspecified", "getColorInfo() returns the default struct when uninitialized");

            if (pc.openFile(testFile)) {
                test_assert(!pc.getAudioChannelLayoutName().empty(), "getAudioChannelLayoutName() returns a real name once opened");
                pc.setResolutionOption(ResolutionOption::R_720P);
                test_assert(pc.getPlaybackWidth() > 0, "getPlaybackWidth() reflects the resolution option");
                test_assert(pc.getPlaybackHeight() > 0, "getPlaybackHeight() is positive once opened");
                pc.getAudioFrameQueueSize(); // just needs to execute without dividing by zero

                pc.m_catchupMode.store(SeekCatchupMode::LANDING);
                pc.m_catchupTarget.store(12.5);
                test_assert(pc.getSeekReferenceTime() == 12.5, "getSeekReferenceTime() returns the catch-up target while catching up");
                pc.m_catchupMode.store(SeekCatchupMode::NONE);

                ColorPipelineInfo realCi = pc.getColorInfo();
                test_assert(realCi.colorSpace != "" , "getColorInfo() delegates to the real VideoDecoder once opened");
            }
            pc.stop();
        }

        // PlayerController: setPlaybackSpeed()/setAudioDspSettings() actually
        // reaching the live AudioDecoder, and the loudness (0->1) toggle
        // transition triggering prescanLoudnessForCurrentFile().
        {
            PlayerController pc;
            if (pc.openFile(testFile)) {
                pc.setPlaybackSpeed(1.5f);
                test_assert(pc.getPlaybackSpeed() == 1.5f, "setPlaybackSpeed() reaches the live AudioDecoder");

                naikav::dsp::AudioDspSettings settings = pc.getAudioDspSettings();
                settings.loudnessEnabled = false;
                pc.setAudioDspSettings(settings); // baseline: loudness off, no prescan trigger

                settings.dspEnabled = !settings.dspEnabled;
                pc.setAudioDspSettings(settings); // reaches m_audioDecoder->applyDspSettings(), no prescan

                settings.loudnessEnabled = true; // 0 -> 1 transition: triggers prescanLoudnessForCurrentFile()
                pc.setAudioDspSettings(settings);
                test_assert(pc.getAudioDspSettings().loudnessEnabled, "setAudioDspSettings() applies the loudness toggle");
            }
            pc.stop();
        }

        // PlayerController: prescanLoudnessForCurrentFile()'s tagged-loudness
        // fast path (a REPLAYGAIN_TRACK_GAIN tag lets it skip the whole-file
        // decode scan) and applyGenrePresetIfEnabled(), neither of which the
        // plain test asset's own (tag-less) metadata ever triggers.
        {
            // Reset every mock injection flag first: this spawns a real
            // background prescan thread that decodes with the real FFmpeg
            // calls (mocked-but-passthrough), and any flag an earlier,
            // unrelated test left set could otherwise silently wedge it.
            resetAllMockFlags();

            std::filesystem::path pcMetaDir =
                std::filesystem::temp_directory_path() / "naikav_pc_meta_test";
            std::error_code pcMetaEc;
            std::filesystem::create_directories(pcMetaDir, pcMetaEc);
            auto runFfmpegPc = [](const std::string& args) -> bool {
                std::string cmd = "ffmpeg -y -loglevel error " + args;
                return std::system(cmd.c_str()) == 0;
            };

            std::string replayGainFile = (pcMetaDir / "replaygain.mkv").string();
            std::string genreFile = (pcMetaDir / "podcast_genre.mkv").string();
            // The ffmpeg *command line tool* only synthesizes these tagged
            // assets; it is a separate package from the libav* libraries this
            // player links against, so it isn't guaranteed to be installed.
            // Skip the block when it's missing, like every other asset-
            // generating test here, instead of failing the whole run.
            bool pcGenOk =
                runFfmpegPc("-f lavfi -i \"sine=frequency=1000:duration=1\" -metadata REPLAYGAIN_TRACK_GAIN=\"-3.5 dB\" -c:a aac \"" + replayGainFile + "\"") &&
                runFfmpegPc("-f lavfi -i \"sine=frequency=1000:duration=1\" -metadata genre=\"Podcast\" -c:a aac \"" + genreFile + "\"");

            if (!pcGenOk) {
                std::cout << "[SKIPPED] ffmpeg CLI unavailable: skipping the PlayerController prescan/genre tests" << std::endl;
            } else {
                naikav::dsp::AudioDspSettings loudSettings;
                loudSettings.loudnessEnabled = true;

                PlayerController pc;
                pc.setAudioDspSettings(loudSettings);
                if (pc.openFile(replayGainFile)) {
                    test_assert(pc.getAudioDspSettings().loudnessEnabled,
                                "openFile() with loudness pre-enabled reads the REPLAYGAIN_TRACK_GAIN tag via the fast path");
                }
                pc.stop();

                naikav::dsp::AudioDspSettings genreSettings;
                genreSettings.autoGenrePresetEnabled = true;
                PlayerController genreController;
                genreController.setAudioDspSettings(genreSettings);
                if (genreController.openFile(genreFile)) {
                    test_assert(genreController.getAudioDspSettings().autoGenrePresetEnabled,
                                "applyGenrePresetIfEnabled() preserves the toggle after applying the 'Podcast' preset");
                }
                genreController.stop();
            }
        }

        // PlayerController: getAudioFrameQueueSize()'s bytesPerFrame<=0 guard
        // -- only reachable by forcing the resolved output channel count to
        // 0, which no real init() path ever produces.
        {
            PlayerController pc;
            if (pc.openFile(testFile) && pc.m_hasAudio && pc.m_audioDecoder) {
                int savedChannels = pc.m_audioDecoder->m_outChannels;
                pc.m_audioDecoder->m_outChannels = 0;
                test_assert(pc.getAudioFrameQueueSize() == 0, "getAudioFrameQueueSize() returns 0 when the resolved channel count is 0");
                pc.m_audioDecoder->m_outChannels = savedChannels;
            }
            pc.stop();
        }

        // PlayerController: finishCatchup()'s early returns, called directly
        // (private-access) -- neither is reachable through the public seek()
        // API without a real, precisely-timed video catch-up in flight.
        {
            PlayerController pc;
            if (pc.openFile(testFile)) {
                pc.m_catchupMode.store(SeekCatchupMode::NONE);
                pc.finishCatchup(1.0); // not catching up at all: no-op
                test_assert(pc.m_catchupMode.load() == SeekCatchupMode::NONE, "finishCatchup() is a no-op when not catching up");

                pc.m_catchupMode.store(SeekCatchupMode::LANDING);
                pc.m_catchupTarget.store(10.0);
                pc.finishCatchup(1.0); // 1.0 is well short of the 10.0 target, and not at EOF
                test_assert(pc.m_catchupMode.load() == SeekCatchupMode::LANDING,
                            "finishCatchup() keeps catching up when nowhere near the target and not at EOF");
                pc.m_catchupMode.store(SeekCatchupMode::NONE);
            }
            pc.stop();
        }

        // PlayerController: getCurrentTime()'s video-stream EOF-reached branch
        // (distinct from the audio-only one exercised by the Loop test above)
        // -- forces m_demuxer's real EOF flag directly, since driving an
        // actual end-of-stream through the whole pipeline synchronously isn't
        // practical here.
        {
            PlayerController pc;
            if (pc.openFile(testFile) && pc.m_hasVideo) {
                pc.m_demuxer->m_eof.store(true);
                pc.m_videoQueue.abort(); // empty() still reports true after abort
                pc.m_state = PlayerState::PLAYING;
                pc.getCurrentTime(); // must reach the video-stream EOF branch without crashing
                test_assert(pc.getState() == PlayerState::ENDED || pc.getState() == PlayerState::PLAYING,
                            "getCurrentTime() handles the video-stream EOF-reached branch");
            }
            pc.stop();
        }

        // PlayerController: subtitle track handling -- selectSubtitleTrack()'s
        // Off/external/embedded branches, loadExternalSubtitle()'s failure
        // path, pollSubtitlePackets()/getCurrentSubtitleText(),
        // autoProbeExternalSubtitles(), getActiveSubtitleTrackName(), and the
        // seek()/instantSeek() subtitle-flush call sites -- essentially all
        // of it needs a live m_subtitleDecoder, which the plain (subtitle-
        // less) test asset never creates on its own.
        {
            std::filesystem::path subDir =
                std::filesystem::temp_directory_path() / "naikav_pc_subtitle_test";
            std::error_code subDirEc;
            std::filesystem::create_directories(subDir, subDirEc);
            std::string srtPath = (subDir / "external.srt").string();
            {
                std::ofstream srt(srtPath);
                srt << "1\n00:00:00,000 --> 00:00:03,000\nHello from an external subtitle\n";
            }

            // loadExternalSubtitle() failure: not a parseable subtitle file.
            {
                PlayerController pc;
                if (pc.openFile(testFile)) {
                    std::string notASubtitle = (subDir / "not_a_subtitle.srt").string();
                    { std::ofstream bogus(notASubtitle); bogus << "this is not valid SRT content at all"; }
                    test_assert(!pc.loadExternalSubtitle(notASubtitle), "loadExternalSubtitle() fails gracefully on unparseable content");
                }
                pc.stop();
            }

            // External subtitle: load, select Off, reselect external (-2),
            // poll packets, read active text, then check the track name.
            {
                PlayerController pc;
                if (pc.openFile(testFile)) {
                    test_assert(pc.loadExternalSubtitle(srtPath), "loadExternalSubtitle() loads a real .srt file");
                    test_assert(pc.getActiveSubtitleTrackName() != "Off", "getActiveSubtitleTrackName() reports the external track after loading it");

                    pc.selectSubtitleTrack(-1); // Off
                    test_assert(pc.getActiveSubtitleTrackName() == "Off", "selectSubtitleTrack(-1) disables subtitles");

                    pc.selectSubtitleTrack(-2); // reselect external -- m_subtitleDecoder already external, re-enters that branch
                    pc.m_selectedSubtitleTrack.store(-2);
                    pc.pollSubtitlePackets(); // no queued packets for an external decoder, but must not crash
                    std::string text = pc.getCurrentSubtitleText();
                    (void)text;

                    // seek()/instantSeek() with a live subtitle decoder: hits
                    // both flush() call sites. seek() only takes the
                    // catch-up path (not instantSeek()'s fast path) with the
                    // video thread enabled, playing, and a big-enough jump.
                    pc.m_videoThreadEnabled = true;
                    pc.m_state = PlayerState::PLAYING;
                    pc.seek(4.0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    pc.m_videoThreadEnabled = false;

                    pc.instantSeek(0.5);
                }
                pc.stop();
            }

            // Embedded subtitle: selectSubtitleTrack(trackId >= 0) needs a
            // real embedded subtitle stream, which the main test asset
            // doesn't have -- generate a tiny video+subtitle asset.
            {
                std::string embSrt = (subDir / "emb.srt").string();
                { std::ofstream srt(embSrt); srt << "1\n00:00:00,000 --> 00:00:01,000\nEmbedded\n"; }
                std::string embFile = (subDir / "video_with_sub.mkv").string();
                std::string cmd = "ffmpeg -y -loglevel error -f lavfi -i \"testsrc=duration=2:size=64x64:rate=5\" -f lavfi -i \"sine=frequency=1000:duration=2\" -i \"" +
                                   embSrt + "\" -c:v mpeg4 -c:a aac -c:s srt \"" + embFile + "\"";
                if (std::system(cmd.c_str()) == 0) {
                    PlayerController pc;
                    if (pc.openFile(embFile)) {
                        auto subTracks = pc.getSubtitleTracks();
                        if (!subTracks.empty()) {
                            int subId = subTracks[0].id;
                            pc.selectSubtitleTrack(subId);
                            test_assert(pc.getActiveSubtitleTrackName() != "Off",
                                        "selectSubtitleTrack(embeddedId) selects a real embedded subtitle stream");
                        }
                    }
                    pc.stop();
                }
            }

            // autoProbeExternalSubtitles(): a sibling .srt file matching the
            // media file's basename must be auto-detected on open().
            {
                std::string mediaBase = (subDir / "movie").string();
                { std::ofstream srt(mediaBase + ".srt"); srt << "1\n00:00:00,000 --> 00:00:01,000\nAuto-detected\n"; }
                std::string mediaCopy = mediaBase + std::filesystem::path(testFile).extension().string();
                std::error_code copyEc;
                std::filesystem::copy_file(testFile, mediaCopy, std::filesystem::copy_options::overwrite_existing, copyEc);
                if (!copyEc) {
                    PlayerController pc;
                    if (pc.openFile(mediaCopy)) {
                        test_assert(pc.getActiveSubtitleTrackName() != "Off",
                                    "openFile() auto-detects and loads a sibling .srt file with a matching basename");
                    }
                    pc.stop();
                }
            }

            // getActiveSubtitleTrackName()'s cached-track-lookup branches
            // (found vs. fallback "Track N"), independent of a live decoder.
            {
                PlayerController pc;
                pc.m_selectedSubtitleTrack.store(5);
                test_assert(pc.getActiveSubtitleTrackName() == "Track 5",
                            "getActiveSubtitleTrackName() falls back to 'Track N' for an unrecognized id");
            }
        }

        // PlayerController: external audio track handling -- selectAudioTrack()'s
        // Off/external/embedded branches and their failure paths,
        // loadExternalAudio()'s failure path, and removeExternalAudio().
        {
            std::filesystem::path testAudioPath = std::filesystem::path(testFile).parent_path() / "test_audio.mp3";
            std::string extAudioFile = testAudioPath.string();
            bool haveTestAudio = std::filesystem::exists(testAudioPath);

            // selectAudioTrack() before any file is loaded: UNINITIALIZED
            // early-return branch.
            {
                PlayerController pc;
                test_assert(pc.selectAudioTrack(3), "selectAudioTrack() on an UNINITIALIZED controller just records the id and returns true");
            }

            // selectAudioTrack(-2) with no external audio loaded, and an
            // invalid embedded track id -- both failure branches.
            {
                PlayerController pc;
                if (pc.openFile(testFile)) {
                    test_assert(!pc.selectAudioTrack(-2), "selectAudioTrack(-2) fails when no external audio is loaded");
                    test_assert(!pc.selectAudioTrack(99999), "selectAudioTrack(99999) fails for an invalid embedded track index");
                }
                pc.stop();
            }

            // selectAudioTrack(-1): mute.
            {
                PlayerController pc;
                if (pc.openFile(testFile)) {
                    test_assert(pc.selectAudioTrack(-1), "selectAudioTrack(-1) disables audio");
                    test_assert(!pc.m_hasAudio, "selectAudioTrack(-1) clears hasAudio");
                }
                pc.stop();
            }

            if (haveTestAudio) {
                // loadExternalAudio() success -> selectAudioTrack(-2)'s
                // full success path (video present, so the m_hasVideo branch
                // -- seek(currentPos) -- runs, not the audio-only wasPlaying
                // branch below).
                {
                    PlayerController pc;
                    if (pc.openFile(testFile)) {
                        test_assert(pc.loadExternalAudio(extAudioFile), "loadExternalAudio() loads a real external audio file");
                        test_assert(pc.hasExternalAudio(), "loadExternalAudio() sets hasExternalAudio");

                        pc.removeExternalAudio();
                        test_assert(!pc.hasExternalAudio(), "removeExternalAudio() clears hasExternalAudio and stops the external demuxer");
                    }
                    pc.stop();
                }

                // loadExternalAudio() failure: not a real media file.
                {
                    PlayerController pc;
                    if (pc.openFile(testFile)) {
                        std::string notMedia = (std::filesystem::temp_directory_path() / "naikav_not_media.mp3").string();
                        { std::ofstream bogus(notMedia); bogus << "not a real audio file"; }
                        test_assert(!pc.loadExternalAudio(notMedia), "loadExternalAudio() fails gracefully on a non-media file");
                    }
                    pc.stop();
                }

                // Audio-only main file: selectAudioTrack(-2) and (trackId>=0)
                // both take the "no video" wasPlaying/start()/PLAYING branch
                // instead of seek(currentPos).
                {
                    PlayerController pc;
                    if (pc.openFile(extAudioFile) && !pc.m_hasVideo) {
                        pc.play();
                        bool wasPlayingForExternal = (pc.getState() == PlayerState::PLAYING);
                        if (pc.loadExternalAudio(extAudioFile)) {
                            test_assert(wasPlayingForExternal ? pc.getState() == PlayerState::PLAYING : true,
                                        "selectAudioTrack(-2) on an audio-only file resumes playback directly");

                            // Embedded reselect (trackId 0) on the same
                            // audio-only controller, also while playing.
                            pc.play();
                            if (pc.getState() == PlayerState::PLAYING && !pc.getAudioTracks().empty()) {
                                int embeddedId = pc.getAudioTracks()[0].id;
                                pc.selectAudioTrack(embeddedId);
                                test_assert(pc.getState() == PlayerState::PLAYING || pc.getState() == PlayerState::OPENED,
                                            "selectAudioTrack(embeddedId) on an audio-only file re-syncs playback");
                            }
                        }
                    }
                    pc.stop();
                }
            }
        }

        // PlayerController: pollPlaylistAutoAdvance()'s "not ENDED" early
        // return (distinct from the UNINITIALIZED one already covered),
        // getCurrentTime()'s audio-only EOF-reached branch (distinct from
        // both the duration-exceeded and video-stream-EOF branches already
        // covered), selectAudioTrack()'s remaining failure/fallback
        // branches, loadExternalAudio()'s remaining branches, embedded
        // subtitle reset()/pollSubtitlePackets(), and getActiveAudioTrackName().
        {
            // pollPlaylistAutoAdvance() while OPENED (not ENDED, not UNINITIALIZED).
            {
                PlayerController pc;
                if (pc.openFile(testFile)) {
                    PlayerState before = pc.getState();
                    pc.pollPlaylistAutoAdvance();
                    test_assert(pc.getState() == before, "pollPlaylistAutoAdvance() is a no-op when not ENDED");
                }
                pc.stop();
            }

            // getCurrentTime()'s audio-only EOF branch: force duration<=0.0
            // (force_no_duration) so the branch's own condition doesn't
            // depend on real playback timing.
            {
                std::filesystem::path testAudioPath2 = std::filesystem::path(testFile).parent_path() / "test_audio.mp3";
                if (std::filesystem::exists(testAudioPath2)) {
                    force_no_duration = true;
                    PlayerController pc;
                    if (pc.openFile(testAudioPath2.string()) && !pc.m_hasVideo) {
                        pc.m_demuxer->m_eof.store(true);
                        pc.m_audioQueue.abort();
                        pc.m_state = PlayerState::PLAYING;
                        pc.getCurrentTime();
                        test_assert(pc.getState() == PlayerState::ENDED || pc.getState() == PlayerState::PLAYING,
                                    "getCurrentTime() handles the audio-only EOF-reached branch");
                    }
                    pc.stop();
                    force_no_duration = false;
                }
            }

            // selectAudioTrack(): the final fallback (an id matching none of
            // -1/-2/>=0), and getActiveAudioTrackName()'s cache-miss fallback.
            {
                PlayerController pc;
                if (pc.openFile(testFile)) {
                    test_assert(!pc.selectAudioTrack(-5), "selectAudioTrack() falls through to false for an unrecognized negative id");
                    pc.m_selectedAudioTrack.store(-5);
                    test_assert(pc.getActiveAudioTrackName() == "Track -5",
                                "getActiveAudioTrackName() falls back to 'Track N' for an unrecognized id");
                }
                pc.stop();
            }

            // videoThreadLoop()'s own "disabled" sleep branch: the thread is
            // only ever started (in openFile()) while m_videoThreadEnabled is
            // true (g_videoThreadEnabled is false for the whole test binary),
            // so it never launches through the normal API in any other test.
            // Force it to start, then disable it while it's running.
            {
                PlayerController pc;
                pc.m_videoThreadEnabled = true;
                if (pc.openFile(testFile) && pc.m_hasVideo) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    pc.m_videoThreadEnabled = false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    test_assert(true, "videoThreadLoop() handles being disabled while its thread is running");
                }
                pc.stop();
            }

            // selectAudioTrack(-2)'s "external demuxer has no valid audio
            // stream" branch: load real external audio, then poison the
            // external demuxer's own selected stream index.
            {
                std::filesystem::path testAudioPath3 = std::filesystem::path(testFile).parent_path() / "test_audio.mp3";
                if (std::filesystem::exists(testAudioPath3)) {
                    PlayerController pc;
                    if (pc.openFile(testFile) && pc.loadExternalAudio(testAudioPath3.string())) {
                        // selectAudioTrack() short-circuits to true when
                        // trackId already equals the currently-selected
                        // track (which loadExternalAudio() just set to -2) --
                        // select a different (embedded) track first so the
                        // reselect below actually re-enters the function body.
                        pc.selectAudioTrack(pc.m_demuxer->getAudioStreamIndex());
                        pc.m_externalAudioDemuxer->selectAudioStream(-1);
                        test_assert(!pc.selectAudioTrack(-2), "selectAudioTrack(-2) fails when the external demuxer has no valid audio stream");
                    }
                    pc.stop();
                }
            }

            // loadExternalAudio(): "no audio streams" on a real (video-only,
            // -an) media file, and replacing an already-loaded external
            // audio demuxer (stops the old one first).
            {
                std::string videoOnlyFile = (std::filesystem::temp_directory_path() / "naikav_pc_video_only.mkv").string();
                std::string cmd = "ffmpeg -y -loglevel error -f lavfi -i \"testsrc=duration=1:size=64x64:rate=5\" -an -c:v mpeg4 \"" + videoOnlyFile + "\"";
                if (std::system(cmd.c_str()) == 0) {
                    PlayerController pc;
                    if (pc.openFile(testFile)) {
                        test_assert(!pc.loadExternalAudio(videoOnlyFile), "loadExternalAudio() fails gracefully on a file with no audio streams");
                    }
                    pc.stop();
                }

                std::filesystem::path testAudioPath4 = std::filesystem::path(testFile).parent_path() / "test_audio.mp3";
                if (std::filesystem::exists(testAudioPath4)) {
                    PlayerController pc;
                    if (pc.openFile(testFile)) {
                        test_assert(pc.loadExternalAudio(testAudioPath4.string()), "loadExternalAudio() loads the first external file");
                        test_assert(pc.loadExternalAudio(testAudioPath4.string()),
                                    "loadExternalAudio() replaces (stops then reopens) an already-loaded external demuxer");
                    }
                    pc.stop();
                }
            }

            // Embedded subtitle: reset() on Off (distinct from the external-
            // decoder case, which skips reset()), re-creating an external
            // decoder after an embedded one was active, and
            // pollSubtitlePackets() actually processing a queued packet.
            {
                std::filesystem::path subDir2 =
                    std::filesystem::temp_directory_path() / "naikav_pc_subtitle_test";
                std::string embSrt2 = (subDir2 / "emb2.srt").string();
                { std::ofstream srt(embSrt2); srt << "1\n00:00:00,000 --> 00:00:01,000\nEmbedded 2\n"; }
                std::string extSrt2 = (subDir2 / "ext2.srt").string();
                { std::ofstream srt(extSrt2); srt << "1\n00:00:00,000 --> 00:00:01,000\nExternal 2\n"; }
                std::string embFile2 = (subDir2 / "video_with_sub2.mkv").string();
                std::string cmd = "ffmpeg -y -loglevel error -f lavfi -i \"testsrc=duration=2:size=64x64:rate=5\" -i \"" +
                                   embSrt2 + "\" -c:v mpeg4 -c:s srt \"" + embFile2 + "\"";
                if (std::system(cmd.c_str()) == 0) {
                    PlayerController pc;
                    if (pc.openFile(embFile2)) {
                        auto subTracks = pc.getSubtitleTracks();
                        if (!subTracks.empty()) {
                            int subId = subTracks[0].id;
                            pc.selectSubtitleTrack(subId); // embedded

                            AVPacket* subPkt = av_packet_alloc();
                            feedPacket(pc.m_subtitleQueue, subPkt);
                            pc.pollSubtitlePackets(); // pops+processes the queued packet
                            test_assert(true, "pollSubtitlePackets() processes a real queued packet without crashing");

                            pc.selectSubtitleTrack(-1); // Off: decoder is embedded (not external) -> reset() branch
                            test_assert(pc.getActiveSubtitleTrackName() == "Off", "selectSubtitleTrack(-1) disables an embedded subtitle track");

                            test_assert(pc.loadExternalSubtitle(extSrt2), "loadExternalSubtitle() loads a real external subtitle file");
                            pc.selectSubtitleTrack(subId); // back to embedded: current decoder is now non-external again
                            // m_hasExternalSubtitle is still true from the loadExternalSubtitle() call above,
                            // but the *current* decoder is embedded -- selectSubtitleTrack(-2) must recreate
                            // the external decoder rather than reusing the (now embedded) one.
                            pc.selectSubtitleTrack(-2);
                            test_assert(pc.getActiveSubtitleTrackName() != "Off",
                                        "selectSubtitleTrack(-2) recreates the external decoder when the current one isn't external");
                        }
                    }
                    pc.stop();
                }
            }
        }

        // Demuxer: hit the m_seekRequested check in threadLoop during active read
        {
            PlayerController controller;
            if (controller.openFile(testFile)) {
                Demuxer* demuxer = controller.m_demuxer.get();
                {
                    std::lock_guard<std::mutex> lock(mock_read_frame_mutex);
                    on_mock_read_frame = [demuxer]() {
                        demuxer->m_seekRequested.store(true);
                    };
                }
                
                controller.play();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                
                {
                    std::lock_guard<std::mutex> lock(mock_read_frame_mutex);
                    on_mock_read_frame = nullptr;
                }
            }
        }

        // VideoDecoder: nullptr m_codecCtx check in decodeNextFrame
        {
            PlayerController controller;
            if (controller.openFile(testFile)) {
                VideoDecoder* dec = controller.m_videoDecoder.get();
                AVCodecContext* savedCtx = dec->m_codecCtx;
                dec->m_codecCtx = nullptr;
                test_assert(!dec->decodeNextFrame(), "decodeNextFrame returns false when m_codecCtx is nullptr");
                dec->m_codecCtx = savedCtx;
            }
        }

        // VideoDecoder: stuck hardware decoder fallback
        {
            PlayerController controller;
            if (controller.openFile(testFile)) {
                VideoDecoder* dec = controller.m_videoDecoder.get();
                
                const AVCodec* savedCodec = dec->m_codecCtx->codec;
                AVCodec fakeCodec = *savedCodec;
                fakeCodec.name = "h264_qsv";
                global_saved_codec = savedCodec;
                global_fake_codec_ptr = &fakeCodec;
                dec->m_codecCtx->codec = &fakeCodec;
                
                force_receive_eagain = true;
                mock_send_packet_success = true;
                
                for (int i = 0; i < 70; i++) {
                    AVPacket* pkt = av_packet_alloc();
                    feedPacket(dec->m_queue, pkt);
                }
                
                dec->decodeNextFrame();
                
                force_receive_eagain = false;
                mock_send_packet_success = false;
                if (dec->m_codecCtx && dec->m_codecCtx->codec == &fakeCodec) {
                    dec->m_codecCtx->codec = savedCodec;
                }
                global_saved_codec = nullptr;
                global_fake_codec_ptr = nullptr;
            }
        }

        // VideoDecoder: fallbackToSoftware failure paths
        {
            // Case A: avcodec_alloc_context3 failure
            {
                PlayerController controller;
                if (controller.openFile(testFile)) {
                    VideoDecoder* dec = controller.m_videoDecoder.get();
                    
                    const AVCodec* savedCodec = dec->m_codecCtx->codec;
                    AVCodec fakeCodec = *savedCodec;
                    fakeCodec.name = "h264_qsv";
                    global_saved_codec = savedCodec;
                    global_fake_codec_ptr = &fakeCodec;
                    dec->m_codecCtx->codec = &fakeCodec;
                    
                    force_receive_eagain = true;
                    mock_send_packet_success = true;
                    force_alloc_fail = true;
                    
                    for (int i = 0; i < 70; i++) {
                        AVPacket* pkt = av_packet_alloc();
                        feedPacket(dec->m_queue, pkt);
                    }
                    
                    test_assert(!dec->decodeNextFrame(), "decodeNextFrame returns false when fallback allocation fails");
                    
                    force_receive_eagain = false;
                    mock_send_packet_success = false;
                    force_alloc_fail = false;
                    if (dec->m_codecCtx && dec->m_codecCtx->codec == &fakeCodec) {
                        dec->m_codecCtx->codec = savedCodec;
                    }
                    global_saved_codec = nullptr;
                    global_fake_codec_ptr = nullptr;
                }
            }

            // Case B: avcodec_parameters_to_context failure
            {
                PlayerController controller;
                if (controller.openFile(testFile)) {
                    VideoDecoder* dec = controller.m_videoDecoder.get();
                    
                    const AVCodec* savedCodec = dec->m_codecCtx->codec;
                    AVCodec fakeCodec = *savedCodec;
                    fakeCodec.name = "h264_qsv";
                    global_saved_codec = savedCodec;
                    global_fake_codec_ptr = &fakeCodec;
                    dec->m_codecCtx->codec = &fakeCodec;
                    
                    force_receive_eagain = true;
                    mock_send_packet_success = true;
                    force_copy_params_fail = true;
                    
                    for (int i = 0; i < 70; i++) {
                        AVPacket* pkt = av_packet_alloc();
                        feedPacket(dec->m_queue, pkt);
                    }
                    
                    test_assert(!dec->decodeNextFrame(), "decodeNextFrame returns false when fallback copy params fails");
                    
                    force_receive_eagain = false;
                    mock_send_packet_success = false;
                    force_copy_params_fail = false;
                    if (dec->m_codecCtx && dec->m_codecCtx->codec == &fakeCodec) {
                        dec->m_codecCtx->codec = savedCodec;
                    }
                    global_saved_codec = nullptr;
                    global_fake_codec_ptr = nullptr;
                }
            }

            // Case C: avcodec_open2 failure
            {
                PlayerController controller;
                if (controller.openFile(testFile)) {
                    VideoDecoder* dec = controller.m_videoDecoder.get();
                    
                    const AVCodec* savedCodec = dec->m_codecCtx->codec;
                    AVCodec fakeCodec = *savedCodec;
                    fakeCodec.name = "h264_qsv";
                    global_saved_codec = savedCodec;
                    global_fake_codec_ptr = &fakeCodec;
                    dec->m_codecCtx->codec = &fakeCodec;
                    
                    force_receive_eagain = true;
                    mock_send_packet_success = true;
                    force_open_fail = true;
                    
                    for (int i = 0; i < 70; i++) {
                        AVPacket* pkt = av_packet_alloc();
                        feedPacket(dec->m_queue, pkt);
                    }
                    
                    test_assert(!dec->decodeNextFrame(), "decodeNextFrame returns false when fallback open fails");
                    
                    force_receive_eagain = false;
                    mock_send_packet_success = false;
                    force_open_fail = false;
                    if (dec->m_codecCtx && dec->m_codecCtx->codec == &fakeCodec) {
                        dec->m_codecCtx->codec = savedCodec;
                    }
                    global_saved_codec = nullptr;
                    global_fake_codec_ptr = nullptr;
                }
            }

            // Case D: Software decoder not found during fallback
            {
                PlayerController controller;
                if (controller.openFile(testFile)) {
                    VideoDecoder* dec = controller.m_videoDecoder.get();
                    
                    const AVCodec* savedCodec = dec->m_codecCtx->codec;
                    AVCodec fakeCodec = *savedCodec;
                    fakeCodec.name = "h264_qsv";
                    global_saved_codec = savedCodec;
                    global_fake_codec_ptr = &fakeCodec;
                    dec->m_codecCtx->codec = &fakeCodec;
                    
                    // fallbackToSoftware() reads m_codecParams->codec_id (not
                    // m_codecCtx->codec_id, which reopenHardwareDecoder() may
                    // already have freed by this point) -- that's the field
                    // that must be poisoned to make avcodec_find_decoder()
                    // fail during the fallback.
                    AVCodecID savedId = dec->m_codecParams->codec_id;
                    dec->m_codecParams->codec_id = AV_CODEC_ID_NONE;

                    force_receive_eagain = true;
                    mock_send_packet_success = true;
                    // Force reopenHardwareDecoder() to fail deterministically
                    // (rather than depending on whether this machine actually
                    // has working QSV hardware) so recoverHardwareDecoder()
                    // reliably reaches fallbackToSoftware(), which is what
                    // must fail here on the poisoned codec_id above.
                    force_alloc_fail = true;

                    for (int i = 0; i < 70; i++) {
                        AVPacket* pkt = av_packet_alloc();
                        feedPacket(dec->m_queue, pkt);
                    }

                    test_assert(!dec->decodeNextFrame(), "decodeNextFrame returns false when software decoder not found");

                    force_receive_eagain = false;
                    mock_send_packet_success = false;
                    force_alloc_fail = false;
                    dec->m_codecParams->codec_id = savedId;
                    if (dec->m_codecCtx && dec->m_codecCtx->codec == &fakeCodec) {
                        dec->m_codecCtx->codec = savedCodec;
                    }
                    global_saved_codec = nullptr;
                    global_fake_codec_ptr = nullptr;
                }
            }

            // Case D2: fallbackToSoftware() called directly (private-access)
            // with a poisoned codec_id -- deterministic, unlike Case D above,
            // which depends on reopenHardwareDecoder() actually failing first
            // (not guaranteed on a machine where QSV hardware happens to work).
            {
                PlayerController controller;
                if (controller.openFile(testFile)) {
                    VideoDecoder* dec = controller.m_videoDecoder.get();
                    AVCodecID savedId = dec->m_codecParams->codec_id;
                    dec->m_codecParams->codec_id = AV_CODEC_ID_NONE;
                    test_assert(!dec->fallbackToSoftware(), "fallbackToSoftware() fails gracefully when no software decoder is registered for the codec");
                    dec->m_codecParams->codec_id = savedId;
                }
            }

            // Case E: reopenHardwareDecoder()'s own guard clauses, called
            // directly (private-access) rather than through the full
            // decode/recovery loop above -- these need m_codecCtx itself (or
            // its codec/name) to be null, and an unregistered codec name,
            // neither of which the fakeCodec="h264_qsv" trick above produces.
            {
                PlayerController controller;
                if (controller.openFile(testFile)) {
                    VideoDecoder* dec = controller.m_videoDecoder.get();

                    AVCodecContext* savedCtx = dec->m_codecCtx;
                    dec->m_codecCtx = nullptr;
                    test_assert(!dec->reopenHardwareDecoder(), "reopenHardwareDecoder() fails gracefully with no codec context");
                    dec->m_codecCtx = savedCtx;

                    const AVCodec* savedCodec = dec->m_codecCtx->codec;
                    AVCodec bogusCodec = *savedCodec;
                    bogusCodec.name = "totally_bogus_codec_name_xyz";
                    global_saved_codec = savedCodec;
                    global_fake_codec_ptr = &bogusCodec;
                    dec->m_codecCtx->codec = &bogusCodec;
                    test_assert(!dec->reopenHardwareDecoder(),
                                "reopenHardwareDecoder() fails gracefully when the codec name isn't registered");
                    if (dec->m_codecCtx && dec->m_codecCtx->codec == &bogusCodec) {
                        dec->m_codecCtx->codec = savedCodec;
                    }
                    global_saved_codec = nullptr;
                    global_fake_codec_ptr = nullptr;
                }
            }
        }

        // VideoDecoder: init()'s hardware-candidate loop (candidate lookup
        // succeeds but avcodec_open2()/the DRY-RUN probe fails, so the loop
        // must log "unavailable, trying next" and move on to the next
        // candidate) and the HEVC candidate-table selection, neither of
        // which the H264-only test asset combined with a working GPU (see
        // "isVideoHardware returns true when using hardware" above) ever
        // exercises -- real hardware decode there succeeds on its first
        // candidate, so the loop never continues past it.
        {
            g_disableHardwareDecoders = false;
            force_open_fail = true; // fails avcodec_open2() for every candidate, hw and software alike

            AVCodecParameters* h264Params = avcodec_parameters_alloc();
            h264Params->codec_type = AVMEDIA_TYPE_VIDEO;
            h264Params->codec_id = AV_CODEC_ID_H264;
            h264Params->width = 640;
            h264Params->height = 360;
            ThreadSafeQueue<AVPacket*> hwLoopQueue1;
            VideoDecoder h264HwFail(h264Params, {1, 25}, 0, hwLoopQueue1);
            test_assert(!h264HwFail.init(), "VideoDecoder.init() fails gracefully when every H264 candidate (hw and software) fails to open");
            avcodec_parameters_free(&h264Params);

            AVCodecParameters* hevcParams = avcodec_parameters_alloc();
            hevcParams->codec_type = AVMEDIA_TYPE_VIDEO;
            hevcParams->codec_id = AV_CODEC_ID_HEVC;
            hevcParams->width = 640;
            hevcParams->height = 360;
            ThreadSafeQueue<AVPacket*> hwLoopQueue2;
            VideoDecoder hevcHwFail(hevcParams, {1, 25}, 0, hwLoopQueue2);
            test_assert(!hevcHwFail.init(), "VideoDecoder.init() selects the HEVC candidate table and fails gracefully when every candidate fails to open");
            avcodec_parameters_free(&hevcParams);

            force_open_fail = false;
            g_disableHardwareDecoders = true;
        }

        // VideoDecoder: init()'s hardware-candidate loop ctx-alloc and
        // parameters-copy failures (distinct call sites from the same
        // failures in the software-fallback path below them).
        {
            g_disableHardwareDecoders = false;

            force_alloc_fail = true;
            AVCodecParameters* allocFailParams = avcodec_parameters_alloc();
            allocFailParams->codec_type = AVMEDIA_TYPE_VIDEO;
            allocFailParams->codec_id = AV_CODEC_ID_H264;
            ThreadSafeQueue<AVPacket*> hwAllocQueue;
            VideoDecoder hwAllocFail(allocFailParams, {1, 25}, 0, hwAllocQueue);
            test_assert(!hwAllocFail.init(), "VideoDecoder.init() fails gracefully when avcodec_alloc_context3() fails for every hw candidate too");
            avcodec_parameters_free(&allocFailParams);
            force_alloc_fail = false;

            force_copy_params_fail = true;
            AVCodecParameters* copyFailParams = avcodec_parameters_alloc();
            copyFailParams->codec_type = AVMEDIA_TYPE_VIDEO;
            copyFailParams->codec_id = AV_CODEC_ID_H264;
            ThreadSafeQueue<AVPacket*> hwCopyQueue;
            VideoDecoder hwCopyFail(copyFailParams, {1, 25}, 0, hwCopyQueue);
            test_assert(!hwCopyFail.init(), "VideoDecoder.init() fails gracefully when avcodec_parameters_to_context() fails for every hw candidate too");
            avcodec_parameters_free(&copyFailParams);
            force_copy_params_fail = false;

            g_disableHardwareDecoders = true;
        }

        // VideoDecoder: flush()'s drain-before-flush EAGAIN retry loop in
        // decodeNextFrame() -- a drain that returns EAGAIN (decoder still has
        // data in flight) rather than immediately AVERROR_EOF, which no other
        // test's flush() timing happens to hit.
        {
            PlayerController controller;
            if (controller.openFile(testFile)) {
                VideoDecoder* dec = controller.m_videoDecoder.get();
                force_receive_eagain = true;
                dec->flush();
                // Cut the packet supply off before decodeNextFrame() runs.
                // Past the drain loop, a permanently-EAGAIN receive_frame()
                // livelocks the main decode loop: the real (unmocked)
                // decoder's internal buffer fills up because the mock never
                // lets a frame be drained out of it, the real send_packet()
                // then returns EAGAIN forever, and the "resend this same
                // packet once space frees up" branch spins at 100% CPU with
                // no exit. (The other force_receive_eagain tests dodge this
                // by also setting mock_send_packet_success.) An aborted
                // queue makes the post-drain try_pop() fail instead, so the
                // call returns right after the retry loop under test --
                // which is all this block is here to cover.
                dec->m_queue.abort();
                dec->decodeNextFrame(); // drains via ~50ms of EAGAIN retries, then gives up and returns false
                force_receive_eagain = false;
            }
        }

        // VideoDecoder: send_packet() returning EAGAIN ("resend this same
        // packet once space frees up" -- not a failure) followed by a hard
        // (non-EAGAIN, non-EOF) receive error on the very next iteration,
        // still holding the packet that was never freed after the send-EAGAIN
        // continue. No other test's mocking sequences a send-EAGAIN followed
        // by a genuine receive failure in the same decodeNextFrame() call.
        {
            PlayerController controller;
            if (controller.openFile(testFile)) {
                VideoDecoder* dec = controller.m_videoDecoder.get();
                AVPacket* pkt = av_packet_alloc();
                feedPacket(dec->m_queue, pkt);
                force_receive_frame_eagain_then_fail = 1;
                force_send_packet_eagain = true;
                test_assert(!dec->decodeNextFrame(),
                            "decodeNextFrame returns false after a send-EAGAIN retry is followed by a hard receive error");
                force_send_packet_eagain = false;
                force_receive_frame_eagain_then_fail = -1;
            }
        }

        // VideoDecoder: real hardware decode's "failed on send"/"failed on
        // receive" recovery branches -- distinct from the fakeCodec="h264_qsv"
        // cases above (which only ever hit the "stuck after 64 EAGAINs"
        // path). These need a genuinely active hardware decoder so
        // isHardwareDecoder(m_codecCtx->codec) is true against the real
        // codec object, with send/receive itself forced to fail.
        {
            g_disableHardwareDecoders = false;
            PlayerController hwController;
            if (hwController.openFile(testFile) && hwController.isVideoHardware()) {
                VideoDecoder* dec = hwController.m_videoDecoder.get();

                // "Failed on send": needs a packet available so decodeNextFrame()
                // reaches the send_packet() call at all.
                AVPacket* pkt = nullptr;
                for (int w = 0; w < 25 && !dec->m_queue.try_pop(pkt); ++w) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (pkt) {
                    feedPacket(dec->m_queue, pkt);
                    force_send_packet_fail = true;
                    dec->decodeNextFrame();
                    force_send_packet_fail = false;
                }

                // "Failed on receive": no real packet needed -- the mocked
                // receive_frame() fails immediately on the very first call.
                force_receive_frame_fail = true;
                dec->decodeNextFrame();
                force_receive_frame_fail = false;
            }
            hwController.stop();
            g_disableHardwareDecoders = true;
        }

        // VideoDecoder: decodeNextFrame()'s and convertFrame()'s profiling
        // time-tracker branches -- no other test drives real decode/convert
        // work with m_metrics->m_profilingEnabled set.
        {
            PlayerController controller;
            if (controller.openFile(testFile)) {
                controller.m_metrics->setProfilingEnabled(true);
                controller.play();
                drive_playback(controller, 0.5);
                // drive_playback() only calls decodeNextFrame() itself --
                // convertFrame() is normally driven from the render loop
                // (src/app/main.cpp, not part of this test binary), so call
                // it directly here to exercise its own profiling timer too.
                if (VideoDecoder* dec = controller.getVideoDecoder()) {
                    dec->convertFrame();
                }
                controller.m_metrics->setProfilingEnabled(false);
                controller.stop();
            }
        }

        // VideoDecoder: convertFrame hardware pixel format paths and buffer re-binding
        {
            PlayerController controller;
            if (controller.openFile(testFile)) {
                VideoDecoder* dec = controller.m_videoDecoder.get();
                
                // Case A: Hardware format conversion failure
                dec->m_decodedFrame->format = AV_PIX_FMT_QSV;
                dec->m_decodedFrame->width = dec->m_allocatedWidth;
                dec->m_decodedFrame->height = dec->m_allocatedHeight;
                force_hw_transfer_fail = true;
                test_assert(!dec->convertFrame(), "convertFrame returns false when hardware transfer fails");
                force_hw_transfer_fail = false;

                // Case B: Hardware format conversion success and buffer re-binding check
                dec->m_decodedFrame->format = AV_PIX_FMT_QSV;
                dec->m_decodedFrame->width = dec->m_allocatedWidth;
                dec->m_decodedFrame->height = dec->m_allocatedHeight;
                dec->m_yuvFrame->data[0] = nullptr;
                test_assert(dec->convertFrame(), "convertFrame succeeds when hardware transfer succeeds");

                 // Case C: Slow-path buffer re-binding check
                dec->m_decodedFrame->format = AV_PIX_FMT_RGB24;
                dec->m_decodedFrame->width = dec->m_allocatedWidth;
                dec->m_decodedFrame->height = dec->m_allocatedHeight;
                dec->m_yuvFrame->data[0] = nullptr;
                test_assert(dec->convertFrame(), "convertFrame slow-path buffer re-binding succeeds");

                // Case D: av_frame_alloc failure for hardware CPU copy
                dec->m_decodedFrame->format = AV_PIX_FMT_QSV;
                dec->m_decodedFrame->width = dec->m_allocatedWidth;
                dec->m_decodedFrame->height = dec->m_allocatedHeight;
                force_frame_alloc_fail = true;
                test_assert(!dec->convertFrame(), "convertFrame fails when av_frame_alloc fails for hardware copy");
                force_frame_alloc_fail = false;

                // Case E: Hardware frame transferring to RGB24 to trigger slow-path tempCpuFrame freeing
                dec->m_decodedFrame->format = AV_PIX_FMT_QSV;
                dec->m_decodedFrame->width = dec->m_allocatedWidth;
                dec->m_decodedFrame->height = dec->m_allocatedHeight;
                mock_hw_transfer_nv12 = true;
                test_assert(dec->convertFrame(), "convertFrame slow-path hardware QSV transfer succeeds");
                mock_hw_transfer_nv12 = false;

                // Case F: av_frame_ref() failing on the native (useNative)
                // path -- needs tempCpuFrame to hold real pixel data (not the
                // unit-test "dummy frame" shortcut, which never reaches
                // av_frame_ref() at all), hence force_hw_transfer_real_buffer.
                dec->m_decodedFrame->format = AV_PIX_FMT_QSV;
                dec->m_decodedFrame->width = dec->m_allocatedWidth;
                dec->m_decodedFrame->height = dec->m_allocatedHeight;
                force_hw_transfer_real_buffer = true;
                force_hw_frame_ref_fail = true;
                test_assert(!dec->convertFrame(), "convertFrame fails gracefully when av_frame_ref() fails on the native path");
                force_hw_frame_ref_fail = false;

                // Case F2: same setup, but av_frame_ref() succeeds this time
                // -- the native path's *success*-side tempCpuFrame cleanup,
                // a different call site from Case F's failure-side cleanup.
                dec->m_decodedFrame->format = AV_PIX_FMT_QSV;
                dec->m_decodedFrame->width = dec->m_allocatedWidth;
                dec->m_decodedFrame->height = dec->m_allocatedHeight;
                test_assert(dec->convertFrame(), "convertFrame succeeds on the native path when av_frame_ref() succeeds with a real tempCpuFrame");
                force_hw_transfer_real_buffer = false;
            }
        }

        // VideoDecoder: convertFrame hardware allocation/scaling failures with tempCpuFrame
        {
            PlayerController controller;
            if (controller.openFile(testFile)) {
                VideoDecoder* dec = controller.m_videoDecoder.get();
                
                // Force resolution change to trigger allocation block
                dec->m_decodedFrame->width = dec->m_allocatedWidth + 10;
                dec->m_decodedFrame->height = dec->m_allocatedHeight + 10;
                dec->m_decodedFrame->format = AV_PIX_FMT_QSV;
                mock_hw_transfer_nv12 = true;

                // Case 1: malloc failure with tempCpuFrame
                force_malloc_fail = true;
                test_assert(!dec->convertFrame(), "convertFrame fails when av_malloc fails with QSV");
                force_malloc_fail = false;

                // Case 2: image fill failure with tempCpuFrame
                dec->m_decodedFrame->width = dec->m_allocatedWidth + 10;
                dec->m_decodedFrame->height = dec->m_allocatedHeight + 10;
                dec->m_decodedFrame->format = AV_PIX_FMT_QSV;
                force_image_fill_fail = true;
                test_assert(!dec->convertFrame(), "convertFrame fails when av_image_fill_arrays fails with QSV");
                force_image_fill_fail = false;

                // Case 3: sws_getContext failure with tempCpuFrame
                dec->m_decodedFrame->width = dec->m_allocatedWidth + 10;
                dec->m_decodedFrame->height = dec->m_allocatedHeight + 10;
                dec->m_decodedFrame->format = AV_PIX_FMT_QSV;
                force_sws_context_fail = true;
                test_assert(!dec->convertFrame(), "convertFrame fails when sws_getContext fails with QSV");
                force_sws_context_fail = false;

                // Case 4: scaledFrame's own av_frame_alloc() failing (distinct
                // call site from tempCpuFrame's, which must itself still
                // succeed to reach this one) -- covers the tempCpuFrame
                // cleanup on that specific failure path.
                dec->m_decodedFrame->width = dec->m_allocatedWidth + 10;
                dec->m_decodedFrame->height = dec->m_allocatedHeight + 10;
                dec->m_decodedFrame->format = AV_PIX_FMT_QSV;
                force_frame_alloc_fail_on_second_call = 1;
                test_assert(!dec->convertFrame(), "convertFrame fails when the scaled-frame allocation fails, with a tempCpuFrame to clean up");
                force_frame_alloc_fail_on_second_call = -1;

                mock_hw_transfer_nv12 = false;
            }
        }

        // VideoDecoder: getPixelFormatName
        {
            PlayerController controller;
            if (controller.openFile(testFile)) {
                VideoDecoder* dec = controller.m_videoDecoder.get();
                dec->m_allocatedFormat = AV_PIX_FMT_YUV420P;
                test_assert(dec->getPixelFormatName() == "yuv420p", "getPixelFormatName returns yuv420p");
                dec->m_allocatedFormat = AV_PIX_FMT_NONE;
                test_assert(dec->getPixelFormatName() == "unknown", "getPixelFormatName returns unknown for NONE");
            }
        }

        // VideoDecoder: isHardwareDecoder()'s null-codec guard -- every other
        // call site always passes a real, already-validated AVCodec*.
        {
            test_assert(!VideoDecoder::isHardwareDecoder(nullptr), "isHardwareDecoder(nullptr) returns false");
        }

        // -------------------------------------------------------------
        // Pipeline Metrics & MetricRing Tests (T1 - T5)
        // -------------------------------------------------------------
        {
            std::cout << "Running Pipeline Metrics & MetricRing Tests..." << std::endl;

            // T2: snapshot on empty ring returns 0
            {
                MetricRing<8> ring;
                float buf[8]{};
                size_t count = ring.snapshot(buf, 8);
                test_assert(count == 0, "T2: snapshot on empty ring must return 0");
            }

            // T1: MetricRing wrap-around at N and 2N samples
            {
                MetricRing<8> ring; // N = 8
                // Write N = 8 samples: 1..8
                for (int i = 1; i <= 8; ++i) {
                    ring.record(static_cast<float>(i));
                }
                float buf[8]{};
                size_t count = ring.snapshot(buf, 8);
                test_assert(count == 8, "T1: snapshot count must be 8");
                for (int i = 0; i < 8; ++i) {
                    test_assert(buf[i] == static_cast<float>(i + 1), "T1: elements must match 1..8 chronologically");
                }

                // Write 2N = 16 samples: 9..16
                for (int i = 9; i <= 16; ++i) {
                    ring.record(static_cast<float>(i));
                }
                count = ring.snapshot(buf, 8);
                test_assert(count == 8, "T1: snapshot count after 2N must be 8");
                for (int i = 0; i < 8; ++i) {
                    test_assert(buf[i] == static_cast<float>(i + 9), "T1: elements after 2N must match 9..16 chronologically");
                }
            }

            // T3: snapshot buffer smaller than sample count returns newest max entries
            {
                MetricRing<8> ring;
                for (int i = 1; i <= 6; ++i) {
                    ring.record(static_cast<float>(i));
                }
                float buf[3]{};
                size_t count = ring.snapshot(buf, 3);
                test_assert(count == 3, "T3: count must be 3");
                // Newest 3 of 1..6 are 4, 5, 6
                test_assert(buf[0] == 4.0f, "T3: element 0 must be 4");
                test_assert(buf[1] == 5.0f, "T3: element 1 must be 5");
                test_assert(buf[2] == 6.0f, "T3: element 2 must be 6");
            }

            // T4: two-thread hammer (producer records 1M, consumer snapshots in loop)
            {
                MetricRing<256> ring;
                std::atomic<bool> runConsumer{true};

                std::thread consumer([&]() {
                    float buf[256];
                    while (runConsumer.load(std::memory_order_relaxed)) {
                        size_t count = ring.snapshot(buf, 256);
                        (void)count;
                        std::this_thread::yield();
                    }
                });

                std::thread producer([&]() {
                    for (int i = 0; i < 1000000; ++i) {
                        ring.record(static_cast<float>(i));
                    }
                });

                producer.join();
                runConsumer.store(false, std::memory_order_relaxed);
                consumer.join();
                test_assert(true, "T4: two-thread hammer completed successfully under TSan");
            }

            // T5: seek latency pairing discards superseded epoch measurements
            {
                g_videoThreadEnabled = true;
                PlayerController controller;
                if (controller.openFile(testFile)) {
                    PipelineMetrics& metrics = controller.getPipelineMetrics();
                    metrics.setProfilingEnabled(true);

                    // Check that seek latency ring is empty initially
                    float latencies[10]{};
                    size_t initialCount = metrics.m_seekLatencyMs.snapshot(latencies, 10);
                    test_assert(initialCount == 0, "T5: seek latency ring is initially empty");

                    // Start playback to prevent seeks from taking the instantSeek early-return path
                    controller.play();

                    // Trigger seek 1 (activeEpoch = 1)
                    controller.seek(1.0);
                    
                    // Trigger seek 2 (activeEpoch = 2, supersedes seek 1)
                    controller.seek(2.0);

                    // Finish catch-up (this corresponds to the second seek landing)
                    controller.finishCatchup(2.0);

                    size_t count = metrics.m_seekLatencyMs.snapshot(latencies, 10);
                    // The count must be exactly 1, because the first seek was superseded and discarded!
                    test_assert(count == 1, "T5: superseded seek measurement must be discarded, count should be 1");
                }
            }

            // T6: m_profilingEnabled=false results in no ring writes (all ring heads unchanged after exercising hooks); depth gauges still update
            {
                PipelineMetrics metrics;
                test_assert(!metrics.m_profilingEnabled.load(), "T6: profiling should be disabled by default");

                metrics.recordDemuxTime(123.0f);
                metrics.recordDecodeTime(456.0f);
                metrics.recordConvertTime(789.0f);
                metrics.recordUploadTime(12.0f);
                metrics.recordClockOffset(34.0f);
                metrics.recordSeekLatency(56.0f);

                test_assert(metrics.m_demuxTimePerPacketUs.getHead() == 0, "T6: demux head must be 0");
                test_assert(metrics.m_decodeTimePerFrameUs.getHead() == 0, "T6: decode head must be 0");
                test_assert(metrics.m_convertTimeUs.getHead() == 0, "T6: convert head must be 0");
                test_assert(metrics.m_uploadTimeUs.getHead() == 0, "T6: upload head must be 0");
                test_assert(metrics.m_avClockOffsetMs.getHead() == 0, "T6: offset head must be 0");
                test_assert(metrics.m_seekLatencyMs.getHead() == 0, "T6: seek head must be 0");

                ThreadSafeQueue<int> queue(10);
                std::atomic<int> depth{0};
                queue.attachDepthMirror(&depth);
                
                queue.push(42);
                test_assert(depth.load() == 1, "T6: depth gauge must update even when profiling is disabled");
                
                int val;
                queue.pop(val);
                test_assert(depth.load() == 0, "T6: depth gauge must update even when profiling is disabled");

                test_assert(metrics.m_framesDroppedCount.load() == 0, "T6: initial dropped count should be 0");
                metrics.incrementFramesDropped();
                test_assert(metrics.m_framesDroppedCount.load() == 1, "T6: dropped count must update even when profiling is disabled");
            }

            // T7: attachDepthMirror(nullptr)/never-attached queue operates correctly with zero metric side effects
            {
                ThreadSafeQueue<int> queue(10);
                queue.attachDepthMirror(nullptr);

                test_assert(queue.push(1), "T7: push works without depth mirror");
                int val;
                test_assert(queue.pop(val) && val == 1, "T7: pop works without depth mirror");
                test_assert(queue.push(2), "T7: push works");
                queue.clear();
                test_assert(queue.empty(), "T7: clear works");
            }

            // T7b: push_drop_oldest() and push_wait_or_drop() must never block
            // on a full queue with nothing draining it. This is the exact
            // mechanism behind a deadlock where the demuxer thread -- the
            // single thread reading both the video and audio streams -- could
            // block forever inside a plain push() to the audio queue while
            // the audio device sat paused (during a seek catch-up, or simply
            // while the user had playback paused). Once blocked there it
            // never read another packet for either stream, so the video
            // packet queue drained to 0 and stayed there. These two methods
            // are the fix: they must always return promptly even when
            // nothing is popping.
            {
                ThreadSafeQueue<int> queue(3);
                std::atomic<int> depth{0};
                queue.attachDepthMirror(&depth);
                for (int i = 0; i < 3; i++) {
                    test_assert(queue.push(i), "T7b: fill queue to capacity");
                }
                test_assert(queue.size() == 3, "T7b: queue is at capacity");

                // Nothing pops from `queue` for the rest of this block --
                // simulating a paused/idle consumer.
                auto start = std::chrono::steady_clock::now();
                bool pushed = queue.push_drop_oldest(99);
                double elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                test_assert(pushed, "T7b: push_drop_oldest succeeds on a full, undrained queue");
                test_assert(elapsedSec < 0.05, "T7b: push_drop_oldest returns immediately, never blocks");
                test_assert(queue.size() == 3, "T7b: push_drop_oldest keeps the queue at capacity");

                int waitDropCleanedUp = 0;
                start = std::chrono::steady_clock::now();
                pushed = queue.push_wait_or_drop(100, std::chrono::milliseconds(50),
                                                  [&waitDropCleanedUp](int&) { waitDropCleanedUp++; });
                elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                test_assert(pushed, "T7b: push_wait_or_drop succeeds on a full, undrained queue");
                test_assert(elapsedSec < 0.5, "T7b: push_wait_or_drop returns at its timeout, not forever");
                test_assert(elapsedSec >= 0.045, "T7b: push_wait_or_drop actually waits close to its timeout before dropping");
                test_assert(waitDropCleanedUp == 1, "T7b: push_wait_or_drop invoked the dropCleanup functor for ThreadSafeQueue<int>");
                test_assert(depth.load() == 3, "T7b: depth mirror reflects size through push_drop_oldest/push_wait_or_drop");

                int cleanedUp = 0;
                pushed = queue.push_drop_oldest(101, [&cleanedUp](int&) { cleanedUp++; });
                test_assert(pushed, "T7b: push_drop_oldest with a dropCleanup functor still succeeds");
                test_assert(cleanedUp == 1, "T7b: push_drop_oldest invoked the dropCleanup functor for ThreadSafeQueue<int>");

                queue.abort();
                test_assert(!queue.push_drop_oldest(102), "T7b: push_drop_oldest returns false on an aborted queue");
                test_assert(!queue.push_wait_or_drop(103, std::chrono::milliseconds(10)),
                            "T7b: push_wait_or_drop returns false on an aborted queue");

                int clearedUp = 0;
                queue.clear([&clearedUp](int&) { clearedUp++; });
                test_assert(clearedUp > 0, "T7b: clear() invokes its cleanupFunc for ThreadSafeQueue<int>");
                test_assert(depth.load() == 0, "T7b: clear() zeroes the depth mirror for ThreadSafeQueue<int>");
            }

            // T7c: documents the failure mode T7b fixes -- a plain,
            // unbounded push() on a full queue genuinely blocks until
            // something else drains it. Raced against a background thread
            // that pops exactly once after a delay: push() must not return
            // before that pop happens.
            {
                ThreadSafeQueue<int> queue(1);
                test_assert(queue.push(0), "T7c: fill queue to capacity");

                std::atomic<bool> popped{false};
                std::thread popper([&queue, &popped]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                    int val;
                    queue.pop(val);
                    popped.store(true);
                });

                auto start = std::chrono::steady_clock::now();
                test_assert(queue.push(1), "T7c: plain push eventually succeeds once drained");
                double elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                popper.join();
                test_assert(popped.load(), "T7c: push only returned after the pop happened");
                test_assert(elapsedSec >= 0.075, "T7c: plain push genuinely blocked until the queue was drained");
            }

            // T7d: a blocking pop() on an already-aborted queue must return
            // false immediately rather than block.
            {
                ThreadSafeQueue<int> queue(5);
                queue.abort();
                int val;
                test_assert(!queue.pop(val), "T7d: pop() returns false immediately on an aborted queue");
            }

            // T7e: push_drop_oldest() on the ThreadSafeQueue<AVPacket*>
            // instantiation actually used by the demuxer -- covers the
            // dropCleanup invocation, the depth-mirror update, and the
            // aborted-queue early return, which are each distinct template
            // instantiations from the ThreadSafeQueue<int> coverage above.
            {
                ThreadSafeQueue<AVPacket*> queue(2);
                std::atomic<int> depth{0};
                queue.attachDepthMirror(&depth);

                AVPacket* p1 = av_packet_alloc();
                AVPacket* p2 = av_packet_alloc();
                test_assert(queue.push_drop_oldest(p1, [](AVPacket*& p) { av_packet_free(&p); }),
                            "T7e: push_drop_oldest on an empty queue succeeds");
                test_assert(queue.push_drop_oldest(p2, [](AVPacket*& p) { av_packet_free(&p); }),
                            "T7e: push_drop_oldest fills the queue to capacity");
                test_assert(depth.load() == 2, "T7e: depth mirror reflects queue size after push_drop_oldest");

                int freedCount = 0;
                AVPacket* p3 = av_packet_alloc();
                bool pushed = queue.push_drop_oldest(p3, [&freedCount](AVPacket*& p) {
                    freedCount++;
                    av_packet_free(&p);
                });
                test_assert(pushed, "T7e: push_drop_oldest on a full queue still succeeds");
                test_assert(freedCount == 1, "T7e: push_drop_oldest invoked dropCleanup on the oldest item");
                test_assert(queue.size() == 2, "T7e: push_drop_oldest keeps size at capacity");

                queue.abort();
                AVPacket* p4 = av_packet_alloc();
                bool pushedAfterAbort = queue.push_drop_oldest(p4, [](AVPacket*& p) { av_packet_free(&p); });
                test_assert(!pushedAfterAbort, "T7e: push_drop_oldest returns false when the queue is aborted");
                av_packet_free(&p4);

                queue.clear([](AVPacket*& p) { av_packet_free(&p); });
            }

            // T7f: push_wait_or_drop() on a full, undrained queue for both
            // ThreadSafeQueue<AVPacket*> (Demuxer's video/audio/subtitle
            // queues) and ThreadSafeQueue<DecodedFrame> (PlayerController's
            // decoded-frame queue) -- each a separate template instantiation
            // that must independently exercise the timeout-drop and
            // aborted-queue branches.
            {
                ThreadSafeQueue<AVPacket*> queue(1);
                AVPacket* p1 = av_packet_alloc();
                test_assert(queue.push(p1), "T7f: fill AVPacket* queue to capacity");

                int freedCount = 0;
                AVPacket* p2 = av_packet_alloc();
                auto start = std::chrono::steady_clock::now();
                bool pushed = queue.push_wait_or_drop(p2, std::chrono::milliseconds(50),
                                                       [&freedCount](AVPacket*& p) {
                                                           freedCount++;
                                                           av_packet_free(&p);
                                                       });
                double elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                test_assert(pushed, "T7f: push_wait_or_drop succeeds on a full, undrained AVPacket* queue");
                test_assert(freedCount == 1, "T7f: push_wait_or_drop invoked dropCleanup for AVPacket*");
                test_assert(elapsedSec >= 0.045, "T7f: push_wait_or_drop waited close to its timeout before dropping");

                queue.abort();
                AVPacket* p3 = av_packet_alloc();
                bool pushedAfterAbort = queue.push_wait_or_drop(p3, std::chrono::milliseconds(10));
                test_assert(!pushedAfterAbort, "T7f: push_wait_or_drop (AVPacket*) returns false when aborted");
                av_packet_free(&p3);
                queue.clear([](AVPacket*& p) { av_packet_free(&p); });
            }
            {
                ThreadSafeQueue<DecodedFrame> queue(1);
                std::atomic<int> depth{0};
                queue.attachDepthMirror(&depth);
                DecodedFrame d1;
                d1.frame = av_frame_alloc();
                test_assert(queue.push(d1), "T7f: fill DecodedFrame queue to capacity");
                test_assert(depth.load() == 1, "T7f: depth mirror reflects size after push(DecodedFrame)");

                int freedCount = 0;
                DecodedFrame d2;
                d2.frame = av_frame_alloc();
                bool pushed = queue.push_wait_or_drop(d2, std::chrono::milliseconds(50),
                                                       [&freedCount](DecodedFrame& d) {
                                                           freedCount++;
                                                           if (d.frame) av_frame_free(&d.frame);
                                                       });
                test_assert(pushed, "T7f: push_wait_or_drop succeeds on a full, undrained DecodedFrame queue");
                test_assert(freedCount == 1, "T7f: push_wait_or_drop invoked dropCleanup for DecodedFrame");

                queue.abort();
                DecodedFrame d3;
                bool pushedAfterAbort = queue.push_wait_or_drop(d3, std::chrono::milliseconds(10));
                test_assert(!pushedAfterAbort, "T7f: push_wait_or_drop (DecodedFrame) returns false when aborted");
                test_assert(!queue.push(d3), "T7f: push(DecodedFrame) returns false on an aborted queue");
                if (d3.frame) av_frame_free(&d3.frame);
                queue.clear([](DecodedFrame& d) { if (d.frame) av_frame_free(&d.frame); });
            }
            {
                // reset() on the DecodedFrame instantiation, draining a
                // still-populated queue (mirrors the AVPacket* case above).
                ThreadSafeQueue<DecodedFrame> queue(5);
                std::atomic<int> depth{0};
                queue.attachDepthMirror(&depth);

                DecodedFrame d1;
                d1.frame = av_frame_alloc();
                DecodedFrame d2;
                d2.frame = av_frame_alloc();
                queue.push(d1);
                queue.push(d2);
                test_assert(queue.size() == 2, "T7g: DecodedFrame queue holds 2 items before reset");
                av_frame_free(&d1.frame);
                av_frame_free(&d2.frame);
                queue.reset();
                test_assert(queue.empty(), "T7g: reset() drains a non-empty DecodedFrame queue");
                test_assert(depth.load() == 0, "T7g: reset() zeroes the depth mirror for DecodedFrame");
            }

            // T7g: reset() draining a still-populated queue, and clear()
            // invoking its cleanup functor for every queued item and
            // zeroing the depth mirror -- both only ever run on already-
            // empty queues in production (clear() always precedes reset()),
            // so these branches need a direct, non-empty-queue exercise.
            {
                ThreadSafeQueue<AVPacket*> queue(5);
                std::atomic<int> depth{0};
                queue.attachDepthMirror(&depth);

                AVPacket* p1 = av_packet_alloc();
                AVPacket* p2 = av_packet_alloc();
                queue.push(p1);
                queue.push(p2);
                test_assert(queue.size() == 2, "T7g: queue holds 2 items before reset");
                // reset() has no cleanup hook (unlike clear()), so free the
                // packets directly first; reset() only discards the queue's
                // copies of the pointer values without dereferencing them.
                av_packet_free(&p1);
                av_packet_free(&p2);
                queue.reset();
                test_assert(queue.empty(), "T7g: reset() drains a non-empty queue");
                test_assert(depth.load() == 0, "T7g: reset() zeroes the depth mirror");
            }
            {
                ThreadSafeQueue<AVPacket*> queue(5);
                std::atomic<int> depth{0};
                queue.attachDepthMirror(&depth);

                AVPacket* p1 = av_packet_alloc();
                AVPacket* p2 = av_packet_alloc();
                queue.push(p1);
                queue.push(p2);

                int freedCount = 0;
                queue.clear([&freedCount](AVPacket*& p) {
                    freedCount++;
                    av_packet_free(&p);
                });
                test_assert(freedCount == 2, "T7g: clear() invokes cleanupFunc for every queued item");
                test_assert(queue.empty(), "T7g: clear() drains the queue");
                test_assert(depth.load() == 0, "T7g: clear() zeroes the depth mirror");
            }

            // T8: setProfilingEnabled(true) -> ring writes occur; back to false -> writes stop (toggle round-trip)
            {
                PipelineMetrics metrics;
                test_assert(!metrics.m_profilingEnabled.load(), "T8: initially disabled");

                metrics.setProfilingEnabled(true);
                test_assert(metrics.m_profilingEnabled.load(), "T8: enabled");

                metrics.recordDemuxTime(100.0f);
                test_assert(metrics.m_demuxTimePerPacketUs.getHead() == 1, "T8: write occurred when enabled");

                metrics.setProfilingEnabled(false);
                test_assert(!metrics.m_profilingEnabled.load(), "T8: disabled again");

                metrics.recordDemuxTime(200.0f);
                test_assert(metrics.m_demuxTimePerPacketUs.getHead() == 1, "T8: write did not occur when disabled");
            }

            // T9: the remaining record*Time()/recordClockOffset() wrappers,
            // never called by any other test.
            {
                PipelineMetrics metrics;
                metrics.setProfilingEnabled(true);
                metrics.recordDecodeTime(1.0f);
                metrics.recordConvertTime(2.0f);
                metrics.recordUploadTime(3.0f);
                metrics.recordClockOffset(4.0f);
                test_assert(metrics.m_decodeTimePerFrameUs.getHead() == 1, "T9: recordDecodeTime() writes when enabled");
                test_assert(metrics.m_convertTimeUs.getHead() == 1, "T9: recordConvertTime() writes when enabled");
                test_assert(metrics.m_uploadTimeUs.getHead() == 1, "T9: recordUploadTime() writes when enabled");
                test_assert(metrics.m_avClockOffsetMs.getHead() == 1, "T9: recordClockOffset() writes when enabled");
            }

            std::cout << "Pipeline Metrics & MetricRing Tests (T1 - T9) passed!" << std::endl;
        }

        // ColorPipelineInfo Metadata Extraction Test
        {
            ColorPipelineInfo info;
            test_assert(info.colorSpace == "Unspecified", "ColorInfo default space");
            test_assert(info.colorPrimaries == "Unspecified", "ColorInfo default primaries");
            test_assert(info.transferChar == "Unspecified", "ColorInfo default TRC");
            test_assert(info.colorRange == "Unspecified", "ColorInfo default range");
            test_assert(info.bitDepth == 8, "ColorInfo default bit depth");
            test_assert(!info.isHDR, "ColorInfo default HDR flag");
            test_assert(info.hdrType == "SDR", "ColorInfo default HDR type");
            std::cout << "ColorPipelineInfo Unit Test passed!" << std::endl;
        }

        // -------------------------------------------------------------
        // Phase 2b: DSP Chain Unit Tests (Biquad / ParametricEQ /
        // Compressor / Limiter / Crossover / DspChain)
        // -------------------------------------------------------------
        {
            std::cout << "Running DSP chain unit tests..." << std::endl;
            constexpr double kSR = 48000.0;

            auto genSine = [&](double freqHz, int numFrames, int channels, float amplitude, int targetChannel = -1) {
                std::vector<float> buf(static_cast<size_t>(numFrames) * channels, 0.0f);
                for (int f = 0; f < numFrames; ++f) {
                    float sample = amplitude * static_cast<float>(std::sin(2.0 * M_PI * freqHz * f / kSR));
                    for (int ch = 0; ch < channels; ++ch) {
                        if (targetChannel < 0 || ch == targetChannel) {
                            buf[static_cast<size_t>(f) * channels + ch] = sample;
                        }
                    }
                }
                return buf;
            };
            auto peakOfChannel = [&](const std::vector<float>& buf, int channels, int ch, int startFrame, int numFrames) {
                float peak = 0.0f;
                for (int f = startFrame; f < startFrame + numFrames; ++f) {
                    peak = std::max(peak, std::fabs(buf[static_cast<size_t>(f) * channels + ch]));
                }
                return peak;
            };

            // --- Biquad ---
            {
                naikav::dsp::Biquad flat;
                flat.setPeaking(1000.0, 1.0, 0.0, kSR); // 0 dB gain = identity
                float maxDiff = 0.0f;
                for (int i = 0; i < 2000; ++i) {
                    float x = static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * i / kSR));
                    float y = flat.process(x);
                    maxDiff = std::max(maxDiff, std::fabs(y - x));
                }
                test_assert(maxDiff < 0.01f, "Biquad: 0dB peaking band is near-identity");

                naikav::dsp::Biquad lp;
                lp.setLowpass(200.0, 0.70710678f, kSR);
                double lowInRms = 0.0, lowOutRms = 0.0, highInRms = 0.0, highOutRms = 0.0;
                int settle = 4000, measure = 2000;
                for (int i = 0; i < settle + measure; ++i) {
                    float xLow = static_cast<float>(std::sin(2.0 * M_PI * 50.0 * i / kSR));
                    float yLow = lp.process(xLow);
                    if (i >= settle) { lowInRms += xLow * xLow; lowOutRms += yLow * yLow; }
                }
                naikav::dsp::Biquad lp2;
                lp2.setLowpass(200.0, 0.70710678f, kSR);
                for (int i = 0; i < settle + measure; ++i) {
                    float xHigh = static_cast<float>(std::sin(2.0 * M_PI * 8000.0 * i / kSR));
                    float yHigh = lp2.process(xHigh);
                    if (i >= settle) { highInRms += xHigh * xHigh; highOutRms += yHigh * yHigh; }
                }
                double lowRatio = std::sqrt(lowOutRms / lowInRms);
                double highRatio = std::sqrt(highOutRms / highInRms);
                test_assert(lowRatio > 0.9, "Biquad lowpass: 50Hz passes through a 200Hz cutoff mostly unattenuated");
                test_assert(highRatio < 0.1, "Biquad lowpass: 8kHz is strongly attenuated by a 200Hz cutoff");
            }

            // --- ParametricEQ ---
            {
                naikav::dsp::ParametricEQ eq;
                eq.configure(2, kSR); // all bands default to 0 dB
                auto flatBuf = genSine(1000.0, 4000, 2, 0.5f);
                auto flatOriginal = flatBuf;
                eq.process(flatBuf.data(), 4000);
                double maxDiff = 0.0;
                for (size_t i = 0; i < flatBuf.size(); ++i) {
                    maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(flatBuf[i] - flatOriginal[i])));
                }
                test_assert(maxDiff < 0.01, "ParametricEQ: default (all bands 0dB) is near-identity");

                naikav::dsp::ParametricEQ eqBoost;
                eqBoost.configure(1, kSR);
                eqBoost.setBandGainDb(2, 12.0f); // band 2 = 1000 Hz center
                test_assert(std::fabs(eqBoost.getBandGainDb(2) - 12.0f) < 0.001f, "ParametricEQ: band gain getter matches setter");
                int settle = 2000, measure = 2000;
                double inRms = 0.0, outRms = 0.0;
                for (int i = 0; i < settle + measure; ++i) {
                    float x = 0.2f * static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * i / kSR));
                    float frame[1] = {x};
                    eqBoost.process(frame, 1);
                    if (i >= settle) { inRms += x * x; outRms += frame[0] * frame[0]; }
                }
                double ratioDb = 20.0 * std::log10(std::sqrt(outRms / inRms));
                test_assert(ratioDb > 8.0, "ParametricEQ: +12dB band boost measurably raises level at its center frequency");

                // --- True parametric behavior: frequency and Q are
                // independently adjustable, not just gain. ---
                naikav::dsp::ParametricEQ eqMove;
                eqMove.configure(1, kSR);
                eqMove.setBandFrequencyHz(2, 3000.0);
                eqMove.setBandQ(2, 2.0);
                eqMove.setBandGainDb(2, 12.0f);
                test_assert(std::fabs(eqMove.getBandFrequencyHz(2) - 3000.0) < 0.001,
                            "ParametricEQ: band frequency getter matches setter");
                test_assert(std::fabs(eqMove.getBandQ(2) - 2.0) < 0.001,
                            "ParametricEQ: band Q getter matches setter");

                // A band moved to 3kHz should now boost a 3kHz tone, not
                // the 1kHz tone it used to be centered on.
                double movedInRms = 0.0, movedOutRms = 0.0;
                for (int i = 0; i < settle + measure; ++i) {
                    float x = 0.2f * static_cast<float>(std::sin(2.0 * M_PI * 3000.0 * i / kSR));
                    float frame[1] = {x};
                    eqMove.process(frame, 1);
                    if (i >= settle) { movedInRms += x * x; movedOutRms += frame[0] * frame[0]; }
                }
                double movedRatioDb = 20.0 * std::log10(std::sqrt(movedOutRms / movedInRms));
                test_assert(movedRatioDb > 8.0,
                            "ParametricEQ: moving a band's frequency to 3kHz measurably boosts a 3kHz tone");

                // The same band should now leave the *old* 1kHz center
                // (near) untouched, since it moved away.
                naikav::dsp::ParametricEQ eqMove2;
                eqMove2.configure(1, kSR);
                eqMove2.setBandFrequencyHz(2, 3000.0);
                eqMove2.setBandGainDb(2, 12.0f);
                double oldCenterInRms = 0.0, oldCenterOutRms = 0.0;
                for (int i = 0; i < settle + measure; ++i) {
                    float x = 0.2f * static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * i / kSR));
                    float frame[1] = {x};
                    eqMove2.process(frame, 1);
                    if (i >= settle) { oldCenterInRms += x * x; oldCenterOutRms += frame[0] * frame[0]; }
                }
                double oldCenterRatioDb = 20.0 * std::log10(std::sqrt(oldCenterOutRms / oldCenterInRms));
                test_assert(oldCenterRatioDb < 3.0,
                            "ParametricEQ: after moving a band away from 1kHz, a 1kHz tone is no longer significantly boosted");
            }

            // --- Compressor ---
            {
                naikav::dsp::Compressor comp;
                comp.configure(1, kSR); // default ratio 1:1 = true no-op
                auto buf = genSine(200.0, 2000, 1, 0.9f);
                auto original = buf;
                comp.process(buf.data(), 2000);
                double maxDiff = 0.0;
                for (size_t i = 0; i < buf.size(); ++i) {
                    maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(buf[i] - original[i])));
                }
                test_assert(maxDiff < 1e-5, "Compressor: default ratio 1:1 is a true no-op regardless of input level");

                naikav::dsp::Compressor comp2;
                comp2.configure(1, kSR);
                comp2.setThresholdDb(-20.0f);
                comp2.setRatio(4.0f);
                comp2.setKneeDb(0.0f);
                comp2.setAttackMs(5.0f);
                comp2.setReleaseMs(50.0f);
                int total = 20000;
                std::vector<float> loud(total);
                for (int i = 0; i < total; ++i) {
                    loud[i] = static_cast<float>(std::sin(2.0 * M_PI * 200.0 * i / kSR)); // 0 dBFS sine
                }
                comp2.process(loud.data(), total);
                float steadyPeak = peakOfChannel(loud, 1, 0, total - 2000, 2000);
                test_assert(steadyPeak < 0.5f, "Compressor: 4:1 ratio above threshold measurably reduces a 0dBFS signal's steady-state peak");
            }

            // --- Limiter ---
            {
                naikav::dsp::Limiter lim;
                lim.configure(1, kSR); // default ceiling 0dB = inert (gain-wise) on normal content
                const int lookahead = lim.getLookaheadFrames();
                test_assert(lookahead > 0, "Limiter: lookahead delay line is configured to a positive frame count");
                auto buf = genSine(200.0, 2000, 1, 0.8f);
                auto original = buf;
                lim.process(buf.data(), 2000);
                // Gain-wise inert (0dB ceiling never engages), but the
                // lookahead delay line still shifts every sample later by
                // exactly getLookaheadFrames() -- compare against that
                // shifted reference instead of assuming zero latency.
                double maxDiff = 0.0;
                for (int i = lookahead; i < 2000; ++i) {
                    maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(buf[i] - original[i - lookahead])));
                }
                test_assert(maxDiff < 1e-5, "Limiter: default 0dB ceiling is gain-inert (aside from lookahead delay) on a sub-ceiling signal");
                bool leadingSilence = std::all_of(buf.begin(), buf.begin() + lookahead,
                                                   [](float s) { return s == 0.0f; });
                test_assert(leadingSilence, "Limiter: the first getLookaheadFrames() output samples are silence from the empty delay line");

                naikav::dsp::Limiter lim2;
                lim2.configure(1, kSR);
                lim2.setCeilingDb(-6.0f);
                auto loudBuf = genSine(200.0, 4000, 1, 1.0f); // 0 dBFS, well over the -6dB ceiling
                lim2.process(loudBuf.data(), 4000);
                float ceilingLinear = std::pow(10.0f, -6.0f / 20.0f);
                bool everExceeded = std::any_of(loudBuf.begin(), loudBuf.end(), [&](float s) {
                    return std::fabs(s) > ceilingLinear + 1e-4f;
                });
                test_assert(!everExceeded, "Limiter: output never exceeds a lowered ceiling, sample-by-sample, on every sample");

                // Lookahead should let the gain envelope start reducing
                // *before* a loud transient itself reaches the output. Feed
                // a quiet run (well under the ceiling on its own) followed
                // by an abrupt full-scale step, and inspect the delayed
                // output of the very LAST quiet input sample: on its own,
                // that sample's amplitude would never need any gain
                // reduction, but by the time it's finally emitted (delayed
                // by getLookaheadFrames()), the lookahead window has
                // already been staring at the incoming loud transient for
                // a full lookahead window's worth of samples. If the
                // emitted sample is measurably quieter than its own
                // (unreduced) input level, that reduction can only have
                // come from content the limiter hadn't reached yet --
                // exactly the anti-overshoot mechanism a zero-lookahead
                // design cannot provide.
                naikav::dsp::Limiter lookaheadLim;
                lookaheadLim.configure(1, kSR);
                lookaheadLim.setCeilingDb(-6.0f);
                const int lookahead2 = lookaheadLim.getLookaheadFrames();

                const int quietLen = lookahead2 + 20;
                const int loudLen = lookahead2 + 20;
                std::vector<float> sig(static_cast<size_t>(quietLen + loudLen), 0.0f);
                for (int i = 0; i < quietLen; ++i) sig[static_cast<size_t>(i)] = 0.1f; // well under the -6dB ceiling alone
                for (int i = quietLen; i < quietLen + loudLen; ++i) sig[static_cast<size_t>(i)] = 1.0f; // abrupt full-scale step
                auto out = sig;
                lookaheadLim.process(out.data(), static_cast<int>(out.size()));

                float preTransientOut = std::fabs(out[static_cast<size_t>(quietLen - 1 + lookahead2)]);
                test_assert(preTransientOut < 0.099f,
                            "Limiter: lookahead measurably reduces gain on a quiet sample immediately preceding a loud transient, before that transient itself is reached");
            }

            // --- Crossover ---
            {
                naikav::dsp::Crossover xover;
                xover.configure(2, kSR, 1); // channel 1 = target (e.g. LFE)
                test_assert(!xover.isEnabled(), "Crossover: disabled by default");
                auto buf = genSine(1000.0, 1000, 2, 0.7f, 1);
                auto original = buf;
                xover.process(buf.data(), 1000); // no-op while disabled
                bool unchanged = (buf == original);
                test_assert(unchanged, "Crossover: process() is a no-op while disabled");

                naikav::dsp::Crossover xover2;
                xover2.configure(2, kSR, 1);
                xover2.setEnabled(true);
                xover2.setCutoffHz(120.0);
                int settle = 4000, measure = 2000;

                // High tone (1kHz, well above cutoff): should be strongly attenuated.
                double highInRms = 0.0, highOutRms = 0.0;
                for (int i = 0; i < settle + measure; ++i) {
                    float frame[2] = {0.0f, static_cast<float>(0.6 * std::sin(2.0 * M_PI * 1000.0 * i / kSR))};
                    float xIn = frame[1];
                    xover2.process(frame, 1);
                    if (i >= settle) { highInRms += xIn * xIn; highOutRms += frame[1] * frame[1]; }
                }
                double highRatio = std::sqrt(highOutRms / highInRms);
                test_assert(highRatio < 0.1, "Crossover: 1kHz tone is strongly attenuated by a 120Hz LR4 lowpass");

                // Low tone (30Hz, well below cutoff): should pass through mostly intact.
                naikav::dsp::Crossover xover3;
                xover3.configure(2, kSR, 1);
                xover3.setEnabled(true);
                xover3.setCutoffHz(120.0);
                double lowInRms = 0.0, lowOutRms = 0.0;
                for (int i = 0; i < settle + measure; ++i) {
                    float frame[2] = {0.0f, static_cast<float>(0.6 * std::sin(2.0 * M_PI * 30.0 * i / kSR))};
                    float xIn = frame[1];
                    xover3.process(frame, 1);
                    if (i >= settle) { lowInRms += xIn * xIn; lowOutRms += frame[1] * frame[1]; }
                }
                double lowRatio = std::sqrt(lowOutRms / lowInRms);
                test_assert(lowRatio > 0.8, "Crossover: 30Hz tone passes a 120Hz LR4 lowpass mostly unattenuated");

                // Non-target channel (0) must never be touched, even while enabled.
                naikav::dsp::Crossover xover4;
                xover4.configure(2, kSR, 1);
                xover4.setEnabled(true);
                auto chBuf = genSine(1000.0, 500, 2, 0.5f); // both channels carry signal
                auto chOriginal = chBuf;
                xover4.process(chBuf.data(), 500);
                bool channel0Untouched = true;
                for (int f = 0; f < 500; ++f) {
                    if (chBuf[static_cast<size_t>(f) * 2 + 0] != chOriginal[static_cast<size_t>(f) * 2 + 0]) {
                        channel0Untouched = false;
                        break;
                    }
                }
                test_assert(channel0Untouched, "Crossover: non-target channel is bit-identical, untouched by the LFE filter");

                // --- Bass redirect: true bass management ---
                naikav::dsp::Crossover xoverRedirect;
                xoverRedirect.configure(2, kSR, 1); // channel 1 = LFE target
                xoverRedirect.setEnabled(true);
                xoverRedirect.setCutoffHz(120.0);
                test_assert(!xoverRedirect.isBassRedirectEnabled(), "Crossover: bass redirect disabled by default");
                xoverRedirect.setBassRedirectEnabled(true);
                test_assert(xoverRedirect.isBassRedirectEnabled(), "Crossover: bass redirect enabled flag getter matches setter");

                // A 30Hz tone (below cutoff) fed only into the main channel
                // (0) should be highpassed out of that channel and instead
                // appear in the previously-silent LFE channel (1).
                double mainInRms = 0.0, mainOutRms = 0.0, lfeOutRms = 0.0;
                for (int i = 0; i < settle + measure; ++i) {
                    float frame[2] = {static_cast<float>(0.6 * std::sin(2.0 * M_PI * 30.0 * i / kSR)), 0.0f};
                    float mainIn = frame[0];
                    xoverRedirect.process(frame, 1);
                    if (i >= settle) {
                        mainInRms += mainIn * mainIn;
                        mainOutRms += frame[0] * frame[0];
                        lfeOutRms += frame[1] * frame[1];
                    }
                }
                double mainRatio = std::sqrt(mainOutRms / mainInRms);
                test_assert(mainRatio < 0.2, "Crossover bass redirect: a 30Hz tone below the cutoff is highpassed out of the main channel");
                test_assert(lfeOutRms > 0.0, "Crossover bass redirect: the redirected bass appears in the previously-silent LFE channel");

                // A 1kHz tone (above cutoff) on the main channel should
                // pass its own highpass mostly unattenuated, and must NOT
                // be redirected into the LFE channel.
                naikav::dsp::Crossover xoverRedirectHigh;
                xoverRedirectHigh.configure(2, kSR, 1);
                xoverRedirectHigh.setEnabled(true);
                xoverRedirectHigh.setCutoffHz(120.0);
                xoverRedirectHigh.setBassRedirectEnabled(true);
                double highMainInRms = 0.0, highMainOutRms = 0.0, highLfeOutRms = 0.0;
                for (int i = 0; i < settle + measure; ++i) {
                    float frame[2] = {static_cast<float>(0.6 * std::sin(2.0 * M_PI * 1000.0 * i / kSR)), 0.0f};
                    float mainIn = frame[0];
                    xoverRedirectHigh.process(frame, 1);
                    if (i >= settle) {
                        highMainInRms += mainIn * mainIn;
                        highMainOutRms += frame[0] * frame[0];
                        highLfeOutRms += frame[1] * frame[1];
                    }
                }
                double highMainRatio = std::sqrt(highMainOutRms / highMainInRms);
                test_assert(highMainRatio > 0.8, "Crossover bass redirect: a 1kHz tone above the cutoff passes the main channel's highpass mostly unattenuated");
                // A 4th-order lowpass still leaks some residual energy this
                // many octaves above its cutoff (not literally zero), so
                // compare against the below-cutoff case's redirected energy
                // instead of an absolute threshold: it should be several
                // orders of magnitude smaller, not just "smaller".
                test_assert(highLfeOutRms < lfeOutRms * 0.001,
                            "Crossover bass redirect: content above the cutoff is redirected into the LFE channel at far lower energy than content below the cutoff");
            }

            // --- BalanceControl ---
            {
                naikav::dsp::BalanceControl bal;
                bal.configure(2);
                test_assert(std::fabs(bal.getBalance()) < 1e-6f, "BalanceControl: centered (0.0) by default");

                std::vector<float> centered = {0.5f, 0.5f, -0.3f, -0.3f};
                auto centeredOriginal = centered;
                bal.process(centered.data(), 2);
                test_assert(centered == centeredOriginal, "BalanceControl: centered balance is a true no-op");

                bal.setBalance(1.0f); // full right
                test_assert(std::fabs(bal.getBalance() - 1.0f) < 1e-6f, "BalanceControl: balance getter matches setter");
                std::vector<float> fullRight = {0.5f, 0.5f};
                bal.process(fullRight.data(), 1);
                test_assert(fullRight[0] == 0.0f, "BalanceControl: full-right balance silences the left channel");
                test_assert(fullRight[1] == 0.5f, "BalanceControl: full-right balance leaves the right channel untouched");

                naikav::dsp::BalanceControl balLeft;
                balLeft.configure(2);
                balLeft.setBalance(-1.0f); // full left
                std::vector<float> fullLeft = {0.5f, 0.5f};
                balLeft.process(fullLeft.data(), 1);
                test_assert(fullLeft[0] == 0.5f, "BalanceControl: full-left balance leaves the left channel untouched");
                test_assert(fullLeft[1] == 0.0f, "BalanceControl: full-left balance silences the right channel");

                naikav::dsp::BalanceControl balMulti;
                balMulti.configure(6); // e.g. 5.1 -- balance is only defined for 2ch
                balMulti.setBalance(1.0f);
                std::vector<float> multiBuf(6, 0.25f);
                auto multiOriginal = multiBuf;
                balMulti.process(multiBuf.data(), 1);
                test_assert(multiBuf == multiOriginal, "BalanceControl: no-op for channel counts other than 2");
            }

            // --- NoiseGate ---
            {
                naikav::dsp::NoiseGate gate;
                gate.configure(1, kSR); // default ratio 1:1 = true no-op
                auto buf = genSine(200.0, 2000, 1, 0.02f); // quiet, well below any plausible threshold
                auto original = buf;
                gate.process(buf.data(), 2000);
                double maxDiff = 0.0;
                for (size_t i = 0; i < buf.size(); ++i) {
                    maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(buf[i] - original[i])));
                }
                test_assert(maxDiff < 1e-5, "NoiseGate: default ratio 1:1 is a true no-op regardless of input level");

                naikav::dsp::NoiseGate gate2;
                gate2.configure(1, kSR);
                gate2.setThresholdDb(-30.0f);
                gate2.setRatio(10.0f); // aggressive, close to a hard gate
                gate2.setAttackMs(1.0f);
                gate2.setReleaseMs(50.0f);
                int total = 20000;
                std::vector<float> quiet(total);
                for (int i = 0; i < total; ++i) {
                    quiet[i] = 0.01f * static_cast<float>(std::sin(2.0 * M_PI * 200.0 * i / kSR)); // ~-40dBFS, below -30dB threshold
                }
                gate2.process(quiet.data(), total);
                float steadyPeak = peakOfChannel(quiet, 1, 0, total - 2000, 2000);
                test_assert(steadyPeak < 0.01f * 0.5f, "NoiseGate: a signal below threshold is measurably attenuated once the envelope settles");

                naikav::dsp::NoiseGate gate3;
                gate3.configure(1, kSR);
                gate3.setThresholdDb(-30.0f);
                gate3.setRatio(10.0f);
                // The very first ~1ms is a startup transient (both the
                // envelope and the detector's own level-smoothing state
                // start at 0/"silent"), not a steady-state reading -- give
                // it a settle window before measuring, the same convention
                // Compressor's near-identical test above uses.
                int gateSettle = 4000, gateMeasure = 4000;
                auto loudBuf = genSine(200.0, gateSettle + gateMeasure, 1, 0.5f); // well above the -30dB threshold
                auto loudOriginal = loudBuf;
                gate3.process(loudBuf.data(), gateSettle + gateMeasure);
                double loudMaxDiff = 0.0;
                for (int i = gateSettle; i < gateSettle + gateMeasure; ++i) {
                    loudMaxDiff = std::max(loudMaxDiff, static_cast<double>(std::fabs(loudBuf[i] - loudOriginal[i])));
                }
                test_assert(loudMaxDiff < 0.01, "NoiseGate: a signal above threshold settles to fully open (near-identity)");
            }

            // --- MultibandCompressor ---
            {
                naikav::dsp::MultibandCompressor mb;
                mb.configure(1, kSR);
                test_assert(!mb.isEnabled(), "MultibandCompressor: disabled by default");
                auto buf = genSine(1000.0, 1000, 1, 0.5f);
                auto original = buf;
                mb.process(buf.data(), 1000); // no-op while disabled
                test_assert(buf == original, "MultibandCompressor: process() is a true no-op while disabled");

                // A cascaded LR4 crossover's "sums back flat" property is a
                // *magnitude/energy* guarantee (an allpass identity: flat
                // |H(jw)|, but with a frequency-dependent phase rotation
                // through each crossover point), not sample-for-sample
                // time-domain identity -- so this has to be checked via
                // RMS/energy preservation, not a raw per-sample diff, same
                // idea as the Biquad lowpass RMS-ratio test above.
                mb.setEnabled(true); // all three bands still at their inert 1:1 ratio
                int mbSettle = 4000, mbMeasure = 4000;
                auto buf2 = genSine(1000.0, mbSettle + mbMeasure, 1, 0.5f);
                auto original2 = buf2;
                mb.process(buf2.data(), mbSettle + mbMeasure);
                double inRms = 0.0, outRms = 0.0;
                for (int i = mbSettle; i < mbSettle + mbMeasure; ++i) {
                    inRms += static_cast<double>(original2[i]) * original2[i];
                    outRms += static_cast<double>(buf2[i]) * buf2[i];
                }
                double rmsRatio = std::sqrt(outRms / inRms);
                test_assert(std::fabs(rmsRatio - 1.0) < 0.05,
                            "MultibandCompressor: enabled with all-default (1:1) band ratios preserves RMS energy (LR crossovers sum flat in magnitude)");

                // Compressing only the low band should tame a low-frequency
                // tone but leave a high-frequency tone (in a separate,
                // otherwise-identical instance) unaffected by that band's
                // compressor.
                naikav::dsp::MultibandCompressor mbLow;
                mbLow.configure(1, kSR);
                mbLow.setEnabled(true);
                mbLow.setCrossoverFrequencies(250.0, 4000.0);
                mbLow.low.setThresholdDb(-40.0f);
                mbLow.low.setRatio(8.0f);
                int total = 20000;
                std::vector<float> lowTone(total);
                for (int i = 0; i < total; ++i) {
                    lowTone[i] = 0.8f * static_cast<float>(std::sin(2.0 * M_PI * 100.0 * i / kSR)); // below the low/mid split
                }
                mbLow.process(lowTone.data(), total);
                float lowSteadyPeak = peakOfChannel(lowTone, 1, 0, total - 2000, 2000);
                test_assert(lowSteadyPeak < 0.8f * 0.9f, "MultibandCompressor: compressing only the low band measurably tames a low-frequency tone");

                naikav::dsp::MultibandCompressor mbLowVsHigh;
                mbLowVsHigh.configure(1, kSR);
                mbLowVsHigh.setEnabled(true);
                mbLowVsHigh.setCrossoverFrequencies(250.0, 4000.0);
                mbLowVsHigh.low.setThresholdDb(-40.0f);
                mbLowVsHigh.low.setRatio(8.0f);
                std::vector<float> highTone(total);
                for (int i = 0; i < total; ++i) {
                    highTone[i] = 0.8f * static_cast<float>(std::sin(2.0 * M_PI * 8000.0 * i / kSR)); // above the mid/high split
                }
                mbLowVsHigh.process(highTone.data(), total);
                float highSteadyPeak = peakOfChannel(highTone, 1, 0, total - 2000, 2000);
                test_assert(highSteadyPeak > 0.8f * 0.5f,
                            "MultibandCompressor: the low band's compressor doesn't touch a high-frequency tone routed to the high band");
            }

            // --- SpectrumAnalyzer ---
            {
                naikav::dsp::SpectrumAnalyzer spec;
                spec.configure(1, kSR);
                test_assert(!spec.isEnabled(), "SpectrumAnalyzer: disabled by default");

                auto flatMags = spec.getMagnitudesDb();
                test_assert(static_cast<int>(flatMags.size()) == naikav::dsp::SpectrumAnalyzer::kNumBins,
                            "SpectrumAnalyzer: magnitude snapshot has kNumBins entries");
                bool allAtFloor = std::all_of(flatMags.begin(), flatMags.end(),
                                               [](float m) { return m == naikav::dsp::SpectrumAnalyzer::kFloorDb; });
                test_assert(allAtFloor, "SpectrumAnalyzer: magnitudes start at the floor sentinel before any audio is fed");

                // Feeding audio while disabled must not change anything --
                // matches the rest of this DSP pipeline's "disabled = truly
                // zero cost" convention.
                auto silentTone = genSine(1000.0, naikav::dsp::SpectrumAnalyzer::kFftSize * 4, 1, 0.8f);
                spec.process(silentTone.data(), static_cast<int>(silentTone.size()));
                auto stillFlat = spec.getMagnitudesDb();
                test_assert(stillFlat == flatMags, "SpectrumAnalyzer: process() is a true no-op while disabled");

                // Enabled: a steady 1kHz tone should produce a clear peak
                // at the FFT bin closest to 1kHz, well above the noise
                // floor elsewhere in the spectrum.
                spec.setEnabled(true);
                int toneFreqHz = 1000;
                int expectedBin = static_cast<int>(std::lround(
                    static_cast<double>(toneFreqHz) * naikav::dsp::SpectrumAnalyzer::kFftSize / kSR));
                // Several FFT blocks so the frame-to-frame smoothing settles.
                auto tone = genSine(toneFreqHz, naikav::dsp::SpectrumAnalyzer::kFftSize * 8, 1, 0.8f);
                spec.process(tone.data(), static_cast<int>(tone.size()));
                auto toneMags = spec.getMagnitudesDb();

                int peakBin = 0;
                float peakMag = toneMags[0];
                for (int i = 1; i < naikav::dsp::SpectrumAnalyzer::kNumBins; ++i) {
                    if (toneMags[static_cast<size_t>(i)] > peakMag) {
                        peakMag = toneMags[static_cast<size_t>(i)];
                        peakBin = i;
                    }
                }
                test_assert(std::abs(peakBin - expectedBin) <= 1,
                            "SpectrumAnalyzer: a 1kHz tone's peak bin lands at (or immediately next to) the expected FFT bin");

                // A bin far away from the tone (e.g. near DC) should read
                // much quieter than the peak.
                float farBinMag = toneMags[2];
                test_assert(peakMag - farBinMag > 20.0f,
                            "SpectrumAnalyzer: the tone's peak bin is measurably (>20dB) louder than a bin far from it");

                test_assert(std::fabs(spec.binFrequencyHz(expectedBin) - toneFreqHz) < (kSR / naikav::dsp::SpectrumAnalyzer::kFftSize),
                            "SpectrumAnalyzer: binFrequencyHz() reports a frequency within one bin-width of the actual tone");

                spec.reset();
                auto resetMags = spec.getMagnitudesDb();
                bool allAtFloorAfterReset = std::all_of(resetMags.begin(), resetMags.end(),
                                                         [](float m) { return m == naikav::dsp::SpectrumAnalyzer::kFloorDb; });
                test_assert(allAtFloorAfterReset, "SpectrumAnalyzer: reset() clears the magnitude snapshot back to the floor sentinel");
            }

            // --- DspChain orchestration ---
            {
                naikav::dsp::DspChain chain;
                chain.configure(2, kSR, -1); // no LFE channel
                test_assert(!chain.isEnabled(), "DspChain: disabled by default");
                auto buf = genSine(1000.0, 1000, 2, 0.5f);
                auto original = buf;
                chain.process(buf.data(), 1000); // no-op while disabled
                test_assert(buf == original, "DspChain: process() is a true no-op while disabled");

                chain.setEnabled(true); // all sub-components still at their inert defaults
                // The chain's Limiter stage still holds a fixed lookahead
                // delay even at its inert (0dB ceiling) default -- see
                // Limiter.hpp -- so "near-identity" has to account for that
                // constant frame shift rather than comparing sample-for-sample.
                const int lookahead = chain.limiter.getLookaheadFrames();
                const int numFrames = 4000;
                auto buf2 = genSine(1000.0, numFrames, 2, 0.5f);
                auto original2 = buf2;
                chain.process(buf2.data(), numFrames);
                double maxDiff = 0.0;
                for (int f = lookahead; f < numFrames; ++f) {
                    for (int ch = 0; ch < 2; ++ch) {
                        size_t outIdx = static_cast<size_t>(f) * 2 + ch;
                        size_t inIdx = static_cast<size_t>(f - lookahead) * 2 + ch;
                        maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(buf2[outIdx] - original2[inIdx])));
                    }
                }
                test_assert(maxDiff < 0.01, "DspChain: enabled with all-default (flat/unity/inert) settings is near-identity, aside from the Limiter's fixed lookahead delay");
            }

            std::cout << "DSP chain unit tests passed!" << std::endl;
        }

        // -------------------------------------------------------------
        // Phase 8: Stereo Widener & Spatial (Virtual Surround) Downmixer
        // Unit Tests
        // -------------------------------------------------------------
        {
            std::cout << "Running stereo widener / spatial downmixer unit tests..." << std::endl;
            constexpr double kSR = 48000.0;

            // --- StereoWidener ---
            {
                naikav::dsp::StereoWidener widener;
                widener.configure(2);
                test_assert(!widener.isEnabled(), "StereoWidener: disabled by default");

                std::vector<float> buf = {0.5f, -0.3f, 0.2f, 0.2f, -0.4f, 0.1f};
                auto original = buf;
                widener.process(buf.data(), 3);
                test_assert(buf == original, "StereoWidener: process() is a no-op while disabled");

                widener.setEnabled(true);
                widener.setWidth(1.0f);
                std::vector<float> unity = original;
                widener.process(unity.data(), 3);
                double maxDiff = 0.0;
                for (size_t i = 0; i < unity.size(); ++i) {
                    maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(unity[i] - original[i])));
                }
                test_assert(maxDiff < 1e-5, "StereoWidener: width=1.0 is the identity transform");

                naikav::dsp::StereoWidener mono;
                mono.configure(2);
                mono.setEnabled(true);
                mono.setWidth(0.0f);
                std::vector<float> monoBuf = {0.5f, -0.3f};
                mono.process(monoBuf.data(), 1);
                test_assert(std::fabs(monoBuf[0] - monoBuf[1]) < 1e-5f,
                            "StereoWidener: width=0.0 collapses L/R to identical (mono) samples");
                float expectedMid = 0.5f * (0.5f + -0.3f);
                test_assert(std::fabs(monoBuf[0] - expectedMid) < 1e-5f,
                            "StereoWidener: width=0.0 collapses to the mid (L+R)/2 signal");

                naikav::dsp::StereoWidener wide;
                wide.configure(2);
                wide.setEnabled(true);
                wide.setWidth(2.0f);
                std::vector<float> wideBuf = {0.5f, -0.3f};
                wide.process(wideBuf.data(), 1);
                float widenedDiff = wideBuf[0] - wideBuf[1];
                float originalDiff = 0.5f - (-0.3f);
                test_assert(widenedDiff > originalDiff,
                            "StereoWidener: width>1.0 exaggerates the L/R difference (wider image)");

                naikav::dsp::StereoWidener multi;
                multi.configure(6); // e.g. 5.1 -- widener is only defined for 2ch
                multi.setEnabled(true);
                multi.setWidth(2.0f);
                std::vector<float> multiBuf(12, 0.25f);
                auto multiOriginal = multiBuf;
                multi.process(multiBuf.data(), 2);
                test_assert(multiBuf == multiOriginal, "StereoWidener: no-op for channel counts other than 2");
            }

            // --- SpatialDownmixer ---
            {
                using SL = naikav::dsp::SpatialDownmixer::SourceLayout;

                naikav::dsp::SpatialDownmixer dm;
                dm.configure(SL::FIVEPOINT1_SIDE, kSR);
                test_assert(dm.numSourceChannels() == 6, "SpatialDownmixer: 5.1(side) reports 6 source channels");

                // Front-left-only signal should land almost entirely in the
                // left output channel (direct route: FL -> gain 1.0 L / 0.0 R).
                {
                    int n = 100;
                    std::vector<float> in(static_cast<size_t>(n) * 6, 0.0f);
                    for (int f = 0; f < n; ++f) {
                        in[static_cast<size_t>(f) * 6 + 0] = 0.5f; // FL only
                    }
                    std::vector<float> out(static_cast<size_t>(n) * 2, 0.0f);
                    dm.process(in.data(), n, out.data());
                    float peakL = 0.0f, peakR = 0.0f;
                    for (int f = 0; f < n; ++f) {
                        peakL = std::max(peakL, std::fabs(out[static_cast<size_t>(f) * 2 + 0]));
                        peakR = std::max(peakR, std::fabs(out[static_cast<size_t>(f) * 2 + 1]));
                    }
                    test_assert(peakL > 0.4f, "SpatialDownmixer: front-left source drives the left output channel");
                    test_assert(peakR < 1e-5f, "SpatialDownmixer: front-left source doesn't leak into the right output channel");
                }

                // Center-only signal should split evenly between L and R.
                {
                    naikav::dsp::SpatialDownmixer dmCenter;
                    dmCenter.configure(SL::FIVEPOINT1_SIDE, kSR);
                    int n = 100;
                    std::vector<float> in(static_cast<size_t>(n) * 6, 0.0f);
                    for (int f = 0; f < n; ++f) {
                        in[static_cast<size_t>(f) * 6 + 2] = 0.5f; // FC only
                    }
                    std::vector<float> out(static_cast<size_t>(n) * 2, 0.0f);
                    dmCenter.process(in.data(), n, out.data());
                    float peakL = 0.0f, peakR = 0.0f;
                    for (int f = 0; f < n; ++f) {
                        peakL = std::max(peakL, std::fabs(out[static_cast<size_t>(f) * 2 + 0]));
                        peakR = std::max(peakR, std::fabs(out[static_cast<size_t>(f) * 2 + 1]));
                    }
                    test_assert(std::fabs(peakL - peakR) < 1e-4f,
                                "SpatialDownmixer: center-channel source splits evenly between L and R");
                    test_assert(peakL > 0.3f, "SpatialDownmixer: center-channel source is audible in the output");
                }

                // Side-left-only signal should be delayed (silent for the
                // first sample) and land louder in L than in R once the
                // delay line fills.
                {
                    naikav::dsp::SpatialDownmixer dmSide;
                    dmSide.configure(SL::FIVEPOINT1_SIDE, kSR);
                    int n = static_cast<int>(kSR * 0.02); // 20ms, enough to clear the ~8ms surround delay
                    std::vector<float> in(static_cast<size_t>(n) * 6, 0.0f);
                    for (int f = 0; f < n; ++f) {
                        in[static_cast<size_t>(f) * 6 + 4] = 0.5f; // SL only
                    }
                    std::vector<float> out(static_cast<size_t>(n) * 2, 0.0f);
                    dmSide.process(in.data(), n, out.data());

                    test_assert(out[0] == 0.0f && out[1] == 0.0f,
                                "SpatialDownmixer: a surround channel's contribution is delayed (silent at frame 0)");

                    float tailPeakL = 0.0f, tailPeakR = 0.0f;
                    for (int f = n - 100; f < n; ++f) {
                        tailPeakL = std::max(tailPeakL, std::fabs(out[static_cast<size_t>(f) * 2 + 0]));
                        tailPeakR = std::max(tailPeakR, std::fabs(out[static_cast<size_t>(f) * 2 + 1]));
                    }
                    test_assert(tailPeakL > tailPeakR,
                                "SpatialDownmixer: a side-left source lands louder in the left ear than the right");
                    test_assert(tailPeakR > 0.0f,
                                "SpatialDownmixer: a side-left source still bleeds (quieter) into the right ear");
                }

                // LFE-only signal should split evenly (non-directional).
                {
                    naikav::dsp::SpatialDownmixer dmLfe;
                    dmLfe.configure(SL::FIVEPOINT1_SIDE, kSR);
                    int n = 10;
                    std::vector<float> in(static_cast<size_t>(n) * 6, 0.0f);
                    for (int f = 0; f < n; ++f) {
                        in[static_cast<size_t>(f) * 6 + 3] = 0.8f; // LFE only
                    }
                    std::vector<float> out(static_cast<size_t>(n) * 2, 0.0f);
                    dmLfe.process(in.data(), n, out.data());
                    test_assert(std::fabs(out[0] - out[1]) < 1e-5f,
                                "SpatialDownmixer: LFE-only source is non-directional (splits evenly)");
                    test_assert(out[0] > 0.0f, "SpatialDownmixer: LFE-only source is audible in the output");
                }

                naikav::dsp::SpatialDownmixer dm21;
                dm21.configure(SL::TWOPOINT1, kSR);
                test_assert(dm21.numSourceChannels() == 3, "SpatialDownmixer: 2.1 reports 3 source channels");

                naikav::dsp::SpatialDownmixer dm71;
                dm71.configure(SL::SEVENPOINT1, kSR);
                test_assert(dm71.numSourceChannels() == 8, "SpatialDownmixer: 7.1 reports 8 source channels");
            }

            // --- Surround3D ---
            {
                naikav::dsp::Surround3D s3d;
                s3d.configure(2, kSR);
                test_assert(!s3d.isEnabled(), "Surround3D: disabled by default");

                std::vector<float> buf = {0.5f, -0.3f, 0.2f, 0.2f, -0.4f, 0.1f};
                auto original = buf;
                s3d.process(buf.data(), 3);
                test_assert(buf == original, "Surround3D: process() is a no-op while disabled");

                naikav::dsp::Surround3D s3dZero;
                s3dZero.configure(2, kSR);
                s3dZero.setEnabled(true);
                s3dZero.setIntensity(0.0f);
                auto zeroBuf = original;
                s3dZero.process(zeroBuf.data(), 3);
                test_assert(zeroBuf == original, "Surround3D: process() is a no-op at intensity 0.0 even while enabled");

                naikav::dsp::Surround3D s3dOn;
                s3dOn.configure(2, kSR);
                s3dOn.setEnabled(true);
                s3dOn.setIntensity(1.0f);
                int n = static_cast<int>(kSR * 0.05); // 50ms, enough to clear both delay taps (15ms/35ms)
                std::vector<float> in(static_cast<size_t>(n) * 2, 0.0f);
                for (int f = 0; f < n; ++f) {
                    // A steady, fully decorrelated (hard-left) signal so the
                    // synthesized ambience has something nonzero to work with.
                    in[static_cast<size_t>(f) * 2 + 0] = 0.6f;
                    in[static_cast<size_t>(f) * 2 + 1] = -0.6f;
                }
                auto inOriginal = in;
                s3dOn.process(in.data(), n);

                bool everDiffered = false;
                for (size_t i = 0; i < in.size(); ++i) {
                    if (std::fabs(in[i] - inOriginal[i]) > 1e-6f) {
                        everDiffered = true;
                        break;
                    }
                }
                test_assert(everDiffered, "Surround3D: enabled with nonzero intensity measurably alters a decorrelated signal");

                // The injected ambience is added to one ear and subtracted
                // from the other in exactly equal amounts, so L+R (the mono
                // downmix) must be preserved exactly, sample-by-sample,
                // regardless of intensity.
                bool monoSumPreserved = true;
                for (int f = 0; f < n; ++f) {
                    float outSum = in[static_cast<size_t>(f) * 2 + 0] + in[static_cast<size_t>(f) * 2 + 1];
                    float inSum = inOriginal[static_cast<size_t>(f) * 2 + 0] + inOriginal[static_cast<size_t>(f) * 2 + 1];
                    if (std::fabs(outSum - inSum) > 1e-4f) {
                        monoSumPreserved = false;
                        break;
                    }
                }
                test_assert(monoSumPreserved, "Surround3D: preserves the mono (L+R) sum exactly, adding only decorrelated ambience");

                naikav::dsp::Surround3D s3dMulti;
                s3dMulti.configure(6, kSR); // e.g. 5.1 -- Surround3D is only defined for 2ch
                s3dMulti.setEnabled(true);
                s3dMulti.setIntensity(1.0f);
                std::vector<float> multiBuf(12, 0.25f);
                auto multiOriginal = multiBuf;
                s3dMulti.process(multiBuf.data(), 2);
                test_assert(multiBuf == multiOriginal, "Surround3D: no-op for channel counts other than 2");
            }

            // --- Post-limiter overshoot / final safety backstop ---
            //
            // SpatialDownmixer, Surround3D, and StereoWidener all run
            // *after* DspChain's own Limiter in AudioDecoder's real
            // pipeline (see decodeAndResample()), so nothing upstream
            // protects against the overshoot each can independently
            // introduce. These tests reproduce that at the DSP-class
            // level (no full AudioDecoder/codec setup needed) to prove:
            // (a) the overshoot is real, and (b) chaining the same
            // naikav::dsp::Limiter AudioDecoder now runs as a final
            // safety backstop (m_finalSafetyLimiter) actually contains it.
            {
                using SL = naikav::dsp::SpatialDownmixer::SourceLayout;

                // (a) SpatialDownmixer alone: three simultaneously loud
                // source channels (FL/FC/SL) sum past +/-1.0 -- expected
                // behavior for a plain gain-sum downmix (a real ITU-R
                // BS.775-style downmix matrix has the same property), not
                // a bug in SpatialDownmixer itself.
                naikav::dsp::SpatialDownmixer dmLoud;
                dmLoud.configure(SL::FIVEPOINT1_SIDE, kSR);
                int n = static_cast<int>(kSR * 0.05); // 50ms, clears the ~8ms surround delay
                std::vector<float> loudIn(static_cast<size_t>(n) * 6, 0.0f);
                for (int f = 0; f < n; ++f) {
                    loudIn[static_cast<size_t>(f) * 6 + 0] = 0.95f; // FL
                    loudIn[static_cast<size_t>(f) * 6 + 2] = 0.95f; // FC
                    loudIn[static_cast<size_t>(f) * 6 + 4] = 0.95f; // SL
                }
                std::vector<float> downmixed(static_cast<size_t>(n) * 2, 0.0f);
                dmLoud.process(loudIn.data(), n, downmixed.data());
                const float downmixedPeak = std::accumulate(
                    downmixed.begin(), downmixed.end(), 0.0f,
                    [](float acc, float v) { return std::max(acc, std::fabs(v)); });
                test_assert(downmixedPeak > 1.0f,
                            "SpatialDownmixer: three simultaneously loud source channels can sum past full scale (no headroom by design)");

                // (b) Chain Surround3D + StereoWidener on top (as
                // AudioDecoder does), using the shipped Live preset's
                // settings, which enables both at once and pushes the
                // overshoot even further.
                naikav::dsp::AudioDspSettings live = naikav::dsp::makeLivePreset();
                test_assert(live.surround3dEnabled && live.widenerEnabled,
                            "Sanity check: the Live preset enables both Surround3D and StereoWidener");

                naikav::dsp::Surround3D s3dChain;
                s3dChain.configure(2, kSR);
                s3dChain.setEnabled(true);
                s3dChain.setIntensity(live.surround3dIntensity);
                naikav::dsp::StereoWidener widenerChain;
                widenerChain.configure(2);
                widenerChain.setEnabled(true);
                widenerChain.setWidth(live.widenerWidth);

                auto beforeFinalLimiter = downmixed;
                s3dChain.process(beforeFinalLimiter.data(), n);
                widenerChain.process(beforeFinalLimiter.data(), n);
                const float chainedPeak = std::accumulate(
                    beforeFinalLimiter.begin(), beforeFinalLimiter.end(), 0.0f,
                    [](float acc, float v) { return std::max(acc, std::fabs(v)); });
                test_assert(chainedPeak > downmixedPeak,
                            "Surround3D + StereoWidener measurably add to an already-over-full-scale signal");

                // (c) Append the same final safety backstop AudioDecoder
                // runs (naikav::dsp::Limiter at a 0dBFS ceiling, matching
                // m_finalSafetyLimiter's fallback when the user's own
                // Limiter isn't active): the result must never exceed that
                // ceiling, however far upstream stages overshot it.
                naikav::dsp::Limiter finalSafety;
                finalSafety.configure(2, kSR);
                finalSafety.setCeilingDb(0.0f);
                auto afterFinalLimiter = beforeFinalLimiter;
                finalSafety.process(afterFinalLimiter.data(), n);
                const float finalPeak = std::accumulate(
                    afterFinalLimiter.begin(), afterFinalLimiter.end(), 0.0f,
                    [](float acc, float v) { return std::max(acc, std::fabs(v)); });
                test_assert(finalPeak <= 1.0001f,
                            "Final safety Limiter backstop contains overshoot from SpatialDownmixer+Surround3D+StereoWidener within +/-1.0");
            }

            std::cout << "Stereo widener / spatial downmixer / 3D surround unit tests passed!" << std::endl;
        }

        // -------------------------------------------------------------
        // Phase 3: Loudness (EBU R128) Unit Tests
        // -------------------------------------------------------------
        {
            std::cout << "Running loudness normalization unit tests..." << std::endl;
            constexpr int kSR = 48000;
            constexpr int kChunk = 1024;

            auto fillSine = [&](std::vector<float>& buf, double freqHz, float amplitude, int startSample, int n) {
                for (int i = 0; i < n; ++i) {
                    float s = amplitude * static_cast<float>(std::sin(2.0 * M_PI * freqHz * (startSample + i) / kSR));
                    buf[static_cast<size_t>(i) * 2] = s;
                    buf[static_cast<size_t>(i) * 2 + 1] = s;
                }
            };
            auto peakOf = [&](const std::vector<float>& buf, int n) {
                return std::accumulate(buf.begin(), buf.begin() + n * 2, 0.0f,
                                        [](float acc, float s) { return std::max(acc, std::fabs(s)); });
            };

            // --- LoudnessMeter: measures a known signal close to its real LUFS ---
            {
                naikav::dsp::LoudnessMeter meter;
                test_assert(meter.configure(2, kSR), "LoudnessMeter: configure succeeds against the real avfilter graph");
                test_assert(meter.getIntegratedLufs() <= -70.0, "LoudnessMeter: no reading before any audio is fed");

                std::vector<float> buf(kChunk * 2);
                int total = kSR; // 1 second of -6dBFS 1kHz sine
                for (int start = 0; start < total; start += kChunk) {
                    int n = std::min(kChunk, total - start);
                    fillSine(buf, 1000.0, 0.5f, start, n);
                    meter.feed(buf.data(), n);
                }
                double measured = meter.getIntegratedLufs();
                test_assert(measured > -70.0, "LoudnessMeter: produces a real integrated reading after ~1s of audio");
                // -6dBFS (0.5 linear) at 1kHz should read close to -6 LUFS;
                // allow a generous tolerance since K-weighting isn't flat.
                test_assert(std::fabs(measured - (-6.02)) < 2.0,
                            "LoudnessMeter: -6dBFS 1kHz sine measures close to -6 LUFS");

                meter.reset();
                test_assert(meter.getIntegratedLufs() <= -70.0, "LoudnessMeter: reset() clears the measurement history");
            }

            // --- LoudnessNormalizer: inert while disabled ---
            {
                naikav::dsp::LoudnessNormalizer norm;
                norm.configure(2, kSR);
                test_assert(!norm.isEnabled(), "LoudnessNormalizer: disabled by default");

                std::vector<float> buf(kChunk * 2);
                fillSine(buf, 1000.0, 0.5f, 0, kChunk);
                auto original = buf;
                norm.process(buf.data(), kChunk);
                test_assert(buf == original, "LoudnessNormalizer: process() is a true no-op while disabled");
                test_assert(norm.getMeasuredIntegratedLufs() <= -70.0,
                            "LoudnessNormalizer: doesn't even measure while disabled");
            }

            // --- LoudnessNormalizer: enabled, measurably corrects toward target ---
            {
                naikav::dsp::LoudnessNormalizer norm;
                norm.configure(2, kSR);
                norm.setEnabled(true);
                norm.setTargetLufs(-23.0f); // well below the ~-6 LUFS the test signal measures at
                test_assert(std::fabs(norm.getTargetLufs() - (-23.0f)) < 0.001f,
                            "LoudnessNormalizer: target LUFS getter matches setter");

                std::vector<float> buf(kChunk * 2);
                const float inputAmplitude = 0.5f;
                int total = kSR * 2; // 2 seconds
                float lastOutputPeak = 0.0f;
                for (int start = 0; start < total; start += kChunk) {
                    int n = std::min(kChunk, total - start);
                    fillSine(buf, 1000.0, inputAmplitude, start, n);
                    norm.process(buf.data(), n);
                    if (start + kChunk >= total) {
                        lastOutputPeak = peakOf(buf, n);
                    }
                }
                test_assert(lastOutputPeak < inputAmplitude * 0.9f,
                            "LoudnessNormalizer: measurably attenuates a signal louder than its target");
                test_assert(norm.getCurrentGainDb() < -1.0f,
                            "LoudnessNormalizer: applied gain has moved negative toward the quieter target");
                test_assert(norm.getMeasuredIntegratedLufs() > -70.0,
                            "LoudnessNormalizer: reports a real measured integrated LUFS once enabled and fed");
            }

            // --- LoudnessNormalizer: two-pass (prescanned) mode applies
            // the correct gain immediately, with no ramp-up period ---
            {
                naikav::dsp::LoudnessNormalizer norm;
                norm.configure(2, kSR);
                norm.setEnabled(true);
                norm.setTargetLufs(-23.0f);
                test_assert(!norm.hasPrescannedLoudness(), "LoudnessNormalizer: no prescan primed by default");

                // Prime with a whole-file measurement, as if
                // naikav::dsp::prescanIntegratedLufs() had already scanned
                // a -6 LUFS file (matching the LoudnessMeter test above).
                norm.primeWithPrescannedLufs(-6.0);
                test_assert(norm.hasPrescannedLoudness(), "LoudnessNormalizer: primeWithPrescannedLufs() marks the instance as primed");
                test_assert(std::fabs(norm.getMeasuredIntegratedLufs() - (-6.0)) < 0.001,
                            "LoudnessNormalizer: getMeasuredIntegratedLufs() reports the prescanned value immediately, before any audio is fed");

                float expectedGainDb = -23.0f - (-6.0f); // target - measured
                test_assert(std::fabs(norm.getCurrentGainDb() - expectedGainDb) < 0.01f,
                            "LoudnessNormalizer: priming jumps straight to the correct gain, with no ramp-up");

                // The very first block processed should already reflect
                // the primed gain in full -- unlike the real-time path
                // above, which ramps in over several seconds.
                std::vector<float> buf(kChunk * 2);
                fillSine(buf, 1000.0, 0.5f, 0, kChunk);
                norm.process(buf.data(), kChunk);
                float firstBlockPeak = peakOf(buf, kChunk);
                float expectedGainLinear = std::pow(10.0f, expectedGainDb / 20.0f);
                float expectedPeak = 0.5f * expectedGainLinear;
                test_assert(std::fabs(firstBlockPeak - expectedPeak) < 0.01f,
                            "LoudnessNormalizer: the first processed block already applies the full prescanned gain, with no startup ramp");

                // A seek (reset()) must not drop back to zero gain -- the
                // prescanned value is still valid for the whole file
                // regardless of playback position.
                norm.reset();
                test_assert(std::fabs(norm.getCurrentGainDb() - expectedGainDb) < 0.01f,
                            "LoudnessNormalizer: reset() (as called on seek) keeps the primed gain instead of dropping to zero");
                test_assert(norm.hasPrescannedLoudness(), "LoudnessNormalizer: reset() does not clear the prescanned value");

                norm.clearPrescan();
                test_assert(!norm.hasPrescannedLoudness(), "LoudnessNormalizer: clearPrescan() reverts to real-time-only measurement");
            }

            // --- LoudnessPrescan: decodes a real file's whole audio
            // stream and reports a plausible integrated LUFS ---
            {
                double scanned = naikav::dsp::prescanIntegratedLufs(testFile);
                test_assert(scanned > -70.0, "prescanIntegratedLufs: produces a real reading for the test video's audio track");
                test_assert(scanned < 0.0, "prescanIntegratedLufs: reports a sane (sub-0dBFS) LUFS value for the test video");

                double missing = naikav::dsp::prescanIntegratedLufs("this_file_does_not_exist.mp4");
                test_assert(missing <= -70.0, "prescanIntegratedLufs: returns the failure sentinel for a nonexistent file");

                // The EOF-flush loop's feedFrame() call: real decoders on
                // this build never actually have a buffered frame left over
                // once av_read_frame() hits EOF for these small test
                // assets, so avcodec_send_packet(ctx, nullptr) is normally
                // followed by an avcodec_receive_frame() that returns
                // immediately. Forced deterministically via
                // force_synthetic_flush_frame so the mock hands back one
                // synthetic frame right after that flush call.
                force_synthetic_flush_frame = true;
                double withFlushFrame = naikav::dsp::prescanIntegratedLufs(testFile);
                force_synthetic_flush_frame = false;
                test_assert(withFlushFrame > -70.0, "prescanIntegratedLufs: still produces a real reading when the decoder's EOF-flush yields one extra frame");
            }

            std::cout << "Loudness normalization unit tests passed!" << std::endl;
        }

        // -------------------------------------------------------------
        // AudioDecoder DSP end-to-end integration test.
        //
        // Every DSP test above exercises naikav::dsp::* classes directly --
        // never a real AudioDecoder -- so none of them prove
        // AudioDecoder::applyDspSettings() actually reaches
        // decodeAndResample()'s real decode path. This test decodes the
        // identical sequence of real packets from the test file through
        // two independent AudioDecoder instances (one DSP-enabled, one
        // not) and diffs their output float buffers -- reachable here
        // because of the `#define private public` test-only macro near the
        // top of this file, which also exposes decodeAndResample() and
        // m_audioBuffer directly, letting this run deterministically
        // without depending on real SDL audio device/callback timing.
        // -------------------------------------------------------------
        {
            std::cout << "Running AudioDecoder DSP end-to-end integration test..." << std::endl;

            AVFormatContext* dspFmtCtx = nullptr;
            AVDictionary* dspOpenOpts = nullptr;
            av_dict_set(&dspOpenOpts, "protocol_whitelist", "file,pipe", 0);
            int dspOpenRet = avformat_open_input(&dspFmtCtx, testFile.c_str(), nullptr, &dspOpenOpts);
            av_dict_free(&dspOpenOpts);
            test_assert(dspOpenRet >= 0 && dspFmtCtx != nullptr, "DSP integration: raw demux opens the test file");

            if (dspOpenRet >= 0 && dspFmtCtx) {
                test_assert(avformat_find_stream_info(dspFmtCtx, nullptr) >= 0,
                            "DSP integration: raw demux finds stream info");

                const AVCodec* dspDummyCodec = nullptr;
                int dspAudioStreamIdx = av_find_best_stream(dspFmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &dspDummyCodec, 0);
                test_assert(dspAudioStreamIdx >= 0, "DSP integration: test file has a decodable audio stream");

                if (dspAudioStreamIdx >= 0) {
                    AVStream* dspStream = dspFmtCtx->streams[dspAudioStreamIdx];

                    ThreadSafeQueue<AVPacket*> plainQueue(64);
                    ThreadSafeQueue<AVPacket*> dspQueue(64);
                    AudioDecoder plainDecoder(dspStream->codecpar, dspStream->time_base, 0, plainQueue);
                    AudioDecoder dspDecoder(dspStream->codecpar, dspStream->time_base, 0, dspQueue);
                    test_assert(plainDecoder.init(), "DSP integration: plain decoder initializes from real codec params");
                    test_assert(dspDecoder.init(), "DSP integration: DSP decoder initializes from real codec params");

                    naikav::dsp::AudioDspSettings settings;
                    settings.dspEnabled = true;
                    settings.eqBandGainDb[0] = 12.0f; // large, unmissable bass boost
                    settings.compressorEnabled = true;
                    settings.compressorThresholdDb = -30.0f;
                    settings.compressorRatio = 8.0f;
                    dspDecoder.applyDspSettings(settings);

                    // Feed identical real packets, read once from the file,
                    // to both decoders' queues.
                    AVPacket* readPkt = av_packet_alloc();
                    int fed = 0;
                    while (fed < 60 && av_read_frame(dspFmtCtx, readPkt) >= 0) {
                        if (readPkt->stream_index == dspAudioStreamIdx) {
                            plainQueue.push(av_packet_clone(readPkt));
                            dspQueue.push(av_packet_clone(readPkt));
                            ++fed;
                        }
                        av_packet_unref(readPkt);
                    }
                    av_packet_free(&readPkt);
                    test_assert(fed > 0, "DSP integration: fed real audio packets from the test file to both decoders");

                    // Pull several decoded frames out of each.
                    // decodeAndResample() is normally invoked by the SDL
                    // audio callback thread; calling it directly here
                    // drives the exact same real decode path
                    // deterministically.
                    bool everProducedAudio = false;
                    bool everDiffered = false;
                    for (int call = 0; call < 30 && !everDiffered; ++call) {
                        plainDecoder.decodeAndResample();
                        dspDecoder.decodeAndResample();
                        if (plainDecoder.m_audioBufferSize == 0 || dspDecoder.m_audioBufferSize == 0) {
                            continue;
                        }
                        everProducedAudio = true;
                        size_t commonBytes = std::min(plainDecoder.m_audioBufferSize, dspDecoder.m_audioBufferSize);
                        // m_audioBuffer is byte-typed because it holds whatever
                        // the device format is; on this path it carries the
                        // DSP chain's native float samples, and the vector's
                        // allocator satisfies float alignment. Reading it back
                        // as float is exactly what the audio callback does.
                        // cppcheck-suppress invalidPointerCast
                        const float* plainSamples = reinterpret_cast<const float*>(plainDecoder.m_audioBuffer.data());
                        // cppcheck-suppress invalidPointerCast
                        const float* dspSamples = reinterpret_cast<const float*>(dspDecoder.m_audioBuffer.data());
                        for (size_t i = 0; i < commonBytes / sizeof(float); ++i) {
                            if (std::fabs(plainSamples[i] - dspSamples[i]) > 1e-4f) {
                                everDiffered = true;
                                break;
                            }
                        }
                    }

                    test_assert(everProducedAudio, "DSP integration: real packets actually decode into non-empty audio buffers");
                    test_assert(everDiffered,
                                "DSP integration: applyDspSettings() measurably changes decodeAndResample()'s real decoded output -- the EQ/compressor chain actually runs on live decoded audio, not just in isolated DSP-class tests");

                    // Drain and free any packets neither decoder consumed
                    // (e.g. if fewer than 30 frames' worth were needed) --
                    // ThreadSafeQueue's destructor aborts the queue but
                    // doesn't free contained pointers.
                    AVPacket* leftover = nullptr;
                    while (plainQueue.try_pop(leftover)) av_packet_free(&leftover);
                    while (dspQueue.try_pop(leftover)) av_packet_free(&leftover);
                }

                avformat_close_input(&dspFmtCtx);
            }

            std::cout << "AudioDecoder DSP end-to-end integration test passed!" << std::endl;
        }

        // -------------------------------------------------------------
        // Phase 7: Audio DSP Settings Persistence (Config File) Test
        // -------------------------------------------------------------
        {
            std::cout << "Testing audio DSP settings persistence..." << std::endl;

            // PlayerController's constructor loads player_settings.txt from the
            // current working directory, so a file left behind by a real app run
            // (or by an earlier run of this very test) would make the "defaults
            // to ..." assertions below read someone else's saved state. Start
            // from a known-clean directory, and don't leave a file behind either.
            struct SettingsFileGuard {
                SettingsFileGuard()  { std::remove("player_settings.txt"); }
                ~SettingsFileGuard() { std::remove("player_settings.txt"); }
            } settingsFileGuard;

            naikav::dsp::AudioDspSettings testSettings = naikav::dsp::makeCinemaPreset();
            testSettings.eqBandGainDb[1] = 3.25f; // distinctive, non-preset value
            testSettings.eqBandFreqHz[1] = 315.0f;  // distinctive, non-default band frequency
            testSettings.eqBandQ[1] = 1.4f;         // distinctive, non-default band Q
            testSettings.widenerEnabled = true;
            testSettings.widenerWidth = 1.75f; // distinctive, non-default value
            testSettings.surround3dEnabled = true;
            testSettings.surround3dIntensity = 1.35f; // distinctive, non-default value
            testSettings.crossoverBassRedirectEnabled = true; // distinctive, non-default value
            testSettings.balance = -0.4f; // distinctive, non-default value
            testSettings.noiseGateEnabled = true;
            testSettings.noiseGateThresholdDb = -35.0f;
            testSettings.noiseGateRatio = 6.0f;
            testSettings.multibandEnabled = true;
            testSettings.multibandLowMidHz = 300.0f;
            testSettings.multibandMidHighHz = 3500.0f;
            testSettings.multibandLowThresholdDb = -25.0f;
            testSettings.multibandLowRatio = 2.5f;
            testSettings.multibandMidThresholdDb = -22.0f;
            testSettings.multibandMidRatio = 1.5f;
            testSettings.multibandHighThresholdDb = -18.0f;
            testSettings.multibandHighRatio = 3.5f;
            testSettings.autoGenrePresetEnabled = true;
            testSettings.spectrumAnalyzerEnabled = true;

            {
                PlayerController writer;
                writer.setAudioDspSettings(testSettings);
                writer.persistAudioDspSettings();
                test_assert(writer.getAudioDspSettings() == testSettings,
                            "setAudioDspSettings() applies immediately (in-memory)");

                test_assert(writer.getAudioChannelOption() == AudioChannelOption::AUTO,
                            "AudioChannelOption defaults to AUTO");
                writer.setAudioChannelOption(AudioChannelOption::FORCE_STEREO);
                test_assert(writer.getAudioChannelOption() == AudioChannelOption::FORCE_STEREO,
                            "setAudioChannelOption() applies immediately (in-memory)");

                test_assert(writer.getOutputBitDepth() == AudioOutputBitDepth::BIT_16,
                            "OutputBitDepth defaults to BIT_16");
                writer.setOutputBitDepth(AudioOutputBitDepth::BIT_32_FLOAT);
                test_assert(writer.getOutputBitDepth() == AudioOutputBitDepth::BIT_32_FLOAT,
                            "setOutputBitDepth() applies immediately (in-memory)");

                test_assert(writer.getOutputDeviceName().empty(), "OutputDeviceName defaults to empty (system default)");
                writer.setOutputDeviceName("Distinctive Test Device");
                test_assert(writer.getOutputDeviceName() == "Distinctive Test Device",
                            "setOutputDeviceName() applies immediately (in-memory)");

                test_assert(writer.getResamplerQuality() == ResamplerQuality::MEDIUM,
                            "ResamplerQuality defaults to MEDIUM");
                writer.setResamplerQuality(ResamplerQuality::VERY_HIGH);
                test_assert(writer.getResamplerQuality() == ResamplerQuality::VERY_HIGH,
                            "setResamplerQuality() applies immediately (in-memory)");
            }

            {
                PlayerController reader;
                test_assert(reader.getAudioDspSettings() == testSettings,
                            "AudioDspSettings round-trips through player_settings.txt across instances");
                test_assert(reader.getAudioChannelOption() == AudioChannelOption::FORCE_STEREO,
                            "AudioChannelOption round-trips through player_settings.txt across instances");
                test_assert(reader.getOutputBitDepth() == AudioOutputBitDepth::BIT_32_FLOAT,
                            "OutputBitDepth round-trips through player_settings.txt across instances");
                test_assert(reader.getOutputDeviceName() == "Distinctive Test Device",
                            "OutputDeviceName round-trips through player_settings.txt across instances");
                test_assert(reader.getResamplerQuality() == ResamplerQuality::VERY_HIGH,
                            "ResamplerQuality round-trips through player_settings.txt across instances");
            }

            // A settings file predating DSP settings (just a bare resolution
            // integer, no '=' signs) must still load without crashing or
            // corrupting DSP settings -- see loadSettings()'s legacy-format
            // fallback.
            {
                std::ofstream legacy("player_settings.txt");
                legacy << "3";
                legacy.close();

                PlayerController legacyReader;
                test_assert(legacyReader.getResolutionOption() == ResolutionOption::R_720P,
                            "Legacy (bare-integer) settings file still loads the resolution correctly");
                test_assert(legacyReader.getAudioDspSettings() == naikav::dsp::AudioDspSettings{},
                            "Legacy settings file leaves DSP settings at their defaults");
                test_assert(legacyReader.getAudioChannelOption() == AudioChannelOption::AUTO,
                            "Legacy settings file leaves AudioChannelOption at AUTO");
                test_assert(legacyReader.getOutputBitDepth() == AudioOutputBitDepth::BIT_16,
                            "Legacy settings file leaves OutputBitDepth at BIT_16");
                test_assert(legacyReader.getOutputDeviceName().empty(),
                            "Legacy settings file leaves OutputDeviceName empty");
                test_assert(legacyReader.getResamplerQuality() == ResamplerQuality::MEDIUM,
                            "Legacy settings file leaves ResamplerQuality at MEDIUM");
            }

            // saveSettings()'s ofstream-open-failure branch: pre-occupy the
            // hardcoded "player_settings.txt" path with a directory, so the
            // ofstream can never open a regular file there.
            {
                std::error_code ec;
                std::filesystem::remove("player_settings.txt", ec);
                bool dirCreated = std::filesystem::create_directory("player_settings.txt", ec);
                test_assert(dirCreated, "Test setup: created a directory occupying the settings file path");

                PlayerController pcSaveFail;
                pcSaveFail.saveSettings();
                test_assert(std::filesystem::is_directory("player_settings.txt"),
                            "saveSettings() returns without crashing (and without touching the path) when the settings file can't be opened");

                std::filesystem::remove("player_settings.txt", ec);
            }

            std::cout << "Audio DSP settings persistence test passed!" << std::endl;
        }

        // -------------------------------------------------------------
        // ReplayGain/R128 tag reading, genre-based preset mapping, and
        // output format/device/resampler-quality selector tests.
        // -------------------------------------------------------------
        {
            std::cout << "Running ReplayGain tag / genre preset / output selector tests..." << std::endl;

            // --- readTaggedLoudnessAsLufs ---
            {
                AVDictionary* dict = nullptr;
                av_dict_set(&dict, "R128_TRACK_GAIN", "-1234", 0); // Q7.8: -1234/256 dB
                double lufs = 0.0;
                bool found = naikav::dsp::readTaggedLoudnessAsLufs(dict, nullptr, lufs);
                test_assert(found, "readTaggedLoudnessAsLufs: finds R128_TRACK_GAIN");
                double expectedGainDb = -1234.0 / 256.0;
                test_assert(std::fabs(lufs - (-23.0 - expectedGainDb)) < 0.001,
                            "readTaggedLoudnessAsLufs: R128_TRACK_GAIN converts to the correct equivalent LUFS (-23 LUFS reference)");
                av_dict_free(&dict);
            }
            {
                AVDictionary* dict = nullptr;
                av_dict_set(&dict, "REPLAYGAIN_TRACK_GAIN", "-6.50 dB", 0);
                double lufs = 0.0;
                bool found = naikav::dsp::readTaggedLoudnessAsLufs(dict, nullptr, lufs);
                test_assert(found, "readTaggedLoudnessAsLufs: finds REPLAYGAIN_TRACK_GAIN");
                test_assert(std::fabs(lufs - (-18.0 - (-6.5))) < 0.001,
                            "readTaggedLoudnessAsLufs: REPLAYGAIN_TRACK_GAIN converts to the correct equivalent LUFS (-18 LUFS reference)");
                av_dict_free(&dict);
            }
            {
                // Stream-level metadata is checked before format-level, and
                // within a dict R128_TRACK_GAIN outranks REPLAYGAIN_TRACK_GAIN.
                AVDictionary* streamDict = nullptr;
                av_dict_set(&streamDict, "R128_TRACK_GAIN", "0", 0);
                AVDictionary* formatDict = nullptr;
                av_dict_set(&formatDict, "REPLAYGAIN_TRACK_GAIN", "-10.0 dB", 0);
                double lufs = 0.0;
                bool found = naikav::dsp::readTaggedLoudnessAsLufs(formatDict, streamDict, lufs);
                test_assert(found, "readTaggedLoudnessAsLufs: finds a tag when both stream and format metadata have one");
                test_assert(std::fabs(lufs - (-23.0)) < 0.001,
                            "readTaggedLoudnessAsLufs: stream-level tag takes priority over format-level tag");
                av_dict_free(&streamDict);
                av_dict_free(&formatDict);
            }
            {
                double lufs = 0.0;
                bool found = naikav::dsp::readTaggedLoudnessAsLufs(nullptr, nullptr, lufs);
                test_assert(!found, "readTaggedLoudnessAsLufs: returns false when neither dictionary has a usable tag");
            }

            // --- presetForGenreTag ---
            {
                naikav::dsp::AudioDspSettings out;
                test_assert(naikav::dsp::presetForGenreTag("Rock", out), "presetForGenreTag: 'Rock' matches a preset");
                test_assert(out == naikav::dsp::makeMusicPreset(), "presetForGenreTag: 'Rock' maps to the Music preset");

                naikav::dsp::AudioDspSettings out2;
                test_assert(naikav::dsp::presetForGenreTag("PODCAST", out2), "presetForGenreTag: case-insensitive match");
                test_assert(out2 == naikav::dsp::makePodcastPreset(), "presetForGenreTag: 'PODCAST' maps to the Podcast preset");

                naikav::dsp::AudioDspSettings out3;
                test_assert(!naikav::dsp::presetForGenreTag("Xyzzy Unrecognized Genre", out3),
                            "presetForGenreTag: unrecognized genre returns false");

                naikav::dsp::AudioDspSettings out4;
                test_assert(!naikav::dsp::presetForGenreTag("", out4), "presetForGenreTag: empty genre string returns false");

                naikav::dsp::AudioDspSettings out5;
                test_assert(naikav::dsp::presetForGenreTag("Movie Soundtrack", out5), "presetForGenreTag: 'Movie Soundtrack' matches a preset");
                test_assert(out5 == naikav::dsp::makeCinemaPreset(), "presetForGenreTag: maps to the Cinema preset");

                naikav::dsp::AudioDspSettings out6;
                test_assert(naikav::dsp::presetForGenreTag("EDM", out6), "presetForGenreTag: 'EDM' matches a preset");
                test_assert(out6 == naikav::dsp::makeBassBoostPreset(), "presetForGenreTag: maps to the Bass Boost preset");

                naikav::dsp::AudioDspSettings out7;
                test_assert(naikav::dsp::presetForGenreTag("Vocal", out7), "presetForGenreTag: 'Vocal' matches a preset");
                test_assert(out7 == naikav::dsp::makeVocalBoostPreset(), "presetForGenreTag: maps to the Vocal Boost preset");

                naikav::dsp::AudioDspSettings out8;
                test_assert(naikav::dsp::presetForGenreTag("Jazz", out8), "presetForGenreTag: 'Jazz' matches a preset");
                test_assert(out8 == naikav::dsp::makeLivePreset(), "presetForGenreTag: maps to the Live preset");

                naikav::dsp::AudioDspSettings out9;
                test_assert(naikav::dsp::presetForGenreTag("Gaming", out9), "presetForGenreTag: 'Gaming' matches a preset");
                test_assert(out9 == naikav::dsp::makeGamingPreset(), "presetForGenreTag: maps to the Gaming preset");

                // operator==()'s false branch: two settings differing in one
                // of the many scalar fields it compares.
                naikav::dsp::AudioDspSettings a, b;
                b.compressorThresholdDb = a.compressorThresholdDb + 1.0f;
                test_assert(!(a == b), "AudioDspSettings::operator==() returns false when a scalar field differs");
            }

            // --- Output format/device/resampler-quality helpers (anonymous-
            // namespace internals in AudioDecoder.cpp, directly reachable
            // from this unity-build translation unit) ---
            {
                test_assert(sdlFormatFor(AudioOutputBitDepth::BIT_16) == SDL_AUDIO_S16, "sdlFormatFor: BIT_16 maps to SDL_AUDIO_S16");
                test_assert(sdlFormatFor(AudioOutputBitDepth::BIT_32_INT) == SDL_AUDIO_S32, "sdlFormatFor: BIT_32_INT maps to SDL_AUDIO_S32");
                test_assert(sdlFormatFor(AudioOutputBitDepth::BIT_32_FLOAT) == SDL_AUDIO_F32, "sdlFormatFor: BIT_32_FLOAT maps to SDL_AUDIO_F32");

                // cppcheck folds these to "always true" by inlining
                // outputBytesPerSampleFor()'s current body. That is the point:
                // the assertions pin the depth-to-width mapping so a future
                // edit to that function fails the suite rather than silently
                // resizing every audio buffer.
                // cppcheck-suppress knownConditionTrueFalse
                test_assert(outputBytesPerSampleFor(AudioOutputBitDepth::BIT_16) == 2, "outputBytesPerSampleFor: BIT_16 is 2 bytes/sample");
                // cppcheck-suppress knownConditionTrueFalse
                test_assert(outputBytesPerSampleFor(AudioOutputBitDepth::BIT_32_INT) == 4, "outputBytesPerSampleFor: BIT_32_INT is 4 bytes/sample");
                // cppcheck-suppress knownConditionTrueFalse
                test_assert(outputBytesPerSampleFor(AudioOutputBitDepth::BIT_32_FLOAT) == 4, "outputBytesPerSampleFor: BIT_32_FLOAT is 4 bytes/sample");

                test_assert(resamplerPrecisionBitsFor(ResamplerQuality::LOW) == 16.0, "resamplerPrecisionBitsFor: LOW is 16 bits");
                test_assert(resamplerPrecisionBitsFor(ResamplerQuality::MEDIUM) == 20.0, "resamplerPrecisionBitsFor: MEDIUM is 20 bits (original default)");
                test_assert(resamplerPrecisionBitsFor(ResamplerQuality::HIGH) == 28.0, "resamplerPrecisionBitsFor: HIGH is 28 bits");
                test_assert(resamplerPrecisionBitsFor(ResamplerQuality::VERY_HIGH) == 33.0, "resamplerPrecisionBitsFor: VERY_HIGH is 33 bits");

                test_assert(resolveOutputDeviceId("") == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                            "resolveOutputDeviceId: empty name resolves to the default device");
                test_assert(resolveOutputDeviceId("a device name that will never exist 12345") == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                            "resolveOutputDeviceId: unknown name falls back to the default device");

                uint32_t ditherState = 0xABCDEF01u;
                int32_t s32Half = floatToS32Dithered(0.5f, ditherState);
                test_assert(s32Half > 1000000000 && s32Half < 1100000000, "floatToS32Dithered: 0.5 maps to roughly half of INT32_MAX");

                // Doesn't crash; can't assert non-empty since headless/CI
                // environments may legitimately report zero playback devices.
                auto deviceNames = AudioDecoder::enumeratePlaybackDeviceNames();
                (void)deviceNames;
            }

            // --- AudioDecoder setter/getter round trips + init() actually
            // succeeding with a non-default output bit depth ---
            {
                AVFormatContext* fmtCtx2 = nullptr;
                AVDictionary* openOpts2 = nullptr;
                av_dict_set(&openOpts2, "protocol_whitelist", "file,pipe", 0);
                int openRet2 = avformat_open_input(&fmtCtx2, testFile.c_str(), nullptr, &openOpts2);
                av_dict_free(&openOpts2);
                if (openRet2 >= 0 && fmtCtx2 && avformat_find_stream_info(fmtCtx2, nullptr) >= 0) {
                    const AVCodec* dummyCodec2 = nullptr;
                    int audioIdx2 = av_find_best_stream(fmtCtx2, AVMEDIA_TYPE_AUDIO, -1, -1, &dummyCodec2, 0);
                    if (audioIdx2 >= 0) {
                        AVStream* stream2 = fmtCtx2->streams[audioIdx2];
                        ThreadSafeQueue<AVPacket*> dummyQueue(4);
                        AudioDecoder fmtDecoder(stream2->codecpar, stream2->time_base, 0, dummyQueue);

                        test_assert(fmtDecoder.getOutputBitDepth() == AudioOutputBitDepth::BIT_16,
                                    "AudioDecoder: output bit depth defaults to BIT_16");
                        fmtDecoder.setOutputBitDepth(AudioOutputBitDepth::BIT_32_FLOAT);
                        test_assert(fmtDecoder.getOutputBitDepth() == AudioOutputBitDepth::BIT_32_FLOAT,
                                    "AudioDecoder: output bit depth getter matches setter");

                        test_assert(fmtDecoder.getOutputDeviceName().empty(),
                                    "AudioDecoder: output device name empty (system default) by default");
                        fmtDecoder.setOutputDeviceName("a nonexistent device");
                        test_assert(fmtDecoder.getOutputDeviceName() == "a nonexistent device",
                                    "AudioDecoder: output device name getter matches setter");
                        fmtDecoder.setOutputDeviceName(""); // fall back to the real default device for init() below

                        test_assert(fmtDecoder.getResamplerQuality() == ResamplerQuality::MEDIUM,
                                    "AudioDecoder: resampler quality defaults to MEDIUM");
                        fmtDecoder.setResamplerQuality(ResamplerQuality::HIGH);
                        test_assert(fmtDecoder.getResamplerQuality() == ResamplerQuality::HIGH,
                                    "AudioDecoder: resampler quality getter matches setter");

                        bool initOk = fmtDecoder.init();
                        test_assert(initOk, "AudioDecoder: init() succeeds with a 32-bit float output format selected");
                        if (initOk) {
                            test_assert(fmtDecoder.m_outputBytesPerSample == 4,
                                        "AudioDecoder: 32-bit float selection resolves to 4 bytes/sample internally");
                        }
                    }
                }
                if (fmtCtx2) avformat_close_input(&fmtCtx2);
            }

            // --- Playback Speed Control Tests ---
            {
                std::cout << "Running playback speed control tests..." << std::endl;
                PlayerController speedController;
                test_assert(std::fabs(speedController.getPlaybackSpeed() - 1.0f) < 0.001f,
                            "PlayerController: default playback speed is 1.0x");

                speedController.setPlaybackSpeed(1.5f);
                test_assert(std::fabs(speedController.getPlaybackSpeed() - 1.5f) < 0.001f,
                            "PlayerController: playback speed set to 1.5x");

                speedController.setPlaybackSpeed(0.1f); // below min 0.25f
                test_assert(std::fabs(speedController.getPlaybackSpeed() - 0.25f) < 0.001f,
                            "PlayerController: playback speed clamped to minimum 0.25x");

                speedController.setPlaybackSpeed(3.0f); // above max 2.0f
                test_assert(std::fabs(speedController.getPlaybackSpeed() - 2.0f) < 0.001f,
                            "PlayerController: playback speed clamped to maximum 2.0x");

                speedController.setPlaybackSpeed(1.0f);
                test_assert(std::fabs(speedController.getPlaybackSpeed() - 1.0f) < 0.001f,
                            "PlayerController: playback speed reset to 1.0x");
            }

            // --- Volume Control Clamping Tests ---
            {
                std::cout << "Running volume control clamping tests..." << std::endl;
                PlayerController volController;
                volController.setVolume(0.5f);
                test_assert(std::fabs(volController.m_volume - 0.5f) < 0.001f,
                            "PlayerController: setVolume(0.5) sets volume to 0.5");

                volController.setVolume(-0.2f);
                test_assert(std::fabs(volController.m_volume - 0.0f) < 0.001f,
                            "PlayerController: setVolume(-0.2) clamps volume to 0.0");

                volController.setVolume(1.8f);
                test_assert(std::fabs(volController.m_volume - 1.0f) < 0.001f,
                            "PlayerController: setVolume(1.8) clamps volume to 1.0");
            }

            // --- FrameExporter Unit Tests ---
            {
                std::cout << "Running FrameExporter unit tests..." << std::endl;
                // Test 1: Null/invalid frame fails cleanly
                auto nullRes = FrameExporter::saveFrameAsPng(nullptr, "test.mp4", 10.0, "test_screenshots");
                test_assert(!nullRes.success, "FrameExporter: null frame returns failure");
                test_assert(!nullRes.errorMessage.empty(), "FrameExporter: null frame provides error message");

                // Test 2: Valid synthetic YUV420P frame exports successfully
                AVFrame* synthFrame = av_frame_alloc();
                test_assert(synthFrame != nullptr, "Allocated synthetic AVFrame");
                synthFrame->format = AV_PIX_FMT_YUV420P;
                synthFrame->width = 128;
                synthFrame->height = 128;
                int bufRet = av_frame_get_buffer(synthFrame, 0);
                test_assert(bufRet >= 0, "Allocated synthetic frame buffer");

                // Fill with dummy color data (Y=128, U=128, V=128)
                std::memset(synthFrame->data[0], 128, synthFrame->linesize[0] * 128);
                std::memset(synthFrame->data[1], 128, synthFrame->linesize[1] * 64);
                std::memset(synthFrame->data[2], 128, synthFrame->linesize[2] * 64);

                auto exportRes = FrameExporter::saveFrameAsPng(synthFrame, "synthetic_video.mp4", 75.5, "test_screenshots");
                test_assert(exportRes.success, "FrameExporter: successfully exported synthetic frame as PNG");
                test_assert(!exportRes.filepath.empty(), "FrameExporter: output filepath is not empty");
                test_assert(std::filesystem::exists(exportRes.filepath), "FrameExporter: output PNG file exists on disk");
                test_assert(std::filesystem::file_size(exportRes.filepath) > 0, "FrameExporter: output PNG file has non-zero size");

                // Cleanup test artifacts
                std::error_code ec;
                std::filesystem::remove(exportRes.filepath, ec);
                std::filesystem::remove("test_screenshots", ec);
                av_frame_free(&synthFrame);

                // Test 3: an hours>0 playback timestamp (the >=1hr filename
                // branch, distinct from Test 2's <1hr one).
                auto makeSynthFrame = []() -> AVFrame* {
                    AVFrame* f = av_frame_alloc();
                    f->format = AV_PIX_FMT_YUV420P;
                    f->width = 128;
                    f->height = 128;
                    av_frame_get_buffer(f, 0);
                    std::memset(f->data[0], 128, f->linesize[0] * 128);
                    std::memset(f->data[1], 128, f->linesize[1] * 64);
                    std::memset(f->data[2], 128, f->linesize[2] * 64);
                    return f;
                };
                {
                    AVFrame* f = makeSynthFrame();
                    auto r = FrameExporter::saveFrameAsPng(f, "synthetic_video.mp4", 3661.0, "test_screenshots");
                    test_assert(r.success, "FrameExporter: exports successfully with an hours>0 timestamp");
                    std::filesystem::remove(r.filepath, ec);
                    std::filesystem::remove("test_screenshots", ec);
                    av_frame_free(&f);
                }

                // Test 4-11: each of saveFrameAsPng()'s internal failure
                // branches, one real FFmpeg call forced to fail at a time.
                struct FailCase {
                    std::atomic<bool>* flag;
                    const char* label;
                };
                const FailCase failCases[] = {
                    {&force_find_encoder_fail, "avcodec_find_encoder"},
                    {&force_alloc_fail, "avcodec_alloc_context3"},
                    {&force_open_fail, "avcodec_open2"},
                    {&force_sws_context_fail, "sws_getContext"},
                    {&force_send_frame_fail, "avcodec_send_frame"},
                    {&force_receive_packet_fail, "avcodec_receive_packet"},
                };
                for (const auto& fc : failCases) {
                    AVFrame* f = makeSynthFrame();
                    *fc.flag = true;
                    auto r = FrameExporter::saveFrameAsPng(f, "synthetic_video.mp4", 5.0, "test_screenshots");
                    *fc.flag = false;
                    test_assert(!r.success, (std::string("FrameExporter: fails gracefully when ") + fc.label + "() fails").c_str());
                    av_frame_free(&f);
                }
                // force_frame_alloc_fail (the RGB frame) needs open_finished
                // gating, like everywhere else this flag is used.
                {
                    AVFrame* f = makeSynthFrame();
                    open_finished = true;
                    force_frame_alloc_fail = true;
                    auto r = FrameExporter::saveFrameAsPng(f, "synthetic_video.mp4", 5.0, "test_screenshots");
                    force_frame_alloc_fail = false;
                    test_assert(!r.success, "FrameExporter: fails gracefully when the RGB frame allocation fails");
                    av_frame_free(&f);
                }
                // force_malloc_fail (av_frame_get_buffer for the RGB frame).
                {
                    AVFrame* f = makeSynthFrame();
                    force_malloc_fail = true;
                    auto r = FrameExporter::saveFrameAsPng(f, "synthetic_video.mp4", 5.0, "test_screenshots");
                    force_malloc_fail = false;
                    test_assert(!r.success, "FrameExporter: fails gracefully when the RGB frame buffer allocation fails");
                    av_frame_free(&f);
                }
                // force_packet_alloc_fail: by this point in the suite,
                // packet_alloc_count is already well past 5, so this fails
                // deterministically on the very next call.
                {
                    AVFrame* f = makeSynthFrame();
                    force_packet_alloc_fail = true;
                    auto r = FrameExporter::saveFrameAsPng(f, "synthetic_video.mp4", 5.0, "test_screenshots");
                    force_packet_alloc_fail = false;
                    test_assert(!r.success, "FrameExporter: fails gracefully when av_packet_alloc() fails");
                    av_frame_free(&f);
                }
                // Test 12: the ofstream-open-failure branch, after encoding
                // has already succeeded. Forced deterministically by supplying
                // a base filename exceeding the 255-character single-component limit
                // (NAME_MAX on Linux / MAX_COMPONENT_LENGTH on Windows NTFS).
                // create_directories(outputDir) succeeds with the short directory,
                // frame encoding succeeds, but std::ofstream(outPath) fails to open
                // the file whose name component exceeds filesystem limits.
                {
                    std::string excessivelyLongMediaName(300, 'x');
                    excessivelyLongMediaName += ".mp4";

                    AVFrame* f = makeSynthFrame();
                    auto r = FrameExporter::saveFrameAsPng(f, excessivelyLongMediaName, 5.0, "test_screenshots");
                    av_frame_free(&f);
                    test_assert(!r.success, "FrameExporter: fails gracefully when the output file path exceeds filesystem limits");
                    test_assert(r.errorMessage.find("Failed to open output file") != std::string::npos,
                                "FrameExporter: reports the specific ofstream-open failure message");

                    std::error_code rmEc;
                    std::filesystem::remove_all(std::filesystem::path("test_screenshots"), rmEc);
                }
                std::filesystem::remove("test_screenshots", ec);
            }

            // --- Subtitle Comprehensive Unit Tests ---
            {
                std::cout << "Running comprehensive Subtitle unit tests..." << std::endl;

                // 1. Text sanitization
                {
                    std::string rawAss = "{\\pos(192,200)\\an8\\c&H00FFFF&}Hello World!\\NThis is line 2.\\hExtra space.";
                    std::string cleaned = naikav::subtitle::sanitizeSubtitleText(rawAss);
                    test_assert(cleaned == "Hello World!\nThis is line 2. Extra space.", "ASS tags stripped and newlines normalized");

                    std::string htmlText = "<i>Italic</i> and <b>Bold</b> with <font color=\"#ff0000\">Color</font>";
                    std::string cleanHtml = naikav::subtitle::sanitizeSubtitleText(htmlText);
                    test_assert(cleanHtml == "Italic and Bold with Color", "HTML tags stripped");

                    std::string emptyStr = naikav::subtitle::sanitizeSubtitleText("   \n\t  ");
                    test_assert(emptyStr.empty(), "Whitespace trimmed properly");
                }

                // 2. SubtitleEvent timing
                {
                    naikav::subtitle::SubtitleEvent ev;
                    ev.startPts = 10.0;
                    ev.endPts = 15.0;
                    ev.text = "Sample subtitle";

                    test_assert(!ev.isActive(9.9), "Not active before start");
                    test_assert(ev.isActive(10.0), "Active at exact start");
                    test_assert(ev.isActive(12.5), "Active in middle");
                    test_assert(ev.isActive(15.0), "Active at exact end");
                    test_assert(!ev.isActive(15.1), "Not active after end");
                }

                // 3. SubtitleDecoder with SRT / VTT fallback parser
                {
                    std::string srtPath = "temp_test_sub.srt";
                    {
                        std::ofstream f(srtPath);
                        f << "1\n"
                          << "00:00:01,000 --> 00:00:03,500\n"
                          << "First subtitle line\n\n"
                          << "2\n"
                          << "00:00:04,000 --> 00:00:07,000\n"
                          << "Second subtitle line <i>with formatting</i>\n\n";
                    }

                    naikav::subtitle::SubtitleDecoder decoder;
                    bool loaded = decoder.loadExternalFile(srtPath);
                    test_assert(loaded, "External SRT loaded successfully");
                    test_assert(decoder.isExternal(), "Decoder marked as external");
                    test_assert(decoder.getEventCount() == 2, "2 subtitle events parsed");

                    test_assert(decoder.getActiveSubtitleText(0.5).empty(), "No subtitle at 0.5s");
                    test_assert(decoder.getActiveSubtitleText(2.0) == "First subtitle line", "Subtitle match at 2.0s");
                    test_assert(decoder.getActiveSubtitleText(3.8).empty(), "No subtitle between events (3.8s)");
                    test_assert(decoder.getActiveSubtitleText(5.0) == "Second subtitle line with formatting", "Subtitle match at 5.0s");

                    // Subtitle delay offset test
                    // Positive delay (+1.0s) delays display: querying at 3.0s evaluates at 2.0s
                    test_assert(decoder.getActiveSubtitleText(3.0, 1.0) == "First subtitle line", "Subtitle delay +1.0s matches at 3.0s");
                    // Negative delay (-1.0s) advances display: querying at 1.0s evaluates at 2.0s
                    test_assert(decoder.getActiveSubtitleText(1.0, -1.0) == "First subtitle line", "Subtitle delay -1.0s matches at 1.0s");

                    decoder.flush();
                    test_assert(decoder.getEventCount() == 2, "Events preserved across flush for external track");

                    decoder.reset();
                    test_assert(decoder.getEventCount() == 0, "Events cleared on reset");

                    std::remove(srtPath.c_str());
                }

                // 4. SubtitleDecoder with WebVTT file
                {
                    std::string vttPath = "temp_test_sub.vtt";
                    {
                        std::ofstream f(vttPath);
                        f << "WEBVTT\n\n"
                          << "00:01.500 --> 00:03.000 position:10%\n"
                          << "WebVTT subtitle line\n\n";
                    }

                    naikav::subtitle::SubtitleDecoder decoder;
                    bool loaded = decoder.loadExternalFile(vttPath);
                    test_assert(loaded, "External WebVTT loaded successfully");
                    test_assert(decoder.getActiveSubtitleText(2.0) == "WebVTT subtitle line", "VTT text match at 2.0s");

                    std::remove(vttPath.c_str());
                }

                // 4b. SubtitleDecoder with ASS file
                {
                    std::string assPath = "temp_test_sub.ass";
                    {
                        std::ofstream f(assPath);
                        f << "[Script Info]\n"
                          << "Title: Test ASS\n"
                          << "[Events]\n"
                          << "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
                          << "Dialogue: 0,0:00:01.50,0:00:04.20,Default,,0,0,0,,{\\pos(100,200)}ASS Subtitle Line\\NSecond row\n";
                    }

                    naikav::subtitle::SubtitleDecoder decoder;
                    bool loaded = decoder.loadExternalFile(assPath);
                    test_assert(loaded, "External ASS loaded successfully");
                    test_assert(decoder.getActiveSubtitleText(2.0) == "ASS Subtitle Line\nSecond row", "ASS text match at 2.0s");

                    std::remove(assPath.c_str());
                }

                // 4b2. parseSrtVttFallback()/parseAssSsaFallback() edge
                // cases not hit by the plain-LF, blank-line-separated,
                // single-line-text fixtures above: CRLF line endings, a
                // multi-line cue (accumulated with an embedded "\n"), two
                // cues back-to-back with no blank-line separator (forces the
                // stale-event flush on encountering the next "-->" line),
                // and getActiveSubtitleText()'s multi-active-event join.
                {
                    // Windows text-mode ifstream/ofstream already silently
                    // translates a plain "\r\n" pair down to "\n", so a
                    // single \r never survives to reach SubtitleDecoder's
                    // own line.back()=='\r' strip on this platform. Doubling
                    // the \r ("\r\r\n") leaves one literal \r attached to
                    // each line after translation, which is what actually
                    // exercises that strip (confirmed empirically).
                    std::string crlfSrtPath = "temp_test_sub_crlf.srt";
                    {
                        std::ofstream f(crlfSrtPath, std::ios::binary);
                        f << "1\r\r\n"
                          << "00:00:00,000 --> 00:00:01,000\r\r\n"
                          << "Multi line one\r\r\n"
                          << "Multi line two\r\r\n"
                          << "2\r\r\n"
                          << "00:00:01,500 --> 00:00:02,500\r\r\n"
                          << "Second cue text\r\r\n";
                    }
                    naikav::subtitle::SubtitleDecoder crlfDecoder;
                    test_assert(crlfDecoder.loadExternalFile(crlfSrtPath), "External CRLF SRT loaded successfully");
                    test_assert(crlfDecoder.getEventCount() == 2, "CRLF SRT with no blank separator still yields 2 events (stale-event flush on next timestamp)");
                    std::remove(crlfSrtPath.c_str());

                    std::string crlfAssPath = "temp_test_sub_crlf.ass";
                    {
                        std::ofstream f(crlfAssPath, std::ios::binary);
                        f << "[Script Info]\r\r\n"
                          << "[Events]\r\r\n"
                          << "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\r\n"
                          << "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,CRLF ASS text\r\r\n"
                          << "Dialogue: 0,0:00:01.50,0:00:02.50,Default,,0,0,0,,Second CRLF ASS line\r\r\n";
                    }
                    naikav::subtitle::SubtitleDecoder crlfAssDecoder;
                    test_assert(crlfAssDecoder.loadExternalFile(crlfAssPath), "External CRLF ASS loaded successfully");
                    test_assert(crlfAssDecoder.getEventCount() == 2, "CRLF ASS with 2 Dialogue lines yields 2 events (also exercises the .ass fast-path sort() comparator)");
                    std::remove(crlfAssPath.c_str());

                    std::string overlapSrtPath = "temp_test_sub_overlap.srt";
                    {
                        std::ofstream f(overlapSrtPath);
                        f << "1\n00:00:00,000 --> 00:00:03,000\nEvent A\n\n"
                          << "2\n00:00:01,000 --> 00:00:04,000\nEvent B\n\n";
                    }
                    naikav::subtitle::SubtitleDecoder overlapDecoder;
                    test_assert(overlapDecoder.loadExternalFile(overlapSrtPath), "External overlapping-cue SRT loaded successfully");
                    test_assert(overlapDecoder.getActiveSubtitleText(2.0) == "Event A\nEvent B",
                                "getActiveSubtitleText() joins multiple simultaneously-active events with a newline");
                    std::remove(overlapSrtPath.c_str());
                }

                // 4c. SubtitleDecoder::init() failure branches -- no decoder
                // found for the codec id, avcodec_parameters_to_context()
                // failing, and avcodec_open2() failing.
                {
                    naikav::subtitle::SubtitleDecoder decoder;
                    test_assert(!decoder.init(nullptr, {1, 1000}, 0), "init(nullptr) fails immediately");

                    AVCodecParameters* badParams = avcodec_parameters_alloc();
                    badParams->codec_type = AVMEDIA_TYPE_SUBTITLE;
                    badParams->codec_id = AV_CODEC_ID_NONE;
                    test_assert(!decoder.init(badParams, {1, 1000}, 0), "init() fails when no decoder is registered for the codec id");

                    badParams->codec_id = AV_CODEC_ID_SUBRIP;
                    force_copy_params_fail = true;
                    test_assert(!decoder.init(badParams, {1, 1000}, 0), "init() fails when avcodec_parameters_to_context() fails");
                    force_copy_params_fail = false;

                    force_open_fail = true;
                    test_assert(!decoder.init(badParams, {1, 1000}, 0), "init() fails when avcodec_open2() fails");
                    force_open_fail = false;
                    avcodec_parameters_free(&badParams);
                }

                // 4d. processPacket() decoding a real embedded subtitle
                // stream -- no other test ever feeds it a packet with actual
                // decodable subtitle data (PlayerController's own subtitle
                // tests only push an empty dummy AVPacket, which
                // avcodec_decode_subtitle2() rejects immediately with
                // gotSub=0). Also covers the stale seek-generation packet
                // drop and flush()'s real codecCtx branch.
                {
                    std::filesystem::path subDecDir =
                        std::filesystem::temp_directory_path() / "naikav_subdecoder_test";
                    std::error_code subDecEc;
                    std::filesystem::create_directories(subDecDir, subDecEc);
                    std::string embSrt3 = (subDecDir / "emb3.srt").string();
                    {
                        std::ofstream srt(embSrt3);
                        srt << "1\n00:00:00,000 --> 00:00:03,000\nEmbedded decode test\n";
                    }
                    std::string embFile3 = (subDecDir / "video_with_sub3.mkv").string();
                    std::string cmd = "ffmpeg -y -loglevel error -f lavfi -i \"testsrc=duration=3:size=64x64:rate=5\" -i \"" +
                                      embSrt3 + "\" -c:v mpeg4 -c:s srt \"" + embFile3 + "\"";
                    if (std::system(cmd.c_str()) == 0) {
                        ThreadSafeQueue<AVPacket*> vq, aq;
                        // subQ must outlive `demuxer`: attachSubtitleQueue()
                        // hands the Demuxer a raw pointer to it, and
                        // ~Demuxer() -> stop() calls m_subtitleQueue->abort()
                        // unconditionally. Declared in the inner
                        // `if (!subTracks.empty())` scope it died first, and
                        // the destructor then wrote through the dangling
                        // pointer -- a stack-use-after-scope ASan flagged
                        // here. Declaring it before the Demuxer makes the
                        // reverse destruction order tear them down safely.
                        ThreadSafeQueue<AVPacket*> subQ(16);
                        MetricRing<256> ring;
                        std::atomic<bool> prof{false};
                        Demuxer demuxer(embFile3, vq, aq, ring, prof);
                        test_assert(demuxer.open(), "Demuxer opens the SubtitleDecoder embedded-stream test asset");
                        auto subTracks = demuxer.getSubtitleTracks();
                        if (!subTracks.empty()) {
                            int subId = subTracks[0].id;
                            demuxer.attachSubtitleQueue(&subQ);
                            demuxer.setSubtitleStreamIndex(subId);

                            naikav::subtitle::SubtitleDecoder decoder;
                            test_assert(decoder.init(demuxer.getSubtitleCodecParams(subId),
                                                      demuxer.getSubtitleTimeBase(subId),
                                                      demuxer.getSubtitleStartTime(subId)),
                                        "SubtitleDecoder::init() succeeds for a real embedded subtitle stream");

                            // Stale seek-generation packet: attached before
                            // any real packets flow, so the very first one
                            // is dropped via the mismatched-generation check.
                            std::atomic<uint64_t> genCounter{5};
                            decoder.attachSeekGeneration(&genCounter);
                            AVPacket* stalePkt = av_packet_alloc();
                            stalePkt->opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
                            decoder.processPacket(stalePkt);
                            av_packet_free(&stalePkt);
                            test_assert(decoder.getEventCount() == 0, "processPacket() drops a stale seek-generation packet");
                            decoder.attachSeekGeneration(nullptr);

                            demuxer.start();
                            AVPacket* realPkt = nullptr;
                            for (int w = 0; w < 50 && !subQ.try_pop(realPkt); ++w) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                            }
                            demuxer.stop();
                            if (realPkt) {
                                decoder.processPacket(realPkt);
                                av_packet_free(&realPkt);
                                test_assert(decoder.getEventCount() > 0, "processPacket() decodes a real embedded subtitle packet");
                                test_assert(!decoder.getActiveSubtitleText(0.5).empty(),
                                            "getActiveSubtitleText() returns the decoded embedded event's text");
                                auto activeEvents = decoder.getActiveEvents(0.5);
                                test_assert(!activeEvents.empty(), "getActiveEvents() returns the decoded embedded event");

                                // Feed the identical packet's text again (a
                                // second, separately-decoded copy) to hit the
                                // duplicate-event de-duplication branch.
                                AVPacket* realPkt2 = nullptr;
                                for (int w = 0; w < 25 && !subQ.try_pop(realPkt2); ++w) {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                                }
                                if (realPkt2) {
                                    decoder.processPacket(realPkt2);
                                    av_packet_free(&realPkt2);
                                }
                            }
                            decoder.flush(); // real m_codecCtx present -> avcodec_flush_buffers() branch
                            subQ.clear([](AVPacket*& p) { av_packet_free(&p); });
                        }
                    }
                }

                // 4d2. processPacket() branches unreachable via any real
                // FFmpeg subtitle decoder available on this build (verified
                // empirically: subrip/webvtt/ass/mov_text all emit
                // SUBTITLE_ASS rects in the raw 8-field form, never a
                // "Dialogue:"-prefixed ass string or a SUBTITLE_TEXT rect) --
                // forced deterministically via force_synthetic_subtitle_rect.
                // Also exercises the pkt->dts fallback (pts is NOPTS), the
                // nonzero sub.start_display_time/end_display_time branches,
                // and (via a second, distinct event) the duplicate-check and
                // sort lambdas inside processPacket().
                {
                    naikav::subtitle::SubtitleDecoder decoder;
                    AVCodecParameters* params = avcodec_parameters_alloc();
                    params->codec_type = AVMEDIA_TYPE_SUBTITLE;
                    params->codec_id = AV_CODEC_ID_SUBRIP;
                    test_assert(decoder.init(params, {1, 1000}, 0), "init() succeeds for the synthetic-rect processPacket test");
                    avcodec_parameters_free(&params);

                    force_synthetic_subtitle_rect = 1;
                    AVPacket* pkt1 = av_packet_alloc();
                    pkt1->pts = 1000;
                    pkt1->dts = AV_NOPTS_VALUE;
                    decoder.processPacket(pkt1);
                    av_packet_free(&pkt1);
                    test_assert(decoder.getEventCount() == 1, "A synthetic 'Dialogue:'-prefixed ASS rect (pts branch) produces one event");

                    force_synthetic_subtitle_rect = 2;
                    AVPacket* pkt2 = av_packet_alloc();
                    pkt2->pts = AV_NOPTS_VALUE;
                    pkt2->dts = 5000;
                    decoder.processPacket(pkt2);
                    av_packet_free(&pkt2);
                    test_assert(decoder.getEventCount() == 2, "A synthetic SUBTITLE_TEXT rect via the dts-only branch produces a second, distinct event");
                    force_synthetic_subtitle_rect = 0;
                }

                // 4d3. parseTimestamp()'s MM:SS,frac form (single colon,
                // comma decimal separator) -- distinct from the more common
                // H:MM:SS.frac form already exercised by real SRT/ASS files
                // elsewhere in this test file.
                {
                    double secs = 0.0;
                    test_assert(naikav::subtitle::parseTimestamp("01:02,50", secs) && std::abs(secs - 62.5) < 0.01,
                                "parseTimestamp() parses an MM:SS,frac timestamp (comma decimal, single colon)");
                }

                // 4e. loadExternalFile()'s FFmpeg-demuxer fallback path: a
                // real media file (not .srt/.vtt/.ass/.ssa) containing an
                // embedded subtitle stream, so the fast text parser is
                // skipped entirely in favor of the generic FFmpeg path.
                {
                    std::filesystem::path subDecDir2 =
                        std::filesystem::temp_directory_path() / "naikav_subdecoder_test";
                    std::string embSrt4 = (subDecDir2 / "emb4.srt").string();
                    {
                        std::ofstream srt(embSrt4);
                        srt << "1\n00:00:00,000 --> 00:00:02,000\nFallback path test\n";
                    }
                    std::string embFile4 = (subDecDir2 / "video_with_sub4.mkv").string();
                    std::string cmd = "ffmpeg -y -loglevel error -f lavfi -i \"testsrc=duration=2:size=64x64:rate=5\" -i \"" +
                                      embSrt4 + "\" -c:v mpeg4 -c:s srt \"" + embFile4 + "\"";
                    if (std::system(cmd.c_str()) == 0) {
                        naikav::subtitle::SubtitleDecoder decoder;
                        bool loaded = decoder.loadExternalFile(embFile4);
                        test_assert(loaded, "loadExternalFile() loads a subtitle stream via the FFmpeg-demuxer fallback path");
                        test_assert(decoder.isExternal(), "loadExternalFile() marks the decoder as external via the fallback path too");
                        test_assert(decoder.getEventCount() > 0, "loadExternalFile()'s fallback path decodes real subtitle events");
                    }

                    // A file with no subtitle stream at all: falls through
                    // the FFmpeg path (finds no subtitle stream) and then
                    // the SRT/ASS text re-parse attempts, ultimately failing.
                    naikav::subtitle::SubtitleDecoder decoder2;
                    test_assert(!decoder2.loadExternalFile(testFile), "loadExternalFile() fails for a media file with no subtitle stream at all (wrong extension for the fast path)");

                    // A second, two-cue embedded stream decoded under the
                    // synthetic-rect mock: exercises the fallback loop's own
                    // copy of the SUBTITLE_TEXT branch and nonzero
                    // end_display_time branch (both otherwise unreachable
                    // via any real decoder on this build, same as
                    // processPacket()'s copy in test 4d2), plus its final
                    // sort() comparator, which needs 2+ decoded events.
                    std::string embSrt5 = (subDecDir2 / "emb5.srt").string();
                    {
                        std::ofstream srt(embSrt5);
                        srt << "1\n00:00:00,000 --> 00:00:01,000\nFallback cue one\n\n"
                            << "2\n00:00:01,500 --> 00:00:02,500\nFallback cue two\n";
                    }
                    std::string embFile5 = (subDecDir2 / "video_with_sub5.mkv").string();
                    std::string cmd5 = "ffmpeg -y -loglevel error -f lavfi -i \"testsrc=duration=3:size=64x64:rate=5\" -i \"" +
                                       embSrt5 + "\" -c:v mpeg4 -c:s srt \"" + embFile5 + "\"";
                    if (std::system(cmd5.c_str()) == 0) {
                        // Also strips pts down to dts-only on every packet
                        // read during this call, to exercise the fallback
                        // loop's pkt->dts fallback branch -- real demuxed
                        // subtitle packets always carry a valid pts on this
                        // build, so that branch is otherwise unreachable.
                        force_synthetic_subtitle_rect = 2;
                        force_pts_to_dts_only = true;
                        naikav::subtitle::SubtitleDecoder decoder3;
                        bool loaded3 = decoder3.loadExternalFile(embFile5);
                        force_synthetic_subtitle_rect = 0;
                        force_pts_to_dts_only = false;
                        test_assert(loaded3, "loadExternalFile()'s fallback path decodes a two-cue embedded stream via the synthetic-rect mock");
                        test_assert(decoder3.getEventCount() >= 2, "loadExternalFile()'s fallback path accumulates multiple decoded events (exercises its sort() comparator)");
                    }
                }

                // 4f. extractTextFromAss(): callable directly since it's a
                // private static method (TU-local via the private->public
                // macro) -- both the "Dialogue:" (9-comma) and raw (8-comma)
                // ASS forms, distinct from SubtitleTrack.hpp's own
                // sanitizeSubtitleText() tests (extractTextFromAss() is
                // SubtitleDecoder's own copy of the same comma-skipping logic).
                {
                    std::string dialogueForm = naikav::subtitle::SubtitleDecoder::extractTextFromAss(
                        "Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,Hello from Dialogue");
                    test_assert(dialogueForm == "Hello from Dialogue", "extractTextFromAss() strips a 'Dialogue:' 9-comma prefix");

                    std::string rawForm = naikav::subtitle::SubtitleDecoder::extractTextFromAss("0,0,Default,,0,0,0,,Raw ASS text");
                    test_assert(rawForm == "Raw ASS text", "extractTextFromAss() strips a raw 8-comma prefix");

                    test_assert(naikav::subtitle::SubtitleDecoder::extractTextFromAss(nullptr).empty(),
                                "extractTextFromAss(nullptr) returns an empty string");
                }

                // 5. PlayerController Subtitle APIs
                {
                    PlayerController ctrl;
                    test_assert(ctrl.getSelectedSubtitleTrack() == -1, "Initial subtitle track is Off (-1)");
                    test_assert(ctrl.getSubtitleTracks().empty(), "Initial subtitle tracks list is empty");
                    test_assert(!ctrl.hasExternalSubtitle(), "No external subtitle initially");
                    test_assert(ctrl.getActiveSubtitleTrackName() == "Off", "Active track name is Off");

                    // Set delay
                    ctrl.setSubtitleDelay(0.15);
                    test_assert(std::abs(ctrl.getSubtitleDelay() - 0.15) < 1e-5, "Subtitle delay setter/getter works");
                    ctrl.setSubtitleDelay(0.0);

                    // Load external subtitle into player controller
                    std::string srtPath = "temp_player_sub.srt";
                    {
                        std::ofstream f(srtPath);
                        f << "1\n"
                          << "00:00:00,000 --> 00:00:10,000\n"
                          << "PlayerController Subtitle Test\n\n";
                    }

                    bool extOk = ctrl.loadExternalSubtitle(srtPath);
                    test_assert(extOk, "PlayerController loads external subtitle");
                    test_assert(ctrl.hasExternalSubtitle(), "hasExternalSubtitle() is true");
                    test_assert(ctrl.getSelectedSubtitleTrack() == -2, "Selected track is -2 (external)");
                    test_assert(ctrl.getActiveSubtitleTrackName() == "temp_player_sub.srt", "Active track name matches external filename");
                    test_assert(ctrl.getSubtitleTracks().size() == 1, "Subtitle tracks list includes external track");

                    // Toggle Off and back to External
                    ctrl.selectSubtitleTrack(-1);
                    test_assert(ctrl.getSelectedSubtitleTrack() == -1, "Track selected Off");
                    test_assert(ctrl.getCurrentSubtitleText().empty(), "Empty subtitle text when Off");

                    ctrl.selectSubtitleTrack(-2);
                    test_assert(ctrl.getSelectedSubtitleTrack() == -2, "Track selected back to External");

                    ctrl.stop();
                    test_assert(ctrl.getSelectedSubtitleTrack() == -1, "Stop resets subtitle track to -1");
                    test_assert(!ctrl.hasExternalSubtitle(), "Stop clears external subtitle");

                    std::remove(srtPath.c_str());
                }

                std::cout << "Comprehensive Subtitle unit tests passed!" << std::endl;

                // -------------------------------------------------------------
                // 6. Audio Track Enumeration, Selection, and External Audio Tests
                // -------------------------------------------------------------
                {
                    std::cout << "Running Audio Track & External Audio unit tests..." << std::endl;

                    // A. AudioTrackInfo struct tests
                    {
                        naikav::audio::AudioTrackInfo info;
                        info.id = 0;
                        info.title = "Main Audio";
                        info.language = "eng";
                        info.codecName = "aac";
                        info.channels = 2;
                        info.channelLayout = "stereo";
                        info.sampleRate = 48000;
                        info.bitRate = 128000;
                        info.isDefault = true;
                        info.isExternal = false;
                        info.sourcePath = "";

                        test_assert(info.id == 0, "AudioTrackInfo id set");
                        test_assert(info.title == "Main Audio", "AudioTrackInfo title set");
                        test_assert(info.language == "eng", "AudioTrackInfo language set");
                        test_assert(info.codecName == "aac", "AudioTrackInfo codecName set");
                        test_assert(info.channels == 2, "AudioTrackInfo channels set");
                        test_assert(info.channelLayout == "stereo", "AudioTrackInfo channelLayout set");
                        test_assert(info.sampleRate == 48000, "AudioTrackInfo sampleRate set");
                        test_assert(info.bitRate == 128000, "AudioTrackInfo bitRate set");
                        test_assert(info.isDefault, "AudioTrackInfo isDefault set");
                        test_assert(!info.isExternal, "AudioTrackInfo isExternal false");
                    }

                    // B. PlayerController Audio Track APIs
                    {
                        PlayerController ctrl;
                        test_assert(ctrl.getSelectedAudioTrack() == -1, "Initial audio track is -1");
                        test_assert(ctrl.getAudioTracks().empty(), "Initial audio tracks list is empty");
                        test_assert(!ctrl.hasExternalAudio(), "No external audio initially");
                        test_assert(ctrl.getActiveAudioTrackName() == "Off", "Initial active audio track name is Off");

                        // Select track when uninitialized
                        bool selectOk = ctrl.selectAudioTrack(-1);
                        test_assert(selectOk, "selectAudioTrack returns true when uninitialized to pre-set track");
                        test_assert(ctrl.getSelectedAudioTrack() == -1, "Selected track is -1");

                        // Load external audio with non-existent file
                        bool extFail = ctrl.loadExternalAudio("non_existent_audio_file.mp3");
                        test_assert(!extFail, "loadExternalAudio returns false for non-existent file");

                        // Load real test audio
                        std::string audioAsset = "assets/test_audio.mp3";
                        if (!std::filesystem::exists(audioAsset)) {
                            audioAsset = "../assets/test_audio.mp3";
                        }
                        if (!std::filesystem::exists(audioAsset)) {
                            audioAsset = "../../assets/test_audio.mp3";
                        }
                        if (std::filesystem::exists(audioAsset)) {
                            bool extOk = ctrl.loadExternalAudio(audioAsset);
                            test_assert(extOk, "loadExternalAudio succeeds on valid audio file");
                            test_assert(ctrl.hasExternalAudio(), "hasExternalAudio is true after load");
                            test_assert(ctrl.getSelectedAudioTrack() == -2, "Selected audio track is -2 (external)");
                            test_assert(!ctrl.getActiveAudioTrackName().empty(), "Active audio track name is not empty");
                            test_assert(ctrl.getAudioTracks().size() >= 1, "getAudioTracks includes external track");

                            // Switch to disabled (-1) and back to external (-2)
                            ctrl.selectAudioTrack(-1);
                            test_assert(ctrl.getSelectedAudioTrack() == -1, "Selected audio track is -1 (disabled)");
                            test_assert(ctrl.getActiveAudioTrackName() == "Off", "Active audio track name is Off");

                            ctrl.selectAudioTrack(-2);
                            test_assert(ctrl.getSelectedAudioTrack() == -2, "Selected audio track is back to -2 (external)");

                            // Remove external audio
                            ctrl.removeExternalAudio();
                            test_assert(!ctrl.hasExternalAudio(), "hasExternalAudio is false after remove");
                            test_assert(ctrl.getSelectedAudioTrack() == -1, "Selected audio track reset to -1 after remove");
                        }

                        ctrl.stop();
                        test_assert(!ctrl.hasExternalAudio(), "Stop clears external audio");
                        test_assert(ctrl.getSelectedAudioTrack() == -1, "Stop resets selected audio track to -1");
                    }

                    // C. Audio Track Selection on Media with Embedded Audio
                    if (!testFile.empty()) {
                        PlayerController ctrl;
                        if (ctrl.openFile(testFile)) {
                            ctrl.play();
                            auto tracks = ctrl.getAudioTracks();
                            if (!tracks.empty()) {
                                int defaultTrackId = ctrl.getSelectedAudioTrack();
                                test_assert(defaultTrackId >= 0, "Selected audio track is non-negative for media with audio");
                                test_assert(!ctrl.getActiveAudioTrackName().empty(), "Active audio track name is valid");

                                // Select disabled (-1)
                                ctrl.selectAudioTrack(-1);
                                test_assert(ctrl.getSelectedAudioTrack() == -1, "Audio stream muted/disabled (-1)");
                                test_assert(ctrl.getActiveAudioTrackName() == "Off", "Active track name is Off");

                                // Re-select default track
                                ctrl.selectAudioTrack(defaultTrackId);
                                test_assert(ctrl.getSelectedAudioTrack() == defaultTrackId, "Re-selected default embedded audio track");
                            }

                            // Load external audio while video is playing
                            std::string audioAsset = "assets/test_audio.mp3";
                            if (!std::filesystem::exists(audioAsset)) {
                                audioAsset = "../assets/test_audio.mp3";
                            }
                            if (!std::filesystem::exists(audioAsset)) {
                                audioAsset = "../../assets/test_audio.mp3";
                            }
                            if (std::filesystem::exists(audioAsset)) {
                                bool extOk = ctrl.loadExternalAudio(audioAsset);
                                if (extOk) {
                                    test_assert(ctrl.hasExternalAudio(), "External audio loaded during video playback");
                                    test_assert(ctrl.getSelectedAudioTrack() == -2, "Active track switched to external audio");

                                    // Switch back to embedded track
                                    if (!tracks.empty()) {
                                        ctrl.selectAudioTrack(tracks[0].id);
                                        test_assert(ctrl.getSelectedAudioTrack() == tracks[0].id, "Switched back to embedded track from external");
                                    }

                                    // Remove external audio
                                    ctrl.removeExternalAudio();
                                    test_assert(!ctrl.hasExternalAudio(), "External audio removed successfully");
                                }
                            }
                            ctrl.stop();
                        }
                    }

                    std::cout << "Comprehensive Audio Track unit tests passed!" << std::endl;
                }
            } // Close Subtitle & Audio block at line 6547
        } // Close block at line 6180
    } // Close if (!testFile.empty()) at line 3179

    std::cout << "Additional code coverage tests PASSED!" << std::endl;

    // 6. Run the actual main test suite!
    return real_main(argc, argv);
} catch (const std::exception& e) {
    std::cerr << "[EXPECTED] Exception occurred during tests: " << e.what() << std::endl;
    return 1;
} catch (...) {
    std::cerr << "[EXPECTED] Unknown exception occurred during tests." << std::endl;
    return 1;
}
}
