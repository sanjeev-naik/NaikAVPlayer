// Standalone diagnostic (not part of the regular test suite): reproduces
// the user-reported crash using the REAL SDL audio device path, with no
// mocking of SDL_OpenAudioDeviceStream like tests.cpp does. That mock
// falls back to a disconnected SDL_CreateAudioStream() (no callback bound)
// whenever the real device open fails in the sandboxed test-runner
// environment, which means AudioDecoder::decodeAndResample() -- and
// therefore the entire DSP chain -- never actually runs under the real
// SDL audio callback thread in the normal test suite. This program talks
// to the real audio device directly so the DSP processing code genuinely
// executes, matching what the GUI app does.
//
// Scenario (from the user's report): open a video-only file (no audio
// stream), enable every DSP option, then open a large real-world file
// that does have audio.
#define SDL_MAIN_HANDLED
#include "player/PlayerController.hpp"
#include "audio/dsp/AudioDspSettings.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <chrono>
#include <iostream>
#include <thread>

naikav::dsp::AudioDspSettings makeAllOnSettings() {
    naikav::dsp::AudioDspSettings s;
    s.dspEnabled = true;
    for (int i = 0; i < naikav::dsp::ParametricEQ::kNumBands; ++i) {
        s.eqBandGainDb[i] = 6.0f;
    }
    s.compressorEnabled = true;
    s.limiterEnabled = true;
    s.crossoverEnabled = true;
    s.crossoverBassRedirectEnabled = true;
    s.loudnessEnabled = true;
    s.widenerEnabled = true;
    s.surround3dEnabled = true;
    s.balance = 0.3f;
    s.noiseGateEnabled = true;
    s.multibandEnabled = true;
    s.autoGenrePresetEnabled = true;
    s.spectrumAnalyzerEnabled = true;
    return s;
}

void pumpFor(PlayerController& controller, double seconds) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        controller.getCurrentTime();
        controller.pollPendingLoudnessPrescan();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <no_audio_file> <big_file_with_audio> [seconds_to_play] [live|preset]" << std::endl;
        return 1;
    }
    double playSeconds = argc > 3 ? std::stod(argv[3]) : 8.0;
    std::string mode = argc > 4 ? argv[4] : "preset";
    bool liveMode = (mode == "live");

    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    {
        const std::string noAudioFile = argv[1];
        const std::string bigFile = argv[2];
        PlayerController controller;

        std::cout << "[1] Opening no-audio file: " << noAudioFile << std::endl;
        bool ok1 = controller.openFile(noAudioFile);
        std::cout << "    openFile() -> " << ok1 << ", hasAudio=" << controller.hasAudio()
                  << ", hasVideo=" << controller.hasVideo() << std::endl;
        if (ok1) {
            controller.play();
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }

        if (!liveMode) {
            // "preset" mode: enable every DSP option BEFORE opening the big
            // file, so applyDspSettings() is picked up once, synchronously,
            // during openFile()'s single-threaded init() -- no audio is
            // flowing yet at that point.
            std::cout << "[2] Enabling every DSP option (before opening big file)..." << std::endl;
            controller.setAudioDspSettings(makeAllOnSettings());
        }

        std::cout << "[3] Opening big file with audio: " << bigFile << std::endl;
        bool ok2 = controller.openFile(bigFile);
        std::cout << "    openFile() -> " << ok2 << ", hasAudio=" << controller.hasAudio()
                  << ", hasVideo=" << controller.hasVideo() << std::endl;
        if (ok2) {
            controller.play();

            if (liveMode) {
                // "live" mode: the big file is already open and PLAYING with
                // real audio actively flowing through the SDL callback
                // thread. Flip each DSP option on one at a time, the way a
                // user actually clicks through checkboxes in the Audio
                // Processing panel -- each call takes m_dspMutex live
                // against decodeAndResample() running concurrently on the
                // audio thread, which the "preset" mode above never
                // exercises (DSP settings there are only ever applied
                // before playback starts).
                std::cout << "[2] Enabling every DSP option ONE AT A TIME while audio is live..." << std::endl;
                naikav::dsp::AudioDspSettings s; // starts all-off
                s.dspEnabled = true;
                controller.setAudioDspSettings(s);
                pumpFor(controller, 0.3);

                auto flip = [&](const char* name, auto apply) {
                    std::cout << "    toggling: " << name << std::endl;
                    apply(s);
                    controller.setAudioDspSettings(s);
                    pumpFor(controller, 0.3);
                };
                flip("EQ", [](naikav::dsp::AudioDspSettings& x) {
                    for (int i = 0; i < naikav::dsp::ParametricEQ::kNumBands; ++i) x.eqBandGainDb[i] = 6.0f;
                });
                flip("compressor", [](naikav::dsp::AudioDspSettings& x) { x.compressorEnabled = true; });
                flip("limiter", [](naikav::dsp::AudioDspSettings& x) { x.limiterEnabled = true; });
                flip("crossover", [](naikav::dsp::AudioDspSettings& x) { x.crossoverEnabled = true; });
                flip("crossover bass redirect", [](naikav::dsp::AudioDspSettings& x) { x.crossoverBassRedirectEnabled = true; });
                flip("noise gate", [](naikav::dsp::AudioDspSettings& x) { x.noiseGateEnabled = true; });
                flip("multiband", [](naikav::dsp::AudioDspSettings& x) { x.multibandEnabled = true; });
                flip("widener", [](naikav::dsp::AudioDspSettings& x) { x.widenerEnabled = true; });
                flip("surround3d", [](naikav::dsp::AudioDspSettings& x) { x.surround3dEnabled = true; });
                flip("balance", [](naikav::dsp::AudioDspSettings& x) { x.balance = 0.3f; });
                flip("spectrum analyzer", [](naikav::dsp::AudioDspSettings& x) { x.spectrumAnalyzerEnabled = true; });
                flip("loudness (triggers prescan)", [](naikav::dsp::AudioDspSettings& x) { x.loudnessEnabled = true; });
                flip("auto genre preset", [](naikav::dsp::AudioDspSettings& x) { x.autoGenrePresetEnabled = true; });
            }

            pumpFor(controller, playSeconds);

            std::cout << "[seek] seeking around while all DSP is live..." << std::endl;
            controller.seek(30.0);
            pumpFor(controller, 1.0);
            controller.seek(5.0);
            pumpFor(controller, 1.0);
            controller.pause();
            pumpFor(controller, 0.5);
            controller.play();
            pumpFor(controller, 1.0);
        }

        std::cout << "[4] Stopping..." << std::endl;
        controller.stop();
    }

    SDL_Quit();
    std::cout << "Completed without crashing." << std::endl;
    return 0;
}
