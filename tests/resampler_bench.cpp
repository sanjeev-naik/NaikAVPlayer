// Standalone microbenchmark (not part of the regular suite): measures
// swr_convert cost per ResamplerQuality tier, in the exact configuration
// AudioDecoder uses -- soxr engine, interleaved float out, 48kHz target.
//
// AudioDecoder::decodeAndResample() runs inside the SDL audio callback, so
// this cost is paid against a hard realtime deadline: the callback must
// produce blockFrames of audio in under blockFrames/48000 seconds or the
// device underruns (audible as clicks/crackle).
//
// Build:
//   g++ -O2 -std=gnu++17 -I src -I thirdparty/ffmpeg/include \
//       tests/resampler_bench.cpp -o build/rsbench.exe \
//       -L thirdparty/ffmpeg/lib -lswresample -lavutil

extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr int kOutRate = 48000;   // AudioDecoder's fixed output rate
constexpr int kChannels = 2;
constexpr int kBlockFrames = 1024;
constexpr int kBlocks = 480;      // ~10s of audio

struct Tier {
    const char* name;
    double precisionBits; // mirrors resamplerPrecisionBitsFor() in AudioDecoder.cpp
};

// Returns percent-of-realtime, or -1.0 on setup failure.
double benchTier(double precisionBits, int inRate, bool useSoxr) {
    AVChannelLayout layout;
    av_channel_layout_default(&layout, kChannels);

    SwrContext* ctx = nullptr;
    int ret = swr_alloc_set_opts2(&ctx, &layout, AV_SAMPLE_FMT_FLT, kOutRate,
                                  &layout, AV_SAMPLE_FMT_FLT, inRate, 0, nullptr);
    if (ret < 0 || !ctx) {
        av_channel_layout_uninit(&layout);
        return -1.0;
    }
    if (useSoxr) {
        if (av_opt_set(ctx, "resampler", "soxr", 0) < 0) {
            swr_free(&ctx);
            av_channel_layout_uninit(&layout);
            return -1.0;
        }
        av_opt_set_double(ctx, "precision", precisionBits, 0);
    }
    if (swr_init(ctx) < 0) {
        swr_free(&ctx);
        av_channel_layout_uninit(&layout);
        return -1.0;
    }

    // Input block sized at the source rate; output buffer generously sized.
    const int inFrames = static_cast<int>(
        static_cast<long long>(kBlockFrames) * inRate / kOutRate) + 1;
    std::vector<float> in(static_cast<size_t>(inFrames) * kChannels);
    std::mt19937 rng(999);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    for (auto& s : in) s = dist(rng);

    std::vector<float> out(static_cast<size_t>(kBlockFrames + 64) * kChannels);
    const uint8_t* inPtr[1] = { reinterpret_cast<const uint8_t*>(in.data()) };
    uint8_t* outPtr[1] = { reinterpret_cast<uint8_t*>(out.data()) };

    swr_convert(ctx, outPtr, kBlockFrames + 64, inPtr, inFrames); // warm up

    auto start = std::chrono::steady_clock::now();
    for (int b = 0; b < kBlocks; ++b) {
        swr_convert(ctx, outPtr, kBlockFrames + 64, inPtr, inFrames);
    }
    auto end = std::chrono::steady_clock::now();

    swr_free(&ctx);
    av_channel_layout_uninit(&layout);

    const double elapsedSec = std::chrono::duration<double>(end - start).count();
    const double audioSec =
        static_cast<double>(kBlocks) * kBlockFrames / kOutRate;
    return 100.0 * elapsedSec / audioSec;
}

void runRate(int inRate) {
    static const Tier kTiers[] = {
        {"LOW       (16-bit)", 16.0},
        {"MEDIUM    (20-bit)", 20.0},
        {"HIGH      (28-bit)", 28.0},
        {"VERY_HIGH (33-bit)", 33.0},
    };

    std::printf("\n  Source %d Hz -> output %d Hz%s\n", inRate, kOutRate,
                inRate == kOutRate ? "  (no rate conversion needed)" : "");
    std::printf("  %-22s %16s\n", "resampler tier", "%% of realtime");
    std::printf("  %-22s %16s\n", "----------------------", "---------------");

    const double swrDefault = benchTier(0.0, inRate, false);
    std::printf("  %-22s %15.3f%%\n", "swresample default", swrDefault);
    for (const auto& t : kTiers) {
        const double pct = benchTier(t.precisionBits, inRate, true);
        if (pct < 0.0) {
            std::printf("  %-22s %16s\n", t.name, "unavailable");
            continue;
        }
        std::printf("  %-22s %15.3f%%%s\n", t.name, pct,
                    pct > 25.0 ? "   <-- heavy" : "");
    }
}

} // namespace

int main() {
    std::printf("\n  swr_convert cost inside the SDL audio callback\n");
    std::printf("  %d ch, %d-frame blocks, %d blocks (%.1fs of audio)\n",
                kChannels, kBlockFrames, kBlocks,
                static_cast<double>(kBlocks) * kBlockFrames / kOutRate);

    runRate(48000); // already at target rate
    runRate(44100); // the common case: CD/most music and lots of video audio
    std::printf("\n");
    return 0;
}
