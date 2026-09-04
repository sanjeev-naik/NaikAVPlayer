// Standalone timing harness for the HDR -> SDR conversion path.
//
// Feeds real packets from a media file into VideoDecoder and times
// decodeNextFrame() and convertFrame() separately, so the tone mapping
// pipeline can be measured without a window, an audio device, or the
// render loop in the way. Not part of the test suite -- it needs a
// 4K HDR file to say anything useful.
//
//   NaikAVPlayer_hdr_bench <file> [frames] [--no-tonemap] [--720p|--1080p]
//                          [--window WxH]
//
// --window stands in for the player's window, capping the tone-mapping
// resolution the way the real render loop does (see capToDisplaySize).
// Omitting it converts uncapped, which is the pre-cap behavior.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include "core/ThreadSafeQueue.hpp"
#include "core/MetricRing.hpp"
#include "video/VideoDecoder.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

static double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <file> [frames] [--no-tonemap] [--1080p|--720p] "
                     "[--window WxH] [--no-dynamic] [--trace-peaks] [--software] [--no-adaptive] "
                     "[--dump PATH]\n",
                     argv[0]);
        return 2;
    }
    const char* path = argv[1];
    int wantFrames = 60;
    bool toneMap = true;
    ResolutionOption res = ResolutionOption::ORIGINAL;
    int dispW = 0;
    int dispH = 0;
    bool dynamicMeta = true;
    bool adaptiveRes = true;
    const char* dumpPath = nullptr;
    bool tracePeaks = false;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-tonemap") toneMap = false;
        else if (a == "--no-dynamic") dynamicMeta = false;
        else if (a == "--trace-peaks") tracePeaks = true;
        else if (a == "--software") g_disableHardwareDecoders = true;
        else if (a == "--no-adaptive") adaptiveRes = false;
        else if (a == "--dump" && i + 1 < argc) dumpPath = argv[++i];
        else if (a == "--1080p") res = ResolutionOption::R_1080P;
        else if (a == "--720p") res = ResolutionOption::R_720P;
        else if (a == "--window" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &dispW, &dispH) != 2 ||
                dispW <= 0 || dispH <= 0) {
                std::fprintf(stderr, "--window wants WxH, e.g. --window 1024x576\n");
                return 2;
            }
        }
        else wantFrames = std::atoi(a.c_str());
    }

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path, nullptr, nullptr) < 0) {
        std::fprintf(stderr, "could not open %s\n", path);
        return 1;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        std::fprintf(stderr, "no stream info\n");
        return 1;
    }
    int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) {
        std::fprintf(stderr, "no video stream\n");
        return 1;
    }
    AVStream* st = fmt->streams[vs];

    ThreadSafeQueue<AVPacket*> queue(4096);
    MetricRing<256> decodeRing, convertRing;
    std::atomic<bool> profiling{false};
    VideoDecoder dec(st->codecpar, st->time_base, 0, queue, decodeRing,
                     convertRing, profiling);
    // Must happen before init(): this is what gives the adaptive tone-map
    // scaler a frame budget to measure against. PlayerController does the
    // same from Demuxer::getVideoFrameRate(). Without it the budget stays 0,
    // adaptation is disabled, and the bench silently measures a fixed-size
    // conversion no matter what --no-adaptive says -- which is not what the
    // player does. AVCodecParameters::framerate is frequently unset, so the
    // rate comes off the stream.
    {
        double fps = 0.0;
        if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
            fps = av_q2d(st->avg_frame_rate);
        } else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0) {
            fps = av_q2d(st->r_frame_rate);
        }
        dec.setSourceFrameRate(fps);
    }
    if (!dec.init()) {
        std::fprintf(stderr, "decoder init failed\n");
        return 1;
    }

    naikav::video::HdrToneMapSettings tm;
    tm.enabled = toneMap;
    tm.targetPeakNits = 100.0f;
    tm.useDynamicMetadata = dynamicMeta;
    tm.adaptiveResolution = adaptiveRes;

    std::vector<double> decodeMs, convertMs;
    int produced = 0;

    AVPacket* pkt = av_packet_alloc();
    while (produced < wantFrames && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != vs) { av_packet_unref(pkt); continue; }
        AVPacket* copy = av_packet_alloc();
        av_packet_ref(copy, pkt);
        av_packet_unref(pkt);
        if (!queue.push(copy)) { av_packet_free(&copy); continue; }

        auto t0 = std::chrono::steady_clock::now();
        bool ok = dec.decodeNextFrame();
        auto t1 = std::chrono::steady_clock::now();
        if (!ok) continue;

        auto t2 = std::chrono::steady_clock::now();
        bool conv = dec.convertFrame(res, tm, dispW, dispH);
        auto t3 = std::chrono::steady_clock::now();
        if (!conv) continue;

        if (tracePeaks) {
            ColorPipelineInfo ci = dec.getColorInfo();
            std::printf("  frame %3d  source=%7.1f nits  dynamic=%7.1f nits\n",
                        produced, ci.toneMapSourceNits, ci.toneMapDynamicNits);
        }

        // Skip the first few: context/LUT setup and cold caches.
        if (produced >= 3) {
            decodeMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            convertMs.push_back(std::chrono::duration<double, std::milli>(t3 - t2).count());
        }
        ++produced;
    }
    av_packet_free(&pkt);

    AVFrame* out = dec.getYUVFrame();
    // FNV-1a over the converted frame, so two builds can be compared
    // pixel-for-pixel -- used to check the legacy swscale path against the
    // threaded one (see NAIKAV_FORCE_LEGACY_SWS).
    unsigned long long sum = 1469598103934665603ull;
    if (out && out->data[0]) {
        const int bpp = (out->format == AV_PIX_FMT_RGB24) ? 3 : 1;
        for (int y = 0; y < out->height; ++y) {
            const unsigned char* row = out->data[0] + (size_t)y * out->linesize[0];
            for (int x = 0; x < out->width * bpp; ++x) {
                sum ^= row[x];
                sum *= 1099511628211ull;
            }
        }
    }
    std::printf("frame checksum: %016llx\n", sum);
    if (dumpPath && out && out->data[0]) {
        // Raw packed output, for comparing one build's pixels against
        // another's (see NAIKAV_FORCE_LEGACY_SWS).
        FILE* fp = std::fopen(dumpPath, "wb");
        if (fp) {
            const int bpp = (out->format == AV_PIX_FMT_RGB24) ? 3 : 1;
            for (int y = 0; y < out->height; ++y) {
                std::fwrite(out->data[0] + (size_t)y * out->linesize[0], 1,
                            (size_t)out->width * bpp, fp);
            }
            std::fclose(fp);
            std::printf("dumped %dx%d to %s\n", out->width, out->height, dumpPath);
        }
    }
    const char* outFmt = out ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(out->format)) : "none";
    ColorPipelineInfo info = dec.getColorInfo();

    const double d = medianOf(decodeMs), c = medianOf(convertMs);
    std::printf("frames=%d  out=%s %dx%d  toneMapped=%s  displayCap=%s\n",
                produced, outFmt, out ? out->width : 0, out ? out->height : 0,
                info.toneMapped ? "yes" : "no",
                (dispW > 0 && dispH > 0) ? "on" : "off");
    std::printf("decode  median %7.1f ms\n", d);
    std::printf("convert median %7.1f ms\n", c);
    std::printf("total   median %7.1f ms  -> %.1f fps\n", d + c,
                (d + c) > 0.0 ? 1000.0 / (d + c) : 0.0);

    avformat_close_input(&fmt);
    return 0;
}
