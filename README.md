# NaikAVPlayer

A native, multi-threaded C++ media engine and video player built for extreme performance, low memory footprint, and zero-latency seek responsiveness.

![NaikAVPlayer Screenshot](assets/screenshot.png)

Built on top of barebones **FFmpeg**, **SDL2**, and **Dear ImGui**, **NaikAVPlayer** implements container parsing, frame rescaling, and sub-frame clock synchronization directly on raw texture planes with no external wrapper overhead.

---

## Key Features

- 🏎️ **Symmetric Seeking:** Near-instantaneous forward and backward seeks without deadlocks or frames freezing.
- ⚙️ **Dynamic Hardware Fallback:** Seamlessly attempts hardware-accelerated H.264 decoding (Intel QSV, NVIDIA CUVID, D3D11VA, DXVA2, VAAPI, or V4L2M2M depending on OS), falling back dynamically to the software `h264` decoder on failure (e.g. driverless or virtualized systems) to prevent playback disruptions.
- ⏱️ **Audio-Video Synchronization:** Sub-frame accurate clock sync maintaining frame drift under `10ms` relative to the audio device master timeline.
- 🎛️ **Software Volume Controls:** Software audio sample attenuator with dynamic byte scaling, including a zero-overhead mute/bypass layout.
- 🔁 **Loop Playback Mode:** Toggle continuous replay (via the Loop control button or the `L` hotkey) to automatically seek back to the start on reaching end-of-file instead of stopping — ideal for kiosks, demos, and long-running validation.
- 📂 **Flexible Media Loading:** Supports drag-and-drop file ingestion directly onto the player window, or custom local file system parsing.
- 🗂️ **Native File Picker:** Cross-platform native OS file dialog powered by **nativefiledialog-extended (NFD)** — uses the Win32 File Explorer on Windows and GTK3/Portal on Linux.
- 📊 **Diagnostics HUD:** Real-time HUD diagnostics overlay displaying player states, playback clock offsets, media metadata, resolution metrics, and a security note warning for decoder maintenance.
- 🎨 **Modern Glassmorphic GUI with Vector Icons:** Floating dock and cinematic header layouts with a frosted translucent obsidian design, circular progress grabs, interactive welcome onboarding cards, toggleable HUD sidebars, and **programmatically-rendered vector control icons** (Play, Pause, Stop, Seek, Volume, Loop, Browse) featuring neon cyan hover highlights and accessibility tooltips.
- 🔤 **Bundled Open-Source Typography:** Integrated with **Noto Sans** fonts (SIL Open Font License 1.1) scanned dynamically from relative and installed system paths, avoiding system-dependent proprietary lookups.
- 🖼️ **Window Branding:** Branded with a custom high-fidelity app icon loaded natively as the SDL2 window and taskbar icon.
- 🧪 **100% Logic Test Coverage:** A fully instrumented functional integration and white-box test suite executing 100% of the player's core playback controller, demuxer, and audio/video decoder logical lines.

---

## Key Performance Indicators (KPIs)

To maintain extreme performance and rendering accuracy, the core engine adheres to strict engineering targets:
- ⏱️ **Audio-Video Drift (`< 10ms`):** Frame-alignment threshold relative to the audio device master clock timeline. If the video frame PTS lags by more than 10ms (`timeNow - 0.010` in `main.cpp`), the player enters a fast-forward decode loop to drop late frames and catch up instantly.
- 🏎️ **Seek Latency (`< 80ms`):** Average seek-to-keyframe catch-up response under normal workloads. The player immediately flushes packet queues and codec caches, then deactivates seeking catch-up once the current frame PTS is within 80ms (`timeNow - 0.080` in `main.cpp`) of the target seek clock.
- 🧪 **Code Quality (`100.00%`):** Unit and integration test line coverage on all core playback, demuxing, and decoding engine files (`AudioDecoder.cpp`, `VideoDecoder.cpp`, `Demuxer.cpp`, `PlayerController.cpp`, and `ThreadSafeQueue.hpp`).
- 🎛️ **Audio Attenuation:** Zero-overhead software attenuator bypass for mute and full volume states. Full volume (`volume >= 0.99f`) bypasses the scaling loop using `std::memcpy`; mute volume (`volume <= 0.01f`) bypasses the loop using `std::memset` to 0.

---

## Architecture

NaikAVPlayer follows the classic multi-threaded media player design: a demuxer thread, two decoder paths, and a render loop, coordinated through bounded thread-safe queues and a single source of truth for "what time is it."

### Thread Model

```
┌─────────────┐     packets      ┌──────────────────┐
│             ├─────────────────►│  Video Queue (100) │──► VideoDecoder ──► SWS Scale ──► SDL Texture
│   Demuxer   │                  └──────────────────┘                                         ▲
│  (thread)   │                                                                                │
│             ├─────────────────►┌──────────────────┐                                          │
└─────────────┘     packets      │  Audio Queue (150) │──► AudioDecoder ──► SDL Callback ───────┘
                                  └──────────────────┘         (master clock)
```

- **Demuxer thread** reads packets via `av_read_frame` and routes them into one of two bounded `ThreadSafeQueue<AVPacket*>` instances (video: 100 packets, audio: 150 — audio packets are smaller, so the queue can hold more before backpressuring).
- **Video decoding happens on the main/render thread**, pulled on-demand each frame via `try_pop` — it never blocks the UI loop waiting on the queue.
- **Audio decoding is driven by SDL's audio callback thread**, which pulls from the audio queue and feeds the device buffer directly.
- The bounded queues use two condition variables (`m_cond_push`/`m_cond_pop`) so a full queue naturally stalls the demuxer (applying backpressure) without spinning, and `abort()` cleanly wakes every blocked thread for shutdown.

#### GPU-Mapped Planar YUV Uploads
Instead of performing costly YUV-to-RGB color space conversions on the CPU, the video decoder pipeline extracts raw YUV 4:2:0 planar frame data directly. The main thread maps this data onto a hardware-accelerated SDL2 streaming texture (`SDL_PIXELFORMAT_IYUV`) using `SDL_UpdateYUVTexture`. This uploads the raw plane segments directly to GPU-mapped texture memory, allowing the graphics hardware to handle color space conversion and scaling efficiently.

#### Dynamic Hardware Decoder Fallback
To achieve optimal playback performance without sacrificing robustness, the video decoder pipeline employs a dynamic hardware-to-software fallback. At initialization, it queries and tries to open native hardware decoders (such as `h264_d3d11va`, `h264_dxva2`, `h264_qsv`, or `h264_cuvid` on Windows; `h264_vaapi`, `h264_v4l2m2m` on Linux). If a hardware decoder fails during initialization or encounters a fatal decoding or surface mapping error at runtime (e.g. running on driverless or virtualized headless environments), the decoder intercepts the failure, releases the hardware context, configures the software `h264` decoder, and resubmits the video packet. This guarantees a seamless transition with zero playback disruption or application crashes.

### Audio-Master Clock

Rather than syncing playback to system wall-clock time, the player treats **audio as the master clock** whenever an audio stream is present — the standard approach in production media players, since audio dropouts and clicks are far more perceptible to the ear than the eye is to a duplicated or dropped video frame.

The audio clock isn't just "the last decoded packet's timestamp." It's reconstructed sample-accurately:

```
audio_clock = base_pts_of_current_frame + (bytes_already_consumed_by_SDL / bytes_per_second)
```

`AudioDecoder::getAudioClock()` combines the PTS of the most recently decoded frame with how far the SDL audio callback has already progressed *into* that frame's buffer, giving sub-frame timing resolution rather than per-packet granularity. This is what makes the `<10ms` drift KPI meaningful rather than aspirational — the reference clock itself is precise enough to support that tolerance.

When there's no audio track, the controller falls back to a wall-clock-driven `m_videoClock` that advances using `steady_clock` deltas between render ticks (`updateClockForVideoOnly`), so video-only files still play at the correct rate.

### Catch-Up Logic: Two Thresholds, Different Jobs

Each render tick, the video path compares the currently-decoded frame's PTS against the master clock and decides whether to drop frames to catch up. Two thresholds govern this, deliberately set apart from each other to avoid thrashing:

| Threshold | Purpose |
|---|---|
| **10ms** (`timeNow - 0.010`) | Steady-state drop sensitivity — if the decoded frame is more than 10ms behind, decode-and-discard the next frame instead of displaying it. Tight enough to keep drift imperceptible. |
| **80ms** (`timeNow - 0.080`) | "Close enough" exit condition — during an active seek catch-up, stop dropping once within 80ms of target. Wide enough to avoid an extra decode cycle just to shave off the last few milliseconds. |

The drop budget itself is adaptive:
- **Normal playback:** up to 8 frame drops per render tick, keeping the UI loop responsive even if momentarily behind.
- **Large drift (>500ms behind):** budget raises to 32 drops/tick — covers cases like a slow disk read or a brief stall.
- **Active seek:** budget raises to 2000 drops/tick, so a seek can decode forward to the target keyframe's neighborhood in a single burst rather than over several render frames.

During seek catch-up, texture updates are suppressed for any frame still more than 80ms behind target — this is what prevents the visual "fast-forward flash" a naive catch-up loop would otherwise show while burning through stale frames.

### Seek Flow

1. UI thread calls `PlayerController::seek()` → pauses audio output, signals the demuxer, and clears both packet queues immediately (don't wait for the demuxer thread to notice).
2. Demuxer thread independently calls `avformat_seek_file()` (binary-search index seek to the nearest keyframe) and re-clears the queues defensively in case any stale packets were in flight.
3. Both decoders are flushed (`avcodec_flush_buffers`) to drop any cached reference frames from the old position.
4. Render loop enters the high-budget catch-up path described above until the decoded PTS lands within 80ms of the seek target.

This two-sided clear (UI thread *and* demuxer thread both clearing the queues) is what keeps perceived seek latency low — the player doesn't wait for an in-flight `av_read_frame()` call to return before discarding old data.

### State Machine Transitions
The player playback engine is governed by a strict state machine to synchronize operations between threads:
* **`UNINITIALIZED`:** The player is empty. Loading a media file starts background demuxing and transitions the state to `OPENED`.
* **`OPENED`:** The media is loaded, and the decoders are prepared. The first frame is decoded and rendered on the screen immediately. Triggering `play()` transitions the state to `PLAYING`.
* **`PLAYING`:** Audio output is unpaused, and the main loop decodes and syncs video frames to the master clock.
* **`PAUSED`:** Playback is frozen. The audio device is paused to hold the current clock position.
* **`ENDED`:** Reached when the demuxer hits EOF and all packet queues are fully drained. The audio device is paused. Seeking back (e.g., `seek(0.0)`) or playing transitions the engine back to active states. If **Loop Mode** is enabled, this transition is bypassed entirely — reaching EOF while `PLAYING` instead calls `seek(0.0)` directly, reusing the same flush/clock-reset pipeline as a manual seek, and playback continues without ever entering `ENDED`.
* **`ERROR_STATE`:** Entered if demuxing or stream initialization fails, prompting safe release of resources.

---

## Tech Stack & Dependencies

- **C++17** (compiled with GCC/MinGW)
- **FFmpeg 8.x (avcodec, avformat, avutil, swscale, swresample)** (automatically downloaded LGPL-shared binaries)
- **SDL2** (automatically fetched and dynamically compiled)
- **Dear ImGui** (automatically fetched and statically compiled)
- **nativefiledialog-extended (NFD)** (fetched and compiled dynamically for cross-platform native file dialogs — zlib license)
- **Noto Sans Font** (bundled open-source SIL OFL 1.1 font files)
- **App Icon Asset** (custom-designed PNG and BMP formats)
- **ccache** (Optional: recommended compiler caching tool to accelerate clean compiles and CI workflows)

---

## Supported Media Formats

Thanks to its barebones FFmpeg demuxer and decoder integration, **NaikAVPlayer** supports a wide range of media containers and codecs, including:

- **Video Containers:** `.mp4`, `.mkv`, `.avi`, `.mov`, `.webm`, `.flv`
- **Audio Containers:** `.mp3`, `.wav`, `.ogg`, `.flac`, `.aac`
- **Video Codecs:** H.264 (AVC), H.265 (HEVC), VP8, VP9, MPEG-4
- **Audio Codecs:** AAC, MP3, Vorbis, FLAC, PCM

---

## Installation & Compilation

The project is natively cross-platform and compiles under **Windows** (via MinGW-w64 GCC) and **Linux** (via GCC).

The build system supports a custom `PLATFORM` configuration variable:
- **`AUTO`** (default): Automatically detects the host operating system.
- **`WINDOWS`**: Explicitly configures the project to build for Windows (links Win32 subsystems, fetches and copies FFmpeg DLLs).
- **`LINUX`**: Explicitly configures the project to build for Linux (links `pthread` and `dl`).

### Prerequisites

#### Windows
Ensure you have **CMake (version 3.16+)** and **MinGW-w64 (GCC)** configured on your path. 

The project features a **fully automated setup** for Windows: CMake will automatically download, extract, and configure the correct pre-compiled FFmpeg shared binaries package in the `thirdparty/ffmpeg` folder.

#### Linux
Install the development libraries via your package manager (e.g. `apt` on Ubuntu):
```bash
sudo apt-get update
sudo apt-get install -y libsdl2-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
```

For the native file dialog to compile and work, ensure the GTK3 development library is installed:
```bash
sudo apt-get install -y libgtk-3-dev
```

---

### Step 1: Configure the Project

Generate the build configurations:

**Auto-detect (Recommended):**
```bash
cmake -B build
```

**Explicitly Target Windows (MinGW):**
```bash
cmake -B build -G "MinGW Makefiles" -DPLATFORM=WINDOWS
```

**Explicitly Target Linux:**
```bash
cmake -B build -DPLATFORM=LINUX
```

---

### Step 2: Compile the Project

Build the primary player binary and the test suite:
```bash
cmake --build build
```
*Note: On Windows, a post-build recipe automatically copies the required FFmpeg shared DLLs, the SDL2 DLL, and the bundled assets directory directly into the compilation target folder so you can run the binaries immediately.*

---

### Step 3: Install the Application (Linux)

To install the application binaries, assets, desktop entry launcher, and system icons, run:
```bash
sudo cmake --install build
```
This registers **NaikAVPlayer** with the desktop environment launcher search and stores assets in system paths (`/usr/local/share/NaikAVPlayer`).

---

### Step 4: Uninstall the Application (Linux)

To completely remove the installed files and clean desktop launcher integration:
```bash
sudo cmake --build build --target uninstall
```

---

## Usage Guide

### Running the Player

Launch the compiled player executable or installed application:

**Windows (PowerShell):**
```powershell
.\build\NaikAVPlayer.exe
```

**Linux (Local Build):**
```bash
./build/NaikAVPlayer
```

**Linux (System-Wide Installed):**
You can launch **NaikAVPlayer** from your desktop environment applications menu or run:
```bash
NaikAVPlayer
```

Or pass a media file path directly as an argument:

**Windows:**
```powershell
.\build\NaikAVPlayer.exe "C:\Path\To\video.mp4"
```

**Linux (Local Build):**
```bash
./build/NaikAVPlayer "/home/user/Videos/video.mp4"
```

**Linux (System-Wide Installed):**
```bash
NaikAVPlayer "/home/user/Videos/video.mp4"
```

### Keyboard Shortcuts
- **`Spacebar`**: Toggle Play / Pause.
- **`Left Arrow`** ($\leftarrow$): Seek backward by 10 seconds.
- **`Right Arrow`** ($\rightarrow$): Seek forward by 10 seconds.
- **`L`**: Toggle Loop Mode on/off.
- **`Escape`**: Close and exit the player.

---

## Verification & Tests

The test video was programmatically generated for testing purposes and contains only synthetic audio and graphics.

### Running Tests Locally with CTest (Same as CI)

You can run the tests using standard CMake test driver commands.

#### 1. Standard Test Execution
Configure the project, build, and run tests via CTest:
```bash
# Configure and Build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Run tests via CTest (with verbose logs on failure)
ctest --test-dir build --output-on-failure
```

#### 2. Running with Address and Undefined Behavior Sanitizers (ASan/UBSan)
To catch memory leaks, out-of-bounds access, and undefined behavior, build with sanitizers enabled:
```bash
# Configure with Sanitizers
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"

# Build
cmake --build build

# Run tests with Sanitizer instrumentation
ctest --test-dir build --output-on-failure
```

#### 3. Native Direct Execution
Alternatively, run the compiled test executable directly:
```powershell
# Windows
.\build\NaikAVPlayer_tests.exe ".\assets\hd_test_video_with_audio.mp4"

# Linux
./build/NaikAVPlayer_tests "./assets/hd_test_video_with_audio.mp4"
```
Or use the `TEST_VIDEO_PATH` environment variable:
```powershell
# Windows (PowerShell)
$env:TEST_VIDEO_PATH=".\assets\hd_test_video_with_audio.mp4"
.\build\NaikAVPlayer_tests.exe

# Linux/macOS
export TEST_VIDEO_PATH="./assets/hd_test_video_with_audio.mp4"
./build/NaikAVPlayer_tests
```

### Code Coverage Statistics
To check the code coverage, run the tests and then generate the stats from the `build` directory:
```powershell
cd build
gcov -o CMakeFiles/NaikAVPlayer_tests.dir/tests/tests.cpp.obj ../src/AudioDecoder.cpp
gcov -o CMakeFiles/NaikAVPlayer_tests.dir/tests/tests.cpp.obj ../src/VideoDecoder.cpp
gcov -o CMakeFiles/NaikAVPlayer_tests.dir/tests/tests.cpp.obj ../src/Demuxer.cpp
gcov -o CMakeFiles/NaikAVPlayer_tests.dir/tests/tests.cpp.obj ../src/PlayerController.cpp
```
This generates `.gcov` files confirming **100.00% Line Coverage** for the core player logic engine.

### CI/CD Pipeline (GitHub Actions)

The project has a robust **GitHub Actions CI/CD pipeline** (configured in [.github/workflows/ci.yml](.github/workflows/ci.yml)) that validates all pull requests, pushes to main/master, and runs nightly builds.

- **Cross-Platform Verification:** Compiles and runs tests on both Windows (MinGW-w64 GCC) and Linux (Ubuntu GCC).
- **Sanitizers:** Runs the integration tests under AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) on every PR/schedule to catch memory safety issues.
- **Static Analysis:** Performs static analysis using `cppcheck` on C++ source files.
- **Compiler Caching (`ccache`):** The pipeline uses `ccache` to cache compile units globally. This speeds up build pipelines by bypassing recompilation of heavy dependencies like SDL2 and Dear ImGui (bringing compilation down from minutes to under 5 seconds).

---

## Open-Source Attribution & Credits

NaikAVPlayer is published under the **MIT License**. It links dynamically and statically to the following libraries and assets:
- **FFmpeg** (Licensed under LGPL v3.0, ensuring full legal compliance with the application's permissive MIT license)
- **SDL2** (Licensed under the Zlib License)
- **Dear ImGui** (Licensed under the MIT License)
- **nativefiledialog-extended (NFD)** (Licensed under the Zlib License — Copyright © 2014-2020 Michael Labbé, Copyright © 2020-2024 btzy)
- **Noto Sans Font** (Licensed under the SIL Open Font License 1.1)
