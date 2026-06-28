# NaikAVPlayer

A native, multi-threaded C++ media engine and video player built for extreme performance, low memory footprint, and zero-latency seek responsiveness.

![NaikAVPlayer Screenshot](assets/screenshot.png)

Built on top of barebones **FFmpeg**, **SDL2**, and **Dear ImGui**, **NaikAVPlayer** implements container parsing, frame rescaling, and sub-frame clock synchronization directly on raw texture planes with no external wrapper overhead.

---

## Key Features

- 🏎️ **Symmetric Seeking:** Near-instantaneous forward and backward seeks without deadlocks or frames freezing.
- ⏱️ **Audio-Video Synchronization:** Sub-frame accurate clock sync maintaining frame drift under `10ms` relative to the audio device master timeline.
- 🎛️ **Software Volume Controls:** Software audio sample attenuator with dynamic byte scaling, including a zero-overhead mute/bypass layout.
- 📂 **Flexible Media Loading:** Supports drag-and-drop file ingestion directly onto the player window, or custom local file system parsing.
- 📊 **Diagnostics HUD:** Real-time HUD diagnostics overlay displaying player states, playback clock offsets, media metadata, resolution metrics, and a security note warning for decoder maintenance.
- 🎨 **Modern Glassmorphic GUI with Vector Icons:** Floating dock and cinematic header layouts with a frosted translucent obsidian design, circular progress grabs, interactive welcome onboarding cards, toggleable HUD sidebars, and **programmatically-rendered vector control icons** (Play, Pause, Stop, Seek, Volume, Browse) featuring neon cyan hover highlights and accessibility tooltips.
- 🔤 **Bundled Open-Source Typography:** Integrated with **Noto Sans** fonts (SIL Open Font License 1.1) scanned dynamically from relative paths, avoiding system-dependent proprietary lookups.
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

## Tech Stack & Dependencies

- **C++17** (compiled with GCC/MinGW)
- **FFmpeg 8.x (avcodec, avformat, avutil, swscale, swresample)** (automatically downloaded LGPL-shared binaries)
- **SDL2** (automatically fetched and dynamically compiled)
- **Dear ImGui** (automatically fetched and statically compiled)
- **Noto Sans Font** (bundled open-source SIL OFL 1.1 font files)
- **App Icon Asset** (custom-designed PNG and BMP formats)

---

## Supported Media Formats

Thanks to its barebones FFmpeg demuxer and decoder integration, **NaikAVPlayer** supports a wide range of media containers and codecs, including:

- **Video Containers:** `.mp4`, `.mkv`, `.avi`, `.mov`, `.webm`, `.flv`
- **Audio Containers:** `.mp3`, `.wav`, `.ogg`, `.flac`, `.aac`
- **Video Codecs:** H.264 (AVC), H.265 (HEVC), VP8, VP9, MPEG-4
- **Audio Codecs:** AAC, MP3, Vorbis, FLAC, PCM

---

## Installation & Compilation

Ensure you have **MinGW-w64 (GCC)** and **CMake (version 3.16+)** configured on your path.

The project features a **fully automated setup**. During the first build configuration, CMake will automatically download the correct pre-compiled FFmpeg shared binaries package from BtbN's stable release mirror, extract it, and place it in the `thirdparty/ffmpeg` folder—completely bypassing the need for manual configuration.

*Note: The build uses the **LGPL-shared** package of FFmpeg to ensure strict licensing compliance with the project's permissive MIT license.*

### Step 1: Configure the Project
Generate the build configurations (this downloads FFmpeg, SDL2, and Dear ImGui):
```bash
cmake -B build -G "MinGW Makefiles"
```

### Step 2: Compile the Project
Build the primary player binary and the test suite:
```bash
cmake --build build
```
*Note: A post-build recipe automatically copies the required FFmpeg shared DLLs, the SDL2 DLL, and the bundled assets directory directly into the compilation target folder so you can run the binaries immediately.*

---

## Usage Guide

### Running the Player
Launch the compiled player:
```powershell
.\build\NaikAVPlayer.exe
```
Or run it passing a media file path directly as an argument:
```powershell
.\build\NaikAVPlayer.exe "C:\Path\To\video.mp4"
```

### Keyboard Shortcuts
- **`Spacebar`**: Toggle Play / Pause.
- **`Left Arrow`** ($\leftarrow$): Seek backward by 10 seconds.
- **`Right Arrow`** ($\rightarrow$): Seek forward by 10 seconds.
- **`Escape`**: Close and exit the player.

---

## Verification & Tests

The test video was programmatically generated for testing purposes and contains only synthetic audio and graphics.

Run the coverage-instrumented functional test suite by passing a media file path directly:
```powershell
.\build\NaikAVPlayer_tests.exe ".\assets\hd_test_video_with_audio.mp4"
```
Alternatively, set the `TEST_VIDEO_PATH` environment variable before running:
```powershell
$env:TEST_VIDEO_PATH=".\assets\hd_test_video_with_audio.mp4"
.\build\NaikAVPlayer_tests.exe
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

---

## Open-Source Attribution & Credits

NaikAVPlayer is published under the **MIT License**. It links dynamically and statically to the following libraries and assets:
- **FFmpeg** (Licensed under LGPL v3.0, ensuring full legal compliance with the application's permissive MIT license)
- **SDL2** (Licensed under the Zlib License)
- **Dear ImGui** (Licensed under the MIT License)
- **Noto Sans Font** (Licensed under the SIL Open Font License 1.1)
