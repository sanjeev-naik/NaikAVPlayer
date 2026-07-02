#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <iostream>
#include <string>
#include "PlayerController.hpp"
#include "PlayerUI.hpp"

// Win32 native headers for file explorer dialog
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <SDL2/SDL_syswm.h>
#endif

#ifdef _WIN32
std::string openNativeFileDialog(SDL_Window* window) {
    char szFile[512] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    HWND hwnd = NULL;
    if (SDL_GetWindowWMInfo(window, &wmInfo)) {
        hwnd = wmInfo.info.win.window;
    }
    
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Video/Audio Files\0*.mp4;*.mkv;*.avi;*.mov;*.webm;*.mp3;*.wav;*.ogg\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn) == TRUE) {
        return std::string(ofn.lpstrFile);
    }
    return "";
}
#endif

int main(int argc, char* argv[]) {
    SDL_SetMainReady();
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

#ifdef _WIN32
    playerUI.setFileDialogCallback([window]() {
        return openNativeFileDialog(window);
    });
#endif

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

    bool quit = false;
    SDL_Event event;

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
                        controller.seek(controller.getCurrentTime() - 10.0);
                        break;
                    case SDLK_RIGHT:
                        controller.seek(controller.getCurrentTime() + 10.0);
                        break;
                    case SDLK_ESCAPE:
                        quit = true;
                        break;
                    default:
                        break;
                }
            }
        }

        // 2. Decode Video Frames & Synchronize to Master Clock
        if ((controller.getState() == PlayerState::PLAYING || controller.getState() == PlayerState::OPENED) 
            && controller.hasVideo()) {
            
            double timeNow = controller.getCurrentTime();
            VideoDecoder* decoder = controller.getVideoDecoder();
            
            // Loop decoding to drop late frames (catch-up mechanism)
            // A small 10ms threshold prevents jumping frames too aggressively
            // We limit the number of drops to max 8 frames per render tick when playing to keep the UI fluid.
            // When paused (OPENED state), we allow more drops (up to 2000) to catch up to the seek target instantly.
            int framesDropped = 0;
            int maxDrops = 8;
            if (decoder->isSeeking()) {
                // If we are seeking, decode frames instantly (up to 2000) to reach the target position
                maxDrops = 2000;
            } else {
                double ptsBeforeLoop = decoder->getCurrentFramePts();
                double timeDiff = timeNow - ptsBeforeLoop;
                if (timeDiff > 0.5) {
                    maxDrops = 32;
                }
            }
            while (decoder->getCurrentFramePts() < timeNow - 0.010 && framesDropped < maxDrops) {
                if (!decoder->decodeNextFrame()) {
                    // Stop skipping if we hit EOF or error
                    break;
                }
                framesDropped++;
            }

            // If we have caught up to the seek target or reached EOF, reset seeking flag
            if (decoder->isSeeking()) {
                if (decoder->getCurrentFramePts() >= timeNow - 0.080 || controller.getState() == PlayerState::ENDED || controller.isEOF()) {
                    decoder->setSeeking(false);
                }
            }

            // Extract the freshly decoded YUV frame
            bool shouldUpdateTexture = true;
            // Suppress intermediate texture updates during catch-up to prevent fast-forward or flashing effects
            if (decoder->getCurrentFramePts() < timeNow - 0.080 && framesDropped > 0) {
                shouldUpdateTexture = false;
            }

            if (shouldUpdateTexture) {
                decoder->convertFrame();
            }

            AVFrame* yuvFrame = decoder->getYUVFrame();
            if (shouldUpdateTexture && yuvFrame && yuvFrame->data[0]) {
                // Re-create video display texture if dimensions changed
                if (!videoTexture || texWidth != decoder->getWidth() || texHeight != decoder->getHeight()) {
                    if (videoTexture) {
                        SDL_DestroyTexture(videoTexture);
                    }
                    texWidth = decoder->getWidth();
                    texHeight = decoder->getHeight();
                    videoTexture = SDL_CreateTexture(
                        renderer,
                        SDL_PIXELFORMAT_IYUV, // Hardware-accelerated planar YUV 4:2:0
                        SDL_TEXTUREACCESS_STREAMING,
                        texWidth,
                        texHeight
                    );
                }

                // Copy raw YUV plane segments directly to GPU-mapped texture memory
                SDL_UpdateYUVTexture(
                    videoTexture,
                    nullptr,
                    yuvFrame->data[0], yuvFrame->linesize[0],
                    yuvFrame->data[1], yuvFrame->linesize[1],
                    yuvFrame->data[2], yuvFrame->linesize[2]
                );
                playerUI.registerVideoFrameRendered(currentSecs);
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

        // Display frame
        SDL_RenderPresent(renderer);

        // Sleep to avoid pegging CPU when idle/VSync limits
        SDL_Delay(5);
    }

    // 4. Cleanup resources on exit
    if (videoTexture) {
        SDL_DestroyTexture(videoTexture);
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
