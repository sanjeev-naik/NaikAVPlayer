// Standalone microbenchmark (not part of the regular suite): measures the
// per-stage cost of AudioDecoder's post-resample DSP path with every
// user-facing effect DISABLED -- i.e. exactly the configuration a user has
// after resetting settings to flat. Anything that still costs real time in
// that state runs unconditionally in the SDL audio callback and is a
// candidate cause of dropouts/crackling.
//
// Reports each stage's cost as a percentage of the realtime budget: the
// callback must produce `blockFrames` of audio in less than
// blockFrames/sampleRate seconds, or the device underruns.
//
// Build (no CMake needed -- the DSP headers are header-only and, apart from
// the loudness meter which this deliberately excludes, have no libav* deps):
//   g++ -O2 -std=c++17 -I src tests/audio_callback_bench.cpp -o build/bench.exe

#include "../src/audio/dsp/DspChain.hpp"
#include "../src/audio/dsp/Limiter.hpp"
#include "../src/audio/dsp/Surround3D.hpp"
#include "../src/audio/dsp/StereoWidener.hpp"
#include "../src/audio/dsp/BalanceControl.hpp"
#include "../src/audio/dsp/SpectrumAnalyzer.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBlockFrames = 1024;   // a typical SDL callback block
constexpr int kBlocks = 480;         // ~10 seconds of audio at 48kHz

// Wall time to process kBlocks blocks, as a percentage of the realtime
// budget those blocks represent.
double measurePercentOfRealtime(const std::function<void(float*, int)>& stage,
                                std::vector<float>& scratch,
                                const std::vector<float>& source) {
    // Warm up so first-touch page faults / lazy allocations don't land in
    // the measured window.
    scratch = source;
    stage(scratch.data(), kBlockFrames);

    auto start = std::chrono::steady_clock::now();
    for (int b = 0; b < kBlocks; ++b) {
        scratch = source;
        stage(scratch.data(), kBlockFrames);
    }
    auto end = std::chrono::steady_clock::now();

    const double elapsedSec = std::chrono::duration<double>(end - start).count();
    const double audioSec =
        static_cast<double>(kBlocks) * kBlockFrames / kSampleRate;
    return 100.0 * elapsedSec / audioSec;
}

// Cost of the copy the harness does per block, so it can be subtracted out.
double measureBaseline(std::vector<float>& scratch,
                       const std::vector<float>& source) {
    auto start = std::chrono::steady_clock::now();
    for (int b = 0; b < kBlocks; ++b) {
        scratch = source;
    }
    auto end = std::chrono::steady_clock::now();
    const double elapsedSec = std::chrono::duration<double>(end - start).count();
    const double audioSec =
        static_cast<double>(kBlocks) * kBlockFrames / kSampleRate;
    return 100.0 * elapsedSec / audioSec;
}

} // namespace

int main() {
    std::vector<float> source(static_cast<size_t>(kBlockFrames) * kChannels);
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    for (auto& s : source) {
        s = dist(rng);
    }
    std::vector<float> scratch(source.size());

    // Every stage configured exactly as AudioDecoder::init() +
    // applyDspSettings() leave it for a flat/all-disabled settings file.
    naikav::dsp::DspChain dsp;
    dsp.configure(kChannels, kSampleRate, -1);
    dsp.setEnabled(false);

    naikav::dsp::Surround3D surround3d;
    surround3d.configure(kChannels, kSampleRate);
    surround3d.setEnabled(false);

    naikav::dsp::StereoWidener widener;
    widener.configure(kChannels);
    widener.setEnabled(false);

    naikav::dsp::BalanceControl balance;
    balance.configure(kChannels);
    balance.setBalance(0.0f);

    naikav::dsp::SpectrumAnalyzer spectrum;
    spectrum.configure(kChannels, kSampleRate);
    spectrum.setEnabled(false);

    // The final safety limiter has no enable flag -- AudioDecoder runs it
    // unconditionally. Ceiling 0dB is its inert setting when no DSP is on.
    naikav::dsp::Limiter safety;
    safety.configure(kChannels, kSampleRate);
    safety.setCeilingDb(0.0f);

    const double baseline = measureBaseline(scratch, source);

    struct Row {
        const char* name;
        double pct;
    };
    std::vector<Row> rows;

    rows.push_back({"DspChain (disabled)",
                    measurePercentOfRealtime(
                        [&](float* b, int n) { dsp.process(b, n); }, scratch, source)});
    rows.push_back({"Surround3D (disabled)",
                    measurePercentOfRealtime(
                        [&](float* b, int n) { surround3d.process(b, n); }, scratch, source)});
    rows.push_back({"StereoWidener (disabled)",
                    measurePercentOfRealtime(
                        [&](float* b, int n) { widener.process(b, n); }, scratch, source)});
    rows.push_back({"BalanceControl (centered)",
                    measurePercentOfRealtime(
                        [&](float* b, int n) { balance.process(b, n); }, scratch, source)});
    rows.push_back({"SpectrumAnalyzer (disabled)",
                    measurePercentOfRealtime(
                        [&](float* b, int n) { spectrum.process(b, n); }, scratch, source)});
    rows.push_back({"Limiter (ALWAYS ON, 0dB)",
                    measurePercentOfRealtime(
                        [&](float* b, int n) { safety.process(b, n); }, scratch, source)});

    std::printf("\n  Audio callback cost with ALL effects disabled\n");
    std::printf("  %d ch @ %d Hz, %d-frame blocks, %d blocks (%.1fs of audio)\n\n",
                kChannels, kSampleRate, kBlockFrames, kBlocks,
                static_cast<double>(kBlocks) * kBlockFrames / kSampleRate);
    std::printf("  %-30s %14s\n", "stage", "%% of realtime");
    std::printf("  %-30s %14s\n", "------------------------------", "-------------");
    std::printf("  %-30s %13.4f%%\n", "(harness copy baseline)", baseline);
    for (const auto& r : rows) {
        const double net = r.pct - baseline;
        std::printf("  %-30s %13.4f%%%s\n", r.name, net < 0.0 ? 0.0 : net,
                    (net > 1.0) ? "   <-- significant" : "");
    }
    std::printf("\n");
    return 0;
}
