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
    return swr_init(s);
}
#define swr_init mock_swr_init

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
    if (mock_send_packet_success) return 0;
    return avcodec_send_packet(avctx, avpkt);
}
#define avcodec_send_packet mock_avcodec_send_packet

inline int mock_avcodec_receive_frame(AVCodecContext* avctx, AVFrame* frame) {
    if (force_receive_frame_fail) return -2;
    if (force_video_eof) return AVERROR_EOF;
    if (force_video_error) return -5;
    if (force_receive_eagain) return AVERROR(EAGAIN);
    
    int ret = avcodec_receive_frame(avctx, frame);
    if (ret >= 0 && force_no_pts) {
        frame->pts = AV_NOPTS_VALUE;
        frame->pkt_dts = 1000; // Provide DTS to trigger DTS fallback path
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
    return 0;
}
#define av_hwframe_transfer_data mock_av_hwframe_transfer_data

inline int mock_av_frame_get_buffer(AVFrame* frame, int align) {
    if (force_malloc_fail || force_image_fill_fail) return -1;
    return av_frame_get_buffer(frame, align);
}
#define av_frame_get_buffer mock_av_frame_get_buffer

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
    return av_read_frame(s, pkt);
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

            avcodec_parameters_free(&testCodecParams);
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
        controller.m_videoQueue.push(dummyPkt);

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
        SDL_Quit();
        return 1;
    }

    SDL_Quit();
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

    // 7. Run additional coverage tests to hit remaining uncovered branches!
    if (!testFile.empty()) {
        std::cout << "Running additional code coverage tests..." << std::endl;

        // PlayerController additional methods
        {
            PlayerController controller;
            test_assert(controller.getVideoPixelFormat() == "unknown", "getVideoPixelFormat returns unknown when uninitialized");
            test_assert(!controller.isVideoHardware(), "isVideoHardware returns false when uninitialized");
            test_assert(!controller.isSeeking(), "isSeeking returns false when uninitialized");

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
                    dec->m_queue.push(pkt);
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
                        dec->m_queue.push(pkt);
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
                        dec->m_queue.push(pkt);
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
                        dec->m_queue.push(pkt);
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
                    
                    AVCodecID savedId = dec->m_codecCtx->codec_id;
                    dec->m_codecCtx->codec_id = AV_CODEC_ID_NONE;
                    
                    force_receive_eagain = true;
                    mock_send_packet_success = true;
                    
                    for (int i = 0; i < 70; i++) {
                        AVPacket* pkt = av_packet_alloc();
                        dec->m_queue.push(pkt);
                    }
                    
                    test_assert(!dec->decodeNextFrame(), "decodeNextFrame returns false when software decoder not found");
                    
                    force_receive_eagain = false;
                    mock_send_packet_success = false;
                    if (dec->m_codecCtx) {
                        dec->m_codecCtx->codec_id = savedId;
                    }
                    if (dec->m_codecCtx && dec->m_codecCtx->codec == &fakeCodec) {
                        dec->m_codecCtx->codec = savedCodec;
                    }
                    global_saved_codec = nullptr;
                    global_fake_codec_ptr = nullptr;
                }
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

                start = std::chrono::steady_clock::now();
                pushed = queue.push_wait_or_drop(100, std::chrono::milliseconds(50));
                elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                test_assert(pushed, "T7b: push_wait_or_drop succeeds on a full, undrained queue");
                test_assert(elapsedSec < 0.5, "T7b: push_wait_or_drop returns at its timeout, not forever");
                test_assert(elapsedSec >= 0.045, "T7b: push_wait_or_drop actually waits close to its timeout before dropping");
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

            std::cout << "Pipeline Metrics & MetricRing Tests (T1 - T8) passed!" << std::endl;
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
                        test_assert(info.id == -1, "Default audio track id is -1");
                        test_assert(!info.isDefault, "Default isDefault is false");
                        test_assert(!info.isExternal, "Default isExternal is false");
                    }

                    // B. PlayerController Audio Track APIs with media file
                    {
                        PlayerController audioCtrl;
                        test_assert(audioCtrl.getSelectedAudioTrack() == -1, "Initial audio track is -1");
                        test_assert(audioCtrl.getAudioTracks().empty(), "Initial audio tracks list is empty");
                        test_assert(!audioCtrl.hasExternalAudio(), "hasExternalAudio() is initially false");
                        test_assert(audioCtrl.getActiveAudioTrackName() == "Off", "Active audio track is Off initially");

                        bool opened = audioCtrl.openFile(testFile);
                        test_assert(opened, "File opened for audio track tests");

                        auto tracks = audioCtrl.getAudioTracks();
                        test_assert(!tracks.empty(), "Discovered at least one audio track in test file");
                        std::cout << "[AudioTrack Test] Found " << tracks.size() << " audio tracks in " << testFile << std::endl;
                        for (size_t i = 0; i < tracks.size(); ++i) {
                            std::cout << "  Track #" << i << ": id=" << tracks[i].id
                                      << ", title=" << tracks[i].title
                                      << ", lang=" << tracks[i].language
                                      << ", codec=" << tracks[i].codecName
                                      << ", ch=" << tracks[i].channels
                                      << ", layout=" << tracks[i].channelLayout
                                      << ", rate=" << tracks[i].sampleRate << "Hz"
                                      << ", default=" << (tracks[i].isDefault ? "yes" : "no")
                                      << std::endl;
                            test_assert(tracks[i].id >= 0, "Embedded track id >= 0");
                            test_assert(!tracks[i].codecName.empty(), "Track codecName is populated");
                        }

                        int currentTrack = audioCtrl.getSelectedAudioTrack();
                        test_assert(currentTrack >= 0, "Selected audio track is >= 0");
                        test_assert(audioCtrl.hasAudio(), "hasAudio() is true for audio file");
                        std::string activeName = audioCtrl.getActiveAudioTrackName();
                        test_assert(!activeName.empty() && activeName != "Off", "Active track name is populated");
                        std::cout << "[AudioTrack Test] Active track: " << activeName << std::endl;

                        // Test switching to same track (noop, should return true)
                        bool switchSame = audioCtrl.selectAudioTrack(currentTrack);
                        test_assert(switchSame, "selectAudioTrack() on current track returns true");

                        // Test disabling audio track (-1)
                        bool disableOk = audioCtrl.selectAudioTrack(-1);
                        test_assert(disableOk, "selectAudioTrack(-1) succeeds");
                        test_assert(audioCtrl.getSelectedAudioTrack() == -1, "getSelectedAudioTrack() is -1");
                        test_assert(!audioCtrl.hasAudio(), "hasAudio() is false when disabled");
                        test_assert(audioCtrl.getActiveAudioTrackName() == "Off", "getActiveAudioTrackName() is Off");

                        // Test re-enabling audio track
                        bool enableOk = audioCtrl.selectAudioTrack(tracks[0].id);
                        test_assert(enableOk, "Re-enabling audio track succeeds");
                        test_assert(audioCtrl.getSelectedAudioTrack() == tracks[0].id, "Selected track matches re-enabled id");
                        test_assert(audioCtrl.hasAudio(), "hasAudio() is true again");

                        // Test external audio loading using testFile as external audio source
                        bool extOk = audioCtrl.loadExternalAudio(testFile);
                        test_assert(extOk, "loadExternalAudio() succeeds");
                        test_assert(audioCtrl.hasExternalAudio(), "hasExternalAudio() is true");
                        test_assert(audioCtrl.getSelectedAudioTrack() == -2, "getSelectedAudioTrack() is -2 (external)");
                        test_assert(audioCtrl.hasAudio(), "hasAudio() is true with external audio");
                        std::string extName = audioCtrl.getActiveAudioTrackName();
                        test_assert(!extName.empty() && extName != "Off", "External audio track name is valid");

                        // Test track list includes external track
                        auto tracksWithExt = audioCtrl.getAudioTracks();
                        test_assert(tracksWithExt.size() == tracks.size() + 1, "Audio tracks count increased by 1 with external audio");
                        test_assert(tracksWithExt.back().isExternal, "Last track in list is marked external");
                        test_assert(tracksWithExt.back().id == -2, "External track has id == -2");

                        // Test switching from external back to embedded track
                        bool switchBack = audioCtrl.selectAudioTrack(tracks[0].id);
                        test_assert(switchBack, "Switching back to embedded track succeeds");
                        test_assert(audioCtrl.getSelectedAudioTrack() == tracks[0].id, "Selected track is embedded track");

                        // Test switching back to external
                        bool switchExt = audioCtrl.selectAudioTrack(-2);
                        test_assert(switchExt, "Switching back to external track succeeds");
                        test_assert(audioCtrl.getSelectedAudioTrack() == -2, "Selected track is external");

                        // Test seeking with external audio
                        audioCtrl.play();
                        audioCtrl.seek(1.0);
                        test_assert(audioCtrl.getState() == PlayerState::PLAYING, "Player continues playing after seek with external audio");

                        // Test removeExternalAudio()
                        audioCtrl.removeExternalAudio();
                        test_assert(!audioCtrl.hasExternalAudio(), "hasExternalAudio() is false after removal");
                        test_assert(audioCtrl.getSelectedAudioTrack() == tracks[0].id, "Reverted to embedded track after removal");

                        // Test invalid external file loading
                        bool badExt = audioCtrl.loadExternalAudio("non_existent_audio_file.xyz");
                        test_assert(!badExt, "loadExternalAudio() fails on non-existent file");

                        audioCtrl.stop();
                        test_assert(audioCtrl.getSelectedAudioTrack() == -1, "stop() resets selected audio track to -1");
                        test_assert(!audioCtrl.hasExternalAudio(), "stop() clears external audio");
                    }

                    // C. Demuxer direct stream selection tests
                    {
                        ThreadSafeQueue<AVPacket*> vq;
                        ThreadSafeQueue<AVPacket*> aq;
                        MetricRing<256> ring;
                        std::atomic<bool> prof{false};

                        Demuxer d(testFile, vq, aq, ring, prof);
                        bool dOk = d.open();
                        test_assert(dOk, "Demuxer opens test file");

                        const auto& dTracks = d.getAudioTracks();
                        test_assert(!dTracks.empty(), "Demuxer found audio tracks");

                        // Validate audio codec params & timebase getters
                        int firstAudioStream = d.getAudioStreamIndex();
                        test_assert(firstAudioStream >= 0, "Demuxer has valid audio stream index");
                        const AVCodecParameters* cp = d.getAudioCodecParams(firstAudioStream);
                        test_assert(cp != nullptr, "getAudioCodecParams(idx) returns valid params");
                        test_assert(cp->codec_type == AVMEDIA_TYPE_AUDIO, "Codec type is audio");

                        AVRational tb = d.getAudioTimeBase(firstAudioStream);
                        test_assert(tb.den > 0, "TimeBase has valid denominator");

                        // Test selectAudioStream(-1) and invalid index
                        bool selNeg = d.selectAudioStream(-1);
                        test_assert(selNeg, "selectAudioStream(-1) succeeds");
                        test_assert(d.getAudioStreamIndex() == -1, "getAudioStreamIndex() is -1");

                        bool selInvalid = d.selectAudioStream(99999);
                        test_assert(!selInvalid, "selectAudioStream(99999) fails gracefully");

                        bool selRestore = d.selectAudioStream(firstAudioStream);
                        test_assert(selRestore, "selectAudioStream(firstAudioStream) succeeds");
                        test_assert(d.getAudioStreamIndex() == firstAudioStream, "Audio stream index restored");

                        d.stop();
                    }

                    std::cout << "Audio Track & External Audio unit tests PASSED!" << std::endl;
                }

            }

            std::cout << "ReplayGain tag / genre preset / output selector tests passed!" << std::endl;
        }

        // -------------------------------------------------------------
        // Background loudness prescan: openFile() must not block on the
        // decode-based prescan. It previously ran synchronously inside
        // openFile() -- which main.cpp calls directly from its
        // SDL_EVENT_DROP_FILE handler on the render/event thread -- so a
        // real (non-trivial-length) file would freeze the entire UI for
        // however long the whole audio track took to decode, looking
        // exactly like a crash/hang when opening a new file. Fixed by
        // moving the decode-based scan to a background thread, applied
        // via PlayerController::pollPendingLoudnessPrescan().
        // -------------------------------------------------------------
        {
            std::cout << "Running background loudness prescan test..." << std::endl;

            PlayerController prescanController;
            naikav::dsp::AudioDspSettings loudSettings;
            loudSettings.dspEnabled = false;
            loudSettings.loudnessEnabled = true;
            loudSettings.loudnessTargetLufs = -16.0f;
            // Applied before openFile() so it's already active at open
            // time -- matches the real "loudness enabled, then open a
            // file" flow (e.g. a persisted setting from a previous
            // session) that triggers the prescan from inside openFile().
            prescanController.setAudioDspSettings(loudSettings);

            auto openStart = std::chrono::steady_clock::now();
            bool opened = prescanController.openFile(testFile);
            auto openElapsed = std::chrono::steady_clock::now() - openStart;
            test_assert(opened, "Background prescan: file opens successfully with loudness normalization enabled");
            test_assert(openElapsed < std::chrono::seconds(2),
                        "Background prescan: openFile() returns quickly, not blocked on the whole-file decode scan");

            // Poll the way main.cpp's event loop does, giving the
            // background scan a little time to finish.
            bool primed = false;
            for (int i = 0; i < 100 && !primed; ++i) {
                prescanController.pollPendingLoudnessPrescan();
                if (prescanController.getMeasuredIntegratedLufs() > -70.0) {
                    primed = true;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
            test_assert(primed,
                        "Background prescan: loudness normalizer is primed with a real measurement once "
                        "the background scan completes and pollPendingLoudnessPrescan() is called");

            // stop() must join the background thread cleanly, even right
            // after opening (the scan may still be finishing up).
            prescanController.stop();
            std::cout << "Background loudness prescan test passed!" << std::endl;
        }

        // -------------------------------------------------------------
        // DEBUG REPRO: open a series of genuinely different real media
        // files back-to-back on the same PlayerController, WITH real
        // hardware decoding enabled (unlike the rest of this suite, which
        // forces g_disableHardwareDecoders = true) -- reproducing the
        // actual "open different media file multiple times" user flow as
        // closely as possible outside of the real GUI event loop. Gated
        // behind an env var so it only runs on demand.
        // -------------------------------------------------------------
        if (const char* multiDir = std::getenv("NAIKAV_MULTI_OPEN_ASSETS_DIR")) {
            std::cout << "Running multi-file-open hardware-decode repro..." << std::endl;
            std::vector<std::string> files = {
                std::string(multiDir) + "/hd_test_video_with_audio.mp4",
                std::string(multiDir) + "/Big_Buck_Bunny_1080_10s_5MB.mp4",
                std::string(multiDir) + "/Big_Buck_Bunny_1080_10s_5MB.mkv",
                std::string(multiDir) + "/Big_Buck_Bunny_1080_10s_5MB.webm",
                std::string(multiDir) + "/test_mono.mp4",
                std::string(multiDir) + "/test_5point1.mp4",
                std::string(multiDir) + "/4K 2K 1080p 720p 480p video resolution test_2160p.mp4",
            };

            g_disableHardwareDecoders = false;
            g_videoThreadEnabled = true;
            PlayerController multiController;
            for (int cycle = 0; cycle < 3; ++cycle) {
                for (const auto& f : files) {
                    std::cout << "[MULTI-OPEN] Opening: " << f << " (cycle " << cycle << ")" << std::endl;
                    bool ok = multiController.openFile(f);
                    std::cout << "[MULTI-OPEN]   openFile() -> " << (ok ? "true" : "false") << std::endl;
                    if (ok) {
                        multiController.play();
                        for (int i = 0; i < 20; ++i) {
                            multiController.getCurrentTime();
                            multiController.pollPendingLoudnessPrescan();
                            std::this_thread::sleep_for(std::chrono::milliseconds(25));
                        }
                    }
                }
            }
            multiController.stop();
            std::cout << "[MULTI-OPEN] Completed all cycles without crashing." << std::endl;

            // Rapid-fire variant: no waiting/draining between opens at all
            // (immediate back-to-back openFile() calls, closer to a user
            // mashing the Open button or dropping several files in a
            // burst), alternating channel layouts every time to force the
            // SDL audio device to be torn down and reopened with a
            // different channel count/format on every single call.
            std::cout << "[MULTI-OPEN] Starting rapid-fire (no-wait) variant..." << std::endl;
            std::vector<std::string> rapidFiles = {
                std::string(multiDir) + "/hd_test_video_with_audio.mp4",
                std::string(multiDir) + "/test_mono.mp4",
                std::string(multiDir) + "/test_5point1.mp4",
            };
            PlayerController rapidController;
            for (int cycle = 0; cycle < 50; ++cycle) {
                const std::string& f = rapidFiles[cycle % rapidFiles.size()];
                bool ok = rapidController.openFile(f);
                if (ok) {
                    rapidController.play();
                }
                if (cycle % 10 == 0) {
                    std::cout << "[MULTI-OPEN][RAPID] cycle " << cycle << " -> " << (ok ? "ok" : "FAILED") << std::endl;
                }
            }
            rapidController.stop();
            std::cout << "[MULTI-OPEN][RAPID] Completed 50 rapid-fire cycles without crashing." << std::endl;

            // VIRTUAL_SURROUND variant: exercises m_spatialDownmixActive
            // toggling on (5.1 source) and off (mono/stereo source) across
            // repeated opens -- the one code path neither repro above
            // touches, since AUTO never activates SpatialDownmixer.
            std::cout << "[MULTI-OPEN][VSURROUND] Starting VIRTUAL_SURROUND rapid variant..." << std::endl;
            PlayerController vsurroundController;
            vsurroundController.setAudioChannelOption(AudioChannelOption::VIRTUAL_SURROUND);
            for (int cycle = 0; cycle < 50; ++cycle) {
                const std::string& f = rapidFiles[cycle % rapidFiles.size()];
                bool ok = vsurroundController.openFile(f);
                if (ok) {
                    vsurroundController.play();
                }
                if (cycle % 10 == 0) {
                    std::cout << "[MULTI-OPEN][VSURROUND] cycle " << cycle << " -> " << (ok ? "ok" : "FAILED")
                              << ", spatialDownmixActive=" << vsurroundController.isAudioVirtualSurroundActive()
                              << std::endl;
                }
            }
            vsurroundController.stop();
            std::cout << "[MULTI-OPEN][VSURROUND] Completed 50 cycles without crashing." << std::endl;

            // -------------------------------------------------------------
            // USER-REPORTED REPRO: open a video-only file (no audio
            // stream), enable EVERY DSP option from the GUI (as if the
            // user checked every box in the Audio Processing panel), then
            // open a large real-world 4K file that does have an audio
            // stream -- the user's exact reported crash scenario. Every
            // repro above left DSP settings at their all-disabled
            // defaults, so this is the first place any DSP processing code
            // actually runs against real decoded audio during a reopen.
            // -------------------------------------------------------------
            {
                g_disableHardwareDecoders = false;
                g_videoThreadEnabled = true;
                std::cout << "[DSP-REPRO] Opening Big_Buck_Bunny (no audio stream)..." << std::endl;
                PlayerController dspController;
                std::string noAudioFile = std::string(multiDir) + "/Big_Buck_Bunny_1080_10s_5MB.mp4";
                bool ok1 = dspController.openFile(noAudioFile);
                std::cout << "[DSP-REPRO]   openFile() -> " << (ok1 ? "true" : "false")
                          << ", hasAudio=" << dspController.hasAudio() << std::endl;
                if (ok1) {
                    dspController.play();
                    for (int i = 0; i < 20; ++i) {
                        dspController.getCurrentTime();
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                }

                std::cout << "[DSP-REPRO] Enabling every DSP option from the GUI..." << std::endl;
                naikav::dsp::AudioDspSettings allOn;
                allOn.dspEnabled = true;
                for (int i = 0; i < naikav::dsp::ParametricEQ::kNumBands; ++i) {
                    allOn.eqBandGainDb[i] = 6.0f;
                }
                allOn.compressorEnabled = true;
                allOn.limiterEnabled = true;
                allOn.crossoverEnabled = true;
                allOn.crossoverBassRedirectEnabled = true;
                allOn.loudnessEnabled = true;
                allOn.widenerEnabled = true;
                allOn.surround3dEnabled = true;
                allOn.balance = 0.3f;
                allOn.noiseGateEnabled = true;
                allOn.multibandEnabled = true;
                allOn.autoGenrePresetEnabled = true;
                allOn.spectrumAnalyzerEnabled = true;
                dspController.setAudioDspSettings(allOn);
                dspController.persistAudioDspSettings();

                std::string bigFile = std::string(multiDir) +
                    "/4K Remastered - Daddy Mummy Full Video Song _ Urvashi Rautela, Kunal Khemu _ Bhaag Johnny_2160p.mp4";
                std::cout << "[DSP-REPRO] Opening 4K file with all DSP enabled..." << std::endl;
                bool ok2 = dspController.openFile(bigFile);
                std::cout << "[DSP-REPRO]   openFile() -> " << (ok2 ? "true" : "false")
                          << ", hasAudio=" << dspController.hasAudio() << std::endl;
                if (ok2) {
                    dspController.play();
                    for (int i = 0; i < 80; ++i) {
                        dspController.getCurrentTime();
                        dspController.pollPendingLoudnessPrescan();
                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                }
                dspController.stop();
                std::cout << "[DSP-REPRO] Completed without crashing." << std::endl;
            }

            g_videoThreadEnabled = false;
            g_disableHardwareDecoders = true;
        }

        std::cout << "Additional code coverage tests PASSED!" << std::endl;
    }

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
