#define SDL_MAIN_HANDLED
#include "player/PlayerController.hpp"
#include "ui/PlayerUI.hpp"
#include "video/FrameExporter.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <iostream>
#include <string>

// Cross-platform native file dialog via Native File Dialog Extended (NFD)
#include <SDL3/SDL_properties.h>
#include <nfd.hpp>
#include <csignal>

#ifdef _WIN32
#include <windows.h>
#endif

static void toggleFullscreen(SDL_Window *window) {
  if (!window)
    return;
  bool isFullscreen =
      (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
  SDL_SetWindowFullscreen(window, !isFullscreen);
}


#ifndef _WIN32
static std::atomic<bool> g_signalQuit{false};
static void signalHandler(int sig) {
  (void)sig;
  g_signalQuit.store(true);
}
static bool isSignalQuitRequested() {
  return g_signalQuit.load();
}
#else
static bool isSignalQuitRequested() {
  return false;
}

// Attaches to whatever console launched this process (if any) and points
// std::cout/cerr at it. The app is built WIN32_EXECUTABLE (no console
// subsystem), so without this the existing log output -- file open/decode/
// DSP/playback-state messages -- has nowhere to go even when launched from
// a terminal.
//
// Deliberately opt-in via --console rather than run automatically at
// startup. A GUI-subsystem process that silently attaches to its parent
// console and reopens CONOUT$ before doing anything else is a recognized
// console-hiding pattern in droppers/stealers, and generic AV heuristics
// score it accordingly -- the same class of false positive that already
// pushed the Windows release build from MinGW to MSVC (see the toolchain
// note in .github/workflows/ci.yml). Requiring an explicit flag keeps the
// full diagnostic capability while removing the unprompted startup
// behavior that heuristics actually key on. Silently does nothing when
// there is no parent console (e.g. launched by double-click).
static void attachParentConsole() {
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE *dummy;
    // Only clear the iostream error state for streams actually redirected
    // onto the console -- if freopen_s() failed, the underlying FILE* is
    // still detached and clearing the state would just advertise an output
    // path that does not work.
    if (freopen_s(&dummy, "CONOUT$", "w", stdout) == 0) {
      std::cout.clear();
    }
    if (freopen_s(&dummy, "CONOUT$", "w", stderr) == 0) {
      std::cerr.clear();
    }
  }
}
#endif

// Populates args.parentWindow from the SDL3 window's native handle.
//
// Why this is needed: NFDe's documented glue headers (nfd_sdl2.h, nfd_glfw3.h)
// rely on SDL2's SDL_GetWindowWMInfo(), which SDL3 removed in favor of the
// properties API (SDL_GetWindowProperties). So on SDL3 we fetch the native
// handle ourselves and fill in nfdwindowhandle_t manually instead of relying
// on an SDL3 glue header that may not exist for this NFDe version.
//
// Without a parent handle, the dialog opens as an independent top-level
// window with no relationship to the main window. On X11/Wayland desktops,
// gtk_dialog_run() then blocks the main thread while nothing answers the
// window manager's liveness ping (_NET_WM_PING) for the main window, so the
// WM reports the app as "not responding" even though it's just waiting on
// the dialog. Parenting fixes this by making the pair read as one app.
static void setNfdParentWindow(nfdwindowhandle_t &parentWindow,
                               SDL_Window *window) {
  parentWindow.type = NFD_WINDOW_HANDLE_TYPE_UNSET;
  parentWindow.handle = nullptr;

  if (!window) {
    return;
  }

  SDL_PropertiesID props = SDL_GetWindowProperties(window);
  if (props == 0) {
    return;
  }

#if defined(SDL_PLATFORM_WIN32)
  void *hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                      nullptr);
  if (hwnd) {
    parentWindow.type = NFD_WINDOW_HANDLE_TYPE_WINDOWS;
    parentWindow.handle = hwnd;
  }
#elif defined(SDL_PLATFORM_LINUX)
  // X11: NFDe accepts this directly. Wayland: NFDe's window-handle enum has
  // no Wayland case yet ("Wayland support will be implemented separately in
  // the future" per nfd.h), so on a Wayland session the dialog will open
  // unparented -- same freeze-looking symptom can still occur there.
  Sint64 x11Window =
      SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
  if (x11Window != 0) {
    parentWindow.type = NFD_WINDOW_HANDLE_TYPE_X11;
    parentWindow.handle =
        reinterpret_cast<void *>(static_cast<uintptr_t>(x11Window));
  }
#endif
}

std::string openNativeFileDialog(SDL_Window *window) {
  NFD::UniquePathU8 outPath;
  nfdfilteritem_t filterItem[1] = {
      {"Video/Audio Files", "mp4,mkv,avi,mov,webm,mp3,wav,ogg"}};

  nfdopendialogu8args_t args = {};
  args.filterList = filterItem;
  args.filterCount = 1;
  setNfdParentWindow(args.parentWindow, window);

  nfdu8char_t *rawPath = nullptr;
  nfdresult_t result = NFD_OpenDialogU8_With(&rawPath, &args);
  if (result == NFD_OKAY) {
    outPath.reset(rawPath);
    return std::string(outPath.get());
  }
  if (result == NFD_ERROR) {
    std::cerr << "Error: NFD_OpenDialogU8_With failed: " << NFD_GetError()
              << std::endl;
  }
  return "";
}

static SDL_Colorspace getSDLColorspace(const AVFrame *frame) {
  if (!frame)
    return SDL_COLORSPACE_UNKNOWN;

  AVColorSpace spc = frame->colorspace;
  AVColorRange rng = frame->color_range;

  // YUVJ420P is legacy and always full range
  if (frame->format == AV_PIX_FMT_YUVJ420P) {
    rng = AVCOL_RANGE_JPEG;
  }

  if (spc == AVCOL_SPC_BT709) {
    if (rng == AVCOL_RANGE_JPEG) {
      return SDL_COLORSPACE_BT709_FULL;
    } else {
      return SDL_COLORSPACE_BT709_LIMITED;
    }
  } else if (spc == AVCOL_SPC_BT2020_NCL || spc == AVCOL_SPC_BT2020_CL) {
    if (rng == AVCOL_RANGE_JPEG) {
      return SDL_COLORSPACE_BT2020_FULL;
    } else {
      return SDL_COLORSPACE_BT2020_LIMITED;
    }
  } else if (spc == AVCOL_SPC_BT470BG || spc == AVCOL_SPC_SMPTE170M ||
             spc == AVCOL_SPC_SMPTE240M) {
    if (rng == AVCOL_RANGE_JPEG) {
      return SDL_COLORSPACE_BT601_FULL;
    } else {
      return SDL_COLORSPACE_BT601_LIMITED;
    }
  } else {
    // Default/fallback: BT.709 for HD (>= 720p), BT.601 for SD
    if (frame->width >= 1280 || frame->height >= 720) {
      if (rng == AVCOL_RANGE_JPEG) {
        return SDL_COLORSPACE_BT709_FULL;
      } else {
        return SDL_COLORSPACE_BT709_LIMITED;
      }
    } else {
      if (rng == AVCOL_RANGE_JPEG) {
        return SDL_COLORSPACE_BT601_FULL;
      } else {
        return SDL_COLORSPACE_BT601_LIMITED;
      }
    }
  }
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
  // Scanned here, ahead of everything else, rather than in the main
  // argument loop further down: SDL/window/renderer/NFD initialization all
  // log before that loop is reached, and those messages are exactly the
  // ones worth seeing when diagnosing a startup failure. See
  // attachParentConsole() for why this is opt-in.
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--console") {
      attachParentConsole();
      break;
    }
  }
#else
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);
#endif
  SDL_SetMainReady();

  // Initialize Native File Dialog Extended (RAII)
  NFD::Guard nfdGuard;

  // Initialize SDL3 (Video and Audio subsystems)
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
    std::cerr << "Error: Could not initialize SDL3: " << SDL_GetError()
              << std::endl;
    return -1;
  }

  // Create the application window.
  //
  // 1024x576 (still 16:9) rather than 960x540: the controls dock caps its
  // width at 95% of the window, and at 960 that capped the bar below the
  // width its right-hand group (resolution / EQ / mute / volume) needs,
  // leaving the volume slider crowded against the edge.
  int winWidth = 1024;
  int winHeight = 576;
  SDL_Window *window =
      SDL_CreateWindow("NaikAVPlayer", winWidth, winHeight,
                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

  if (!window) {
    std::cerr << "Error: Could not create SDL3 Window: " << SDL_GetError()
              << std::endl;
    SDL_Quit();
    return -1;
  }

  // Floor the window at the previous default size. The controls dock scales
  // with the window, and below roughly this width its centered playback
  // buttons start overlapping the right-hand volume/resolution group.
  SDL_SetWindowMinimumSize(window, 960, 540);

  // Set the application window icon
  std::string iconPaths[] = {"assets/app_icon.bmp", "../assets/app_icon.bmp",
                             "../../assets/app_icon.bmp", "./app_icon.bmp"};
  SDL_Surface *iconSurface = nullptr;
  for (const auto &path : iconPaths) {
    iconSurface = SDL_LoadBMP(path.c_str());
    if (iconSurface) {
      break;
    }
  }
  if (iconSurface) {
    SDL_SetWindowIcon(window, iconSurface);
    SDL_DestroySurface(iconSurface);
  }

  // Create hardware-accelerated renderer
  SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);

  if (!renderer) {
    std::cerr << "Error: Could not create SDL3 Renderer: " << SDL_GetError()
              << std::endl;
    SDL_DestroyWindow(window);
    SDL_Quit();
    return -1;
  }
  SDL_SetRenderVSync(renderer, 1); // Enable VSync

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |=
      ImGuiConfigFlags_NavEnableKeyboard; // Enable keyboard navigation

  // Bind ImGui to SDL3 and SDL_Renderer
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  // Initialize player modules
  PlayerController controller;
  PlayerUI playerUI(controller);
  playerUI.init();

  // Register native file dialog for all platforms (nativefiledialog-extended
  // handles Windows/Linux natively). The window is captured so the dialog
  // can be parented to it -- see setNfdParentWindow() for why that matters.
  playerUI.setFileDialogCallback(
      [window]() { return openNativeFileDialog(window); });

  // Enable drag and drop events
  SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);

  // Parse command line arguments
  bool metricsEnabled = false;
  std::string mediaPath = "";
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--metrics") {
      metricsEnabled = true;
    } else if (arg == "--console") {
      // Already consumed before SDL initialization above (Windows only).
      // Still matched here so it is never mistaken for the media path by
      // the branch below -- and so it is accepted, not treated as a
      // filename, on non-Windows builds where it does nothing.
      continue;
    } else if (mediaPath.empty()) {
      mediaPath = arg;
    }
  }

  if (metricsEnabled) {
    controller.getPipelineMetrics().setProfilingEnabled(true);
    playerUI.setDiagnosticsVisible(true);
  }

  if (!mediaPath.empty()) {
    if (controller.openFile(mediaPath)) {
      controller.play();
    }
  }

  // Video texture parameters
  SDL_Texture *videoTexture = nullptr;
  int texWidth = 0;
  int texHeight = 0;
  SDL_PixelFormat texFormat = SDL_PIXELFORMAT_UNKNOWN;
  SDL_Colorspace texColorspace = SDL_COLORSPACE_UNKNOWN;
  DecodedFrame currentFrame;

  bool quit = false;
  SDL_Event event;
  bool windowMinimized = false;
  bool isCursorHidden = false;
  double lastMouseActivityTime = 0.0;

  std::cout << "Application loop started. Press Space to pause, Left/Right "
               "arrow keys to seek 10s."
            << std::endl;

  // cppcheck-suppress knownConditionTrueFalse
  // isSignalQuitRequested() is a hardcoded `return false` stub on Windows
  // (only POSIX gets real signal-handling via g_signalQuit) -- always-true
  // here is intentional cross-platform behavior, not a bug.
  while (!quit && !isSignalQuitRequested()) {
    double currentSecs = SDL_GetTicks() / 1000.0;

    // The whole per-frame body is guarded: PlayerController::openFile()
    // (reached below from drag & drop) recreates decode threads and
    // reallocates several buffers on every call, so an uncaught
    // std::bad_alloc or std::system_error (thread-creation failure) from
    // repeatedly opening/closing files here would otherwise propagate all
    // the way out of main() and std::terminate() the whole app instead of
    // just failing to load that one file.
    try {

    // 1. Process Windows Events & Input Routing
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);

      // Notify UI of mouse, keyboard or wheel events to prevent auto-hiding
      if (event.type == SDL_EVENT_MOUSE_MOTION ||
          event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
          event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
          event.type == SDL_EVENT_MOUSE_WHEEL ||
          event.type == SDL_EVENT_KEY_DOWN) {
        lastMouseActivityTime = currentSecs;
        playerUI.notifyMouseActivity(currentSecs);
        if (isCursorHidden) {
          SDL_ShowCursor();
          isCursorHidden = false;
        }
      }

      if (event.type == SDL_EVENT_QUIT) {
        quit = true;
      } else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
        winWidth = event.window.data1;
        winHeight = event.window.data2;
      } else if (event.type == SDL_EVENT_WINDOW_MINIMIZED) {
        windowMinimized = true;
      } else if (event.type == SDL_EVENT_WINDOW_RESTORED) {
        windowMinimized = false;
      } else if (event.type == SDL_EVENT_DROP_FILE) {
        // Drag & Drop media file loading
        const char *droppedFilepath = event.drop.data;
        std::cout << "Dropped file detected: " << droppedFilepath << std::endl;
        if (controller.openFile(droppedFilepath)) {
          controller.play();
        }
      } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !io.WantCaptureMouse) {
        // Double-click on video canvas toggles fullscreen
        if (event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 2) {
          toggleFullscreen(window);
        }
      } else if (event.type == SDL_EVENT_MOUSE_WHEEL && !io.WantCaptureMouse) {
        // Mouse wheel adjusts volume
        float wheelDelta = event.wheel.y * 5.0f;
        if (wheelDelta != 0.0f) {
          playerUI.adjustVolume(wheelDelta);
        }
      } else if (event.type == SDL_EVENT_KEY_DOWN && !io.WantCaptureKeyboard) {
        // Process keyboard hotkeys
        switch (event.key.key) {
        case SDLK_SPACE:
          if (controller.getState() == PlayerState::PLAYING) {
            controller.pause();
          } else if (controller.getState() == PlayerState::PAUSED ||
                     controller.getState() == PlayerState::OPENED) {
            controller.play();
          }
          break;
        case SDLK_F11:
          toggleFullscreen(window);
          break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          if (event.key.mod & SDL_KMOD_ALT) {
            toggleFullscreen(window);
          }
          break;
        case SDLK_M:
          playerUI.toggleMute();
          break;
        case SDLK_UP:
          playerUI.adjustVolume(5.0f);
          break;
        case SDLK_DOWN:
          playerUI.adjustVolume(-5.0f);
          break;
        case SDLK_S:
          if (currentFrame.frame && currentFrame.frame->data[0]) {
            auto res = FrameExporter::saveFrameAsPng(
                currentFrame.frame, controller.getFilename(),
                controller.getCurrentTime());
            if (res.success) {
              playerUI.showToast("Screenshot saved: " + res.filepath, false, 3.5);
            } else {
              playerUI.showToast("Screenshot failed: " + res.errorMessage, true, 3.5);
            }
          } else {
            playerUI.showToast("No active video frame to capture", true, 2.5);
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
          // If in fullscreen, exit fullscreen first; otherwise quit
          if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0) {
            SDL_SetWindowFullscreen(window, false);
          } else {
            quit = true;
          }
          break;
        case SDLK_L:
          controller.setLoopEnabled(!controller.isLoopEnabled());
          break;
        case SDLK_LEFTBRACKET: {
          float current = controller.getPlaybackSpeed();
          controller.setPlaybackSpeed(std::round((current - 0.25f) * 100.0f) / 100.0f);
          break;
        }
        case SDLK_RIGHTBRACKET: {
          float current = controller.getPlaybackSpeed();
          controller.setPlaybackSpeed(std::round((current + 0.25f) * 100.0f) / 100.0f);
          break;
        }
        case SDLK_BACKSPACE:
          controller.setPlaybackSpeed(1.0f);
          break;
        case SDLK_D:
          playerUI.toggleDiagnostics();
          break;
        case SDLK_A:
          playerUI.toggleAudioSettings();
          break;
        default:
          break;
        }
      }
    }

    // Auto-hide mouse cursor after 2.5s of inactivity when not interacting with ImGui
    if (!isCursorHidden && !io.WantCaptureMouse && (currentSecs - lastMouseActivityTime > 2.5)) {
      SDL_HideCursor();
      isCursorHidden = true;
    }


    // Applies a completed background loudness prescan, if one just
    // finished -- see PlayerController::prescanLoudnessForCurrentFile()
    // (the decode-based path runs on a background thread specifically so
    // opening a new file above never blocks this event loop).
    controller.pollPendingLoudnessPrescan();

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
        // Exception: if we don't have any frame displayed yet and we are paused
        // (OPENED/PAUSED), we allow popping the first frame even if its PTS is
        // slightly in the future to show a first frame.
        if (nextFrame.pts > timeNow && !seekCatchup) {
          if (!currentFrame.frame && !hasTarget) {
            // Allow popping the first frame whenever no frame is currently displayed (e.g. after seek or loop)
          } else {
            break;
          }
        }

        DecodedFrame poppedFrame;
        if (controller.getDecodedFrameQueue().pop(poppedFrame)) {
          if (hasTarget && targetFrame.frame) {
            av_frame_free(&targetFrame.frame);
            controller.getPipelineMetrics().m_framesDroppedCount.fetch_add(1, std::memory_order_relaxed);
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
        SDL_PixelFormat targetFormat = SDL_PIXELFORMAT_IYUV;
        if (currentFrame.frame->format == AV_PIX_FMT_NV12) {
          targetFormat = SDL_PIXELFORMAT_NV12;
        } else if (currentFrame.frame->format == AV_PIX_FMT_NV21) {
          targetFormat = SDL_PIXELFORMAT_NV21;
        }

        SDL_Colorspace colorspace = getSDLColorspace(currentFrame.frame);

        // Re-create video display texture if dimensions, pixel format, or
        // colorspace changed
        if (!videoTexture || texWidth != currentFrame.width ||
            texHeight != currentFrame.height || texFormat != targetFormat ||
            texColorspace != colorspace) {
          if (videoTexture) {
            SDL_DestroyTexture(videoTexture);
          }
          texWidth = currentFrame.width;
          texHeight = currentFrame.height;
          texFormat = targetFormat;
          texColorspace = colorspace;

          SDL_PropertiesID props = SDL_CreateProperties();
          SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                                texFormat);
          SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                                SDL_TEXTUREACCESS_STREAMING);
          SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,
                                texWidth);
          SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,
                                texHeight);
          SDL_SetNumberProperty(
              props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, texColorspace);
          videoTexture = SDL_CreateTextureWithProperties(renderer, props);
          SDL_DestroyProperties(props);

          if (videoTexture) {
            SDL_SetTextureScaleMode(videoTexture, SDL_SCALEMODE_LINEAR);
          }
        }

        if (shouldUpdateTexture) {
          if (controller.getPipelineMetrics().m_profilingEnabled.load(std::memory_order_relaxed)) {
            float offsetMs = static_cast<float>((currentFrame.pts - timeNow) * 1000.0);
            controller.getPipelineMetrics().m_avClockOffsetMs.record(offsetMs);
          }
          // Frame pacing: time elapsed since the last texture update
          static auto lastFrameTime = std::chrono::steady_clock::now();
          auto now = std::chrono::steady_clock::now();
          if (controller.getState() == PlayerState::PLAYING) {
            uint64_t pacingUs = std::chrono::duration_cast<std::chrono::microseconds>(now - lastFrameTime).count();
            // cap pacing to a reasonable value (e.g. 1 second) to prevent huge values after pausing/seeking
            if (pacingUs < 1000000) {
              controller.setFramePacingUs(pacingUs);
            }
          }
          lastFrameTime = now;

          auto renderStart = std::chrono::steady_clock::now();
          // Copy raw plane segments directly to GPU-mapped texture memory
          if (texFormat == SDL_PIXELFORMAT_NV12 ||
              texFormat == SDL_PIXELFORMAT_NV21) {
            SDL_UpdateNVTexture(
                videoTexture, nullptr, currentFrame.frame->data[0],
                currentFrame.frame->linesize[0], currentFrame.frame->data[1],
                currentFrame.frame->linesize[1]);
          } else {
            SDL_UpdateYUVTexture(
                videoTexture, nullptr, currentFrame.frame->data[0],
                currentFrame.frame->linesize[0], currentFrame.frame->data[1],
                currentFrame.frame->linesize[1], currentFrame.frame->data[2],
                currentFrame.frame->linesize[2]);
          }
          auto renderEnd = std::chrono::steady_clock::now();
          uint64_t renderUs = std::chrono::duration_cast<std::chrono::microseconds>(renderEnd - renderStart).count();
          controller.setVideoRenderTimeUs(renderUs);
          if (controller.getPipelineMetrics().m_profilingEnabled.load(std::memory_order_relaxed)) {
            controller.getPipelineMetrics().m_uploadTimeUs.record(static_cast<float>(renderUs));
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
        texColorspace = SDL_COLORSPACE_UNKNOWN;
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
      SDL_FRect dstRect;
      float windowAspect = static_cast<float>(winWidth) / winHeight;
      float videoAspect = static_cast<float>(texWidth) / texHeight;

      if (windowAspect > videoAspect) {
        // Window is wider than video -> pillarbox (draw side bars)
        dstRect.h = static_cast<float>(winHeight);
        dstRect.w = winHeight * videoAspect;
        dstRect.x = static_cast<float>(winWidth - dstRect.w) / 2.0f;
        dstRect.y = 0.0f;
      } else {
        // Window is taller than video -> letterbox (draw top/bottom bars)
        dstRect.w = static_cast<float>(winWidth);
        dstRect.h = winWidth / videoAspect;
        dstRect.x = 0.0f;
        dstRect.y = static_cast<float>(winHeight - dstRect.h) / 2.0f;
      }
      SDL_RenderTexture(renderer, videoTexture, nullptr, &dstRect);
    }

    // B. Render Dear ImGui UI Overlay
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    playerUI.draw(winWidth, winHeight, currentSecs);

    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

    // Display frame. Skipped while minimized: Windows throttles/blocks
    // vsync Present() on an occluded swapchain, and letting that block
    // this thread is what causes the slowdown/freeze while minimized
    // and the multi-second catch-up after restoring (see comment above
    // windowMinimized's declaration).
    auto presentStart = std::chrono::steady_clock::now();
    if (!windowMinimized) {
      SDL_RenderPresent(renderer);
    }
    auto presentEnd = std::chrono::steady_clock::now();
    uint64_t presentUs = std::chrono::duration_cast<std::chrono::microseconds>(presentEnd - presentStart).count();
    controller.setPresentTimeUs(presentUs);

    // Sleep to avoid pegging CPU when minimized or if VSync is disabled.
    // If VSync is enabled, SDL_RenderPresent blocks and rate-limits naturally,
    // so we do not sleep to avoid missing refresh boundaries.
    int vsync = 0;
    SDL_GetRenderVSync(renderer, &vsync);
    if (windowMinimized || vsync <= 0) {
      SDL_Delay(5);
    }

    } catch (const std::exception& e) {
      std::cerr << "Error: unhandled exception in main loop: " << e.what()
                << std::endl;
    } catch (...) {
      std::cerr << "Error: unhandled non-standard exception in main loop"
                << std::endl;
    }
  }

  // 4. Cleanup resources on exit
  if (videoTexture) {
    SDL_DestroyTexture(videoTexture);
  }
  if (currentFrame.frame) {
    av_frame_free(&currentFrame.frame);
  }
  controller.stop();

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();

  std::cout << "Application closed successfully" << std::endl;
  return 0;
}
