# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **HDR → SDR Tone Mapping (`naikav::video::ToneMapper`):** HDR10, HDR10+, Dolby Vision (base layer) and HLG sources are now converted for SDR display instead of being truncated to 8 bits with their transfer curve intact. Decodes the PQ (SMPTE ST 2084) or HLG (ARIB STD-B67, including its OOTF) transfer function to linear light, rolls highlights off with the ITU-R BT.2390 EETF, converts BT.2020 → BT.709 in linear light with out-of-gamut colors desaturated toward their own luminance, and re-encodes with the BT.709 OETF. Source peak is read from mastering-display metadata when present; display peak defaults to the 100-nit SDR reference and is adjustable. LUT-driven and split across worker threads.
- **HDR tone mapping panel (`C` hotkey / controls-dock `[HDR]` button):** Reports the source's HDR standard and the live tone mapping status, and carries the HDR → SDR on/off toggle and the display-peak slider (50–1000 nits). Both settings persist to `player_settings.txt` (`hdr_tone_map_enabled`, `hdr_target_peak_nits`) and take effect on the next decoded frame.
- **Tone mapping status in the diagnostics HUD:** `ColorPipelineInfo::toneMapped` reports whether the pipeline actually converted the frame, shown as a read-only line beside the rest of the color information. The HUD reports the pipeline; the HDR panel changes it.

### Fixed

- **HDR content no longer renders dark and desaturated.** The frame conversion path accepted only 8-bit `YUV420P`/`NV12` on its zero-copy route and sent everything else — including all 10-bit PQ content — through `sws_scale` to 8-bit `YUV420P`, which resamples the bit depth but leaves the PQ curve applied. The diagnostics HUD reported "HDR Standard: HDR10 (PQ)" over the resulting uncorrected picture. 100-nit reference white was reaching the display at code 130 of 255; it now reaches it at 213.

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
