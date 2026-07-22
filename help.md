# NaikAVPlayer Build, Usage & Architecture Guide

This document describes how to configure, compile, install, run, profile, and troubleshoot NaikAVPlayer across supported platforms.

---

## 1. Prerequisites & Dependencies

NaikAVPlayer requires a C++17 compliant compiler and CMake 3.16+.

### Windows (Native MinGW-w64)
- **CMake (version 3.16+)**: Build system generator.
- **MinGW-w64 GCC / Clang**: Native Windows compiler toolchain.
- **FFmpeg**: Bundled prebuilt shared libraries (v8.1 / n8.1.2) automatically downloaded into `thirdparty/ffmpeg/` during CMake configure step.
- **SDL3, Dear ImGui, nativefiledialog-extended (NFD)**: Automatically downloaded and compiled from source via CMake `FetchContent`.

### Linux (x86_64 / aarch64 / Raspberry Pi)
- **GCC / Clang**: C++17 compiler.
- **FFmpeg Development Packages**: `libavcodec-dev`, `libavformat-dev`, `libavutil-dev`, `libswscale-dev`, `libswresample-dev` (required when using system FFmpeg or on ARM64 Linux for V4L2 M2M hardware decoding).
- **GTK3 Development Headers**: `libgtk-3-dev` (required for native file dialog integration).
- **SDL3, Dear ImGui, nativefiledialog-extended (NFD)**: Automatically retrieved and built from source via CMake `FetchContent`.

---

## 2. Compilation Guide

NaikAVPlayer officially supports two build workflows:
1. **Development Build (`Debug`)**: Intended for active daily development, debugging with GDB/LLDB, and running unit tests.
2. **Release Build (`Release`)**: Intended for production packaging, distribution, and official releases.

---

### Workflow 1: Development Build (Recommended for Developers)

```bash
# 1. Configure for local development (Debug mode)
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug

# 2. Build all development targets (executable & test suite)
cmake --build build-debug -j$(nproc)

# 3. Run test suite
ctest --test-dir build-debug --output-on-failure
```

*Windows (MinGW):*
```powershell
cmake -B build-debug -G "MinGW Makefiles" -DPLATFORM=WINDOWS -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

---

### Workflow 2: Release Build (Production & Packaging)

```bash
# 1. Configure for release
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 2. Compile release binaries
cmake --build build -j$(nproc)
```

*Windows (MinGW):*
```powershell
cmake -B build -G "MinGW Makefiles" -DPLATFORM=WINDOWS -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

---

### Platform Configurations & Advanced Options

- **Raspberry Pi (System FFmpeg)**:
  ```bash
  cmake -B build -DPLATFORM=LINUX -DNAIKAV_FORCE_BUNDLED_FFMPEG=OFF
  cmake --build build -j$(nproc)
  ```

- **Cross-Compile Windows on Linux (MinGW-w64)**:
  ```bash
  cmake -B build-windows \
    -DPLATFORM=WINDOWS \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
  cmake --build build-windows -j$(nproc)
  ```

> **Advanced Options**: CMake options like `-DENABLE_SANITIZERS=ON` (ASan/UBSan), `-DENABLE_TSAN=ON` (ThreadSanitizer), `RelWithDebInfo`, and `MinSizeRel` remain supported for specialized static analysis or profiling.

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
- **Package Verification**: Automated `Verify Package Compliance` step asserts presence of executable, dynamic libraries, `LICENSE`, `README.md`, and non-empty `licenses/` / `LICENSES/` directories before uploading release artifacts.

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
