# NaikAVPlayer Build & Usage Guide

This guide describes how to configure, compile, install, run, and uninstall the native C++ video player on Windows and Linux platforms.

---

## 1. Prerequisites & Dependencies

The project is cross-platform and requires CMake and C++17 compliant compilers.

### Windows (Native MinGW-w64)
- **CMake (version 3.16+)**: Build configuration generation tools.
- **MinGW-w64 (GCC)**: Native GCC compiler toolchain for Windows.
- **FFmpeg**: Shared release libraries. Automatically downloaded and configured in `thirdparty/ffmpeg/` during the CMake configuration step.
- **SDL3, Dear ImGui, nativefiledialog-extended (NFD)**: Automatically downloaded and compiled from source via FetchContent.

### Linux
- **GCC**: Compiler supporting C++17.
- **FFmpeg development headers**: Must be installed via the system package manager (e.g. `libavcodec-dev`, `libavformat-dev`, `libavutil-dev`, `libswscale-dev`, `libswresample-dev`).
- **GTK3 development libraries**: Must be installed via the system package manager (`libgtk-3-dev`) to compile the native file dialog backend.
- **SDL3, Dear ImGui, nativefiledialog-extended (NFD)**: Automatically downloaded and compiled from source via FetchContent.

---

## 2. Compilation Instructions

The build system supports the `PLATFORM` cache variable to configure platform-specific linking:
- **`AUTO`** (default): Automatically detects the host operating system.
- **`WINDOWS`**: Configures project subsystems and libraries for Windows targets.
- **`LINUX`**: Configures linking targets for Linux environments.

### Step A: Configure the CMake Build Target

Execute configuration command from the project root directory:

**Auto-detect (Default):**
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

**Cross-Compile for Windows on Linux:**
To cross-compile Windows binaries on a Linux host:
```bash
# Install the cross-compiler toolchain
sudo apt-get install -y mingw-w64

# Configure CMake with cross-compiler toolchain settings
cmake -B build-windows \
  -DPLATFORM=WINDOWS \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++

# Build binaries
cmake --build build-windows
```

### Step B: Compile Project
Build target executables:
```bash
cmake --build build
```
*(On Windows platforms, a post-build target automatically copies FFmpeg and SDL3 DLLs from the source path directly into the output directory).*

### Step C: Install Application (Linux)
Install target binaries, desktop launchers, icons, and typography to system paths:
```bash
sudo cmake --install build
```

### Step D: Uninstall Application (Linux)
Remove installed binaries and desktop reference entries:
```bash
sudo cmake --build build --target uninstall
```

---

## 3. Running the Player

Execute the target binary from the build directory or the installed path.

**Windows (PowerShell):**
```powershell
.\build\NaikAVPlayer.exe
```

**Linux (Local Build):**
```bash
./build/NaikAVPlayer
```

**Linux (System Installed):**
```bash
NaikAVPlayer
```

**Provide media file path argument:**

**Windows:**
```powershell
.\build\NaikAVPlayer.exe "C:\Path\To\video.mp4"
```

**Linux:**
```bash
./build/NaikAVPlayer "/home/user/Videos/video.mp4"
```

**Run with telemetry profiling HUD enabled:**

**Windows:**
```powershell
.\build\NaikAVPlayer.exe --metrics "C:\Path\To\video.mp4"
```

**Linux:**
```bash
./build/NaikAVPlayer --metrics "/home/user/Videos/video.mp4"
```

---

## 4. UI Features & Controls

The player interface is built with Dear ImGui, rendering a frosted translucency overlay.

### Media Ingestion
1. **Drag-and-Drop**: Dragging supported video or audio formats onto the player window initiates demuxing and starts playback.
2. **Native File Selector**: Clicking the "Open Media File" button on the onboarding interface or the folder icon on the controls dock launches the system file selector (Win32 File Explorer on Windows, GTK3/Portal on Linux).

### Controls Interface
- **Progress bar**: Jump to target time positions by clicking or dragging on the timeline.
- **Volume controls**: Attenuates output audio samples. Mute/bypass controls use memset/memcpy shortcuts at thresholds (<= 1% and >= 99% volume).
- **Loop controls**: Continuous playback loop wraps around to 0.0 on end-of-file.

### Keyboard Shortcuts
- **`Spacebar`**: Toggle Play / Pause.
- **`Left Arrow`**: Seek backward by 10 seconds.
- **`Right Arrow`**: Seek forward by 10 seconds.
- **`L`**: Toggle Loop playback mode.
- **`D`**: Toggle Diagnostics HUD / telemetry metrics collection.
- **`Escape`**: Exit application.

---

## 5. Hardware Decoding & Fallback

The player supports hardware-accelerated H.264 video decoding.
- **Initialization candidates:** The decoder checks and attempts initialization of platform-specific hardware decoders (Windows: `h264_d3d11va`, `h264_dxva2`, `h264_qsv`, `h264_cuvid`; Linux: `h264_v4l2m2m`, `h264_vaapi`, `h264_qsv`, `h264_cuvid`).
- **Dynamic software fallback:** If hardware initialization fails, or if a runtime error occurs during hardware context extraction, the system automatically allocates the software `h264` codec, resubmits pending packets, and continues playback.

---

## 6. Security & Dependency Maintenance

- **Upstream Security**: The application parses untrusted container metadata and packet formats via FFmpeg. To mitigate vulnerability risks, downstream builds should keep upstream dependencies updated.
- **Dependency Pinning**: Build configurations are pinned to verified, checksum-tested releases. FFmpeg downloads use month-end releases validated with SHA-256 hashes during CMake configuration. SDL3, Dear ImGui, and nativefiledialog-extended are pinned to specific version tags.
- **Updating dependencies**: Use `update_ffmpeg_pin.py --tag <target-tag>` to generate a new checksum pair for `CMakeLists.txt`. To update SDL3/ImGui/NFD, update the corresponding `GIT_TAG` entries in `CMakeLists.txt`.

---

## 7. GitHub Actions CI/CD & Caching

The CI/CD pipeline verifies build stability on every push or pull request:
- **Warnings as errors:** Builds are compiled with `-Werror` flags to prevent warning compilation.
- **Native verification:** Native Linux builds run under standard compilation, AddressSanitizer/UndefinedBehaviorSanitizer (ASan/UBSan) instrumentation, and ThreadSanitizer (TSan) data race checking.
- **Static Analysis**: Runs static code analysis checks using `cppcheck`.
- **Cross-Compilation**: Verifies Windows target compilation on Linux runners.
- **Compiler Cache**: Uses `ccache` to persist intermediate object code on runner nodes, speeding up successive dependency compilation steps.

---

## 8. Pipeline Instrumentation & Metrics Summary

The execution pipeline tracks 9 metrics using lock-free Single Producer Single Consumer (SPSC) metric rings.

### Hook Sites & Thread Ownership

| Metric ID | Metric Name | Hook Site (File:Function) | Producing Thread | Type | Gating |
|---|---|---|---|---|---|
| **M1** | `video_packet_queue_depth` | `ThreadSafeQueue.hpp:push/pop/try_pop/clear/reset` | Multiple (Demuxer & Video Decoder) | std::atomic<int> (Gauge) | Always-On |
| **M2** | `audio_packet_queue_depth` | `ThreadSafeQueue.hpp:push/pop/try_pop/clear/reset` | Multiple (Demuxer & Audio Decoder callback) | std::atomic<int> (Gauge) | Always-On |
| **M3** | `decoded_frame_queue_depth` | `ThreadSafeQueue.hpp:push/pop/try_pop/clear/reset` | Multiple (Video Decoder & Main Render) | std::atomic<int> (Gauge) | Always-On |
| **M4** | `demux_time_per_packet_us` | `Demuxer.cpp:threadLoop()` | Demuxer thread | MetricRing<256> (SPSC) | gated |
| **M5** | `decode_time_per_frame_us` | `VideoDecoder.cpp:decodeNextFrame()` | Video Decoder thread | MetricRing<256> (SPSC) | gated |
| **M6-A** | `convert_time_us` | `VideoDecoder.cpp:convertFrame()` | Video Decoder thread | MetricRing<256> (SPSC) | gated |
| **M6-B** | `upload_time_us` | `main.cpp:main()` | Main / Render thread | MetricRing<256> (SPSC) | gated |
| **M7** | `av_clock_offset_ms` | `main.cpp:main()` | Main / Render thread | MetricRing<256> (SPSC) | gated |
| **M8** | `frames_dropped_count` | `main.cpp:main()` | Main / Render thread | std::atomic<uint64_t> (Counter) | Always-On |
| **M9** | `seek_latency_ms` | `PlayerController.cpp:seek()` (start) & `finishCatchup()` (end) | Video Decoder thread (write) / Main thread (read) | MetricRing<256> (SPSC) | gated |

### Metric Details & Design Decisions

#### Thread Ownership & Safety
- **SPSC Ring Verification**: Each `MetricRing` is modified by a single producer thread and snapshot/read by the main render thread, adhering to the SPSC model.
  - **Convert Time vs. Upload Time**: Both the hardware CPU copy (`av_hwframe_transfer_data`) and the software color-conversion/scaling path (`sws_scale`) run on the background video decoder thread. `convert_time_us` is recorded on the video decoder thread. `upload_time_us` is recorded on the main render thread where `SDL_UpdateYUVTexture` / `SDL_UpdateNVTexture` occurs. This separation preserves the single-producer constraint.
  - **Seek Latency**: Latency delta is recorded on the transition side (`finishCatchup`) on the video decoder thread, passing the start timestamp from `seek()` through catch-up epoch state variables protected under `m_catchupMutex`. This avoids concurrent writes to the SPSC ring.
- **Lock-Free Ring Writes**: MetricRing writes and gauge/counter updates are lock-free `std::atomic` operations with no condition variables or heap allocations on the hot path. The only lock is the pre-existing `m_catchupMutex` used by the seek catch-up state machine to copy the start timestamp for M9.
- **Profiling Activation Gate**: Instantiations of time-series ring measurements (`MetricRing::record`) are gated behind the `m_profilingEnabled` relaxed load check. Instantaneous gauges (M1-M3) and the dropped-frame counter (M8) are always-on.

#### Omissions or splits
- `convert_time_us` and `upload_time_us` are separated into distinct rings as they execute on different threads, satisfying the SPSC thread constraints. Other frame-loop timings displayed on the HUD (Present/VSync wait, frame pacing, audio callback duration) are measured with local steady_clock deltas on their respective threads.
