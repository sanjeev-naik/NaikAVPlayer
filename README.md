# NaikAVPlayer

[![CI/CD Pipeline](https://github.com/sanjeev-naik/NaikAVPlayer/actions/workflows/ci.yml/badge.svg)](https://github.com/sanjeev-naik/NaikAVPlayer/actions/workflows/ci.yml)
[![Coverage Status](https://codecov.io/gh/sanjeev-naik/NaikAVPlayer/graph/badge.svg)](https://codecov.io/gh/sanjeev-naik/NaikAVPlayer)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

NaikAVPlayer is a native, multi-threaded C++17 media engine and video player built using raw FFmpeg APIs, SDL3, and Dear ImGui. It performs container parsing, hardware/software video decoding, sample-accurate audio resampling, and clock synchronization directly using GPU-mapped texture updates without intermediate heavy frameworks. It achieves low-latency seeking and sub-10ms audio-video clock synchronization using dedicated worker threads coordinated through bounded blocking queues and a lock-free Single Producer Single Consumer (SPSC) ring for hot-path telemetry.

![NaikAVPlayer Screenshot](assets/screenshot.png)

---

## Key Features

- **Symmetric Low-Latency Seeking:** Rapid keyframe seek operations flushing packet queues and decoding pipelines under 80ms.
- **Dynamic Hardware Decoder Fallback:** Tries platform-specific hardware decoders (D3D11VA, DXVA2, QSV, CUVID on Windows; V4L2M2M, VAAPI, QSV, CUVID on Linux), falling back dynamically to software H.264 decoding if hardware context allocation fails or encounters runtime surface mapping errors.
- **Sub-10ms Audio-Video Synchronization:** Reconstructs the audio clock sample-accurately from PCM sample offsets to maintain A/V drift under 10ms.
- **Dynamic Resolution Scaling:** Real-time playback scaling supporting dynamic output resolution selection (Original source, 360p, 480p, 720p, 1080p, 1440p, 4K) from the UI dropdown to optimize GPU upload bandwidth.
- **Software Volume Attenuation:** Scalable audio output level adjustments with memcpy/memset bypasses for 100% and 0% volume states.
- **Loop Playback:** Wraparound seek to 0.0 upon reaching end-of-file for continuous playback.
- **Native File Picker:** Cross-platform native file picker integration using `nativefiledialog-extended` (NFD) on Win32 and GTK3/Portal backends.
- **Pipeline Diagnostics & System Info HUD:** Real-time overlay (`--metrics` or `D` key) displaying active player states, media telemetry (native vs. playback resolution, pixel format, hardware vs. software decoder type), Color & HDR pipeline characteristics (Color Space, Primaries, TRC, Range, Chroma Subsampling, Bit Depth, HDR10/HDR10+/Dolby Vision/HLG standard), pipeline queue depth levels, decode/render frame pacing budgets, and rolling clock synchronization offsets.
- **Translucent User Interface:** ImGui-based desktop interface using bundled Noto Sans typography.

---

## Architecture

NaikAVPlayer follows a multi-threaded media player design with decoupled worker threads coordinated through bounded thread-safe queues and an audio-master clock reference.

### Thread Model

```text
  ┌──────────────────┐
  │ Media File/Stream│
  └────────┬─────────┘
           │
           ▼
  ┌──────────────────────────────────────────────────────────┐
  │                      Demuxer Thread                      │
  └─────────────┬──────────────────────────────┬─────────────┘
                │ packets                      │ packets
                ▼                              ▼
  ┌───────────────────────────┐  ┌───────────────────────────┐
  │   Video Packet Queue      │  │   Audio Packet Queue      │
  │     (100 packets)         │  │     (150 packets)         │
  └─────────────┬─────────────┘  └─────────────┬─────────────┘
                │                              │
                ▼                              ▼
  ┌───────────────────────────┐  ┌───────────────────────────┐
  │   Video Decoder Thread    │  │   Audio Decoder Callback  │
  │    (HW / SW Fallback)     │  │   (SDL3 Audio Thread)     │
  └─────────────┬─────────────┘  └─────────────┬─────────────┘
                │ decoded frames               │ PCM Audio & PTS
                ▼                              ▼
  ┌───────────────────────────┐  ┌───────────────────────────┐
  │   Decoded Frame Queue     │  │     Audio Master Clock    │
  │        (8 frames)         │  │  (Sub-10ms A/V Sync Ref)  │
  └─────────────┬─────────────┘  └─────────────┬─────────────┘
                │                              │
                └──────────────┬───────────────┘
                               │
                               ▼
  ┌──────────────────────────────────────────────────────────┐
  │                   Main / Render Loop                     │
  │  ┌────────────────────────────────────────────────────┐  │
  │  │ Dequeue Frames ──► Query Master Clock (A/V Sync)   │  │
  │  │                            │                       │  │
  │  │                            ▼                       │  │
  │  │ Drop Late Frames ──► GPU YUV Texture Upload & UI   │  │
  │  └────────────────────────────────────────────────────┘  │
  └──────────────────────────────────────────────────────────┘
```

- **Demuxer Thread**: Reads raw packets via `av_read_frame` and routes them into bounded `ThreadSafeQueue<AVPacket*>` instances (video capacity: 100 packets, audio capacity: 150 packets).
- **Video Decoder Thread**: Background worker thread that pops packets from the video queue, decodes them (via hardware or software fallback), converts frames, and pushes them into the bounded `m_decodedFrameQueue` (capacity: 8 frames).
- **Audio Decoding**: Executed sample-accurately inside the SDL3 Audio Stream callback thread. It pulls packets from the audio queue, decodes them to PCM, resamples as necessary via `swr_convert`, and feeds the output stream buffer.
- **Main / Render Thread**: Dequeues decoded frames from `m_decodedFrameQueue` whose PTS matches the master clock time, updates the SDL YUV texture on the GPU, and renders the Dear ImGui interface overlay.

#### GPU-Mapped Planar YUV Uploads
Instead of performing CPU-side YUV-to-RGB color space conversion, the video decoder pipeline extracts raw YUV 4:2:0 planar frame data directly. The main thread maps this data onto a hardware-accelerated SDL3 streaming texture (`SDL_PIXELFORMAT_IYUV`) using `SDL_UpdateYUVTexture`. This uploads plane segments directly to GPU texture memory, allowing graphics hardware to handle color space conversion and scaling efficiently.

#### Dynamic Hardware Decoder Fallback
At initialization, the video decoder queries native hardware codecs (`h264_d3d11va`, `h264_dxva2`, `h264_qsv`, `h264_cuvid` on Windows; `h264_vaapi`, `h264_v4l2m2m` on Linux). If hardware initialization fails or encounters runtime frame mapping errors (e.g. running inside headless or virtualized environments), the system intercepts the error, releases the hardware context, configures software `h264`, and resubmits pending packets seamlessly.

### Audio-Master Clock Synchronization

Playback uses **audio as the master clock** whenever an audio stream is present:

```
audio_clock = base_pts_of_current_frame + (bytes_already_consumed_by_SDL / bytes_per_second)
```

`AudioDecoder::getAudioClock()` combines the PTS of the current frame with the progress of the SDL audio stream callback into that frame's buffer, delivering sub-frame timing resolution to maintain `<10ms` drift. When no audio stream is present, the engine falls back to a wall-clock `m_videoClock` driven by `std::chrono::steady_clock` deltas.

### State Machine Transitions
* **`UNINITIALIZED`**: Initial state. Loading media starts background demuxing and transitions to `OPENED`.
* **`OPENED`**: Media metadata loaded; decoders initialized; initial frame rendered. Triggering `play()` transitions to `PLAYING`.
* **`PLAYING`**: Audio output unpaused; main loop syncs video frames to the master clock.
* **`PAUSED`**: Audio device paused; current clock frozen.
* **`ENDED`**: Demuxer hits EOF and packet queues drain. If **Loop Mode** is enabled, reaching EOF directly invokes `seek(0.0)` to restart playback continuously.
* **`ERROR_STATE`**: Entered on demuxing/decoder failure, releasing resources safely.

---

## Build Guide

NaikAVPlayer officially supports two build workflows:
1. **Development Build (`Debug`)**: Optimized for active daily development, debugging with GDB/LLDB, and running tests with full debug symbols enabled.
2. **Release Build (`Release`)**: Optimized production build with compiler optimizations (`-O3`) enabled for deployment, packaging, and distribution.

### Prerequisites

#### Linux
Install development libraries via your package manager:
```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  libavcodec-dev \
  libavformat-dev \
  libavutil-dev \
  libswscale-dev \
  libswresample-dev \
  libgtk-3-dev
```
*(SDL3 v3.4.0, Dear ImGui v1.91.9, and nativefiledialog-extended v1.2.1 are automatically fetched and built from source via CMake FetchContent).*

#### Windows (Native MinGW-w64)
Ensure CMake 3.16+ and MinGW-w64 GCC are configured in your `PATH`. The build system automatically downloads and configures prebuilt FFmpeg shared binaries.

---

### Workflow 1: Development Build (Recommended for Developers)

Use the Development build for feature development, debugging, and running unit tests.

```bash
# 1. Configure for local development (Debug)
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug

# 2. Build all targets (executable & test suite)
cmake --build build-debug -j$(nproc)

# 3. Run unit tests
ctest --test-dir build-debug --output-on-failure
```

*For Windows (MinGW):*
```powershell
cmake -B build-debug -G "MinGW Makefiles" -DPLATFORM=WINDOWS -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

---

### Workflow 2: Release Build (Production & Packaging)

Use the Release build for creating production binaries and release packages.

```bash
# 1. Configure for production release
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 2. Compile release binaries
cmake --build build -j$(nproc)
```

*For Windows (MinGW):*
```powershell
cmake -B build -G "MinGW Makefiles" -DPLATFORM=WINDOWS -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

---

### Platform-Specific Configurations

- **Raspberry Pi (System FFmpeg)**: Use system FFmpeg packages to leverage hardware V4L2 M2M decoding:
  ```bash
  cmake -B build -DPLATFORM=LINUX -DNAIKAV_FORCE_BUNDLED_FFMPEG=OFF
  cmake --build build -j$(nproc)
  ```

- **Cross-Compile for Windows on Linux (MinGW-w64)**:
  ```bash
  cmake -B build-windows \
    -DPLATFORM=WINDOWS \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
  cmake --build build-windows -j$(nproc)
  ```

> **Advanced Build Options**: Standard CMake flags like `-DENABLE_SANITIZERS=ON` (ASan/UBSan), `-DENABLE_TSAN=ON` (ThreadSanitizer), `RelWithDebInfo`, and `MinSizeRel` are also supported for specialized profiling and static analysis.

---

### Install & Uninstall (Linux)

**Install to `/usr/local/`:**
```bash
sudo cmake --install build
```

**Uninstall:**
```bash
sudo cmake --build build --target uninstall
```

---

## Usage Guide

**Launch with media file argument:**

- **Windows (PowerShell):**
  ```powershell
  .\build\NaikAVPlayer.exe "C:\Path\To\video.mp4"
  ```
- **Linux:**
  ```bash
  ./build/NaikAVPlayer "/home/user/Videos/video.mp4"
  ```

**Launch with Telemetry profiling HUD enabled:**
```bash
./build/NaikAVPlayer --metrics "/home/user/Videos/video.mp4"
```

### Keyboard Controls

| Key | Action |
| :--- | :--- |
| **`Spacebar`** | Toggle Play / Pause |
| **`Left Arrow`** | Seek backward by 10 seconds |
| **`Right Arrow`** | Seek forward by 10 seconds |
| **`L`** | Toggle Loop Mode |
| **`D`** | Toggle Diagnostics HUD & Telemetry Metrics |
| **`Escape`** | Exit Application |

---

## Release Packaging & Compliance

Release packages generated by the CI/CD pipeline (`NaikAVPlayer-windows-x64`, `NaikAVPlayer-linux-x64`) are validated for full open-source redistribution compliance.

Every distributed release package archive includes:
- **Executable**: `NaikAVPlayer` or `NaikAVPlayer.exe`
- **Dynamic Libraries**: Bundled shared libraries (`.dll` or `lib/*.so*`)
- **Licenses Directory**: Complete `licenses/` and `LICENSES/` folder containing project and third-party license text files:
  - Project `LICENSE` (MIT License)
  - `LICENSE.lgpl-2.1` & `FFMPEG_CREDITS.txt` (FFmpeg LGPL v2.1+)
  - `LICENSE.sdl3` (SDL3 Zlib License)
  - `LICENSE.imgui` (Dear ImGui MIT License)
  - `LICENSE.nfd` (nativefiledialog-extended Zlib License)
  - `LICENSE.winpthread` (MinGW Winpthread License)
- **Documentation**: Project `README.md`
- **Assets**: Fonts and icons in `assets/`

Automated CI package compliance verification asserts that all executable, library, documentation, and license files are present before publishing artifacts.

---

## License & Attributions

NaikAVPlayer is released under the **[MIT License](LICENSE)**.

Third-party component licensing:
- **FFmpeg** (n8.1.2): Licensed under **LGPL v2.1+**
- **SDL3** (v3.4.0): Licensed under the **Zlib License**
- **Dear ImGui** (v1.91.9): Licensed under the **MIT License**
- **nativefiledialog-extended** (v1.2.1): Licensed under the **Zlib License**
- **Noto Sans Fonts**: Licensed under the **SIL Open Font License 1.1**
