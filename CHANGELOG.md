# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-07-19

### Added

- **Multi-threaded Architecture:** Coordinates a demuxer thread, video decoder thread, audio callback thread, and main render loop using bounded thread-safe blocking queues to eliminate CPU spinning and ensure smooth backpressure handling.
- **Symmetric Seeking:** Instantaneous keyframe seek operations flushing queues and decoding pipelines, resolving in under 80ms.
- **Dynamic Hardware Fallback:** Platform-specific hardware-accelerated video decoding (D3D11VA, DXVA2, QSV, and CUVID on Windows; VAAPI, V4L2M2M, QSV, and CUVID on Linux) with automatic, runtime fallback to software H.264 decoding on initialization or decoding failure.
- **Audio-Video Synchronization:** Sub-10ms audio-to-video synchronization using sample-accurate audio clock reconstruction from PCM sample offsets, falling back to a steady-clock wall clock for video-only streams.
- **Dynamic Resolution Scaling:** Real-time playback scaling supporting original source resolution down to 360p, 480p, 720p, 1080p, 1440p, and 4K configurations on the fly via UI dropdown.
- **GPU-Mapped Planar YUV Upload:** High-performance direct upload of YUV 4:2:0 planar frame data to GPU-mapped texture memory (`SDL_PIXELFORMAT_IYUV`), bypassing expensive CPU color space conversion.
- **Software Volume Attenuation:** Linear audio gain attenuation with optimized memcpy/memset bypasses for muted (0%) and full (100%) volume states.
- **Loop Playback:** Automatic wraparound seek to start of file (0.0s) upon reaching end-of-file for continuous playback.
- **Native File Dialog:** Win32/GTK3/Portal integration via `nativefiledialog-extended` (NFD) for cross-platform file opening.
- **Diagnostics HUD & System Info:** UI overlay showing real-time statistics for queue depths, decode/render latency budgets, audio-video clock drift, and decoding mode (HW vs. SW).
- **ImGui Interface:** Sleek, translucent desktop user interface utilizing bundled custom Noto Sans typography.
- **Testing Framework:** Custom test suite featuring mock FFmpeg and SDL components, validating core player states and pipeline synchronization.

[1.0.0]: https://github.com/sanjeev-naik/NaikAVPlayer/releases/tag/v1.0.0
