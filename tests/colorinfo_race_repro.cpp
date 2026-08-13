// Standalone diagnostic (not part of the regular test suite): isolates and
// stress-tests the getColorInfo() data race between the UI thread (this is
// called every frame by PlayerUI's Diagnostics HUD) and the video decode
// thread, which concurrently mutates/frees the same AVFrame
// (m_decodedFrame) under m_videoDecoderMutex in videoThreadLoop(). Before
// the fix, PlayerController::getColorInfo() read m_decodedFrame's
// side_data/buffer-ref fields without taking that same mutex.
//
// A real render loop only calls getColorInfo() once per frame (~60-144Hz),
// which is why this was hard to hit organically in a short test run. This
// program hammers it from a dedicated thread at a much higher rate to
// raise the odds of catching the exact unlucky interleaving quickly.
#define SDL_MAIN_HANDLED
#include "player/PlayerController.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <video_file> [seconds]" << std::endl;
        return 1;
    }
    std::string file = argv[1];
    double seconds = argc > 2 ? std::stod(argv[2]) : 15.0;

    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    {
        PlayerController controller;
        std::cout << "Opening: " << file << std::endl;
        if (!controller.openFile(file)) {
            std::cerr << "openFile() failed" << std::endl;
            SDL_Quit();
            return 1;
        }
        controller.play();

        std::atomic<bool> stop{false};
        std::atomic<uint64_t> calls{0};
        std::thread racer([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                volatile auto info = controller.getColorInfo();
                (void)info;
                calls.fetch_add(1, std::memory_order_relaxed);
            }
        });

        auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
            controller.getCurrentTime();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        stop.store(true);
        racer.join();
        std::cout << "getColorInfo() called " << calls.load() << " times concurrently with decoding." << std::endl;

        controller.stop();
    }

    SDL_Quit();
    std::cout << "Completed without crashing." << std::endl;
    return 0;
}
