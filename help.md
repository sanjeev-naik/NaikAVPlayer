# NaikAVPlayer Build & Usage Guide

This guide describes how to build, compile, install, run, and uninstall the native C++ video player on Windows and Linux.

---

## 1. Prerequisites & Dependencies

The project is natively cross-platform:
- **CMake (version 3.16+)**: Cross-platform build generation suite.
- **SDL3, Dear ImGui & nativefiledialog-extended**: Fetched and compiled from source automatically during configure.

### Windows
- **MinGW-w64 (GCC)**.
- **FFmpeg (Shared release)**: Automatically downloaded and configured in the local `thirdparty/ffmpeg` directory.

### Linux
- **GCC**.
- **FFmpeg development packages**: Must be installed via the system package manager (e.g. `sudo apt-get install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev`).
- **GTK3 development package**: Must be installed via the system package manager (`sudo apt-get install libgtk-3-dev`) to compile the native file dialog backend.

---

## 2. Compilation Instructions

The build system supports a custom `PLATFORM` configuration variable:
- **`AUTO`** (default): Automatically detects the host operating system.
- **`WINDOWS`**: Explicitly configures the project to build for Windows.
- **`LINUX`**: Explicitly configures the project to build for Linux.

### Step A: Configure CMake Build Target

Open your terminal in the project root directory and run:

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

**Cross-Compile for Windows on Linux:**
If you are on a Linux machine (e.g. Ubuntu) and want to cross-compile Windows binaries:
```bash
# Install the cross-compiler
sudo apt-get install -y mingw-w64

# Configure CMake for Windows target
cmake -B build-windows \
  -DPLATFORM=WINDOWS \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++

# Build binaries
cmake --build build-windows
```

### Step B: Compile Project
Build the binaries:
```bash
cmake --build build
```
*Note: On Windows, a post-build script automatically copies all necessary DLLs (like `SDL3.dll` and FFmpeg shared `.dll`s) from `thirdparty/ffmpeg/bin` directly to the `build/` directory so you can run it immediately.*

### Step C: Install Application (Linux)
To install the binaries, assets, desktop entry, and icon onto the system:
```bash
sudo cmake --install build
```
This registers the application so it can be launched from the applications menu and stores font assets in standard shared directories.

### Step D: Uninstall Application (Linux)
To remove all installed files and clean desktop launcher references:
```bash
sudo cmake --build build --target uninstall
```

---

## 3. Running the Player

Launch the compiled executable or installed application:

**Windows (PowerShell):**
```powershell
.\build\NaikAVPlayer.exe
```

**Linux (Local Build):**
```bash
./build/NaikAVPlayer
```

**Linux (System-Wide Installed):**
You can launch the app from your desktop launcher or run:
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

---

## 4. UI Features & Controls

The player is designed with a premium dark glassmorphism interface.

### How to Open Media Files:
1. **Drag-and-Drop**: Drag any video (`.mp4`, `.mkv`, `.avi`, `.webm`, etc.) or audio (`.mp3`, `.wav`) file directly onto the player window. It will open and start playing instantly!
2. **Native File Dialog**: Click the **"Open Media File"** button on the welcome screen or the **Folder (Browse)** icon button at the bottom-left of the controls bar to open the native OS file picker (Win32 File Explorer on Windows, GTK3/Portal on Linux).

### GUI Controls:
- **Timeline Slider**: Left-click or drag on the bottom progress bar to jump to any segment.
- **Volume Slider & Toggle**: Adjust the software volume attenuator from 0% to 100%. Click the **Volume/Mute Icon** button next to the volume slider to toggle mute instantly (using a zero-overhead CPU bypass).
- **Loop Toggle**: Click the **Loop Icon** button (next to Seek Forward) to enable or disable continuous playback. When enabled, the media automatically seeks back to the beginning and keeps playing once it reaches the end, instead of stopping.

### Keyboard Hotkeys:
- **Spacebar**: Toggle Play / Pause.
- **Left Arrow key ($\leftarrow$)**: Seek backward by 10 seconds.
- **Right Arrow key ($\rightarrow$)**: Seek forward by 10 seconds.
- **L**: Toggle Loop Mode on/off.
- **Escape**: Immediately close and exit the player.

---

## 5. Hardware Decoding & Dynamic Fallback

NaikAVPlayer natively supports hardware-accelerated H.264 decoding to deliver maximum frame rates and low CPU utilization:
- **Supported Decoders:** The player queries and attempts to initialize OS-specific hardware codecs (`h264_d3d11va`, `h264_dxva2`, `h264_qsv`, or `h264_cuvid` on Windows; `h264_vaapi` or `h264_v4l2m2m` on Linux).
- **Dynamic Fallback:** If a hardware decoder fails during initialization or encounters a fatal decoding or surface mapping error at runtime (e.g. running on driverless or virtualized headless environments), the player dynamically and seamlessly switches to the software `h264` decoder. This guarantees compatibility across all environments without manual configuration.

---

## 6. Security & Dependency Maintenance

- **Upstream Security**: NaikAVPlayer relies on external decoders (FFmpeg) to parse and process media streams. Because parsing media files carries inherent security risks (e.g. malformed files attempting to exploit decoder bugs), keeping your shared libraries updated is the best defense.
- **Updating Libraries**: The project is designed to automatically download the latest stable dependency packages during configuration. To force an update of the local FFmpeg binaries, delete the `thirdparty/ffmpeg` directory and compile the project again:
  ```bash
  cmake -B build -G "MinGW Makefiles"
  cmake --build build
  ```

---

## 7. GitHub Actions CI/CD & Caching

The project includes an automated GitHub Actions pipeline (configured in [.github/workflows/ci.yml](.github/workflows/ci.yml)) that runs on every commit (`push`) and pull request (`pull_request`) to verify code health:
- **C++ Compiler Warnings Check**: Builds the application targets with compiler warnings treated as errors (`-Werror`) for strict compliance.
- **Native Linux Build & Test**: Compiles the codebase using GCC on an Ubuntu runner and executes the test suite.
- **Sanitizers Verification**: Compiles and runs the native Linux test suite under AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) to detect memory management bugs and undefined behavior.
- **ThreadSanitizer (TSan) Verification**: Compiles and runs the native Linux test suite under ThreadSanitizer (TSan) to detect data races and thread synchronization issues.
- **Static Analysis**: Performs static analysis checks on all source and test C++ files using `cppcheck`.
- **Windows Cross-Compilation**: Cross-compiles Windows executables on the Linux runner using the MinGW-w64 GCC cross-compiler (`x86_64-w64-mingw32-gcc`/`g++`).
- **Compiler Caching (`ccache`)**: Speeds up verification times by caching compiled object units globally on GitHub Actions. It bypasses recompiling large upstream dependencies like SDL3 and Dear ImGui, cutting down build times significantly.
