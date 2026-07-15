#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <iostream>
#include <string>
#include "PlayerController.hpp"
#include "PlayerUI.hpp"

// Cross-platform native file dialog via Native File Dialog Extended (NFD)
#include <nfd.hpp>

std::string openNativeFileDialog() {
    NFD::UniquePathU8 outPath;
    nfdfilteritem_t filterItem[1] = {
        { "Video/Audio Files", "mp4,mkv,avi,mov,webm,mp3,wav,ogg" }
    };
    nfdresult_t result = NFD::OpenDialog(outPath, filterItem, 1);
    if (result == NFD_OKAY) {
        return std::string(outPath.get());
    }
    return "";
}

int main(int argc, char* argv[]) {
    SDL_SetMainReady();

    // Initialize Native File Dialog Extended (RAII)
    NFD::Guard nfdGuard;

    // Initialize SDL2 (Video, Audio, Timer, and Event subsystems)
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS) < 0) {
        std::cerr << "Error: Could not initialize SDL2: " << SDL_GetError() << std::endl;
        return -1;
    }

    // Set texture filtering to linear for high-quality scaling
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    // Create the application window
    int winWidth = 960;
    int winHeight = 540;
    SDL_Window* window = SDL_CreateWindow(
        "NaikAVPlayer",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        winWidth,
        winHeight,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI
    );

    if (!window) {
        std::cerr << "Error: Could not create SDL2 Window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return -1;
    }

    // Set the application window icon
    std::string iconPaths[] = {
        "assets/app_icon.bmp",
        "../assets/app_icon.bmp",
        "../../assets/app_icon.bmp",
        "./app_icon.bmp"
    };
    SDL_Surface* iconSurface = nullptr;
    for (const auto& path : iconPaths) {
        iconSurface = SDL_LoadBMP(path.c_str());
        if (iconSurface) {
            break;
        }
    }
    if (iconSurface) {
        SDL_SetWindowIcon(window, iconSurface);
        SDL_FreeSurface(iconSurface);
    }

    // Create hardware-accelerated renderer with VSync enabled
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, 
        -1, 
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );

    if (!renderer) {
        std::cerr << "Error: Could not create SDL2 Renderer: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable keyboard navigation

    // Bind ImGui to SDL2 and SDL_Renderer
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    // Initialize player modules
    PlayerController controller;
    PlayerUI playerUI(controller);
    playerUI.init();

    // Register native file dialog for all platforms (nativefiledialog-extended
    // handles Windows/Linux/macOS natively)
    playerUI.setFileDialogCallback([]() {
        return openNativeFileDialog();
    });

    // Enable drag and drop events
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    // If a file was passed as a command-line argument, load it immediately
    if (argc > 1) {
        std::string filename = argv[1];
        if (controller.openFile(filename)) {
            controller.play();
        }
    }

    // Video texture parameters
    SDL_Texture* videoTexture = nullptr;
    int texWidth = 0;
    int texHeight = 0;
    Uint32 texFormat = SDL_PIXELFORMAT_UNKNOWN;
    DecodedFrame currentFrame;

    bool quit = false;
    SDL_Event event;
    // Windows throttles/blocks SDL_RenderPresent() (vsync) for an occluded
    // or minimized swapchain (DWM composition throttling). Since this loop
    // does event polling, decoded-frame consumption (paced against the
    // audio clock, which keeps advancing in real time on its own SDL audio
    // thread), and presentation all in one iteration, a blocked Present()
    // stalls frame consumption too -> video falls behind audio while
    // minimized and has to decode through the backlog to catch up once
    // restored. Skip only the Present() call while minimized so the rest of
    // the loop keeps pace with real time and there's no backlog to drain.
    bool windowMinimized = false;

    std::cout << "Application loop started. Press Space to pause, Left/Right arrow keys to seek 10s." << std::endl;

    while (!quit) {
        double currentSecs = SDL_GetTicks() / 1000.0;

        // 1. Process Windows Events & Input Routing
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);

            // Notify UI of mouse or keyboard events to prevent auto-hiding
            if (event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_KEYDOWN) {
                playerUI.notifyMouseActivity(currentSecs);
            }

            if (event.type == SDL_QUIT) {
                quit = true;
            } 
            else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    winWidth = event.window.data1;
                    winHeight = event.window.data2;
                } else if (event.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    windowMinimized = true;
                } else if (event.window.event == SDL_WINDOWEVENT_RESTORED) {
                    windowMinimized = false;
                }
            }
            else if (event.type == SDL_DROPFILE) {
                // Drag & Drop media file loading
                char* droppedFilepath = event.drop.file;
                std::cout << "Dropped file detected: " << droppedFilepath << std::endl;
                if (controller.openFile(droppedFilepath)) {
                    controller.play();
                }
                SDL_free(droppedFilepath); // Free drop filepath memory
            } 
            else if (event.type == SDL_KEYDOWN && !io.WantCaptureKeyboard) {
                // Process keyboard hotkeys
                switch (event.key.keysym.sym) {
                    case SDLK_SPACE:
                        if (controller.getState() == PlayerState::PLAYING) {
                            controller.pause();
                        } else if (controller.getState() == PlayerState::PAUSED || controller.getState() == PlayerState::OPENED) {
                            controller.play();
                        }
                        break;
                    case SDLK_LEFT:
                        // Reference time lets repeated presses stack onto a
                        // catch-up that is still in flight
                        controller.seek(controller.getSeekReferenceTime() - 10.0);
                        break;
                    case SDLK_RIGHT:
                        controller.seek(controller.getSeekReferenceTime() + 10.0);
                        break;
                    case SDLK_ESCAPE:
                        quit = true;
                        break;
                    case SDLK_l:
                        controller.setLoopEnabled(!controller.isLoopEnabled());
                        break;
                    default:
                        break;
                }
            }
        }

        // 2. Decode Video Frames & Synchronize to Master Clock
        if (controller.hasVideo()) {
            if (controller.hasSeeked()) {
                if (currentFrame.frame) {
                    av_frame_free(&currentFrame.frame);
                }
                controller.clearSeeked();
            }

            double timeNow = controller.getCurrentTime();
            // During a seek catch-up only the target frame is delivered;
            // present it as soon as it arrives instead of pacing it against
            // the playback clock.
            bool seekCatchup = controller.isCatchingUp();

            DecodedFrame targetFrame;
            bool hasTarget = false;

            while (true) {
                DecodedFrame nextFrame;
                if (!controller.getDecodedFrameQueue().peek(nextFrame)) {
                    break; // Queue is empty
                }

                // Present the frame when its PTS is <= current playback time.
                // Exception: if we don't have any frame displayed yet and we are paused (OPENED/PAUSED),
                // we allow popping the first frame even if its PTS is slightly in the future to show a first frame.
                if (nextFrame.pts > timeNow && !seekCatchup) {
                    if (!currentFrame.frame && !hasTarget && (controller.getState() == PlayerState::OPENED || controller.getState() == PlayerState::PAUSED)) {
                        // Allow popping the first frame
                    } else {
                        break;
                    }
                }

                DecodedFrame poppedFrame;
                if (controller.getDecodedFrameQueue().pop(poppedFrame)) {
                    if (hasTarget && targetFrame.frame) {
                        av_frame_free(&targetFrame.frame);
                    }
                    targetFrame = poppedFrame;
                    hasTarget = true;
                }
            }

            bool shouldUpdateTexture = false;
            if (hasTarget) {
                if (currentFrame.frame) {
                    av_frame_free(&currentFrame.frame);
                }
                currentFrame = targetFrame;
                shouldUpdateTexture = true;
            }

            if (currentFrame.frame && currentFrame.frame->data[0]) {
                Uint32 targetFormat = SDL_PIXELFORMAT_IYUV;
                if (currentFrame.frame->format == AV_PIX_FMT_NV12) {
                    targetFormat = SDL_PIXELFORMAT_NV12;
                } else if (currentFrame.frame->format == AV_PIX_FMT_NV21) {
                    targetFormat = SDL_PIXELFORMAT_NV21;
                }

                // Re-create video display texture if dimensions or pixel format changed
                if (!videoTexture || texWidth != currentFrame.width || texHeight != currentFrame.height || texFormat != targetFormat) {
                    if (videoTexture) {
                        SDL_DestroyTexture(videoTexture);
                    }
                    texWidth = currentFrame.width;
                    texHeight = currentFrame.height;
                    texFormat = targetFormat;
                    videoTexture = SDL_CreateTexture(
                        renderer,
                        texFormat,
                        SDL_TEXTUREACCESS_STREAMING,
                        texWidth,
                        texHeight
                    );
                }

                if (shouldUpdateTexture) {
                    // Copy raw plane segments directly to GPU-mapped texture memory
                    if (texFormat == SDL_PIXELFORMAT_NV12 || texFormat == SDL_PIXELFORMAT_NV21) {
                        SDL_UpdateNVTexture(
                            videoTexture,
                            nullptr,
                            currentFrame.frame->data[0], currentFrame.frame->linesize[0],
                            currentFrame.frame->data[1], currentFrame.frame->linesize[1]
                        );
                    } else {
                        SDL_UpdateYUVTexture(
                            videoTexture,
                            nullptr,
                            currentFrame.frame->data[0], currentFrame.frame->linesize[0],
                            currentFrame.frame->data[1], currentFrame.frame->linesize[1],
                            currentFrame.frame->data[2], currentFrame.frame->linesize[2]
                        );
                    }
                    playerUI.registerVideoFrameRendered(currentSecs);
                }
            }
        } else {
            // Cleanup video texture and frame if player is stopped/uninitialized
            if (videoTexture) {
                SDL_DestroyTexture(videoTexture);
                videoTexture = nullptr;
                texWidth = 0;
                texHeight = 0;
                texFormat = SDL_PIXELFORMAT_UNKNOWN;
            }
            if (currentFrame.frame) {
                av_frame_free(&currentFrame.frame);
            }
        }

        // 3. Rendering Pipeline
        SDL_SetRenderDrawColor(renderer, 15, 15, 17, 255); // Dark grey background
        SDL_RenderClear(renderer);

        // A. Draw Centered Letterboxed Video Frame
        if (videoTexture) {
            SDL_Rect dstRect;
            float windowAspect = static_cast<float>(winWidth) / winHeight;
            float videoAspect = static_cast<float>(texWidth) / texHeight;

            if (windowAspect > videoAspect) {
                // Window is wider than video -> pillarbox (draw side bars)
                dstRect.h = winHeight;
                dstRect.w = static_cast<int>(winHeight * videoAspect);
                dstRect.x = (winWidth - dstRect.w) / 2;
                dstRect.y = 0;
            } else {
                // Window is taller than video -> letterbox (draw top/bottom bars)
                dstRect.w = winWidth;
                dstRect.h = static_cast<int>(winWidth / videoAspect);
                dstRect.x = 0;
                dstRect.y = (winHeight - dstRect.h) / 2;
            }
            SDL_RenderCopy(renderer, videoTexture, nullptr, &dstRect);
        }

        // B. Render Dear ImGui UI Overlay
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        playerUI.draw(winWidth, winHeight, currentSecs);

        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());

        // Display frame. Skipped while minimized: Windows throttles/blocks
        // vsync Present() on an occluded swapchain, and letting that block
        // this thread is what causes the slowdown/freeze while minimized
        // and the multi-second catch-up after restoring (see comment above
        // windowMinimized's declaration).
        if (!windowMinimized) {
            SDL_RenderPresent(renderer);
        }

        // Sleep to avoid pegging CPU when idle/VSync limits
        SDL_Delay(5);
    }

    // 4. Cleanup resources on exit
    if (videoTexture) {
        SDL_DestroyTexture(videoTexture);
    }
    if (currentFrame.frame) {
        av_frame_free(&currentFrame.frame);
    }
    controller.stop();

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    std::cout << "Application closed successfully" << std::endl;
    return 0;
}
