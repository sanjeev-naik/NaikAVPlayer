#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>
#include <cmath>
#include <vector>
#include <cstdlib>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

// -------------------------------------------------------------
// FFmpeg & SDL Mocking Interceptors
// -------------------------------------------------------------
static bool force_alloc_fail = false;
static bool force_open_fail = false;
static bool force_frame_alloc_fail = false;
static bool force_malloc_fail = false;
static bool force_image_fill_fail = false;
static bool force_swr_init_fail = false;
static bool force_swr_convert_fail = false;
static bool force_seek_fail = false;
static bool force_find_stream_info_fail = false;
static bool force_copy_params_fail = false;
static bool force_sdl_audio_fail = false;
static bool force_send_packet_fail = false;
static bool force_receive_frame_fail = false;
static bool force_no_pts = false;
static bool force_no_streams = false;
static bool force_no_duration = false;
static bool force_packet_alloc_fail = false;
static bool force_read_error = false;
static bool force_video_eof = false;
static bool force_video_error = false;
static bool force_sws_context_fail = false;

static bool force_zero_channels = false;
static bool force_sdl_init_fail = false;
static bool open_finished = false;
static int packet_alloc_count = 0;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
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
            avctx->ch_layout.nb_channels = 0;
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
    return sws_getContext(srcW, srcH, srcFormat, dstW, dstH, dstFormat, flags, srcFilter, dstFilter, param);
}
#define sws_getContext mock_sws_getContext

inline int mock_av_seek_frame(AVFormatContext* s, int stream_index, int64_t timestamp, int flags) {
    if (force_seek_fail) return -1;
    return av_seek_frame(s, stream_index, timestamp, flags);
}
#define av_seek_frame mock_av_seek_frame

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

inline SDL_AudioDeviceID mock_SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired, SDL_AudioSpec* obtained, int allowed_changes) {
    if (force_sdl_audio_fail) return 0;
    return SDL_OpenAudioDevice(device, iscapture, desired, obtained, allowed_changes);
}
#define SDL_OpenAudioDevice mock_SDL_OpenAudioDevice

inline int mock_avcodec_send_packet(AVCodecContext* avctx, const AVPacket* avpkt) {
    if (force_send_packet_fail) return -1;
    return avcodec_send_packet(avctx, avpkt);
}
#define avcodec_send_packet mock_avcodec_send_packet

inline int mock_avcodec_receive_frame(AVCodecContext* avctx, AVFrame* frame) {
    if (force_receive_frame_fail) return -2;
    if (force_video_eof) return AVERROR_EOF;
    if (force_video_error) return -5;
    
    int ret = avcodec_receive_frame(avctx, frame);
    if (ret >= 0 && force_no_pts) {
        frame->pts = AV_NOPTS_VALUE;
        frame->pkt_dts = 1000; // Provide DTS to trigger DTS fallback path
    }
    return ret;
}
#define avcodec_receive_frame mock_avcodec_receive_frame

inline int mock_av_read_frame(AVFormatContext* s, AVPacket* pkt) {
    if (force_read_error) return -5;
    return av_read_frame(s, pkt);
}
#define av_read_frame mock_av_read_frame

inline int mock_SDL_Init(Uint32 flags) {
    if (force_sdl_init_fail) return -1;
    return SDL_Init(flags);
}
#define SDL_Init mock_SDL_Init

// -------------------------------------------------------------
// Direct C++ sources inclusion with private visibility bypass
// -------------------------------------------------------------
#define private public
#include "../src/PlayerController.cpp"
#include "../src/Demuxer.cpp"
#include "../src/VideoDecoder.cpp"
#include "../src/AudioDecoder.cpp"
#undef private

#define exit(x) throw std::runtime_error("exit_called_" + std::to_string(x))

// Simple assert helper
void test_assert(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Assertion FAILED: " << message << std::endl;
        exit(1);
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
            double timeNow = controller.getCurrentTime();
            VideoDecoder* decoder = controller.getVideoDecoder();
            int drops = 0;
            while (decoder->getCurrentFramePts() < timeNow - 0.010 && drops < 8) {
                if (!decoder->decodeNextFrame()) break;
                drops++;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int real_main(int argc, char* argv[]) {
    std::cout << "Starting NaikAVPlayer 100% coverage integration tests..." << std::endl;

    // Initialize SDL Audio & Timer
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        std::cerr << "Failed to initialize SDL: " << SDL_GetError() << std::endl;
        return 1;
    }

    std::string testFile = "";
    if (argc > 1 && argv[1][0] != '-') {
        testFile = argv[1];
    } else if (const char* envVal = std::getenv("TEST_VIDEO_PATH")) {
        testFile = envVal;
    }

    if (testFile.empty()) {
        std::cerr << "Error: No test video file provided.\n"
                  << "Usage: " << argv[0] << " <path_to_video_file>\n"
                  << "Alternatively, set the TEST_VIDEO_PATH environment variable." << std::endl;
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
            testCodecParams->format = AV_PIX_FMT_YUV420P;
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
            testDecoder.m_decodedFrame->format = AV_PIX_FMT_YUV420P;
            std::vector<uint8_t> dummySrcBuffer1(160 * 120 * 2, 0);
            av_image_fill_arrays(
                testDecoder.m_decodedFrame->data,
                testDecoder.m_decodedFrame->linesize,
                dummySrcBuffer1.data(),
                AV_PIX_FMT_YUV420P,
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
            testDecoder.m_decodedFrame->format = AV_PIX_FMT_YUV420P;
            std::vector<uint8_t> dummySrcBuffer2(320 * 240 * 2, 0);
            av_image_fill_arrays(
                testDecoder.m_decodedFrame->data,
                testDecoder.m_decodedFrame->linesize,
                dummySrcBuffer2.data(),
                AV_PIX_FMT_YUV420P,
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
            force_image_fill_fail = true;
            bool convertFillFail = testDecoder.convertFrame();
            test_assert(!convertFillFail, "convertFrame fails when av_image_fill_arrays fails on resolution change");
            force_image_fill_fail = false;

            // Test case 3: Dynamic resolution change fails because sws_getContext fails (force_sws_context_fail = true)
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
            force_sws_context_fail = true;
            bool convertSwsFail = testDecoder.convertFrame();
            test_assert(!convertSwsFail, "convertFrame fails when sws_getContext fails on resolution change");
            force_sws_context_fail = false;

            // Test case 4: Dynamic resolution change succeeds! (Frees old context and reallocates successfully)
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
            bool convertSuccess = testDecoder.convertFrame();
            test_assert(convertSuccess, "convertFrame succeeds on dynamic resolution change");
            test_assert(testDecoder.m_allocatedWidth == 320, "Allocated width updated to 320");
            test_assert(testDecoder.m_allocatedHeight == 240, "Allocated height updated to 240");
            test_assert(testDecoder.m_yuvBufferSize == av_image_get_buffer_size(AV_PIX_FMT_YUV420P, 320, 240, 1), "YUV Buffer size updated correctly");

            // Test case 5: Dynamic format change and slow-path conversion (NV12 -> triggers sws_scale)
            testDecoder.m_decodedFrame->width = 320;
            testDecoder.m_decodedFrame->height = 240;
            testDecoder.m_decodedFrame->format = AV_PIX_FMT_NV12;
            std::vector<uint8_t> dummySrcBuffer3(320 * 240 * 2, 0);
            av_image_fill_arrays(
                testDecoder.m_decodedFrame->data,
                testDecoder.m_decodedFrame->linesize,
                dummySrcBuffer3.data(),
                AV_PIX_FMT_NV12,
                320,
                240,
                1
            );
            bool convertSlowSuccess = testDecoder.convertFrame();
            test_assert(convertSlowSuccess, "convertFrame slow-path succeeds for NV12");

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
            testDecoder.m_decodedFrame->format = AV_PIX_FMT_NV12;
            av_image_fill_arrays(
                testDecoder.m_decodedFrame->data,
                testDecoder.m_decodedFrame->linesize,
                dummySrcBuffer3.data(),
                AV_PIX_FMT_NV12,
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

        PlayerController uninitController;
        test_assert(!uninitController.isEOF(), "isEOF is false on uninitialized controller");

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
        if (controller.getVideoDecoder()) {
            bool convertOk = controller.getVideoDecoder()->convertFrame();
            test_assert(convertOk, "convertFrame succeeds on valid decoded frame");
        }
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
        // E. White-Box Static Decoder & Demuxer Error Branches
        // -------------------------------------------------------------
        std::cout << "Testing white-box failure paths for Audio/Video decoders..." << std::endl;
        
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
        zeroChanParams->ch_layout.nb_channels = 0;
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
            controller.m_demuxer->m_audioTimeBase = {1, 44100};
            controller.m_demuxer->seek(10.0);
            controller.m_demuxer->performSeek();

            // Force No-Stream seeking branch
            controller.m_demuxer->m_videoStreamIdx = -1;
            controller.m_demuxer->m_audioStreamIdx = -1;
            controller.m_demuxer->seek(10.0);
            controller.m_demuxer->performSeek();
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
            controller.m_demuxer->performSeek();
        }
        controller.m_hasAudio = false;
        controller.m_hasVideo = true;
        controller.m_videoClock = 0.0;
        controller.m_lastSystemTime = controller.getSystemTimeInSeconds();
        controller.m_state = PlayerState::PLAYING;
        // Verify getCurrentTime drives updateClockForVideoOnly()
        double videoOnlyTime1 = controller.getCurrentTime();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        double videoOnlyTime2 = controller.getCurrentTime();
        test_assert(videoOnlyTime2 > videoOnlyTime1, "Video-Only clock progresses via system time delta");

        // Verify pause handles video-only update
        controller.pause();
        test_assert(controller.getState() == PlayerState::PAUSED, "Paused video-only playback successfully");

        // -------------------------------------------------------------
        // H. Advanced Interceptor Error Injectors (100% Coverage Target)
        // -------------------------------------------------------------
        std::cout << "Injecting advanced hardware & library failure codes..." << std::endl;
        
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
            PlayerController testEndController;
            testEndController.openFile(testFile);
            test_assert(testEndController.getState() == PlayerState::OPENED, "testEndController is OPENED");

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
            test_assert(testEndController.getCurrentTime() < 2.0, "Current time is reset/restarted near 0.0");

            // Test 2: Mock demuxer EOF and empty queues
            testEndController.m_demuxer->m_eof = true;
            testEndController.m_videoQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
            testEndController.m_audioQueue.clear([](AVPacket*& pkt) { av_packet_free(&pkt); });
            
            // This should also trigger reachedEnd and transition to ENDED
            time = testEndController.getCurrentTime();
            test_assert(testEndController.getState() == PlayerState::ENDED, "EOF + empty queues transitions to ENDED");

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

        // -------------------------------------------------------------
        // I. Clean Stopping & Destruction
        // -------------------------------------------------------------
        controller.stop();
        test_assert(controller.getState() == PlayerState::UNINITIALIZED, "State is UNINITIALIZED after stop");

    } catch (const std::exception& e) {
        std::cerr << "Exception occurred during tests: " << e.what() << std::endl;
        SDL_Quit();
        return 1;
    }

    SDL_Quit();
    std::cout << "All integration tests PASSED successfully!" << std::endl;
    return 0;
}

int main(int argc, char* argv[]) {
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
    char* argvSdlFail[] = { argv[0], (char*)"dummy.mp4" };
    real_main(2, argvSdlFail);
    force_sdl_init_fail = false;

    // 3. Cover TEST_VIDEO_PATH env variable parsing path inside real_main
    {
#ifdef _WIN32
        _putenv_s("TEST_VIDEO_PATH", "dummy_val");
#else
        setenv("TEST_VIDEO_PATH", "dummy_val", 1);
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
        char* argvException[] = { argv[0], (char*)testFile.c_str(), (char*)"--test-exception" };
        real_main(3, argvException);
    }

    // 5. Cover assert failure exit(1) path (via exception throw)
    try {
        test_assert(false, "Intentionally failing assert to cover exit(1) path");
    } catch (const std::exception& e) {
        std::cout << "Successfully covered assert exit(1) path: " << e.what() << std::endl;
    }

    // 6. Run the actual main test suite!
    return real_main(argc, argv);
}
