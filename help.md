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
  libavfilter-dev \
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

### Advanced Build Options

All are supported across configurations:

| Option | Default | Purpose |
|---|---|---|
| `-DENABLE_SANITIZERS=ON` | `OFF` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `-DENABLE_TSAN=ON` | `OFF` | ThreadSanitizer |
| `-DTREAT_WARNINGS_AS_ERRORS=OFF` | `ON` | Disable `-Werror` |
| `-DENABLE_COVERAGE=OFF` | `ON` | gcov/lcov coverage instrumentation |
| `-DNAIKAV_FORCE_BUNDLED_FFMPEG=OFF` | — | Use the system FFmpeg (via `pkg-config`) instead of the downloaded prebuilt archive; required when cross-compiling for ARM64 |
| `-DPLATFORM=LINUX\|WINDOWS` | autodetected | Target platform selection |

The `RelWithDebInfo` and `MinSizeRel` build types are also supported.

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

**Viewing console log output (Windows):**
```bash
./build/NaikAVPlayer.exe --console "/path/to/video.mp4"
```
The Windows build is a GUI-subsystem executable, so it has no console of its
own and its log output (file open, decode, DSP, playback-state messages) is
discarded by default. `--console` attaches the app to the terminal that
launched it and routes that output there. It does nothing when the app is
started by double-click (no parent console) and is ignored on Linux, where
the same output already goes to the terminal.

> [!NOTE]
> Console attachment is opt-in rather than automatic. A GUI-subsystem process
> that silently attaches to its parent console at startup matches a known
> console-hiding pattern used by malware, and generic antivirus heuristics
> score it accordingly — the same class of false positive that already moved
> the Windows release build from MinGW to MSVC. Requiring the flag keeps the
> diagnostics without the unprompted startup behavior.

---

## 4. UI Controls & Shortcuts

The user interface uses Dear ImGui with frosted translucency overlay.

### Media Selection
- **Drag-and-Drop**: Drag a single video or audio file onto the application window to open and play it immediately, replacing whatever playlist was loaded. Dropping *more than one* file at once appends them all to the playlist instead and plays the first — see [Playlist](#playlist) below.
- **Native File Dialog**: Click "Open Media File" or the folder icon in the controls dock to launch the platform-native file selector (automatically pauses background playback during file selection).

### Audio Track Selection & External Audio
- **Controls Dock Audio Button (`[Audio]`)**: Click the headphone icon button in the controls dock bar to open the audio tracks popup menu.
  - **Embedded Audio Streams**: Lists all audio streams present in the container with language tags, titles, codecs (e.g. AAC, Opus, FLAC, AC3, DTS, TrueHD), channel layouts (Stereo, 5.1, 7.1), and sample rates.
  - **External Audio Tracks**: Select and play loaded external audio tracks with auto-synced A/V clock pacing.
  - **Load External Audio File**: Native file picker to load standalone audio files (`.m4a`, `.aac`, `.ac3`, `.mp3`, `.wav`, `.flac`, `.ogg`, `.opus`, `.wma`, `.mka`).
  - **Remove External Audio**: Unloads external audio and automatically restores the default embedded track.
  - **Disable Audio (Mute Stream)**: Turns off audio decoding and routes silent frames.
- **Hotkey `B`**: Press `B` to cycle sequentially through available audio streams (Off -> Embedded -> External -> Off) with on-screen Toast feedback.

### Playback Speed Control
- **Controls Dock Speed Button (`[Speed]`)**: Click the dedicated Speedometer icon button next to Resolution in the dock bar to open the speed popup:
  - Continuous slider (`0.25x` to `2.0x`).
  - One-click preset buttons (`0.25x`, `0.5x`, `0.75x`, `1.0x (Normal)`, `1.25x`, `1.5x`, `1.75x`, `2.0x`) with active checkmarks.
  - Fine-tuning step buttons (`-0.1x`, `Reset 1.0x`, `+0.1x`).
- **Hotkeys**: Press `[` to decrease speed by 0.25x, `]` to increase speed by 0.25x, and `Backspace` to reset to normal (1.0x).
- **Real-Time Resampling**: Uses SDL3 dynamic audio stream frequency resampling (`SDL_SetAudioStreamFrequencyRatio`) synchronized with master audio/video clock pacing for pitch-preserving speed changes.

### Loop Playback Mode
- **Controls Dock Loop Button (`[Loop]`)**: Click the loop icon button in the transport group (`[<<] [Play/Pause] [Stop] [>>] [Loop]`) to toggle continuous playback. The button lights up with a cyan accent glow when active.
- **Hotkey `L`**: Press `L` to toggle continuous loop mode on/off with on-screen Toast feedback.

### Playlist
- **Controls Dock Playlist Button**: Click the list icon button next to Subtitles in the controls dock to open the Playlist panel.
  - **Add Files...**: Native multi-select file picker to append one or more media files.
  - **Add Folder...**: Native folder picker; appends every supported media file found directly in that folder (not subfolders), sorted alphabetically.
  - **Reorder**: Drag any row to a new position in the list.
  - **Remove**: Click a row's `x` button, or select a row and press `Delete`.
  - **Double-click** a row to jump straight to playing it.
  - **Repeat mode**: `Off` / `All` / `One`, and a **Shuffle** toggle -- both apply once playback naturally reaches end-of-file, independently of the transport `[Loop]` button (which always just repeats the current file and takes priority over the playlist while it's on).
- **Multi-File Drag-and-Drop**: Dropping more than one file onto the window adds them all to the playlist and starts playing the first one (dropping a single file behaves as before -- it just opens and plays, replacing the playlist with that one file).
- **Hotkey `P`**: Press `P` to toggle the Playlist panel.
- Playlist contents and repeat/shuffle state persist across restarts, the same way other player settings do.

### Keyboard Shortcuts & Gestures

| Key / Gesture | Action |
| :--- | :--- |
| **`Spacebar`** | Toggle Play / Pause |
| **`F11`** / **`Alt+Enter`** | Toggle Fullscreen Mode |
| **`Double-Click`** (on video) | Toggle Fullscreen Mode |
| **`M`** | Toggle Audio Mute / Unmute |
| **`Up Arrow`** / **`Down Arrow`** | Increase / Decrease Volume (±5%) |
| **`Mouse Wheel`** (over video) | Adjust Volume Up / Down |
| **`S`** | Capture Screenshot / Export Video Frame as PNG |
| **`B`** | Cycle Audio Tracks (Off -> Embedded -> External -> Off) |
| **`V`** | Cycle Subtitle Tracks (Off -> Embedded -> External -> Off) |
| **`G`** / **`H`** | Adjust Subtitle Synchronization Delay (-50ms / +50ms) |
| **`Left Arrow`** / **`Right Arrow`** | Seek backward / forward by 10 seconds |
| **`[`** / **`]`** | Decrease / Increase playback speed by 0.25x (0.25x – 2.0x) |
| **`Backspace`** | Reset playback speed to normal (1.0x) |
| **`L`** | Toggle Continuous Loop Mode |
| **`P`** | Toggle Playlist panel |
| **`D`** | Toggle Diagnostics HUD overlay |
| **`A`** | Toggle Audio Processing panel (EQ, noise gate, compressor, multiband compressor, limiter, crossover, loudness, 3D surround, widener, balance, channel/device/format selection) |
| **`Escape`** | Exit Fullscreen (if in fullscreen) or Exit application |

---

## 4a. Source Layout

Sources are grouped by subsystem under `src/`. Headers are included by that subsystem-relative path (e.g. `#include "audio/dsp/DspChain.hpp"`), which is why manual compiles and static analysis both need `-I src` / `-I src/`:

```text
src/
├── app/       main.cpp — entry point, SDL window/event loop, render loop, CLI flags
├── audio/     AudioDecoder.{hpp,cpp}, AudioTrack.hpp — decode, resample, SDL callback, track models, output selectors
│   └── dsp/   header-only DSP module (see Section 5b)
├── core/      ThreadSafeQueue.hpp, MetricRing.hpp, PipelineMetrics.hpp
├── media/     Demuxer.{hpp,cpp} — packet reading, multi-stream track enumeration and routing
├── player/    PlayerController.{hpp,cpp} — state machine, track switching, seeking, settings persistence
├── playlist/  header-only queue/repeat/shuffle/M3U8 module (see Section 5f)
├── subtitle/  SubtitleDecoder.{hpp,cpp}, SubtitleTrack.hpp — decoding, parsing, sync, sanitization
├── ui/        PlayerUI.{hpp,cpp} — ImGui controls dock, diagnostics HUD, audio panel, subtitle overlay
└── video/     VideoDecoder.{hpp,cpp} — HW/SW decode, frame conversion
```


`src/audio/dsp/` is entirely header-only: `AudioDspSettings.hpp` (settings struct, the 9 presets, and genre mapping), `Biquad.hpp`, `ParametricEQ.hpp`, `NoiseGate.hpp`, `Compressor.hpp`, `MultibandCompressor.hpp`, `Limiter.hpp`, `Crossover.hpp`, `DspChain.hpp`, `LoudnessMeter.hpp`, `LoudnessNormalizer.hpp`, `LoudnessPrescan.hpp`, `ReplayGainTags.hpp`, `SpatialDownmixer.hpp`, `Surround3D.hpp`, `StereoWidener.hpp`, `BalanceControl.hpp`, `SpectrumAnalyzer.hpp`, `DspMath.hpp`.

> [!NOTE]
> `DspMath.hpp` exists so this module never depends on `M_PI`, which is a POSIX extension rather than standard C++. libstdc++ exposes it from `<cmath>` by default and SDL3's `SDL_stdinc.h` defines it as a fallback, but MSVC only defines it when `_USE_MATH_DEFINES` is set *before* the first `<cmath>` include — an ordering constraint these headers cannot guarantee, since `AudioDecoder.hpp` includes them ahead of any SDL header. Use `naikav::dsp::kPi` in new DSP code, not `M_PI`.

---

## 5. Hardware Acceleration & Dynamic Fallback

- **Windows Decoders:** Tries `h264_d3d11va`, `h264_dxva2`, `h264_qsv`, `h264_cuvid`.
- **Linux Decoders:** Tries `h264_v4l2m2m` (V4L2 M2M), `h264_vaapi`, `h264_qsv`, `h264_cuvid`.
- **Dynamic Software Fallback:** If hardware decoder initialization fails or encounters runtime surface mapping errors, the decoder pipeline releases the hardware context, configures software `h264`, and resubmits pending packets seamlessly without crashing or dropping playback state.
- **Raspberry Pi 5 note:** The BCM2712 SoC has no hardware H.264 M2M decode block (only HEVC, via `rpi-hevc-dec`), so `h264_v4l2m2m` always fails to open for H.264 content on Pi 5 and the pipeline falls back to software `h264` decoding by design — see [Section 9](#9-troubleshooting).

---

## 5a. Pipeline Backpressure & Deadlock Prevention

The single demuxer thread reads both the video and audio packet streams via `av_read_frame`, so if a push into either queue ever blocked indefinitely, packet delivery to *both* streams would silently stop — for example, nothing drains the audio packet queue while the audio device is paused (a seek catch-up, or simply the user pausing playback). `ThreadSafeQueue` (`src/core/ThreadSafeQueue.hpp`) provides two bounded push variants used in place of a plain blocking `push()` wherever a producer thread cannot afford to stall:

- **`push_wait_or_drop(value, timeoutMs, dropCleanup)`**: waits up to `timeoutMs` for room, then drops the oldest queued entry (invoking `dropCleanup` on it, if given) and pushes anyway. This is the structural backstop for the demuxer's video/audio packet pushes and the video decode thread's push into the decoded frame queue (all with a 500ms timeout) — no matter what stalls the consumer (a paused device, a wedged hardware decoder, a stuck render loop), the producer thread always returns and keeps making progress.
- **`push_drop_oldest(value, dropCleanup)`**: never waits at all — drops the oldest entry immediately if full. Used for audio packets specifically while the audio consumer is known to be idle (paused, or mid seek catch-up), since waiting on a consumer that isn't running serves no purpose.

This also replaced the previous PTS-based `throttleCatchupReadahead()` mechanism that capped video read-ahead during seek catch-up: the queue's own bounded-wait-then-drop behavior now throttles read-ahead naturally, tracking actual decoder throughput instead of a fixed "1 second past target" heuristic. See [Section 9](#9-troubleshooting) for the hang symptom this fixes, and `tests/tests.cpp` (`T7b`, `T7c`, and the "rapid consecutive seek recovery" integration case) for the regression coverage.

---

## 5b. Audio DSP & Loudness Pipeline

Every decoded audio buffer runs through a fixed, in-place signal chain inside the SDL3 audio callback (`AudioDecoder::decodeAndResample()`, `src/audio/AudioDecoder.cpp`), entirely as interleaved `AV_SAMPLE_FMT_FLT` — never the S16 device format — until the very last step:

```text
decode -> resample (swresample, libsoxr engine, selectable quality) -> DSP chain
  (parametric EQ -> noise gate -> compressor -> multiband compressor
   -> lookahead limiter -> bass-management crossover)
  -> loudness normalization (EBU R128 real-time, two-pass-primed, or tag-primed)
  -> 3D surround -> stereo widener -> balance -> spectrum analyzer (read-only tap)
  -> final safety limiter -> TPDF dither -> device-format truncation
  (S16 / S32 / F32) -> device output
```

- **DSP chain** (`src/audio/dsp/`): hand-rolled `Biquad`/`ParametricEQ` (RBJ cookbook, 5 bands -- frequency, Q, and gain all independently adjustable per band, not just gain), `NoiseGate` (downward expander below threshold, the mirror of `Compressor`, with its own short detector-smoothing stage so a sustained tone's own zero-crossings don't chatter the gate open/closed), `Compressor` (soft-knee, linked-multichannel detection), `MultibandCompressor` (splits into low/mid/high bands via two Linkwitz-Riley crossovers, each band compressed independently by its own `Compressor`; the bands sum back to a flat *magnitude/energy* response at the default 1:1 ratios -- an allpass identity, not sample-for-sample time-domain identity, since there's a frequency-dependent phase rotation through each crossover point), `Limiter` (a short internal lookahead delay line plus fast attack, so the gain envelope reduces *ahead of* a fast transient instead of only clamping it after the fact, with a hard-ceiling backstop for the remaining rare case), `Crossover` (Linkwitz-Riley 4th-order: always-available LFE-channel lowpass, plus an optional bass-redirect mode that highpasses every other channel and sums exactly what was removed into the LFE channel -- true bass management, not just LFE tone control). Orchestrated by `DspChain`, disabled by default -- every stage is a true no-op (0 dB / 1:1 ratio / 0 dB ceiling) until configured, so wiring it into the pipeline didn't change existing playback behavior on its own.
- **Loudness normalization** (`src/audio/dsp/LoudnessMeter.hpp` / `LoudnessNormalizer.hpp` / `LoudnessPrescan.hpp` / `ReplayGainTags.hpp`): wraps a minimal `libavfilter` graph (`abuffer -> ebur128 -> abuffersink`) for real-time momentary/integrated LUFS, reading FFmpeg's own metadata output (`lavfi.r128.M`/`.I`) rather than a hand-rolled K-weighting/gating implementation -- loudness *measurement* needs to match the real ITU-R BS.1770 spec to mean anything, unlike the DSP effects above. By default, applies a heavily-smoothed (multi-second) real-time gain correction toward a configurable LUFS target. Two ways to prime it with a whole-file figure instead of that real-time ramp, both feeding `LoudnessNormalizer::primeWithPrescannedLufs()`, checked in this order: (1) `naikav::dsp::readTaggedLoudnessAsLufs()` reads a `R128_TRACK_GAIN`/`REPLAYGAIN_TRACK_GAIN` (or `_ALBUM_` fallback) container tag if present -- no decoding needed at all; (2) failing that, `naikav::dsp::prescanIntegratedLufs()` does a decode-only (no video, no device I/O) pass over the whole file. Either way, priming applies from the very first block instead of ramping in, and survives `reset()` (called on seek) instead of dropping back to zero gain.
- **Automatic genre-based presets** (`AudioDspSettings.hpp`'s `presetForGenreTag()`): opt-in (`autoGenrePresetEnabled`) -- on file open, `PlayerController::applyGenrePresetIfEnabled()` reads the container's genre tag and, via simple case-insensitive keyword matching (not a real genre taxonomy), applies a matching canned preset, preserving the toggle itself so it doesn't turn itself off after one use.
- **3D Surround, Stereo Widener & Balance** (`Surround3D.hpp` / `StereoWidener.hpp` / `BalanceControl.hpp`): run after loudness normalization, on the final output-channel-count buffer. `Surround3D` synthesizes spatial ambience on any stereo output (including plain stereo sources); `StereoWidener` does mid-side width adjustment; `BalanceControl` attenuates whichever channel the balance is pulled away from (the classic mixer "balance" behavior, not a mono-source "pan"). All three are 2-channel-only no-ops on other channel counts, all disabled/centered by default.
- **Virtual Surround downmix** (`SpatialDownmixer.hpp`): when `AudioChannelOption::VIRTUAL_SURROUND` is selected and the source is a directly-supported discrete surround layout (2.1/5.1/5.1(back)/7.1), folds it down to stereo with positional delay/filter cues instead of a flat downmix matrix, so the device still only ever receives 2 channels but retains a spatial impression of the original layout.
- **Spectrum analyzer** (`SpectrumAnalyzer.hpp`): opt-in (`spectrumAnalyzerEnabled`), reads the final post-DSP-chain signal (downmixed to mono), accumulating it into a ring buffer and running a hand-rolled iterative radix-2 Cooley-Tukey FFT (1024-point, Hann-windowed) every full non-overlapping block (~21ms at 48kHz). The resulting dB-scaled magnitude-per-bin spectrum is exponentially smoothed frame-to-frame and exposed via `getMagnitudesDb()`, a self-synchronized snapshot getter with its own internal mutex -- the UI thread reads it without needing `AudioDecoder`'s `m_dspMutex` at all. A pure display tap: it never modifies the signal, and (like every other stage here) is a true no-op while disabled.
- **Not implemented (deliberately scoped out)**: real HRTF/binaural rendering (the above are hand-rolled ambience/positional-cue synthesis, not a convolution against a measured head-related impulse response dataset -- no such dataset is vendored, and fabricating one would be dishonest) and true gapless playback/crossfade -- a real playlist/track-queue engine now exists (see [Section 5f](#5f-playlist-architecture)), but sample-accurate gapless transitions and crossfade need cross-instance `AudioDecoder` handoff and a dedicated crossfade DSP stage on top of it, a larger architectural addition than anything else in this section. Today's auto-advance opens and starts the next file the same way manually opening a new file does (brief re-init gap, no crossfade).
- **Resampler**: `swr_alloc_set_opts2`'s `"resampler"` AVOption is set to `"soxr"` (SoX Resampler) instead of swresample's own default engine, for better stopband rejection. This project's vendored FFmpeg build has libsoxr compiled in (confirmed via `--enable-libsoxr` in its `ffmpeg -version` configuration string); if a build ever lacks it, `av_opt_set()` fails gracefully and swresample's own (still correct, just lower-quality) resampler is used instead. `ResamplerQuality` (Low/Medium/High/Very High, panel dropdown, applied on next file open) maps to soxr's `"precision"` AVOption (16/20/28/33 bits); Medium (20 bits) matches this project's original, pre-selector default.
- **Resampler latency & zero-output blocks**: `swr_convert()` legally returns **0** while the resampler is still filling its internal buffer, and soxr's latency grows with the precision tier — so the higher the `ResamplerQuality`, the more often a decoded frame yields no output samples yet. This is a normal buffering state, *not* an error and *not* end of stream. `decodeAndResample()` treats it as "feed the resampler another frame" and loops. It must never fall through to setting `m_audioBufferSize = 0`, because `sdlAudioStreamCallback()` cannot distinguish that from a genuinely starved queue and would memset a full block of digital silence into an otherwise healthy stream — a hard discontinuity, audible as a click. See [Section 9](#9-troubleshooting) for the symptom this caused when it was mishandled.
- **Output format & device** (`AudioOutputBitDepth`, `AudioDecoder::setOutputDeviceName()`/`enumeratePlaybackDeviceNames()`): the internal pipeline is always float; `AudioOutputBitDepth` (S16 default / S32 / F32, panel dropdown) only controls the final device format. F32 skips truncation/dither entirely (lossless); S32 gets the same TPDF dither technique as S16, rescaled to a true 32-bit LSB. Device selection resolves a persisted device *name* (not ID, since `SDL_AudioDeviceID` values aren't stable across sessions) back to whatever ID currently matches it at `init()` time, falling back to the OS default if that device is no longer present.
- **Dither**: applied once, at the final float-to-device-format truncation in the SDL callback (skipped for F32 output) -- triangular (TPDF) dither, the sum of two independent uniform draws, decorrelating quantization error from the signal far better than plain rounding.
- **Thread safety**: `AudioDecoder::applyDspSettings()` lets the UI thread change any DSP/loudness parameter while the SDL audio callback thread concurrently calls `process()` on the same objects -- both sides take a single short-held mutex (`m_dspMutex`). `AudioDecoder::primeLoudnessPrescan()` (used internally by the two-pass/tag loudness paths above) takes the same lock. The older `dsp()`/`loudness()` raw accessors remain but are **not** synchronized; they're for tests and pre-playback setup only, not live control. Output format/device/resampler-quality/channel-option selectors are UI-thread-only settings applied before `init()`, not live-mutable during playback (matching `AudioChannelOption`'s existing rule).
- **Multichannel routing**: `AudioDecoder` preserves 2.1/5.1/5.1(back)/7.1 source layouts straight through to a matching multichannel `SDL_AudioSpec` when possible (see [Section 1](#1-linux-binary-compatibility-limitation--prerequisites) for the underlying audio backend prerequisites), with a stereo fallback if the device rejects it. A successful device open does not by itself prove real multichannel hardware is connected -- shared-mode audio APIs silently downmix -- so `AudioDecoder` also queries the device's native channel count via `SDL_GetAudioDeviceFormat` and surfaces both numbers in the Audio Processing panel. `AudioChannelOption::FORCE_STEREO`/`VIRTUAL_SURROUND` (panel dropdown, applied on next file open) let a user override automatic detection outright.
- **Config file**: all of the above persists in `player_settings.txt` (key=value lines: `dsp_enabled`, `eq_band0`-`eq_band4` plus per-band frequency/Q, `noise_gate_*`, `compressor_*`, `multiband_*`, `limiter_*`, `crossover_*` including bass redirect, `loudness_*`, `surround3d_*`, `widener_*`, `balance`, `auto_genre_preset_enabled`, `spectrum_analyzer_enabled`, `channel_option`, `output_bit_depth`, `output_device_name`, `resampler_quality`), with a tested fallback for the pre-existing legacy format (a single bare resolution integer).

---

## 5c. Audio-Only Playback Pipeline & Real-Time Visualizer

NaikAVPlayer provides a dedicated audio-only playback mode that activates automatically whenever a media file contains audio streams without a motion video stream (including MP3, AAC, FLAC, OGG, WAV):

- **Embedded Album Cover Art Handling (`AV_DISPOSITION_ATTACHED_PIC`)**: ID3 APIC / attached pictures are exposed by FFmpeg as video streams with the `AV_DISPOSITION_ATTACHED_PIC` or `AV_DISPOSITION_TIMED_THUMBNAILS` disposition. `Demuxer::open()` explicitly filters out attached picture streams when selecting `m_videoStreamIdx`, ensuring tracks with embedded artwork are accurately classified as audio-only media (`hasAudio() == true`, `hasVideo() == false`) without stalling the video decoder pipeline.
- **Timebase-Accurate Audio Clock**: `AudioDecoder::decodeAndResample()` converts decoded frame presentation timestamps directly from the stream timebase (`m_timeBase`, e.g., `1/14112000` for MP3) into seconds via `static_cast<double>(pts - startTime) * av_q2d(m_timeBase)`. This guarantees sample-accurate master clock progression at true 1.0x playback speed across all audio formats and sample rates.
- **Demuxer Backpressure for Audio Streams**: For audio-only media (`m_videoStreamIdx < 0`), the demuxer uses blocking queue push `m_audioQueue.push(packet)` during paused or initial open states rather than dropping packets, preserving the full track buffer.
- **Hardware-Accelerated Real-Time Visualizer**: When an audio-only file is loaded, `PlayerUI::drawAudioVisualizer()` automatically renders an interactive audio visualizer across the viewport using Dear ImGui `ImDrawList` primitives (<0.1% CPU overhead):
  - **Neon Equalizer Bars (Default)**: 64 logarithmic frequency bands covering ~20 Hz to 20 kHz, smoothed with fast attack and exponential decay, gravity-falling peak cap indicators, floor reflections, and bass-reactive central ambient glow.
  - **Smooth Waveform**: Continuous oscillating audio oscilloscope ribbon with multi-layer glow and gradient fill.
  - **Radial Audio Disc**: 360-degree circular visualizer with rotating vinyl disc aesthetics, pulsating center bass ring, and radiating frequency spikes.
  - **Mirrored Stereo Spectrum**: Symmetrical top-and-bottom frequency equalizer bars meeting at a glowing horizon line.
  - **Vibrant Color Palettes**: `CYBER` (Cyan/Magenta), `SUNSET` (Amber/Hot Pink), `MINT` (Mint/Emerald), and `VIOLET` (Aqua/Purple).
  - **Telemetry Snapshots**: Self-synchronized thread-safe getters `getSpectrumMagnitudesDb()` (512 FFT bins) and `getWaveformSamples()` (1024 time-domain samples) feed the visualizer without requiring audio callback locks.

---

## 5d. Subtitle Pipeline (Internal & External)

NaikAVPlayer provides end-to-end subtitle handling supporting both embedded container streams and external sidecar files:

- **Embedded Subtitle Stream Detection & Routing**: `Demuxer::open()` enumerates all `AVMEDIA_TYPE_SUBTITLE` streams in the media file (e.g. SubRip/SRT, ASS/SSA, WebVTT, MOV Text). When a track is selected, the demuxer routes packets to `m_subtitleQueue` (bounded 50-packet queue). During seek operations, `m_subtitleQueue` is cleared and packet timestamps from stale seek epochs are rejected using `m_seekGeneration`.
- **External Subtitle File Support**: Users can load external subtitle files (`.srt`, `.vtt`, `.ass`, `.ssa`, `.sub`) through the native file dialog or hotkeys. External subtitle files adjacent to media files with matching names are automatically probed and loaded on file open. External subtitles are parsed upfront into memory (`<2ms`), enabling instant random seeks without disk I/O.
- **ASS Tag Stripping & Text Sanitization**: `sanitizeSubtitleText()` filters ASS override tags (e.g., `{\pos(x,y)}`, `{\an8}`, `{\b1}`, `{\c&H00FFFF&}`), normalizes ASS dialogue field headers and hard/soft breaks (`\N`, `\n`, `\h`), and strips standard HTML formatting tags (`<i>`, `<b>`, `<font>`).
- **Timing Synchronization & Delay Offset**: `SubtitleDecoder` matches active subtitle events against the video playback presentation timestamp (`currentPts`). Users can fine-tune subtitle synchronization in real-time in 50ms increments via the Subtitle menu or keyboard hotkeys (`G` / `H`), with delay offsets displayed in the UI.
- **Overlay Presentation & Video Contrast**: Subtitles are rendered directly over the video canvas in `PlayerUI::drawSubtitleOverlay()`. The overlay dynamically accounts for letterboxing/pillarboxing bounding boxes, centers multi-line text near the bottom edge above the controls dock, and renders text against a translucent dark rounded backdrop box (`rgba(5, 5, 8, 0.78)`) with high-contrast white text for legibility on both bright and dark scenes.
- **UI & Control Options**:
  - **Controls Dock**: `[CC]` icon button with popup menu for selecting `Off`, embedded tracks by language/title/codec, external tracks, loading new external files, and adjusting timing delay.
  - **Diagnostics HUD (`D`)**: Displays active subtitle track name and live `Subtitle Packet Q` depth.
  - **Hotkeys**: `V` cycles through available subtitle tracks; `G` / `H` adjusts delay offset (-/+50ms).

---

## 5e. Multiple Audio Track Switching & External Audio Architecture

NaikAVPlayer provides full multi-track audio stream discovery, selection, and external audio demuxing:

- **Multi-Stream Audio Discovery**: `Demuxer::open()` traverses all streams in the media container, identifying every stream with `AVMEDIA_TYPE_AUDIO`. It populates `m_audioTracks` with detailed stream metadata, including FFmpeg stream index, ISO 639-1/2 language tags (e.g., `eng`, `jpn`, `hin`, `und`), track titles (e.g., `Main Audio`, `Commentary`, `Surround 5.1`), codec descriptors (AAC, Opus, FLAC, AC3, DTS, TrueHD, Vorbis), channel layouts (Mono, Stereo, 5.1, 7.1), and sample rates.
- **Dynamic Audio Stream Switching**: When a new audio track is selected via `PlayerController::selectAudioTrack(int trackId)` or the `B` hotkey:
  1. The controller switches the active stream in `Demuxer` via `selectAudioStream(streamIdx)`.
  2. The demuxer flushes the audio packet queue (`m_audioQueue.clear()`) and increments `m_seekGeneration` to drop stale packets from the previous stream.
  3. The `AudioDecoder` re-initializes its `AVCodecContext` with the new stream's codec parameters without stopping video playback or dropping video frames.
  4. The audio master clock resumes sample-accurate pacing from the current video presentation timestamp.
- **External Audio File Demuxing & Synchronization**:
  - Standalone audio files (`.m4a`, `.aac`, `.ac3`, `.mp3`, `.wav`, `.flac`, `.ogg`, `.opus`, `.wma`, `.mka`) can be loaded concurrently with video.
  - A dedicated background `m_externalAudioDemuxer` thread is spawned to demux packets from the external audio container into `m_audioQueue`.
  - On seek operations, `PlayerController::seek()` seeks both the primary video demuxer and the external audio demuxer in parallel, synchronizing PTS timebases accurately.
  - Users can switch back and forth between external audio and embedded tracks at any time without losing the loaded external file reference.
- **Stream Muting & Silence Injections**: When audio is disabled (`trackId == -1`), `AudioDecoder` generates silent PCM frames at the configured device format, keeping the master clock progression stable.

---

## 5f. Playlist Architecture

NaikAVPlayer builds and plays a queue of local media files, with repeat/shuffle modes and M3U8 persistence:

- **Header-only data model** (`src/playlist/`, no SDL/FFmpeg/ImGui dependency, main-thread only): `PlaylistItem.hpp` (`id`, `path`, `displayName`, `MediaKind` classification, `isValid`), `MediaFileFilter.hpp` (extension-based `classifyMediaKind()`/`isSupportedMediaFile()`, the single source of truth also used by the "Add Files" dialog filter and folder-scan filtering), `PlaylistIO.hpp` (M3U/M3U8 parse/write), and `Playlist.hpp` (the queue itself). Matches this codebase's existing convention for FFmpeg/SDL/ImGui-free logic (`src/audio/dsp/*.hpp`, `src/core/ThreadSafeQueue.hpp`) — no `.cpp` counterpart, so it's implicitly picked up by both the app target and the `tests.cpp` unity build with no `CMakeLists.txt` changes.
- **Identity-based current-item tracking**: each `PlaylistItem` gets a stable `uint64_t id` on insert; `Playlist` tracks the *current item's id*, not a raw index, deriving the display index by lookup (`indexOfId()`). This means `move()` (drag-reorder) and `removeAt()` on other rows never disturb which item is "current" — no index-arithmetic bookkeeping to get wrong.
- **`removeAt()` on the current item**: whatever slides into that slot after the erase (the item that was previously next, or the new last item if the removed row was last) becomes current — i.e. "advance to next remaining item" falls out of the erase itself rather than a separate step.
- **`next()` / `previous()`**: respect `RepeatMode` (`Off` stops at the list end; `All` wraps in both directions; `One` replays the current item without advancing) and, when shuffle is on, walk a Fisher-Yates-shuffled permutation of indices (regenerated on shuffle-toggle or any structural change) instead of display order. Both skip over `isValid == false` entries automatically, bounded to one full pass so an all-invalid list returns `std::nullopt` instead of looping forever.
- **`addFolder()`**: a non-recursive `std::filesystem::directory_iterator` scan (top-level files only, not subfolders), filtered through `MediaFileFilter::isSupportedMediaFile()` and appended in alphabetical order.
- **M3U/M3U8 I/O** (`PlaylistIO.hpp`): parses `#EXTM3U`/`#EXTINF:duration,title` directives; resolves relative entries against the playlist file's own directory; skips any `scheme://`-prefixed entry on import (no network-stream support in this player); keeps a resolved-but-missing local path in the list with `isValid = false` rather than silently dropping it, so a broken entry stays visible instead of vanishing.
- **`PlayerController` integration**:
  - Owns a `naikav::playlist::Playlist m_playlist` member.
  - `openFile(path, resetPlaylist = true)` — the added `resetPlaylist` parameter defaults to `true` so the CLI-argument open, a single drag-and-drop, and the "Open File" dialog all continue to work unchanged *and* now also collapse the playlist to that one file, keeping "what's playing" and "what's queued" from disagreeing. `playlistPlayIndex()` / `playlistNext()` / `playlistPrevious()` call `openFile(path, false)` internally so playlist-driven navigation doesn't clobber the list it's iterating; all three go through the existing `stop()` + `openFile()` + `play()` sequence, never touching `m_videoQueue`/`m_audioQueue`/etc. directly.
  - `pollPlaylistAutoAdvance()`, called once per frame from `main.cpp`'s event loop: forces this frame's `ENDED`-transition check (by calling `getCurrentTime()`, so it has no ordering dependency on whatever else calls that this frame), and if the state is `ENDED`, calls `Playlist::next()` and opens/plays the result. Returns immediately if `next()` yields nothing (`RepeatMode::Off` at the end of the list) — playback just stays at `ENDED`, exactly as before this feature existed. The existing per-file `[Loop]` toggle needed no changes at all: it already keeps playback from ever reaching `ENDED` (see [Section 4](#4-ui-controls--shortcuts)'s Loop Playback Mode and the `ENDED` state note in the main `README.md`), so the playlist's own repeat mode only ever matters once `[Loop]` is off.
- **Multi-file drag-and-drop batching**: previously, `SDL_EVENT_DROP_FILE` was handled inline per-event, so a multi-file OS drag (SDL3 delivers one event per file, all within the same poll drain) called `openFile()` once per file and only the *last* one survived — each call tears down and replaces the previous session. `main.cpp` now collects all paths dropped within one `SDL_PollEvent` drain into a `std::vector`, then after the loop: exactly one path keeps the original single-file behavior; more than one calls `Playlist::addMany()` followed by `playlistPlayIndex()` on the first newly-added item.
- **NFD dialogs**: `openNativeMultiFileDialog()` (`NFD_OpenDialogMultipleU8_With`, `NFD::UniquePathSet`/`NFD::UniquePathSetPathU8` for cleanup) and `openNativeFolderDialog()` (`NFD_PickFolderU8_With`) in `main.cpp`, following the same structure as the pre-existing single-file dialog helpers. Both are vendored, standard NFDe APIs (confirmed present in `build/_deps/nativefiledialog-extended-src`), not new dependencies.
- **UI panel** (`PlayerUI::drawPlaylistPanel()`): a persistent overlay window (same flag-gated idiom as the Diagnostics HUD / Audio Processing panel, toggled by the controls-dock `[Playlist]` button and the `P` hotkey — *not* a transient popup menu like the Audio Track/Subtitles buttons, since the toolbar plus reorderable list is too much content for a dropdown). Reordering uses Dear ImGui's native `BeginDragDropSource()`/`AcceptDragDropPayload()` on each row (already available in the bundled v1.91.9, no new dependency). `Delete` removes the selected row via `ImGui::IsKeyPressed(ImGuiKey_Delete)`, which reads raw key state directly rather than through ImGui's nav system, so it's unaffected by the `ImGuiWindowFlags_NoNav` note below.
- **Keyboard-nav capture fix**: both the Welcome HUD (`drawWelcomeHUD()`, the only window shown before any file is opened) and the Playlist panel now pass `ImGuiWindowFlags_NoNav`. Without it, Dear ImGui sets `io.NavActive` — and therefore `io.WantCaptureKeyboard` — true as soon as either window is focused (see `imgui.cpp`'s `UpdateKeyboardInputs()`/`NavUpdate()`), which is essentially always, since one of them is normally the only or topmost window. `main.cpp`'s entire hotkey switch is gated behind `!io.WantCaptureKeyboard`, so before this fix *every* hotkey (not just `P`) was silently swallowed whenever the Welcome HUD was showing (i.e. before a file was ever opened) or whenever the Playlist panel was open. `NoNav` only disables Tab-based keyboard navigation *within* those two windows; mouse interaction with every widget in them (buttons, combo, checkbox, row selection, drag-reorder) is unaffected.
- **Persistence**: `Playlist::saveM3U("playlist.m3u8")` plus three new `player_settings.txt` keys (`playlist_current_index`, `playlist_repeat_mode`, `playlist_shuffle`), following that file's existing tolerant `key=value` format (see [Section 5b](#5b-audio-dsp--loudness-pipeline)'s config-file bullet) — unknown/missing keys are silently ignored, so this is safe for settings files written by older builds. Saved immediately on every playlist mutation (add/remove/move/clear/repeat/shuffle/navigate), matching this codebase's existing "persist on every change" convention (e.g. `setAudioChannelOption()`). Loaded once at `PlayerController` construction, after the existing `loadSettings()` call; does **not** auto-play, matching the existing "no CLI arg → sit idle" startup behavior.

---

## 6. Security, Maintenance & Dependency Management


- **Upstream Dependencies**: Build dependencies are pinned in `CMakeLists.txt` by commit SHA, with the semantic version in a trailing comment — `SDL3` `release-3.4.0`, `imgui` `v1.91.9`, `nativefiledialog-extended` `v1.2.1` (all via `FetchContent`), and FFmpeg `n8.1.2` (prebuilt archive, verified by SHA-256).
- **Updating Dependencies**: Update the `GIT_TAG` commit SHAs or the FFmpeg archive filename/SHA-256 hashes inside `CMakeLists.txt`, keeping the trailing version comments in sync.
- **Packaging Compliance**: Release packages compiled by CI include a complete `licenses/` directory containing third-party licenses (`LICENSE.lgpl-3`, `LICENSE.sdl3`, `LICENSE.imgui`, `LICENSE.nfd`, `LICENSE.winpthread`, `FFMPEG_CREDITS.txt`), the project `LICENSE`, both `README.md` and `help.md`, the `assets/` directory, and the executable plus its runtime libraries. `FFMPEG_CREDITS.txt` also credits the bundled libsoxr resampler and the `avfilter`/`ebur128` component used for loudness metering (see [Section 5b](#5b-audio-dsp--loudness-pipeline)) -- this build uses FFmpeg's `--enable-version3` flag, so LGPL v3 applies to the FFmpeg binaries themselves (libsoxr keeps its own LGPL v2.1+ terms).
- **`LICENSE.winpthread` scope**: retained in `licenses/` for locally MinGW-built binaries. It does **not** apply to the published Windows release, which is MSVC-built against the static CRT (`/MT`) and links no `libwinpthread` — see [Section 7](#7-cicd-pipeline--package-verification).

---

## 7. CI/CD Pipeline & Package Verification

GitHub Actions ([ci.yml](.github/workflows/ci.yml)) runs two jobs:

**`test-and-analysis`** (Native Linux Testing & Analysis, `ubuntu-latest`):
- **Repository Integrity**: Fails the build if `README.md`, `help.md`, `LICENSE`, or a non-empty `licenses/` directory is missing.
- **Warning Enforcement**: Builds are compiled with `-Werror` (`-DTREAT_WARNINGS_AS_ERRORS=ON`).
- **Tests & Coverage**: Runs the `ctest` suite in Release, captures coverage with `lcov`, and uploads it to Codecov.
- **Static Analysis**: `cppcheck` over `src/` and `tests/` with `--error-exitcode=1` (passed `-I src/` so it resolves the subsystem-relative includes).
- **Sanitizers**: Separate ASan/UBSan and TSan configure/build/test passes in Debug.
- **Extra Configuration Check**: A `RelWithDebInfo` configure-and-build pass.
- **Reports**: Test execution, cppcheck, ASan and UBSan results are converted to XLS and uploaded as workflow artifacts.

**`build-packages`** (matrix):
- **Linux x86_64** on `ubuntu-latest`, and **Windows x86_64** on `windows-latest` using the **native MSVC toolchain** — not MinGW cross-compiled from Linux, since MinGW PE binaries trip generic antivirus heuristics while MSVC output trips far fewer and supports Control Flow Guard. The MSVC build uses the static CRT (`/MT`), so no VC++ redistributable and no MinGW `libwinpthread` are shipped.
- **Package Verification**: The `Verify Package Compliance` step asserts presence of the executable, `LICENSE`, `README.md`, `help.md`, and non-empty `licenses/` **and** `LICENSES/` directories.
- **Release Artifact Publishing**: Uploads the Windows package (`NaikAVPlayer-windows-x64`) only for branches starting with `release`. Linux release artifact uploads are currently suspended until a portable build strategy is implemented.

**Supply-chain pinning** (both jobs): every `uses:` reference is pinned to an immutable commit SHA (with the semantic version in a trailing comment), not a mutable `@v4`-style tag — a moving tag would silently pull new code into the build if that action's repository were ever compromised. This matches how `CMakeLists.txt` already pins SDL3/ImGui/NFD by commit SHA and verifies the FFmpeg archive by SHA-256. When updating an action, resolve the new tag to its commit (`gh api repos/OWNER/REPO/git/ref/tags/vN`, dereferencing to `object.sha` for annotated tags) and update the trailing version comment to match.

> [!NOTE]
> There is no MinGW cross-compilation job in CI. The MinGW cross-compile workflow in [Section 2](#2-compilation--local-cross-compilation-guide) is supported for local builds, but is not exercised by the pipeline.

---

## 8. Pipeline Instrumentation & Metrics Reference

The execution pipeline tracks 9 metrics using lock-free Single Producer Single Consumer (SPSC) metric rings, plus 3 always-on audio-underrun counters (M10-M12, below).

| Metric ID | Metric Name | Hook Site (File:Function) | Producing Thread | Type | Gating |
|---|---|---|---|---|---|
| **M1** | `video_packet_queue_depth` | `core/ThreadSafeQueue.hpp:push/pop/try_pop/clear/reset` | Demuxer & Video Decoder | std::atomic<int> (Gauge) | Always-On |
| **M2** | `audio_packet_queue_depth` | `core/ThreadSafeQueue.hpp:push/pop/try_pop/clear/reset` | Demuxer & Audio Decoder callback | std::atomic<int> (Gauge) | Always-On |
| **M3** | `decoded_frame_queue_depth` | `core/ThreadSafeQueue.hpp:push/pop/try_pop/clear/reset` | Video Decoder & Main Render | std::atomic<int> (Gauge) | Always-On |
| **M3b** | `subtitle_packet_queue_depth` | `core/ThreadSafeQueue.hpp:push/pop/try_pop/clear/reset` | Demuxer & Subtitle Decoder | std::atomic<int> (Gauge) | Always-On |
| **M4** | `demux_time_per_packet_us` | `media/Demuxer.cpp:threadLoop()` | Demuxer thread | MetricRing<256> (SPSC) | gated |
| **M5** | `decode_time_per_frame_us` | `video/VideoDecoder.cpp:decodeNextFrame()` | Video Decoder thread | MetricRing<256> (SPSC) | gated |
| **M6-A** | `convert_time_us` | `video/VideoDecoder.cpp:convertFrame()` | Video Decoder thread | MetricRing<256> (SPSC) | gated |
| **M6-B** | `upload_time_us` | `app/main.cpp:main()` | Main / Render thread | MetricRing<256> (SPSC) | gated |
| **M7** | `av_clock_offset_ms` | `app/main.cpp:main()` | Main / Render thread | MetricRing<256> (SPSC) | gated |
| **M8** | `frames_dropped_count` | `app/main.cpp:main()` | Main / Render thread | std::atomic<uint64_t> (Counter) | Always-On |
| **M9** | `seek_latency_ms` | `player/PlayerController.cpp:seek()` & `finishCatchup()` | Video Decoder & Main thread | MetricRing<256> (SPSC) | gated |
| **M10** | `audio_callback_count` | `audio/AudioDecoder.cpp:sdlAudioStreamCallback()` | SDL audio callback thread | std::atomic<uint64_t> (Counter) | Always-On |
| **M11** | `audio_silence_injections` | `audio/AudioDecoder.cpp:sdlAudioStreamCallback()` | SDL audio callback thread | std::atomic<uint64_t> (Counter) | Always-On |
| **M12** | `audio_silence_bytes` | `audio/AudioDecoder.cpp:sdlAudioStreamCallback()` | SDL audio callback thread | std::atomic<uint64_t> (Counter) | Always-On |

### Audio Underrun Counters (M10-M12)

`sdlAudioStreamCallback()` fills the remainder of its output block with digital silence whenever `decodeAndResample()` cannot produce samples. Every such event is a hard discontinuity in the output — an audible click — so **M11 / M10** (silence injections as a fraction of callbacks) is the direct, quantitative measure of playback glitching. In a healthy stream it is `0.00%`.

> [!IMPORTANT]
> `SDL_GetAudioStreamQueued()` is **not** usable as an audio-health signal in this architecture, and will mislead you. The callback puts exactly the number of bytes SDL asked for and SDL consumes them immediately, so that query reads ~0 at all times whether playback is perfect or glitching badly. Use M10-M12 instead.

Accessors: `AudioDecoder::getCallbackCount()` / `getSilenceInjectionCount()` / `getSilenceBytes()`, surfaced through `PlayerController::getAudioCallbackCount()` / `getAudioSilenceInjectionCount()` / `getAudioSilenceBytes()`. Relaxed-ordering atomics — monotonic counters read for reporting, never used to make playback decisions, so they cost nothing on the realtime path and are always on.

### Standalone Diagnostic Harnesses

Five self-contained programs under `tests/`, none of which are part of the `ctest` suite (only `NaikAVPlayer_tests` is registered via `add_test`). Two are CMake targets built alongside the app; three are built manually.

**Built by CMake** (produced by a normal `cmake --build`, but never run by `ctest`):

| Target | Source | Purpose |
|---|---|---|
| `NaikAVPlayer_dsp_repro` | `tests/dsp_repro_standalone.cpp` | Exercises the DSP chain through the **real** SDL audio device path, with none of the `SDL_OpenAudioDeviceStream` mocking `tests.cpp` uses. That mock falls back to a disconnected `SDL_CreateAudioStream()` with no callback bound whenever the real device open fails in a sandboxed runner, meaning `decodeAndResample()` — and therefore the whole DSP chain — never actually executes under a real audio callback thread in the normal suite. This program makes it run, matching what the GUI app does. |
| `NaikAVPlayer_colorinfo_race` | `tests/colorinfo_race_repro.cpp` | Stress-tests the `getColorInfo()` data race between the UI thread (which calls it every frame for the Diagnostics HUD) and the video decode thread, which concurrently mutates and frees the same `AVFrame`. A real render loop only calls it once per frame, so the race was hard to hit organically. |

**Built manually** (not wired into CMake; the smoke test needs a real audio device):

| Source | Purpose |
|---|---|
| `tests/audio_underrun_smoke.cpp` | Drives the real `PlayerController` → `AudioDecoder` → SDL device path against a real media file, samples every queue depth over time, and reports M10-M12 with per-exit-path attribution. This is the tool that isolates a crackling report to a specific cause. |
| `tests/audio_callback_bench.cpp` | Per-stage cost of the post-resample DSP path with every effect disabled, as a percentage of the realtime budget — i.e. what the callback still pays for unconditionally. |
| `tests/resampler_bench.cpp` | `swr_convert` cost per `ResamplerQuality` tier at both matched (48k→48k) and converting (44.1k→48k) rates. |

The manually-built ones compile with the compiler directly, since the DSP headers are header-only. Note the `-I src` — headers are included by subsystem-relative path:

```bash
g++ -O2 -std=gnu++17 -I src tests/audio_callback_bench.cpp -o build/bench
./build/bench
```

The smoke test additionally links the player sources and FFmpeg/SDL3 — see the comment block at the top of each file for its exact command line.

---

## 9. Troubleshooting

### `Could not initialize SDL3: No available audio device` (Linux)

SDL3 was compiled with only its `dummy`/`disk` audio drivers because none of `libasound2-dev` / `libpipewire-0.3-dev` / `libpulse-dev` were present at configure time — see [Section 1](#1-linux-binary-compatibility-limitation--prerequisites). CMake now fails configuration outright with instructions rather than producing a build that only breaks at runtime. Install the missing package(s) and **reconfigure** (`cmake -B build ...`); a plain `cmake --build` will not re-run `pkg-config` detection.

### `h264_v4l2m2m unavailable` / software decode fallback on Raspberry Pi 5

Expected, not a bug. The Pi 5's BCM2712 exposes only a hardware **HEVC/H.265** decode block (`rpi-hevc-dec`); unlike the Pi 4's BCM2711 it has **no hardware H.264 M2M decoder**, so `h264_v4l2m2m` cannot open and the pipeline correctly falls back to software `h264` (see [Section 5](#5-hardware-acceleration--dynamic-fallback)). The Cortex-A76 cores handle 1080p H.264 in software comfortably.

### Crackling / clicking audio during otherwise normal playback

Fixed. `swr_convert()` returning **0** — the normal "resampler is still filling its internal buffer" state — was handled only for the `< 0` (genuine error) case. A zero-sample result therefore fell through to `m_audioBufferSize = 0`, which `sdlAudioStreamCallback()` cannot distinguish from a starved queue, so it wrote a full block of digital silence into a perfectly healthy stream. Each block is a hard discontinuity: an audible click.

Because soxr's internal latency grows with its precision setting, the defect scaled directly with `ResamplerQuality` — worst at **Very High**, and only present at all when the source rate differs from the 48 kHz output (so 44.1 kHz content, i.e. most music, was hit hardest). Measured on a 44.1 kHz stereo source at Very High: **17.25% of all audio callbacks** emitted silence, with the audio packet queue sitting at 149/150 the entire time. After the fix: **0.00%**, verified across mono, stereo and 5.1 sources. See the resampler-latency bullet in [Section 5b](#5b-audio-dsp--loudness-pipeline).

If you hear crackling on a current build, run `tests/audio_underrun_smoke.cpp` (see [Section 8](#8-pipeline-instrumentation--metrics-reference)) against the offending file. A non-zero silence-injection percentage with per-path attribution tells you exactly which `decodeAndResample()` exit is responsible; a **0.00%** result means the callback never starved and the artifact is coming from somewhere other than underruns — most commonly gain staging (see below).

### Distorted, harsh, or pumping audio (not clicks)

Almost always gain staging rather than a pipeline defect. Overlapping parametric EQ bands stack: five bands at +6 dB each is roughly **+10 to +15 dB broadband** in the overlap regions, not +6 dB, which drives normal program material far past full scale and leaves both limiters in continuous heavy gain reduction. Loudness normalization can add up to a further +24 dB on top.

Press **Flat** in the Audio Processing panel, or delete `player_settings.txt` and relaunch. Note that the master **Enable Audio Processing** toggle alone is not sufficient: loudness normalization, 3D surround, stereo widener, balance and the spectrum analyzer are all deliberately *outside* that switch (it gates only EQ / noise gate / compressor / multiband / limiter / crossover), so each must be turned off on its own.

### Keyboard hotkeys (e.g. `P` for Playlist) don't respond before a file is opened, or while the Playlist panel is open

Fixed — see [Section 5f](#5f-playlist-architecture)'s keyboard-nav capture bullet. Dear ImGui sets `io.WantCaptureKeyboard` true whenever a *focused* window has keyboard-nav enabled (this app turns on `ImGuiConfigFlags_NavEnableKeyboard` globally), and `main.cpp`'s entire hotkey switch is gated behind `!io.WantCaptureKeyboard`. Before a file is opened, the Welcome HUD is the only window on screen and didn't opt out of nav, so it was always focused and silently blocked *every* hotkey, not just Playlist-related ones — the same thing happened whenever the Playlist panel itself was open and focused. Both windows now pass `ImGuiWindowFlags_NoNav`; only Tab-based keyboard navigation within them is affected, not mouse interaction or `main.cpp`'s global hotkeys.

### Playback freezes / system appears to hang after rapid seeking or seeking while paused

Fixed — see [Section 5a](#5a-pipeline-backpressure--deadlock-prevention). The demuxer thread, which reads both streams, used to block indefinitely on a plain `push()` into the audio packet queue whenever nothing was draining it (audio is muted during a seek catch-up and stays paused while playback is paused). Once blocked it stopped calling `av_read_frame` entirely, starving the video queues too. All queue pushes are now bounded-wait with a drop fallback, so the pipeline self-recovers.

### Audio-only / MP3 files skip rapidly or finish playback in ~4 seconds

Fixed — see [Section 5c](#5c-audio-only-playback-pipeline--real-time-visualizer). `AudioDecoder` previously computed decoded audio frame PTS by dividing `pts` by `sample_rate` instead of converting from stream `m_timeBase`. On MP3 files (where `time_base` is typically `1/14112000`), timestamps scaled ~320x too fast, advancing the master clock past the track duration within milliseconds. In addition, the demuxer's initial paused-state push was dropping packets. Both issues are resolved and verified by automated test suites.

### MP3 files with album artwork showing a blank screen instead of the visualizer

Fixed — see [Section 5c](#5c-audio-only-playback-pipeline--real-time-visualizer). Embedded cover art (ID3 APIC / attached pictures) is demuxed by FFmpeg as a video stream with `AV_DISPOSITION_ATTACHED_PIC`. `Demuxer::open()` now filters out attached picture streams from `m_videoStreamIdx`, ensuring tracks with cover art correctly identify as audio-only files (`hasAudio() == true`, `hasVideo() == false`) and automatically activate the real-time visualizer.

---

