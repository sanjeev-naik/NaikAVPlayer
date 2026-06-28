# NaikAVPlayer Build & Usage Guide

This guide describes how to build, compile, and run the native C++ video player on Windows using open-source dependencies.

---

## 1. Prerequisites & Dependencies

The project is configured to build on Windows with zero setup for the major dependencies:
- **MinGW-w64 (GCC)**: High-performance C++ compiler.
- **CMake (version 3.16+)**: Build generation suite.
- **FFmpeg (Shared release)**: Automatically configured from the local `thirdparty/ffmpeg` directory.
- **SDL2**: Fetched and compiled from source automatically during configure.
- **Dear ImGui**: Fetched and compiled from source automatically during configure.

---

## 2. Compilation Instructions

To compile the video player from scratch, open your terminal (PowerShell or Command Prompt) in the project root directory and run:

### Step A: Configure CMake Build Target
Generate the build configurations (this downloads SDL2 and Dear ImGui repositories):
```bash
cmake -B build -G "MinGW Makefiles"
```

### Step B: Compile Project
Build the executable `NaikAVPlayer.exe`:
```bash
cmake --build build
```
*Note: A post-build script automatically copies all necessary DLLs (like `SDL2.dll` and FFmpeg shared `.dll`s) from `thirdparty/ffmpeg/bin` directly to the `build/` directory so you can run it immediately.*

---

## 3. Running the Player

Launch the compiled executable:
```powershell
.\build\NaikAVPlayer.exe
```

---

## 4. UI Features & Controls

The player is designed with a premium dark glassmorphism interface.

### How to Open Media Files:
1. **Drag-and-Drop**: Drag any video (`.mp4`, `.mkv`, `.avi`, `.webm`, etc.) or audio (`.mp3`, `.wav`) file directly onto the player window. It will open and start playing instantly!
2. **Controls Modal**: Click the **Folder (Browse)** icon button at the bottom-left of the controls bar to browse or input the file path.

### GUI Controls:
- **Timeline Slider**: Left-click or drag on the bottom progress bar to jump to any segment.
- **Volume Slider & Toggle**: Adjust the software volume attenuator from 0% to 100%. Click the **Volume/Mute Icon** button next to the volume slider to toggle mute instantly (using a zero-overhead CPU bypass).

### Keyboard Hotkeys:
- **Spacebar**: Toggle Play / Pause.
- **Left Arrow key ($\leftarrow$)**: Seek backward by 10 seconds.
- **Right Arrow key ($\rightarrow$)**: Seek forward by 10 seconds.
- **Escape**: Immediately close and exit the player.

---

## 5. Security & Dependency Maintenance

- **Upstream Security**: NaikAVPlayer relies on external decoders (FFmpeg) to parse and process media streams. Because parsing media files carries inherent security risks (e.g. malformed files attempting to exploit decoder bugs), keeping your shared libraries updated is the best defense.
- **Updating Libraries**: The project is designed to automatically download the latest stable dependency packages during configuration. To force an update of the local FFmpeg binaries, delete the `thirdparty/ffmpeg` directory and compile the project again:
  ```bash
  cmake -B build -G "MinGW Makefiles"
  cmake --build build
  ```
