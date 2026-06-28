# NaikAVPlayer

A native, multi-threaded C++ media engine and video player built for extreme performance, low footprint, and zero-latency seek responsiveness.

![NaikAVPlayer Screenshot](assets/screenshot.png)

Built on top of barebones **FFmpeg**, **SDL2**, and **Dear ImGui**, **NaikAVPlayer** implements container parsing, frame rescaling, and sub-frame clock synchronization directly on raw texture planes with no external wrapper overhead.

---

## Key Features

- 🏎️ **Symmetric Seeking:** Near-instantaneous forward and backward seeks without deadlocks or frames freezing.
- ⏱️ **Audio-Video Synchronization:** Sub-frame accurate clock sync maintaining frame drift under `10ms` relative to the audio device master timeline.
- 🎛️ **Software Volume Controls:** Software audio sample attenuator with dynamic byte scaling, including a zero-overhead mute/bypass layout.
- 📂 **Flexible Media Loading:** Supports drag-and-drop file ingestion directly onto the player window, or custom local file system parsing.
- 📊 **Diagnostics HUD:** Real-time HUD diagnostics overlay displaying player states, playback clock offsets, media metadata, and resolution metrics.
- 🎨 **Modern Glassmorphic GUI with Vector Icons:** Floating dock and cinematic header layouts with a frosted translucent obsidian design, circular progress grabs, interactive welcome onboarding cards, toggleable HUD sidebars, and **programmatically-rendered vector control icons** (Play, Pause, Stop, Seek, Volume, Browse) featuring neon cyan hover highlights and accessibility tooltips.
- 🔤 **Bundled Open-Source Typography:** Integrated with **Noto Sans** fonts (SIL Open Font License 1.1) scanned dynamically from relative paths, avoiding system-dependent proprietary lookups.
- 🖼️ **Window Branding:** Branded with a custom high-fidelity app icon loaded natively as the SDL2 window and taskbar icon.
- 🧪 **100% Logic Test Coverage:** A fully instrumented functional integration and white-box test suite executing 100% of the player's core playback controller, demuxer, and audio/video decoder logical lines.

---

## Tech Stack & Dependencies

- **C++17** (compiled with GCC/MinGW)
- **FFmpeg 8.x (avcodec, avformat, avutil, swscale, swresample)** (automatically downloaded LGPL-shared binaries)
- **SDL2** (automatically fetched and dynamically compiled)
- **Dear ImGui** (automatically fetched and statically compiled)
- **Noto Sans Font** (bundled open-source SIL OFL 1.1 font files)
- **App Icon Asset** (custom-designed PNG and BMP formats)

---

## Installation & Compilation

Ensure you have **MinGW-w64 (GCC)** and **CMake (version 3.18+)** configured on your path.

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
*Note: A post-build recipe automatically copies the required FFmpeg shared DLLs and the SDL2 DLL directly into the compilation target folder so you can run the binaries immediately.*

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
.\build\NaikAVPlayer_tests.exe "C:\Path\To\video.mp4"
```
Alternatively, set the `TEST_VIDEO_PATH` environment variable before running:
```powershell
$env:TEST_VIDEO_PATH="C:\Path\To\video.mp4"
```

Generate the code coverage statistics:
```powershell
cd build
gcov -o CMakeFiles/NaikAVPlayer_tests.dir/tests/tests.cpp.obj ../src/AudioDecoder.cpp
gcov -o CMakeFiles/NaikAVPlayer_tests.dir/tests/tests.cpp.obj ../src/VideoDecoder.cpp
gcov -o CMakeFiles/NaikAVPlayer_tests.dir/tests/tests.cpp.obj ../src/Demuxer.cpp
gcov -o CMakeFiles/NaikAVPlayer_tests.dir/tests/tests.cpp.obj ../src/PlayerController.cpp
```

---

## Open-Source Attribution & Credits

NaikAVPlayer is published under the **MIT License**. It links dynamically and statically to the following libraries and assets:
- **FFmpeg** (Licensed under LGPL v3.0, ensuring full legal compliance with the application's permissive MIT license)
- **SDL2** (Licensed under the Zlib License)
- **Dear ImGui** (Licensed under the MIT License)
- **Noto Sans Font** (Licensed under the SIL Open Font License 1.1)
