# NaikAVPlayer Build, Usage & Architecture Guide

This document describes how to configure, compile, install, run, profile, and troubleshoot NaikAVPlayer across supported platforms.

---

## 1. Linux Binary Compatibility Limitation & Prerequisites

> [!IMPORTANT]
> **Linux Binary Compatibility Limitation:**
> Linux executables built in the CI environment (on modern Ubuntu runners) are **not uploaded to official GitHub Release artifacts**. 
> 
> Because CI binaries depend on modern C runtime symbol versions (`glibc` requirements such as `GLIBC_2.34`+), C++ standard library runtimes (`libstdc++.so.6`), and dynamic system libraries (e.g., GTK3), binaries compiled in CI may fail to run on older Linux distributions (such as Ubuntu 20.04/22.04 LTS or older Debian/RHEL systems).
> 
> **Linux users should build NaikAVPlayer from source directly on their target operating system.** This ensures the resulting binary matches the target environment's local library versions and GLIBC ABI.
> 
> **Future Portable Builds Roadmap:**
> Linux release artifact uploads will be resumed once a portable, universally compatible packaging workflow (such as an older baseline toolchain container build, AppImage bundle, or Flatpak package) is established.

### Host Platform Prerequisites & Toolchain Setup

NaikAVPlayer requires a C++17 compliant compiler and CMake 3.16+.

#### Linux (Ubuntu / Debian, including Raspberry Pi OS)
```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  pkg-config \
  libavcodec-dev \
  libavformat-dev \
  libavutil-dev \
  libswscale-dev \
  libswresample-dev \
  libgtk-3-dev \
  libxss-dev \
  libasound2-dev \
  libpipewire-0.3-dev \
  ccache
```

> [!IMPORTANT]
> `libasound2-dev` and `libpipewire-0.3-dev` (or `libpulse-dev` as an alternative to either) are **required**, not optional. SDL3 detects audio backends via `pkg-config` at CMake configure time; without at least one of these, it silently builds with only its `dummy`/`disk` drivers, and NaikAVPlayer fails at launch with `Could not initialize SDL3: No available audio device`. This only affects Linux — Windows audio (WASAPI) ships in the OS SDK, so there's no equivalent package to miss. See [Section 9: Troubleshooting](#9-troubleshooting) for the configure-time check that now guards against this.

#### Linux (Fedora / RHEL)
```bash
sudo dnf install -y \
  gcc-c++ \
  cmake \
  pkg-config \
  ffmpeg-free-devel \
  gtk3-devel \
  libXScrnSaver-devel \
  alsa-lib-devel \
  pipewire-devel \
  ccache
```

#### Linux (Arch Linux)
```bash
sudo pacman -S --needed \
  base-devel \
  cmake \
  pkgconf \
  ffmpeg \
  gtk3 \
  alsa-lib \
  libpipewire \
  ccache
```

#### Windows (Native MinGW-w64 / MSVC)
- **CMake (version 3.16+)**: Build generator.
- **MinGW-w64 GCC / Clang / MSVC**: C++17 compiler toolchain.
- **FFmpeg**: Prebuilt shared libraries are automatically downloaded into `thirdparty/ffmpeg/` during CMake configure.
- **SDL3, Dear ImGui, nativefiledialog-extended (NFD)**: Automatically fetched and built from source via CMake `FetchContent`.

---

## 2. Compilation & Local Cross-Compilation Guide

NaikAVPlayer supports native builds on your local system as well as local cross-compilation across architectures.

---

### Workflow 1: Development Build (`Debug`)

Intended for active daily development, debugging with GDB/LLDB, and running unit tests:

```bash
# 1. Configure for local development (Debug mode)
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug

# 2. Build executable & unit test runner
cmake --build build-debug -j$(nproc)

# 3. Run unit tests
ctest --test-dir build-debug --output-on-failure
```

*Windows (MinGW):*
```powershell
cmake -B build-debug -G "MinGW Makefiles" -DPLATFORM=WINDOWS -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

---

### Workflow 2: Production Release Build (`Release`)

Generates optimized release binaries (`-O3`) configured specifically for your host system:

```bash
# 1. Configure for production release
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPLATFORM=LINUX

# 2. Compile release binary
cmake --build build -j$(nproc)
```

*Windows (MSVC Native):*
```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64 -DPLATFORM=WINDOWS
cmake --build build --config Release
```

*Windows (MinGW):*
```powershell
cmake -B build -G "MinGW Makefiles" -DPLATFORM=WINDOWS -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

---

### Local Cross-Compilation Guides

#### A. Cross-Compiling for ARM64 / Raspberry Pi (`aarch64-linux-gnu`)

To build binaries for ARM64 target platforms (e.g., Raspberry Pi 4/5) from an x86_64 Linux host machine:

1. Install cross-compiler on host:
   ```bash
   sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
   ```
2. Configure and cross-compile:
   ```bash
   cmake -B build-arm64 \
     -DCMAKE_SYSTEM_NAME=Linux \
     -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
     -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
     -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
     -DPLATFORM=LINUX \
     -DNAIKAV_FORCE_BUNDLED_FFMPEG=OFF
   cmake --build build-arm64 -j$(nproc)
   ```

#### B. Cross-Compiling for Windows on Linux (MinGW-w64)

To build Windows 64-bit executables from a Linux development machine:

1. Install MinGW cross-toolchain:
   ```bash
   sudo apt-get install -y mingw-w64
   ```
2. Configure and cross-compile:
   ```bash
   cmake -B build-windows \
     -DPLATFORM=WINDOWS \
     -DCMAKE_SYSTEM_NAME=Windows \
     -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
     -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
   cmake --build build-windows -j$(nproc)
   ```

> **Advanced Build Options**: Specialized build options such as `-DENABLE_SANITIZERS=ON` (ASan/UBSan), `-DENABLE_TSAN=ON` (ThreadSanitizer), `RelWithDebInfo`, and `MinSizeRel` remain supported across all configurations.

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

## 3. Running the Player

**Windows (PowerShell):**
```powershell
.\build\NaikAVPlayer.exe
```

**Linux:**
```bash
./build/NaikAVPlayer
```

**Opening a media file via CLI:**
```bash
./build/NaikAVPlayer "/path/to/video.mp4"
```

**Running with Telemetry Profiling Overlay:**
```bash
./build/NaikAVPlayer --metrics "/path/to/video.mp4"
```

---

## 4. UI Controls & Shortcuts

The user interface uses Dear ImGui with frosted translucency overlay.

### Media Selection
- **Drag-and-Drop**: Drag any video or audio file onto the application window to open and play immediately.
- **Native File Dialog**: Click "Open Media File" or the folder icon to launch the platform-native file selector.

### Keyboard Shortcuts

| Key | Action |
| :--- | :--- |
| **`Spacebar`** | Toggle Play / Pause |
| **`Left Arrow`** | Seek backward 10 seconds |
| **`Right Arrow`** | Seek forward 10 seconds |
| **`L`** | Toggle Loop Mode |
| **`D`** | Toggle Diagnostics HUD overlay |
| **`Escape`** | Exit application |

---

## 5. Hardware Acceleration & Dynamic Fallback

- **Windows Decoders:** Tries `h264_d3d11va`, `h264_dxva2`, `h264_qsv`, `h264_cuvid`.
- **Linux Decoders:** Tries `h264_v4l2m2m` (V4L2 M2M), `h264_vaapi`, `h264_qsv`, `h264_cuvid`.
- **Dynamic Software Fallback:** If hardware decoder initialization fails or encounters runtime surface mapping errors, the decoder pipeline releases the hardware context, configures software `h264`, and resubmits pending packets seamlessly without crashing or dropping playback state.
- **Raspberry Pi 5 note:** The BCM2712 SoC has no hardware H.264 M2M decode block (only HEVC, via `rpi-hevc-dec`), so `h264_v4l2m2m` always fails to open for H.264 content on Pi 5 and the pipeline falls back to software `h264` decoding by design — see [Section 9](#9-troubleshooting).

---

## 5a. Pipeline Backpressure & Deadlock Prevention

The single demuxer thread reads both the video and audio packet streams via `av_read_frame`, so if a push into either queue ever blocked indefinitely, packet delivery to *both* streams would silently stop — for example, nothing drains the audio packet queue while the audio device is paused (a seek catch-up, or simply the user pausing playback). `ThreadSafeQueue` (`src/ThreadSafeQueue.hpp`) provides two bounded push variants used in place of a plain blocking `push()` wherever a producer thread cannot afford to stall:

- **`push_wait_or_drop(value, timeoutMs, dropCleanup)`**: waits up to `timeoutMs` for room, then drops the oldest queued entry (invoking `dropCleanup` on it, if given) and pushes anyway. This is the structural backstop for the demuxer's video/audio packet pushes and the video decode thread's push into the decoded frame queue (all with a 500ms timeout) — no matter what stalls the consumer (a paused device, a wedged hardware decoder, a stuck render loop), the producer thread always returns and keeps making progress.
- **`push_drop_oldest(value, dropCleanup)`**: never waits at all — drops the oldest entry immediately if full. Used for audio packets specifically while the audio consumer is known to be idle (paused, or mid seek catch-up), since waiting on a consumer that isn't running serves no purpose.

This also replaced the previous PTS-based `throttleCatchupReadahead()` mechanism that capped video read-ahead during seek catch-up: the queue's own bounded-wait-then-drop behavior now throttles read-ahead naturally, tracking actual decoder throughput instead of a fixed "1 second past target" heuristic. See [Section 9](#9-troubleshooting) for the hang symptom this fixes, and `tests/tests.cpp` (`T7b`, `T7c`, and the "rapid consecutive seek recovery" integration case) for the regression coverage.

---

## 6. Security, Maintenance & Dependency Management

- **Upstream Dependencies**: Build dependencies are pinned in `CMakeLists.txt` (`SDL3` `release-3.4.0`, `imgui` `v1.91.9`, `nativefiledialog-extended` `v1.2.1`, FFmpeg `n8.1.2`).
- **Updating Dependencies**: Update tag entries or archive SHA-256 hashes inside `CMakeLists.txt`.
- **Packaging Compliance**: Release packages compiled by CI include a complete `licenses/` directory containing third-party licenses (`LICENSE.lgpl-2.1`, `LICENSE.sdl3`, `LICENSE.imgui`, `LICENSE.nfd`, `LICENSE.winpthread`, `FFMPEG_CREDITS.txt`), project `LICENSE`, `README.md`, and executable binaries.

---

## 7. CI/CD Pipeline & Package Verification

GitHub Actions workflows ([ci.yml](.github/workflows/ci.yml)) perform:
- **Warning Enforcement**: Builds are compiled with `-Werror` (`-DTREAT_WARNINGS_AS_ERRORS=ON`).
- **Sanitizers**: ASan, UBSan, and TSan automated test runs.
- **Cross-Compilation**: MinGW cross-compilation testing.
- **Package Verification**: Automated `Verify Package Compliance` step asserts presence of executable, dynamic libraries, `LICENSE`, `README.md`, and non-empty `licenses/` / `LICENSES/` directories.
- **Release Artifact Publishing**: Publishes Windows release packages (`NaikAVPlayer-windows-x64`). Linux release artifact uploads are currently suspended until a portable build strategy is implemented.

---

## 8. Pipeline Instrumentation & Metrics Reference

The execution pipeline tracks 9 metrics using lock-free Single Producer Single Consumer (SPSC) metric rings.

| Metric ID | Metric Name | Hook Site (File:Function) | Producing Thread | Type | Gating |
|---|---|---|---|---|---|
| **M1** | `video_packet_queue_depth` | `ThreadSafeQueue.hpp:push/pop/try_pop/clear/reset` | Demuxer & Video Decoder | std::atomic<int> (Gauge) | Always-On |
| **M2** | `audio_packet_queue_depth` | `ThreadSafeQueue.hpp:push/pop/try_pop/clear/reset` | Demuxer & Audio Decoder callback | std::atomic<int> (Gauge) | Always-On |
| **M3** | `decoded_frame_queue_depth` | `ThreadSafeQueue.hpp:push/pop/try_pop/clear/reset` | Video Decoder & Main Render | std::atomic<int> (Gauge) | Always-On |
| **M4** | `demux_time_per_packet_us` | `Demuxer.cpp:threadLoop()` | Demuxer thread | MetricRing<256> (SPSC) | gated |
| **M5** | `decode_time_per_frame_us` | `VideoDecoder.cpp:decodeNextFrame()` | Video Decoder thread | MetricRing<256> (SPSC) | gated |
| **M6-A** | `convert_time_us` | `VideoDecoder.cpp:convertFrame()` | Video Decoder thread | MetricRing<256> (SPSC) | gated |
| **M6-B** | `upload_time_us` | `main.cpp:main()` | Main / Render thread | MetricRing<256> (SPSC) | gated |
| **M7** | `av_clock_offset_ms` | `main.cpp:main()` | Main / Render thread | MetricRing<256> (SPSC) | gated |
| **M8** | `frames_dropped_count` | `main.cpp:main()` | Main / Render thread | std::atomic<uint64_t> (Counter) | Always-On |
| **M9** | `seek_latency_ms` | `PlayerController.cpp:seek()` & `finishCatchup()` | Video Decoder & Main thread | MetricRing<256> (SPSC) | gated |

---

