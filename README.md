# NaikAVPlayer

[![CI/CD Pipeline](https://github.com/sanjeev-naik/NaikAVPlayer/actions/workflows/ci.yml/badge.svg)](https://github.com/sanjeev-naik/NaikAVPlayer/actions/workflows/ci.yml)
[![Coverage Status](https://codecov.io/gh/sanjeev-naik/NaikAVPlayer/graph/badge.svg)](https://codecov.io/gh/sanjeev-naik/NaikAVPlayer)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

NaikAVPlayer is a native, multi-threaded C++17 media engine and video player built using raw FFmpeg APIs, SDL3, and Dear ImGui. It performs container parsing, hardware/software video decoding, sample-accurate audio resampling, and clock synchronization directly using GPU-mapped texture updates without intermediate heavy frameworks. It achieves low-latency seeking and sub-10ms audio-video clock synchronization using dedicated worker threads coordinated through bounded blocking queues and a lock-free Single Producer Single Consumer (SPSC) ring for hot-path telemetry.

![NaikAVPlayer Screenshot](assets/screenshot.png)

---

## Key Features

- **Symmetric Low-Latency Seeking:** Rapid keyframe seek operations flushing packet queues and decoding pipelines under 80ms.
- **Stall-Proof Pipeline Backpressure:** Producer threads never block indefinitely on a full queue — bounded-wait pushes fall back to dropping the oldest queued item once a timeout elapses, so a paused audio device, a stalled render loop, or a wedged decoder can never freeze the single demuxer thread that feeds both the video and audio queues.
- **Dynamic Hardware Decoder Fallback:** Tries platform-specific hardware decoders (D3D11VA, DXVA2, QSV, CUVID on Windows; V4L2M2M, VAAPI, QSV, CUVID on Linux), falling back dynamically to software H.264 decoding if hardware context allocation fails or encounters runtime surface mapping errors.
- **Sub-10ms Audio-Video Synchronization:** Reconstructs the audio clock sample-accurately from PCM sample offsets to maintain A/V drift under 10ms.
- **Multichannel Audio Preservation:** Reads the source stream's real channel layout and drives 2.1/5.1/5.1(back)/7.1 straight through to a matching SDL3 multichannel device instead of always downmixing to stereo, with a device-native-channel check that flags when the OS is silently downmixing anyway, and a manual Auto/Force-Stereo override.
- **Studio DSP Chain:** Live-adjustable parametric 5-band EQ, soft-knee compressor, fast-attack limiter, and Linkwitz-Riley LFE bass crossover, running on a float-internal signal path — every stage is a true no-op until explicitly configured.
- **EBU R128 Loudness Normalization:** Real-time integrated/momentary LUFS metering with a smoothed gain correction toward a configurable target, keeping perceived loudness consistent across a mixed playlist.
- **High-Quality Resampling & Dither:** libsoxr resampling engine (in place of swresample's default) plus triangular (TPDF) dither applied only at the final 16-bit truncation.
- **DSP Presets & Persistent Settings:** One-click Music / Movie / Night / Flat presets for the whole audio chain, all settings (resolution, DSP, loudness, channel selection) surviving restarts via `player_settings.txt`.
- **Dynamic Resolution Scaling:** Real-time playback scaling supporting dynamic output resolution selection (Original source, 360p, 480p, 720p, 1080p, 1440p, 4K) from the UI dropdown to optimize GPU upload bandwidth.
- **Software Volume Attenuation:** Scalable audio output level adjustments with memcpy/memset bypasses for 100% and 0% volume states.
- **Loop Playback:** Wraparound seek to 0.0 upon reaching end-of-file for continuous playback.
- **Native File Picker:** Cross-platform native file picker integration using `nativefiledialog-extended` (NFD) on Win32 and GTK3/Portal backends.
- **Pipeline Diagnostics & System Info HUD:** Real-time overlay (`--metrics` or `D` key) displaying active player states, media telemetry (native vs. playback resolution, pixel format, hardware vs. software decoder type), Color & HDR pipeline characteristics (Color Space, Primaries, TRC, Range, Chroma Subsampling, Bit Depth, HDR10/HDR10+/Dolby Vision/HLG standard), pipeline queue depth levels, decode/render frame pacing budgets, and rolling clock synchronization offsets.
- **Audio Processing Panel:** Dedicated overlay (`A` key) for the EQ/compressor/limiter/crossover/loudness controls and channel selection, separate from the diagnostics HUD.
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
  │Decode -> Convert -> Queue │  │ Resample(soxr)->DSP Chain │
  │  (GPU-Mapped YUV Planes)  │  │  ->Loudness->Dither->S16  │
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

- **Demuxer Thread**: Reads raw packets via `av_read_frame` and routes them into bounded `ThreadSafeQueue<AVPacket*>` instances (video capacity: 100 packets, audio capacity: 150 packets). Pushes never block indefinitely — see [Stall-Proof Queue Backpressure](#stall-proof-queue-backpressure-deadlock-prevention) below.
- **Video Decoder Thread**: Background worker thread that pops packets from the video queue, decodes them (via hardware or software fallback), converts frames, and pushes them into the bounded `m_decodedFrameQueue` (capacity: 8 frames), using the same bounded-wait backpressure.
- **Audio Decoding**: Executed sample-accurately inside the SDL3 Audio Stream callback thread. It pulls packets from the audio queue, decodes them, resamples to the output layout/rate as interleaved float (`swr_convert`, libsoxr engine), runs the DSP chain and loudness normalization in place, then dithers and truncates to the device's 16-bit format — see [Audio DSP & Loudness Pipeline](#audio-dsp--loudness-pipeline) below.
- **Main / Render Thread**: Dequeues decoded frames from `m_decodedFrameQueue` whose PTS matches the master clock time, updates the SDL YUV texture on the GPU, and renders the Dear ImGui interface overlay.

#### GPU-Mapped Planar YUV Uploads
Instead of performing CPU-side YUV-to-RGB color space conversion, the video decoder pipeline extracts raw YUV 4:2:0 planar frame data directly. The main thread maps this data onto a hardware-accelerated SDL3 streaming texture (`SDL_PIXELFORMAT_IYUV`) using `SDL_UpdateYUVTexture`. This uploads plane segments directly to GPU texture memory, allowing graphics hardware to handle color space conversion and scaling efficiently.

#### Dynamic Hardware Decoder Fallback
At initialization, the video decoder queries native hardware codecs (`h264_d3d11va`, `h264_dxva2`, `h264_qsv`, `h264_cuvid` on Windows; `h264_vaapi`, `h264_v4l2m2m` on Linux). If hardware initialization fails or encounters runtime frame mapping errors (e.g. running inside headless or virtualized environments), the system intercepts the error, releases the hardware context, configures software `h264`, and resubmits pending packets seamlessly.

#### Stall-Proof Queue Backpressure (Deadlock Prevention)
The demuxer thread is the single reader for both the video and audio packet queues, so a producer that blocks indefinitely on either one would silently stop delivery to both — for example, the audio queue has no active consumer while the audio device is paused (during a seek catch-up, or simply while the user has playback paused). `ThreadSafeQueue` addresses this with two non-blocking-forever push variants used throughout the pipeline instead of a plain blocking `push()`:
- **`push_wait_or_drop(value, timeout)`**: waits briefly for room, then drops the oldest queued item and pushes anyway once the timeout elapses. Used by the demuxer for both packet queues and by the video decoder thread for the decoded frame queue, so a paused consumer, a stalled render loop, or a wedged hardware decoder can never block a producer thread forever.
- **`push_drop_oldest(value)`**: never waits — drops the oldest item immediately if the queue is full. Used for audio packets while the audio device is known to be idle (paused, or mid seek catch-up), since there's no point waiting on a consumer that isn't running at all.

Both variants keep the pipeline making forward progress under any stall condition, and self-recover without needing an external nudge (such as another seek) to unblock a wedged queue.

### Audio-Master Clock Synchronization

Playback uses **audio as the master clock** whenever an audio stream is present:

```
audio_clock = base_pts_of_current_frame + (bytes_already_consumed_by_SDL / bytes_per_second)
```

`AudioDecoder::getAudioClock()` combines the PTS of the current frame with the progress of the SDL audio stream callback into that frame's buffer, delivering sub-frame timing resolution to maintain `<10ms` drift. When no audio stream is present, the engine falls back to a wall-clock `m_videoClock` driven by `std::chrono::steady_clock` deltas.

### Multichannel Output & Channel Selection

`AudioDecoder` reads the source stream's real `AVCodecContext::ch_layout` and, for layouts it can drive directly (2.1, 5.1, 5.1(back), 7.1), requests a matching multichannel `SDL_AudioSpec` instead of always downmixing to stereo. If the audio device rejects the surround request, it falls back to a stereo downmix automatically rather than failing playback.

> [!NOTE]
> A successful device open does **not** by itself prove real multichannel speakers are connected — Windows/Linux audio APIs accept a high channel-count request in shared mode and silently downmix it to whatever the physical device actually supports. NaikAVPlayer also queries the device's *native* channel count (`SDL_GetAudioDeviceFormat`) and surfaces both numbers side by side in the Audio Processing panel (`A` key), flagging when a layout labeled "5.1" is actually being downmixed by the OS. Users who know their real output is stereo can force it explicitly via the panel's Output Channels selector (`Auto` / `Force Stereo`); the choice is persisted across sessions and takes effect on the next file opened (changing channel count requires reopening the audio device, so it isn't hot-swapped mid-playback).

### Audio DSP & Loudness Pipeline

Every decoded audio buffer passes through a fixed, in-place signal chain inside the SDL3 audio callback, entirely on interleaved float samples (not the final 16-bit output format) so each stage has full headroom and precision before the very last truncation step:

```text
Decode (FFmpeg)
     │
     ▼
Resample to output rate/layout (swresample, libsoxr engine)
     │
     ▼
DSP Chain  ── Parametric EQ (5-band biquad)
           ──► Compressor (soft-knee, linked multichannel)
           ──► Limiter (fast-attack + hard-ceiling backstop)
           ──► LFE Bass Crossover (Linkwitz-Riley 4th-order)
     │
     ▼
Loudness Normalization (EBU R128, smoothed gain toward
a configurable LUFS target)
     │
     ▼
TPDF Dither → 16-bit truncation → SDL3 device output
```

- **Float-internal, dither only at the end:** `swr_convert` outputs `AV_SAMPLE_FMT_FLT`, not `S16` — every DSP stage runs at full float precision, with quantization noise (triangular/TPDF dither, ±1 LSB) introduced exactly once, at the final truncation to the device's 16-bit format.
- **Zero cost when disabled:** default settings (0 dB EQ, 1:1 compression ratio, 0 dB limiter ceiling, loudness off) make each stage a true no-op — enabling the chain doesn't change the sound, or add measurable overhead, until something is actually configured.
- **Live, thread-safe control:** `AudioDecoder::applyDspSettings()` lets the UI thread update every parameter (EQ bands, compressor, limiter, crossover, loudness target) while the SDL audio callback thread concurrently processes audio — both sides are guarded by one short-held mutex, so switching presets never blocks or glitches playback.
- **EBU R128 via FFmpeg, not a hand-rolled meter:** loudness measurement uses FFmpeg's own `ebur128` `libavfilter` filter (a minimal `abuffer → ebur128 → abuffersink` graph, reading momentary/integrated LUFS back via frame metadata) rather than a custom K-weighting/gating implementation — spec-compliance for loudness numbers matters in a way DSP *effects* don't.
- **Presets & persistence:** Music / Movie / Night / Flat presets (Audio Processing panel, `A` key) apply a canned combination of every parameter in one step; all settings persist to `player_settings.txt` across sessions.

### State Machine Transitions
* **`UNINITIALIZED`**: Initial state. Loading media starts background demuxing and transitions to `OPENED`.
* **`OPENED`**: Media metadata loaded; decoders initialized; initial frame rendered. Triggering `play()` transitions to `PLAYING`.
* **`PLAYING`**: Audio output unpaused; main loop syncs video frames to the master clock.
* **`PAUSED`**: Audio device paused; current clock frozen.
* **`ENDED`**: Demuxer hits EOF and packet queues drain. If **Loop Mode** is enabled, reaching EOF directly invokes `seek(0.0)` to restart playback continuously.
* **`ERROR_STATE`**: Entered on demuxing/decoder failure, releasing resources safely.

---

## Linux Binary Compatibility Limitation & Source Build Strategy

> [!IMPORTANT]
> **Linux Release Binary Limitation:**
> The pre-built Linux x86_64 release binaries generated by the CI environment (built on modern Ubuntu runners) are **not currently published** to GitHub Release artifacts. 
> 
> Due to differences in C runtime library symbol versions (specifically `glibc` requirements such as `GLIBC_2.34`+), C++ standard libraries (`libstdc++.so.6`), GTK3 UI dependencies, and graphics stack interfaces, binaries built on modern CI environments may fail to execute on older Linux distributions (e.g., Ubuntu 20.04/22.04 LTS, older Debian releases, or enterprise Linux distros).
> 
> **Action Required for Linux Users:**
> Linux users should currently **build NaikAVPlayer from source** on their target operating system to guarantee 100% ABI and library compatibility.
> 
> **Future Improvement Roadmap:**
> Linux release binary artifacts will be re-enabled once a reproducible, universally portable packaging build system is implemented (for example, using an older baseline toolchain container, AppImage packaging, or Flatpak binaries).

---

## Build & Local Cross-Compilation Guide

NaikAVPlayer supports building natively from source across host operating systems as well as cross-compiling for target architectures.

### 1. Supported Host Platforms & Toolchain Prerequisites

| Host OS | Compiler Requirement | Build System | Package Manager Setup |
| :--- | :--- | :--- | :--- |
| **Ubuntu / Debian** | GCC 9+ / Clang 10+ | CMake 3.16+ | `apt-get` |
| **Fedora / RHEL** | GCC 9+ | CMake 3.16+ | `dnf` |
| **Arch Linux** | GCC 13+ / Clang 16+ | CMake 3.16+ | `pacman` |
| **Windows** | MSVC 2019+ or MinGW-w64 GCC | CMake 3.16+ | Visual Studio / MSYS2 |
| **macOS** | Apple Clang 12+ / GCC | CMake 3.16+ | Homebrew |

#### Dependency Installation

**Ubuntu / Debian (including Raspberry Pi OS):**
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
  libavfilter-dev \
  libgtk-3-dev \
  libxss-dev \
  libasound2-dev \
  libpipewire-0.3-dev \
  ccache
```
*(SDL3 v3.4.0, Dear ImGui v1.91.9, and nativefiledialog-extended v1.2.1 are automatically fetched and compiled from source via CMake FetchContent).*

> [!IMPORTANT]
> **`libasound2-dev` / `libpipewire-0.3-dev` are required, not optional.** SDL3 detects available audio backends via `pkg-config` at CMake configure time. If neither package (nor `libpulse-dev`) is installed, SDL3 silently builds with only its `dummy`/`disk` audio drivers — the build succeeds, but the app fails at launch with `Could not initialize SDL3: No available audio device`. This is a Linux-only failure mode: Windows audio (WASAPI) ships inside the OS SDK, so there is no equivalent dev package to forget. CMake now checks for this at configure time (see [Troubleshooting](#troubleshooting) below) and fails fast with instructions instead of producing a silently broken build.

**Fedora / RHEL:**
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

**Arch Linux:**
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

**Windows (Native MinGW-w64):**
Ensure CMake 3.16+ and MinGW-w64 GCC are present in `PATH`. CMake automatically downloads prebuilt FFmpeg shared binaries during configuration.

---

### 2. Native Build Workflows

#### Workflow 1: Development Build (`Debug`)
Use for active daily development, GDB/LLDB debugging, and running unit test suites:

```bash
# 1. Configure for local development (Debug)
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug

# 2. Build executable & test suite
cmake --build build-debug -j$(nproc)

# 3. Execute unit tests
ctest --test-dir build-debug --output-on-failure
```

*For Windows (MinGW):*
```powershell
cmake -B build-debug -G "MinGW Makefiles" -DPLATFORM=WINDOWS -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

#### Workflow 2: Production Build (`Release`)
Generates optimized binaries (`-O3`) matched specifically to your local Linux system's `glibc` and library environment:

```bash
# 1. Configure production release
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPLATFORM=LINUX

# 2. Compile release executable
cmake --build build -j$(nproc)
```

*For Windows (MSVC Native):*
```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64 -DPLATFORM=WINDOWS
cmake --build build --config Release
```

*For Windows (MinGW):*
```powershell
cmake -B build -G "MinGW Makefiles" -DPLATFORM=WINDOWS -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

---

### 3. Local Cross-Compilation Guides

NaikAVPlayer can be cross-compiled locally from a single Linux host machine for target hardware architectures or other operating systems.

#### A. Cross-Compiling for ARM64 / Raspberry Pi (`aarch64-linux-gnu`)

To build binaries for ARM64 targets (e.g. Raspberry Pi 4/5 or ARM64 single-board computers) from an x86_64 Linux host:

1. **Install Cross-Toolchain (Host):**
   ```bash
   sudo apt-get install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
   ```
2. **Configure & Cross-Compile:**
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

#### B. Cross-Compiling Windows Binaries on Linux (MinGW-w64)

To build native Windows 64-bit `.exe` binaries on a Linux workstation:

1. **Install MinGW Toolchain (Host):**
   ```bash
   sudo apt-get install -y mingw-w64
   ```
2. **Configure & Cross-Compile:**
   ```bash
   cmake -B build-windows \
     -DPLATFORM=WINDOWS \
     -DCMAKE_SYSTEM_NAME=Windows \
     -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
     -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
   
   cmake --build build-windows -j$(nproc)
   ```

> **Advanced Build Flags**: Additional CMake options such as `-DENABLE_SANITIZERS=ON` (ASan/UBSan), `-DENABLE_TSAN=ON` (ThreadSanitizer), `RelWithDebInfo`, and `MinSizeRel` are fully supported for analysis across build configurations.

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
| **`A`** | Toggle Audio Processing Panel (EQ, Compressor, Limiter, Loudness, Channel Selection) |
| **`Escape`** | Exit Application |

---

## Troubleshooting

### `Could not initialize SDL3: No available audio device` (Linux)

This means SDL3 was compiled with only its `dummy`/`disk` audio drivers, because `libasound2-dev` / `libpipewire-0.3-dev` (or `libpulse-dev`) weren't present when the project was configured — see the [Dependency Installation](#dependency-installation) note above. CMake now catches this itself: configuring on Linux without any of those `pkg-config` modules available fails immediately with instructions, instead of producing a build that only breaks at runtime. If you hit this on an existing build tree, install the missing package(s) and reconfigure (`cmake -B build ...` again) so SDL3's own dependency detection re-runs; a plain `cmake --build` does not re-check `pkg-config`.

### `h264_v4l2m2m unavailable` / falls back to software decode on Raspberry Pi 5

This is expected on Raspberry Pi 5 and is not a bug. The Pi 5's SoC (BCM2712) only exposes a hardware **HEVC/H.265** decode block (`rpi-hevc-dec`, visible as `/dev/video19`); unlike the Pi 4 (BCM2711), it has **no hardware H.264 M2M decoder**. FFmpeg's `h264_v4l2m2m` decoder therefore can't find a matching V4L2 device, and `NaikAVPlayer`'s [Dynamic Hardware Decoder Fallback](#dynamic-hardware-decoder-fallback) correctly drops to software `h264` decoding, as logged. The Pi 5's Cortex-A76 cores decode 1080p H.264 in software without issue; only very high bitrate/resolution H.264 content may need to be transcoded to HEVC to make use of hardware decode.

### Playback freezes / whole system appears to hang after rapid seeking or seeking while paused

Fixed. This was caused by the demuxer thread — the single thread that reads both the video and audio streams — blocking indefinitely on a plain `push()` into the audio packet queue whenever nothing was draining it (audio is muted during a seek catch-up, and stays paused for as long as playback is paused). Once blocked there, it stopped calling `av_read_frame` entirely, so the video packet queue and decoded frame queue also drained to empty and playback appeared to hang. The only thing that used to "fix" it was issuing another seek, which happened to force a queue `clear()` that woke the blocked push. See [Stall-Proof Queue Backpressure](#stall-proof-queue-backpressure-deadlock-prevention) above for the fix — the pipeline now self-recovers from rapid seek storms and paused-seek sequences without any further nudge.

---

## Release Packaging & Compliance

Release packages generated and published by the CI/CD pipeline (`NaikAVPlayer-windows-x64`) are validated for full open-source redistribution compliance.

*(Note: Linux release binary artifact uploads are currently suspended until a portable AppImage/containerized build process is available. Windows release executables continue to be published for every release).*

Every published release package archive includes:
- **Executable**: `NaikAVPlayer.exe` (or local native `NaikAVPlayer`)
- **Dynamic Libraries**: Bundled shared libraries (`.dll` or `lib/*.so*`)
- **Licenses Directory**: Complete `licenses/` and `LICENSES/` folder containing project and third-party license text files:
  - Project `LICENSE` (MIT License)
  - `LICENSE.lgpl-3` & `FFMPEG_CREDITS.txt` (FFmpeg LGPL v3 -- this build uses `--enable-version3`; also credits the bundled libsoxr resampler and the `avfilter`/`ebur128` loudness metering it ships)
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
- **FFmpeg** (n8.1.2): Licensed under **LGPL v3** (this vendored build was compiled with `--enable-version3`; see [`licenses/FFMPEG_CREDITS.txt`](licenses/FFMPEG_CREDITS.txt) and [`licenses/LICENSE.lgpl-3`](licenses/LICENSE.lgpl-3))
- **libsoxr** (SoX Resampler, bundled inside the FFmpeg build above): Licensed under **LGPL v2.1+** — used as swresample's resampling engine; see [`licenses/FFMPEG_CREDITS.txt`](licenses/FFMPEG_CREDITS.txt) for details. Not shipped as a separate DLL, and licensed independently of the LGPL v3 term above (it wasn't compiled with any GPLv3-requiring option).
- **SDL3** (v3.4.0): Licensed under the **Zlib License**
- **Dear ImGui** (v1.91.9): Licensed under the **MIT License**
- **nativefiledialog-extended** (v1.2.1): Licensed under the **Zlib License**
- **Noto Sans Fonts**: Licensed under the **SIL Open Font License 1.1**
