// Standalone smoke test (not part of the regular suite): drives the REAL
// PlayerController -> AudioDecoder -> SDL audio device path against a real
// media file and samples the pipeline's buffer depths over time, looking
// for the starvation that produces audible clicks/crackle.
//
// AudioDecoder::sdlAudioStreamCallback() emits a block of digital silence
// whenever decodeAndResample() cannot produce samples (empty packet queue,
// decode error, EOF). Each of those is a hard discontinuity in the output
// -- an audible click. This test counts how often the pipeline gets close
// to that condition during steady-state playback.
//
// Usage: audio_underrun_smoke.exe <media-file> [seconds]

#define SDL_MAIN_HANDLED
#include "player/PlayerController.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <media-file> [seconds]\n", argv[0]);
        return 2;
    }
    const std::string path = argv[1];
    const double seconds = (argc >= 3) ? std::atof(argv[2]) : 8.0;

    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::printf("SDL_Init(AUDIO) failed: %s\n", SDL_GetError());
        return 1;
    }

    // The video thread stays ENABLED on purpose. It is what drains the video
    // packet queue, and the demuxer feeds both queues from one read loop --
    // so disabling it lets the video queue fill and back the demuxer up,
    // starving audio as a pure artifact of the test rather than of the bug
    // under investigation. Matching the real app is the only honest setup.
    PlayerController controller;
    if (!controller.openFile(path)) {
        std::printf("failed to open: %s\n", path.c_str());
        SDL_Quit();
        return 1;
    }
    if (!controller.hasAudio()) {
        std::printf("file has no audio stream: %s\n", path.c_str());
        SDL_Quit();
        return 1;
    }

    std::printf("\n  file        : %s\n", path.c_str());
    std::printf("  codec       : %s\n", controller.getAudioCodecName().c_str());
    std::printf("  layout      : %s (%d ch)\n",
                controller.getAudioChannelLayoutName().c_str(),
                controller.getAudioChannelCount());
    std::printf("  device native: %d ch\n", controller.getAudioDeviceNativeChannels());

    controller.play();

    // Sample faster than the audio device's own buffer turnover so a
    // transient dip to empty is actually observable.
    const auto interval = std::chrono::milliseconds(5);
    const int samples = static_cast<int>(seconds * 1000 / 5);

    std::vector<size_t> pktDepth;
    std::vector<size_t> sdlQueued;
    std::vector<size_t> vidDepth;
    std::vector<size_t> frameDepth;
    pktDepth.reserve(samples);
    sdlQueued.reserve(samples);
    vidDepth.reserve(samples);
    frameDepth.reserve(samples);

    int pktEmpty = 0;
    int sdlEmpty = 0;
    int vidFull = 0;      // video packet queue at/near capacity
    int frameFull = 0;    // decoded-frame queue at its throttle point
    int bothBad = 0;      // video queue full AND audio queue empty at once
    double maxDecodeMs = 0.0;

    // Let the pipeline reach steady state before counting: the first
    // fraction of a second legitimately starts from empty queues.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    for (int i = 0; i < samples; ++i) {
        // Stand in for main.cpp's render loop, which pops decoded frames
        // paced against the playback clock. Without this the decoded-frame
        // queue pins at its cap, the video thread stops draining video
        // packets, and the whole pipeline backs up as an artifact of the
        // test rather than of the bug. Frames are freed, not displayed.
        {
            const double now = controller.getCurrentTime();
            DecodedFrame df;
            while (controller.getDecodedFrameQueue().peek(df) && df.pts <= now) {
                DecodedFrame popped;
                if (!controller.getDecodedFrameQueue().pop(popped)) break;
                if (popped.frame) av_frame_free(&popped.frame);
            }
        }

        const size_t pkt = controller.getAudioPacketQueueSize();
        const size_t queued = controller.getAudioFrameQueueSize();
        const size_t vid = controller.getVideoPacketQueueSize();
        const size_t frames = controller.getVideoFrameQueueSize();
        pktDepth.push_back(pkt);
        sdlQueued.push_back(queued);
        vidDepth.push_back(vid);
        frameDepth.push_back(frames);
        if (pkt == 0) ++pktEmpty;
        if (queued == 0) ++sdlEmpty;
        if (vid >= controller.getVideoPacketQueueCapacity() - 2) ++vidFull;
        if (frames >= controller.getVideoFrameQueueCapacity()) ++frameFull;
        if (pkt == 0 && vid >= controller.getVideoPacketQueueCapacity() - 2) ++bothBad;
        maxDecodeMs = std::max(maxDecodeMs, controller.getAudioDecodeTimeMs());
        std::this_thread::sleep_for(interval);
    }

    auto avg = [](const std::vector<size_t>& v) {
        if (v.empty()) return 0.0;
        double t = 0.0;
        for (size_t x : v) t += static_cast<double>(x);
        return t / v.size();
    };
    auto minOf = [](const std::vector<size_t>& v) {
        return v.empty() ? size_t{0} : *std::min_element(v.begin(), v.end());
    };

    std::printf("\n  sampled %d times over %.1fs of playback\n\n", samples, seconds);
    std::printf("  %-34s %10s %10s %10s\n", "buffer", "min", "avg", "empty%%");
    std::printf("  %-34s %10s %10s %10s\n",
                "----------------------------------", "---------", "---------", "---------");
    std::printf("  %-34s %10zu %10.1f %9.1f%%\n", "audio packet queue (of 150)",
                minOf(pktDepth), avg(pktDepth), 100.0 * pktEmpty / samples);
    std::printf("  %-34s %10zu %10.1f %9.1f%%\n", "SDL device queue (frames)",
                minOf(sdlQueued), avg(sdlQueued), 100.0 * sdlEmpty / samples);
    std::printf("  %-34s %10zu %10.1f %9.1f%%\n", "video packet queue (of 100)",
                minOf(vidDepth), avg(vidDepth), 100.0 * vidFull / samples);
    std::printf("  %-34s %10zu %10.1f %9.1f%%\n", "decoded frame queue (of 8)",
                minOf(frameDepth), avg(frameDepth), 100.0 * frameFull / samples);
    std::printf("\n  max decodeAndResample time: %.3f ms\n", maxDecodeMs);
    std::printf("  video queue FULL *and* audio queue EMPTY simultaneously: %.1f%%\n",
                100.0 * bothBad / samples);

    // The authoritative underrun measure for this architecture -- see
    // AudioDecoder::getSilenceInjectionCount(). Note the SDL device-queue
    // row above is NOT evidence of anything: the callback puts exactly what
    // SDL asks for and SDL consumes it immediately, so it reads ~0 always.
    const uint64_t cbs = controller.getAudioCallbackCount();
    const uint64_t sil = controller.getAudioSilenceInjectionCount();
    const uint64_t silBytes = controller.getAudioSilenceBytes();
    std::printf("\n  audio callbacks           : %llu\n",
                static_cast<unsigned long long>(cbs));
    std::printf("  silence injections (clicks): %llu  (%.2f%% of callbacks)\n",
                static_cast<unsigned long long>(sil),
                cbs ? 100.0 * static_cast<double>(sil) / static_cast<double>(cbs) : 0.0);
    std::printf("  total silence emitted      : %llu bytes\n",
                static_cast<unsigned long long>(silBytes));

    if (AudioDecoder* ad = controller.audioDecoderForDiagnostics()) {
        char errbuf[128] = {0};
        const int reason = ad->getLastFailReason();
        av_strerror(reason, errbuf, sizeof(errbuf));
        std::printf("\n  attribution of silent blocks:\n");
        std::printf("    packet queue empty      : %llu\n",
                    static_cast<unsigned long long>(ad->getQueueEmptyCount()));
        std::printf("    avcodec_send_packet <0  : %llu\n",
                    static_cast<unsigned long long>(ad->getSendFailCount()));
        std::printf("    avcodec_receive_frame <0: %llu\n",
                    static_cast<unsigned long long>(ad->getReceiveFailCount()));
        std::printf("    last error              : %d (%s)%s\n", reason, errbuf,
                    reason == AVERROR(EAGAIN) ? "  <-- EAGAIN is NOT fatal" : "");
    }

    const bool glitching = cbs > 0 && (100.0 * static_cast<double>(sil) /
                                       static_cast<double>(cbs)) > 0.5;
    std::printf("\n  VERDICT: %s\n\n",
                glitching ? "UNDERRUNS CONFIRMED -- silence injection is producing the clicks"
                          : "callback never starved -- clicks are NOT underruns");

    controller.stop();
    SDL_Quit();
    return 0;
}
