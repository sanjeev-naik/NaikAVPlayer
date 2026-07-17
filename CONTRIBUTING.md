# Contributing to NaikAVPlayer

Thank you for your interest in contributing to **NaikAVPlayer**! We welcome contributions of all kinds, including bug fixes, performance optimizations, new features, documentation updates, and issue reports.

By contributing to this project, you help maintain a high-performance, low-latency, and robust native C++ media engine.

---

## Table of Contents

1. [Code of Conduct](#code-of-conduct)
2. [Contribution Workflow](#contribution-workflow)
3. [Repository Directory Structure](#repository-directory-structure)
4. [Branch Naming Conventions](#branch-naming-conventions)
5. [Commit Message Conventions](#commit-message-conventions)
6. [Coding Standards](#coding-standards)
7. [Testing Requirements](#testing-requirements)
8. [Documentation Requirements](#documentation-requirements)
9. [Issue Reporting Guidelines](#issue-reporting-guidelines)
10. [Pull Request Guidelines](#pull-request-guidelines)
11. [Code Review Expectations](#code-review-expectations)

---

## Code of Conduct

We expect all contributors to adhere to a respectful, welcoming, and collaborative code of conduct. Please be professional and constructive in all communication.

---

## Contribution Workflow

We follow a typical fork-and-pull-request model for code contributions:

1. **Find an Issue**: Search the [Issue Tracker](https://github.com/sanjeev-naik/NaikAVPlayer/issues) for open tasks. If you want to build a new feature or fix a new bug, please open an issue first to discuss the design.
2. **Fork the Repository**: Create your own copy of the repository on GitHub.
3. **Clone & Setup**:
   ```bash
   git clone https://github.com/<your-username>/NaikAVPlayer.git
   cd NaikAVPlayer
   git remote add upstream https://github.com/sanjeev-naik/NaikAVPlayer.git
   ```
4. **Create a Branch**: Create a branch off `main` for your work. Keep your branches focused on a single logical change. (See [Branch Naming Conventions](#branch-naming-conventions)).
   ```bash
   git checkout -b feature/your-feature-name
   ```
5. **Develop and Build**: Implement your changes and verify that they compile without warnings.
6. **Write Tests**: Add unit or integration tests for your new code. (See [Testing Requirements](#testing-requirements)).
7. **Run Tests**: Verify that the entire test suite passes locally.
8. **Commit Changes**: Commit your changes using Conventional Commits. (See [Commit Message Conventions](#commit-message-conventions)).
9. **Push and Open PR**: Push the branch to your fork and submit a Pull Request to the `main` branch.

---

## Repository Directory Structure

Familiarize yourself with the layout of the repository before making changes:

```text
NaikAVPlayer/
├── .github/                 # GitHub configuration and templates
│   ├── ISSUE_TEMPLATE/      # Issue templates (bug report, features, performance)
│   ├── PULL_REQUEST_TEMPLATE.md
│   └── COMMIT_MESSAGE_GUIDELINES.md
├── assets/                  # Media files, fonts, and screenshots
│   ├── fonts/               # Bundled open-source typography (Noto Sans)
│   └── app_icon.png         # Project launcher icon
├── build/                   # Recommended directory for CMake builds (ignored by git)
├── src/                     # Source files for the playback engine and UI
│   ├── main.cpp             # Application entry point & render loop
│   ├── PlayerController.cpp # Media state machine & sync coordinator
│   ├── Demuxer.cpp          # FFmpeg container packet parser
│   ├── VideoDecoder.cpp     # Video decoding (with HW-accelerated fallback)
│   ├── AudioDecoder.cpp     # Audio sample-accurate decoding & clock tracking
│   ├── PlayerUI.cpp         # Dear ImGui overlay rendering & HUD controls
│   └── ThreadSafeQueue.hpp  # Bounded thread-safe queue template
├── tests/                   # Test suite files
│   ├── tests.cpp            # Test cases, custom mocks, and assertion runner
│   └── lsan.supp            # LeakSanitizer suppression list
├── CMakeLists.txt           # Main CMake build definition
├── LICENSE                  # Open-source license terms
└── README.md                # Project documentation and KPIs
```

---

## Branch Naming Conventions

Use lowercase names with forward-slash groupings to organize your branches:

- **Features**: `feature/<short-description>` (e.g., `feature/frame-stepping`)
- **Bug Fixes**: `fix/<short-description>` (e.g., `fix/decoder-seek-freeze`)
- **Documentation**: `docs/<short-description>` (e.g., `docs/raspberry-pi-guide`)
- **Performance**: `perf/<short-description>` (e.g., `perf/gpu-texture-upload`)
- **Refactoring**: `refactor/<short-description>` (e.g., `refactor/audio-attenuation`)
- **Releases/Hotfixes**: `release/v<version>` or `hotfix/<short-description>`

---

## Commit Message Conventions

We enforce the **Conventional Commits** specification. Commits must follow this structure:

```text
<type>(<scope>): <short summary>

[optional body description]

[optional footer(s)]
```

### Quick Example
```text
feat(player): add frame stepping support

Introduce frame-by-frame forward playback when paused by mapping the right arrow
key to seek target offsets.

Fixes #128
```

For the complete list of types, scope examples, rules, and common mistakes, refer to the [Commit Message Guidelines](.github/COMMIT_MESSAGE_GUIDELINES.md).

---

## Coding Standards

NaikAVPlayer is a high-performance C++ application. The following rules keep the codebase clean, readable, and free of memory leaks.

### 1. Language & Standard
- We use **C++17** features. Avoid platform-dependent extensions unless guarded by macros.
- Enforce clean compiles. Compiler warnings are treated as errors (`TREAT_WARNINGS_AS_ERRORS=ON`).

### 2. Naming Conventions
- **Classes / Structs**: `PascalCase` (e.g., `PlayerController`, `ThreadSafeQueue`).
- **Methods / Functions**: `camelCase` (e.g., `decodeNextFrame()`, `convertFrame()`).
- **Member Variables**: Prefix with `m_` followed by camelCase (e.g., `m_videoDecoder`, `m_decodedFrameQueue`).
- **Local Variables**: `camelCase` (e.g., `packetAllocCount`, `savedCodec`).
- **Constants / Macros**: `ALL_CAPS_WITH_UNDERSCORES` (e.g., `SDL_MAIN_HANDLED`, `MAX_QUEUE_SIZE`).

### 3. Memory & Resource Management
- **Smart Pointers**: Prefer `std::unique_ptr` and `std::shared_ptr` to manage ownership. Avoid raw `new`/`delete`.
- **FFmpeg Pointer Wrappers**: FFmpeg objects (`AVFrame`, `AVPacket`, `AVCodecContext`) must be allocated and deallocated correctly.
  - Utilize dedicated custom deleters or ensure they are explicitly freed via `av_frame_free`, `av_packet_free`, or `avcodec_free_context` in destructors.
  - If sharing frames across threads, verify ref-counting rules (`av_frame_ref` / `av_frame_unref`).

### 4. Multi-Threading & Thread Safety
- **No CPU Spinning**: Bounded queues must use condition variables (`std::condition_variable` with `std::unique_lock`) to coordinate between demuxer, decoder, and renderer threads.
- **Mutex Scoping**: Keep mutex locks as small as possible. Lock only the code access that manipulates shared states to avoid thread contention.
- **Shutdown Safety**: Ensure all threads can be cleanly interrupted (via `abort()` or exit flags) and that condition variables are woken up during teardown to avoid deadlocks.

### 5. Performance Guidelines
- **Zero-Overhead Bypass**: When parameters have inactive states (e.g., full volume `volume >= 0.99f` or mute `volume <= 0.01f`), bypass loops with fast memory copy (`std::memcpy`/`std::memset`).
- **Minimize GPU Uploads**: Do not upload redundant textures. Only update the SDL texture when a new frame is popped from the queue.
- **Pass by Reference**: Pass complex objects, strings, and structures by reference-to-const (`const T&`) to avoid unnecessary copying.

---

## Testing Requirements

We maintain a strict quality target of **100% code coverage** on all core playback, demuxing, and decoding files.

### Running Tests Locally
1. Configure the build with Address/Undefined sanitizers enabled:
   ```bash
   mkdir build && cd build
   cmake -DENABLE_COVERAGE=ON -DENABLE_SANITIZERS=ON ..
   cmake --build .
   ctest --output-on-failure
   ```
2. Or configure the build with ThreadSanitizer enabled to detect data races (note: TSan and ASan are mutually exclusive):
   ```bash
   mkdir build-tsan && cd build-tsan
   cmake -DENABLE_COVERAGE=ON -DENABLE_TSAN=ON ..
   cmake --build .
   ctest --output-on-failure
   ```
   *Note: CMake automatically generates a dummy test video if `assets/hd_test_video_with_audio.mp4` is missing.*

### Code Coverage
- If you add files or logic in `AudioDecoder.cpp`, `VideoDecoder.cpp`, `Demuxer.cpp`, `PlayerController.cpp`, or `ThreadSafeQueue.hpp`, you **must** write corresponding tests in `tests/tests.cpp` to maintain coverage.

### Memory & Thread Safety Verification
- Keep the Address/Undefined Behavior Sanitizers active during development to ensure zero memory leaks.
- If a known external dependency has a leak, add it to `tests/lsan.supp` with comments instead of bypassing testing.
- Utilize ThreadSanitizer (`ENABLE_TSAN=ON`) regularly when working on multi-threaded code changes to catch potential data races and deadlocks.

---

## Documentation Requirements

- **API Documentation**: Document all public class methods in their header files (`.h`) explaining parameters, return values, thread-safety guarantees, and exceptional behaviors.
- **Comments**: Write self-documenting code. Use comments only to explain *why* something is done (e.g., complex FFmpeg configurations, clock sync calculations), not *what* the code does.
- **README Updates**: If your feature introduces user-facing controls, HUD elements, or configuration changes, update the `README.md` to reflect this.

---

## Issue Reporting Guidelines

Before reporting an issue, check the open issues to see if it has already been reported. When reporting:
1. **Choose the Right Template**: Bug reports, feature requests, and performance issues each have their own templates.
2. **Provide Logs & Environments**: Provide the exact operating system, graphics driver, and FFmpeg/SDL versions.
3. **Provide Steps to Reproduce**: Detailed instructions on how to replicate the issue. Minimal sample files or media metadata are highly appreciated.

---

## Pull Request Guidelines

1. **Link Related Issues**: Use keywords (`Fixes #123`, `Closes #123`) in the PR description so the issue closes automatically upon merge.
2. **Keep it Small**: Focus your PR on a single issue. Large PRs will take much longer to review and may be rejected.
3. **Pre-Submit Checklist**:
   - [ ] Clean build with no warnings.
   - [ ] All tests pass successfully.
   - [ ] No memory leaks reported by sanitizers.
   - [ ] Code formatted correctly.
   - [ ] Documentation updated where necessary.

---

## Code Review Expectations

All contributions require review and approval from at least one core maintainer before merging.
- **Be Open to Feedback**: Reviews are meant to maintain project quality. We will suggest changes to formatting, architecture, or performance if needed.
- **Responsiveness**: Address review comments by updating your existing branch. Once updated, request another review.
- **Merging**: Once approved and CI checks pass, a maintainer will merge your PR.
