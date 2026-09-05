# NaikAVPlayer — High-Performance Cross-Platform C++ Media Engine & Video Player

[![GitHub Repository](https://img.shields.io/badge/GitHub-NaikAVPlayer-181717?style=flat&logo=github)](https://github.com/sanjeev-naik/NaikAVPlayer)
[![GitHub Releases](https://img.shields.io/github/v/release/sanjeev-naik/NaikAVPlayer?logo=github&label=Release)](https://github.com/sanjeev-naik/NaikAVPlayer/releases)
[![CI/CD Pipeline](https://github.com/sanjeev-naik/NaikAVPlayer/actions/workflows/ci.yml/badge.svg)](https://github.com/sanjeev-naik/NaikAVPlayer/actions/workflows/ci.yml)
[![Coverage Status](https://codecov.io/gh/sanjeev-naik/NaikAVPlayer/graph/badge.svg)](https://codecov.io/gh/sanjeev-naik/NaikAVPlayer)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20%2F17-00599C?style=flat&logo=c%2B%2B)](https://en.cppreference.com/)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-blue?style=flat&logo=linux)](https://github.com/sanjeev-naik/NaikAVPlayer)
[![FFmpeg](https://img.shields.io/badge/FFmpeg-n8.1.2-007808?style=flat&logo=ffmpeg)](https://ffmpeg.org/)
[![SDL3](https://img.shields.io/badge/SDL-3.4.0-darkblue?style=flat)](https://www.libsdl.org/)
[![ImGui](https://img.shields.io/badge/Dear%20ImGui-1.91.9-purple?style=flat)](https://github.com/ocornut/imgui)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**[NaikAVPlayer](https://github.com/sanjeev-naik/NaikAVPlayer)** is an ultra-fast, native multi-threaded **C++20/C++17 media player and audio-video processing engine** engineered with direct [FFmpeg](https://ffmpeg.org/) libavcodec/libavformat APIs, [SDL3](https://www.libsdl.org/), and [Dear ImGui](https://github.com/ocornut/imgui). Designed for low-latency media rendering and professional audio processing, NaikAVPlayer bypasses bloated middleware frameworks in favor of direct GPU-mapped texture streaming, sub-10ms audio-video synchronization, hardware-accelerated video decoding (D3D11VA, DXVA2, QSV, NVDEC/CUVID, VAAPI, V4L2M2M), and a full 64-bit real-time digital signal processing (DSP) pipeline with [EBU R128](https://tech.ebu.ch/loudness) loudness normalization.

🔗 **Official Links & Resources:**
- **Source Code & Repository:** [https://github.com/sanjeev-naik/NaikAVPlayer](https://github.com/sanjeev-naik/NaikAVPlayer)
- **Binary Releases & Downloads:** [https://github.com/sanjeev-naik/NaikAVPlayer/releases](https://github.com/sanjeev-naik/NaikAVPlayer/releases)
- **Issue Tracker & Discussions:** [https://github.com/sanjeev-naik/NaikAVPlayer/issues](https://github.com/sanjeev-naik/NaikAVPlayer/issues)
- **Developer Profile:** [Sanjeev Naik (@sanjeev-naik)](https://github.com/sanjeev-naik)

---

## 📑 Table of Contents

- [Overview & Architecture](#architecture)
- [Key Features](#key-features)
- [Hardware Video Acceleration](#dynamic-hardware-decoder-fallback)
- [Real-Time Audio DSP & Loudness Pipeline](#audio-dsp--loudness-pipeline)
- [Dedicated Audio-Only Playback & Visualizer](#dedicated-audio-only-playback--real-time-visualizer)
- [Multiple Audio Tracks & Subtitles](#multiple-audio-track-switching--external-audio)
- [Building from Source](#build--local-cross-compilation-guide)
- [Usage Guide & Hotkeys](#usage-guide)
- [Frequently Asked Questions (FAQ)](#frequently-asked-questions-faq)
- [External References & Standards](#external-references--standards)
- [License & Attributions](#license--attributions)

---

## Key Features

- **Symmetric Low-Latency Seeking:** Rapid keyframe seek operations flushing packet queues and decoding pipelines under 80ms.
- **Stall-Proof Pipeline Backpressure:** Producer threads never block indefinitely on a full queue — bounded-wait pushes fall back to dropping the oldest queued item once a timeout elapses, so a paused audio device, a stalled render loop, or a wedged decoder can never freeze the single demuxer thread that feeds both the video and audio queues.
- **Dynamic Hardware Decoder Fallback:** Tries platform-specific hardware decoders (D3D11VA, DXVA2, QSV, CUVID on Windows; V4L2M2M, VAAPI, QSV, CUVID on Linux), falling back dynamically to software H.264 decoding if hardware context allocation fails or encounters runtime surface mapping errors.
- **HDR → SDR Tone Mapping (BT.2390):** HDR10, HDR10+, Dolby Vision (base layer) and HLG sources are converted for an SDR display rather than merely truncated to 8 bits. The transfer function is decoded to linear light (SMPTE ST 2084 PQ, or ARIB STD-B67 HLG including its luminance-dependent OOTF), highlights are rolled off with the ITU-R BT.2390 EETF toward the display's peak, the BT.2020 gamut is converted to BT.709 in linear light with out-of-gamut colors desaturated toward their own luminance rather than hard-clipped per channel, and the result is re-encoded with the BT.709 OETF. The source peak is taken from the file rather than assumed: HDR10+ (`maxscl`) or Dolby Vision (RPU L1) per-frame metadata when the stream carries it, otherwise the mastering-display peak reconciled with MaxCLL. Where a hardware decoder strips that metadata it is recovered from the stream header, or failing that by decoding a single frame in software at open. The display peak defaults to the 100-nit SDR reference, and both are overridable. See `naikav::video::ToneMapper`. Without this step a PQ signal reaches the display still PQ-encoded, which renders it dark and desaturated — 100-nit reference white lands at code 130 of 255 instead of 213.
- **Tone Mapping Sized to the Window, and to the Machine:** Tone mapping costs strictly per output pixel and the result is scaled to fit the window before anyone sees it, so the HDR target is capped to the renderer's output size — mapping a 4K frame for a 1024x576 window did about seven times the work the letterbox blit then threw away. On top of that the target shrinks a step at a time whenever the measured conversion overruns the source's frame interval, and is restored once it fits. Without it, software-decoded 4K60 delivered 10–44 fps in bursts with 200 ms gaps — which reads as flashing rather than as a low frame rate; with it, 40–59 fps. Content with headroom never leaves full resolution.
- **Runtime-Dispatched AVX2 Tone Mapper:** The per-pixel loop is compiled twice and selected once at runtime via `__builtin_cpu_supports`, so the binary still runs on machines without AVX2 — which building the whole player with `-mavx2` would not. Worth about 20% of the conversion (77 ms to 62 ms on a native 4K frame). FMA is deliberately left off: contracting the HLG OOTF's multiply-adds changed that path's rounding, and the two code paths are verified to produce byte-identical output instead.
- **Sub-10ms Audio-Video Synchronization:** Reconstructs the audio clock sample-accurately from PCM sample offsets to maintain A/V drift under 10ms. Because the demuxer is paced by audio consumption, video can never decode ahead to recover a deficit it starts with — so playback also holds the audio clock at the starting line until the first video frame is ready, rather than beginning every file already behind by however long the decoder took to open.
- **Fullscreen, Cursor Auto-Hide & Usability Gestures:** Fullscreen mode toggling via `F11`, `Alt+Enter`, or double-clicking anywhere on the video canvas; automatic cursor and controls dock hiding after 2.5 seconds of playback inactivity; instant volume adjustment via `Up`/`Down` arrow keys (±5%), mouse wheel over video, or mute toggle (`M`); and auto-pausing background playback during native file explorer dialogs.
- **Lossless Video Frame Screenshot Export:** Capture and export the current video frame as a full-resolution PNG image (`S` hotkey) saved directly to the `screenshots/` directory with automatic timestamps (`NaikAVPlayer_<basename>_<YYYYMMDD_HHMMSS>_<time>.png`) using FFmpeg's native PNG encoder and `sws_scale` RGB24 conversion, accompanied by animated on-screen Toast feedback.
- **Multiple Audio Track Switching & External Audio Support:** Full multi-stream container discovery for MP4/MKV files containing multiple embedded audio tracks (e.g. multi-language English/Japanese/Hindi, Director's commentary, stereo vs 5.1/7.1 surround). Supports seamless runtime audio track switching via the dedicated `[Audio]` headphone button or `B` hotkey without stalling video playback. Includes loading external standalone audio files (`.m4a`, `.aac`, `.ac3`, `.mp3`, `.wav`, `.flac`, `.ogg`, `.opus`, `.wma`, `.mka`) with automatic demuxer clock synchronization, switching back and forth between external and embedded tracks, and stream muting/disabling.
- **Complete Subtitle Support (Internal & External):** Detects, decodes, and renders both embedded container subtitles (SRT, ASS, SSA, WebVTT, MOV Text) and external subtitle files (`.srt`, `.vtt`, `.ass`, `.ssa`, `.sub`). Features auto-detection of matching subtitle files adjacent to media, interactive track selection menu (`[CC]` button in controls dock), on-the-fly subtitle cycling (`V`), millisecond-accurate sync timing delay offset adjustments (`G` / `H`), and clean on-screen rendering with dark backdrop contrast overlays.
- **Multichannel Audio Preservation:** Reads the source stream's real channel layout and drives 2.1/5.1/5.1(back)/7.1 straight through to a matching SDL3 multichannel device instead of always downmixing to stereo, with a device-native-channel check that flags when the OS is silently downmixing anyway, and a manual Auto / Force Stereo / Virtual Surround override.
- **Virtual Surround & 3D Spatial Audio:** `VIRTUAL_SURROUND` folds a discrete 2.1/5.1/5.1(back)/7.1 source down to stereo with positional delay/filter cues (`SpatialDownmixer`) instead of a flat downmix, so surround content stays spatial on headphones/stereo speakers; a separate "3D Surround" ambience synthesizer (`Surround3D`) adds a similar spatial feel to any plain stereo source, and a mid-side Stereo Widener adjusts the perceived image width independently of either.
- **Studio DSP Chain:** Live-adjustable true parametric 5-band EQ (frequency, Q, and gain all independently adjustable per band), a noise gate/expander, a soft-knee full-band compressor plus an independent 3-band multiband compressor (two crossover-split bands, each with its own threshold/ratio), a lookahead peak limiter (delay-line based, so the gain envelope reduces *ahead of* fast transients instead of only clamping after the fact), and a Linkwitz-Riley bass-management crossover — LFE tone control by default, with an optional true bass-redirect mode that highpasses every other channel and sums exactly what was removed into the LFE channel. A left/right balance control and 9 presets (see below) round out the chain. Runs on a float-internal signal path — every stage is a true no-op until explicitly configured.
- **EBU R128 Loudness Normalization — Real-Time, Two-Pass, or Tag-Based:** Real-time integrated/momentary LUFS metering with a smoothed gain correction toward a configurable target. A file is also pre-scanned once (decode-only, no video, no device I/O) before playback starts and on seek, so the correct gain applies from the very first sample instead of ramping in — see `naikav::dsp::prescanIntegratedLufs()`. When a file already carries a ReplayGain or EBU R128 gain tag (`REPLAYGAIN_TRACK_GAIN`/`R128_TRACK_GAIN`, from taggers like `mp3gain`/`rsgain`/`loudgain` or the encoder itself), that's used instead — no decoding needed at all, and at least as accurate since it's exactly what already measured the whole file. See `naikav::dsp::readTaggedLoudnessAsLufs()`.
- **Automatic Genre-Based Presets (opt-in):** When enabled, reads a file's genre tag on open and applies a matching preset (e.g. "Podcast"/"Speech" → Podcast, "Soundtrack" → Cinema) via simple keyword matching — see `naikav::dsp::presetForGenreTag()`.
- **Dedicated Audio-Only Playback & Real-Time Visualizer:** Native, sample-accurate playback pipeline for audio-only formats (MP3, AAC, FLAC, OGG, WAV, etc.) with automatic exclusion of embedded album artwork (`AV_DISPOSITION_ATTACHED_PIC`) from the video pipeline. When an audio file is loaded, a hardware-accelerated, real-time reactive audio visualizer automatically takes over the main viewport with 4 visualization styles (Neon Equalizer Bars with falling peak physics and reflections, Smooth Flowing Waveform, Radial Audio Disc, and Mirrored Stereo Spectrum) and 4 color palettes (Cyberpunk, Sunset Fire, Mint Emerald, Electric Violet).
- **Real-Time Spectrum Analyzer & Waveform Snapshots:** Opt-in FFT-based magnitude spectrum visualizer (`SpectrumAnalyzer`, 1024-point hand-rolled radix-2 FFT, Hann-windowed, frame-smoothed) and time-domain oscilloscope snapshotting reading the final post-DSP-chain signal — display only, never modifies the audio.
- **High-Quality Resampling & Dither:** libsoxr resampling engine (in place of swresample's default), with a user-facing Low/Medium/High/Very High quality selector (soxr's `precision` bits-of-precision option), plus triangular (TPDF) dither applied only at the final truncation. Resampler buffering latency is handled explicitly, so raising the quality tier never trades clean audio for precision.
- **Audio Underrun Instrumentation:** Always-on counters (M10-M12) track how often the SDL audio callback has to emit silence because no samples were available — the direct, quantitative measure of playback glitching, and the signal `SDL_GetAudioStreamQueued()` cannot provide in this architecture. A standalone smoke test (`tests/audio_underrun_smoke.cpp`) drives the real audio path against a real file and attributes any underrun to a specific cause.
- **Selectable Output Format & Device:** 16-bit integer (default), 32-bit integer, or 32-bit float device output (the internal pipeline is always float — 32-bit float skips dithering/truncation entirely), and a playback device picker (`AudioDecoder::enumeratePlaybackDeviceNames()`) instead of always using the OS default device.
- **DSP Presets & Persistent Settings:** One-click Flat / Music / Cinema / Night / Podcast / Gaming / Live / Bass Boost / Vocal Boost presets for the whole audio chain, all settings (resolution, DSP, loudness, output format/device, channel selection) surviving restarts via `player_settings.txt`.
- **Dynamic Resolution Scaling:** Real-time playback scaling supporting dynamic output resolution selection (Original source, 360p, 480p, 720p, 1080p, 1440p, 4K) from the UI dropdown to optimize GPU upload bandwidth.
- **Variable Playback Speed Control (0.25x - 2.0x):** Real-time, pitch-preserving playback rate adjustments using SDL3 dynamic audio stream frequency resampling (`SDL_SetAudioStreamFrequencyRatio`) synchronized with master audio/video clock pacing. Accessible via a dedicated Speedometer icon button in the controls dock, popup slider with preset buttons, fine-tuning buttons, and keyboard hotkeys (`[` / `]` / `Backspace`).
- **Auto-Pause on File Selection:** Automatically pauses background audio and video playback whenever the native file explorer dialog is opened, preserving the exact playback position and keeping the audio silent while browsing.
- **Software Volume Attenuation:** Scalable audio output level adjustments with memcpy/memset bypasses for 100% and 0% volume states.
- **Loop Playback:** Wraparound seek to 0.0 upon reaching end-of-file for continuous playback, toggleable via the controls dock `[Loop]` icon button and `L` hotkey.
- **Playlist & Auto-Advance:** Build a queue of local media files — multi-select "Add Files" dialog, non-recursive "Add Folder" directory scan, or dropping more than one file onto the window at once — with drag-to-reorder, per-row removal, and double-click-to-play, via the `[Playlist]` button in the controls dock or the `P` hotkey. `Off` / `All` / `One` repeat modes plus a Shuffle toggle govern auto-advance once playback naturally reaches end-of-file, independent of the per-file `[Loop]` button above (which always just repeats the current file and takes priority while it's on). Playlist contents and repeat/shuffle state persist across restarts as standard M3U8 (`playlist.m3u8`).
- **Native File Picker:** Cross-platform native file picker integration using `nativefiledialog-extended` (NFD) on Win32 and GTK3/Portal backends.
- **Pipeline Diagnostics & System Info HUD:** Real-time overlay (`--metrics` or `D` key) displaying active player states, media telemetry (native vs. playback resolution, pixel format, hardware vs. software decoder type), Color & HDR pipeline characteristics (Color Space, Primaries, TRC, Range, Chroma Subsampling, Bit Depth, HDR10/HDR10+/Dolby Vision/HLG standard, plus a read-only line reporting whether tone mapping is actually active and between which peak luminances — the toggle and display-peak slider themselves live in the HDR panel, see below), pipeline queue depth levels, decode/render frame pacing budgets, and rolling clock synchronization offsets. Measuring the pipeline must not disturb it: every video property the HUD reads is fetched without ever waiting on the decoder (see [Reading Decoder State Without Stalling](#reading-decoder-state-without-stalling)), so turning the overlay on does not cost frames.
- **HDR Tone Mapping Panel:** Dedicated overlay (`C` key or the controls-dock `[HDR]` button) reporting the source’s HDR standard, the live tone mapping status and whether a *dynamic* curve is actually in use, with the HDR → SDR on/off toggle, the display-peak slider (50–1000 nits), an optional source-peak override (100–10000 nits, for files whose metadata is wrong), and toggles for following HDR10+/Dolby Vision metadata and for adapting resolution to hold the frame rate. Settings persist across restarts and apply to the next decoded frame — no seek or reopen needed. Sliders apply live while dragging and only write to disk once released. Kept separate from the diagnostics HUD, which only reports the pipeline rather than changing it.
- **Audio Processing Panel:** Dedicated overlay (`A` key) for the full DSP chain (EQ, noise gate, compressor, multiband compressor, limiter, crossover, loudness, 3D surround, widener, balance), a live FFT spectrum visualizer, plus channel/output-device/bit-depth/resampler-quality selection, separate from the diagnostics HUD.
- **Translucent User Interface:** ImGui-based desktop interface using bundled Noto Sans typography.

---

## Architecture

NaikAVPlayer follows a multi-threaded media player design with decoupled worker threads coordinated through bounded thread-safe queues and an audio-master clock reference.

### Source Layout

Sources are grouped by subsystem under `src/`, and headers are included by that path (e.g. `#include "audio/dsp/DspChain.hpp"`):

```text
src/
├── app/       main.cpp — entry point, SDL window/event loop, render loop, CLI flags
├── audio/     AudioDecoder.{hpp,cpp}, AudioTrack.hpp — decode, resample, SDL callback, track models, output selectors
│   └── dsp/   header-only DSP module (see Audio DSP & Loudness Pipeline below)
├── core/      ThreadSafeQueue.hpp, MetricRing.hpp, PipelineMetrics.hpp, FFmpegCompat.hpp
├── media/     Demuxer.{hpp,cpp} — packet reading, multi-stream track enumeration and routing
├── player/    PlayerController.{hpp,cpp} — state machine, track switching, seeking, settings persistence
├── playlist/  header-only Playlist module (queue, repeat/shuffle, M3U8 I/O — see Playlist & Auto-Advance below)
├── subtitle/  SubtitleDecoder.{hpp,cpp}, SubtitleTrack.hpp — decoding, parsing, sync, sanitization
├── ui/        PlayerUI.{hpp,cpp} — ImGui controls dock, diagnostics HUD, audio panel, subtitle overlay
└── video/     VideoDecoder.{hpp,cpp}, ToneMapper.hpp, FrameExporter.hpp — HW/SW decode, frame conversion, HDR→SDR tone mapping, PNG screenshot export
```


### Thread Model

```text
  ┌──────────────────┐
  │ Media File/Stream│
  └────────┬─────────┘
           │
           ▼
  ┌──────────────────────────────────────────────────────────┐
  │                      Demuxer Thread                      │
  └───────┬─────────────────────┬────────────────────┬───────┘
          │ video packets       │ audio packets      │ subtitle packets
          ▼                     ▼                    ▼
  ┌──────────────┐      ┌──────────────┐     ┌──────────────┐
  │ Video Packet │      │ Audio Packet │     │Subtitle Queue│
  │ Queue (100)  │      │Queue(150-4000│     │ (100 pkts)   │
  └───────┬──────┘      └───────┬──────┘     └───────┬──────┘
          │                     │                    │
          ▼                     ▼                    ▼
  ┌──────────────┐      ┌──────────────┐     ┌──────────────┐
  │Video Decoder │      │Audio Decoder │     │  Subtitle    │
  │Thread (HW/SW)│      │  (SDL3 Audio)│     │Decoder/Parser│
  └───────┬──────┘      └───────┬──────┘     └───────┬──────┘
          │                     │ PCM Audio & PTS    │ events
          ▼                     ▼                    │
  ┌───────────────────┐ ┌──────────────┐             │
  │ Late? backlogged? │ │ Audio Master │             │
  │  └─► drop, skip   │ │    Clock     │             │
  │      conversion   │ │              │             │
  └───────┬───────────┘ └───────┬──────┘             │
          │ kept frames         │                    │
          ▼                     │                    │
  ┌───────────────────┐         │                    │
  │  convertFrame()   │         │                    │
  │  SDR: zero-copy   │         │                    │
  │       planar YUV  │         │                    │
  │  HDR: cap to      │         │                    │
  │       window ──►  │         │                    │
  │       unpack RGB48│         │                    │
  │       ──► BT.2390 │         │                    │
  │       ──► RGB24   │         │                    │
  └───────┬───────────┘         │                    │
          │ decoded frames      │                    │
          ▼                     │                    │
  ┌──────────────┐              │                    │
  │Decoded Frame │              │                    │
  │  Queue (8)   │              │                    │
  └───────┬──────┘              │                    │
          │                     │                    │
          └──────────────┬──────┴────────────────────┘
                         │
                         ▼
  ┌──────────────────────────────────────────────────────────┐
  │                   Main / Render Loop                     │
  │  ┌────────────────────────────────────────────────────┐  │
  │  │ Dequeue Frames ──► Query Master Clock (A/V Sync)   │  │
  │  │                            │                       │  │
  │  │                            ▼                       │  │
  │  │ Publish window size ──► GPU Texture Upload & UI    │  │
  │  │   (caps HDR work)        YUV planar, or RGB24      │  │
  │  │                          when tone mapped          │  │
  │  │                            │                       │  │
  │  │                            ▼                       │  │
  │  │ Subtitle Overlay Match ──► Render Contrast Backdrop│  │
  │  └────────────────────────────────────────────────────┘  │
  └──────────────────────────────────────────────────────────┘

  Startup only: play() holds the audio clock until the first frame is
  queued, so video never begins a file already behind (prerollVideo()).
```

- **Demuxer Thread**: Reads raw packets via `av_read_frame` and routes them into bounded `ThreadSafeQueue<AVPacket*>` instances (video capacity: 100 packets, subtitle capacity: 100, audio capacity sized per file, 150–4000 packets). Pushes never block indefinitely — see [Stall-Proof Queue Backpressure](#stall-proof-queue-backpressure-deadlock-prevention) below.

  The audio queue is counted in packets but what matters is the *time* it holds, and that varies by two orders of magnitude between codecs: an AAC packet is ~23 ms, a TrueHD access unit under 1 ms. So a file **with video** sizes this queue by the clock rather than by a packet count: `audioQueuePacketsForFormat()` asks for however many packets hold three seconds of *this* stream's audio, from the samples-per-packet the decoder declares, bounded to 150–4000. It has to buffer audio through the startup preroll without the demuxer blocking here and starving video of the very packets the preroll is waiting for — and a flat 150 packets is three seconds of AAC but only ~125 ms of TrueHD. Ordinary codecs therefore land on the 150 floor, exactly the bound that has always shipped, which matters because the demuxer's backpressure against this queue is load-bearing. An **audio-only** file keeps the floor unconditionally: it never prerolls, and buffering that far ahead would let the demuxer swallow a short file whole and report end-of-stream while playback is still paused at the start.
- **Video Decoder Thread**: Background worker thread that pops packets from the video queue, decodes them (via hardware or software fallback), and pushes converted frames into the bounded `m_decodedFrameQueue` (capacity: 8 frames), using the same bounded-wait backpressure. A frame more than 100 ms behind the clock is dropped here — *before* the conversion that would be wasted on it — but only while the packet queue is actually backed up, which is what distinguishes a pipeline that has fallen behind from one that is merely being fed at real time (see [Startup Preroll and A/V Sync](#startup-preroll-and-av-sync)).
- **Audio Decoding**: Executed sample-accurately inside the SDL3 Audio Stream callback thread. It pulls packets from the audio queue, decodes them, resamples to the output layout/rate as interleaved float (`swr_convert`, libsoxr engine), runs the DSP chain and loudness normalization in place, then dithers and truncates to the device's 16-bit format — see [Audio DSP & Loudness Pipeline](#audio-dsp--loudness-pipeline) below.
- **Subtitle Decoder & Parser**: Handles container-embedded subtitle streams via FFmpeg decoders and standalone external files (`.srt`, `.vtt`, `.ass`, `.ssa`, `.sub`) parsed directly into in-memory timed events.
- **Main / Render Thread**: Dequeues decoded frames from `m_decodedFrameQueue` whose PTS matches the master clock time, matches active subtitle events against playback PTS + delay offset, uploads to the SDL streaming texture, and renders the Dear ImGui interface overlay. It also publishes the renderer's output size back to the controller every frame, which is what lets the HDR path avoid tone mapping more pixels than the window can show. The texture is planar YUV for the zero-copy SDR path and RGB24 for tone-mapped HDR.

#### GPU-Mapped Planar YUV Uploads
Instead of performing CPU-side YUV-to-RGB color space conversion, the video decoder pipeline extracts raw YUV 4:2:0 planar frame data directly. The main thread maps this data onto a hardware-accelerated SDL3 streaming texture (`SDL_PIXELFORMAT_IYUV`) using `SDL_UpdateYUVTexture`. This uploads plane segments directly to GPU texture memory, allowing graphics hardware to handle color space conversion and scaling efficiently.

#### HDR → SDR Tone Mapping Pipeline
HDR frames cannot go down the [GPU-mapped YUV path](#gpu-mapped-planar-yuv-uploads) above, because that path hands the decoded signal to the display unchanged — correct for BT.709 SDR, wrong for a PQ- or HLG-encoded one, which then renders dark and desaturated no matter how many bits it is carried in. `VideoDecoder::convertFrame()` routes any frame whose `color_trc` is `AVCOL_TRC_SMPTE2084` or `AVCOL_TRC_ARIB_STD_B67` through two stages instead:

```text
HDR YUV (10/12-bit, BT.2020, PQ or HLG)
     │  swscale, sliced across every core, scaled to the playback resolution
     ▼
RGB48 (16-bit, BT.2020, still HDR-encoded)
     │  naikav::video::ToneMapper
     │    ├─ EOTF        PQ (ST 2084) or HLG (STD-B67 + its OOTF) → linear light
     │    ├─ Tone curve  ITU-R BT.2390 EETF, driven by the brightest channel so
     │    │              hue holds steady and nothing exceeds the display peak
     │    ├─ Gamut       BT.2020 → BT.709 in linear light, with out-of-gamut
     │    │              colors desaturated toward their own luminance
     │    └─ OETF        BT.709 → 8-bit
     ▼
RGB24, tagged BT.709 / sRGB — uploaded straight to an RGB streaming texture
```

- **Vectorised where it counts:** the per-pixel loop is compiled twice -- once baseline, once for AVX2 -- and selected at runtime, so the binary still runs on machines without AVX2. Worth about 20% of the conversion (77 ms to 62 ms on a native 4K frame). FMA is deliberately left off: contracting the HLG OOTF's multiply-adds changed its rounding, and the two paths are verified byte-identical for both PQ and HLG instead.
- **Tables, not transcendentals:** every scalar stage (both EOTFs, the tone curve, the OETF) is precomputed into a lookup table once per `(transfer, source peak, target peak)` combination — in practice once per file. The per-pixel path is table reads plus a 3×3 matrix, with no `pow`, `exp` or `sqrt` in it.
- **Scaling happens first:** the conversion to RGB48 also does the resolution scaling, so selecting a lower playback resolution shrinks the tone mapping work rather than adding to it. The kernel follows the direction of the resample — bilinear at native size (where the only thing being resampled is chroma), an area average when downscaling, bicubic when upscaling.
- **No detour through YUV:** the mapper writes RGB24 and the renderer uploads it as an RGB texture. Packing it back into YUV420P purely so the GPU could convert it to RGB again cost a third full-frame pass — 47 ms per 4K frame — for a picture identical either way.
- **Threaded on both sides:** the unpack is built with `sws_alloc_context()` and driven through `sws_scale_frame()`, because that is the only path on which swscale will slice a conversion across cores. Left single-threaded it ran 95 ms on a 4K frame, more than the tone mapper it feeds.
- **Never more work than the frame budget allows:** when the measured conversion overruns the source's frame interval, the tone-mapping target is shrunk a step at a time and restored once it fits again (`AdaptiveToneMapScale`), reacting within three frames and recovering over about two seconds. This is what makes software-decoded 4K60 playable at all: with the CPU already spent on AV1 decode, the fixed-size mapping overran the 16.7 ms budget, frames arrived late and were dropped in bursts, and delivery swung between 10 and 44 fps with 200 ms gaps -- visible as flashing rather than as a low frame rate. Adapting holds 40-59 fps instead. Content with headroom never leaves full size.
- **Never more pixels than the window shows:** the tone-mapping target is capped to the renderer's output size (`capToDisplaySize()`), preserving aspect ratio and only ever downscaling. Mapping a 4K frame for a 1024x576 window meant doing seven times the work the letterbox blit immediately threw away. The display box is rounded up to a multiple of 64 px so that dragging a window edge does not reallocate the pipeline on every pixel of the drag. This applies to the HDR path only — the SDR path still honours the resolution selector exactly.
- **Metadata recovered whatever the decoder:** mastering-display luminance and MaxCLL usually arrive as frame side data from the decoder's SEI parsing, which hardware decoders do not do. The values are therefore looked for in three places in order -- the frame, the stream header, and failing both a one-time software decode of a single frame at open (`Demuxer::probeHdrMetadata()`). The last is what Matroska needs: it commonly carries no stream-level copy, so a 4K HDR MKV played on the GPU would otherwise have nothing at all to tone map from and would silently use the generic default.
- **Mapped from the peak the content actually reaches:** the source peak is taken from the frame's mastering-display metadata reconciled with its `MaxCLL` — the lower of the two, since content cannot be brighter than the display it was graded on, and a grade delivered on a 4000-nit monitor whose brightest shot only reaches 800 spends most of the tone curve on range the file never uses. Overridable from the HDR panel for files whose metadata is wrong.
- **Correct by construction where it matters:** the BT.2020 → BT.709 matrix is applied as identity-plus-difference rather than as a plain 3×3 product. Both gamuts share a D65 white, so every row of the true matrix sums to exactly 1 and neutral greys must survive untouched; the published six-decimal coefficients sum to 1.000001 on the green row, which is enough to shift greys off neutral by a code value.
- **Truthful reporting:** `ColorPipelineInfo::toneMapped` records what the pipeline actually did, not just what the source claimed. The diagnostics HUD (`D`) shows the source's HDR standard *and* whether it was converted, so "HDR10 (PQ)" can never again be displayed over an uncorrected picture.

> [!NOTE]
> **Cost.** This is a CPU conversion — SDL3's 2D renderer exposes no custom shader stage to do it on the GPU. Row ranges are split across up to 8 worker threads. Measured on a 4-core/8-thread desktop against a 3840×2160 HDR10+ source (`tests/hdr_bench_standalone.cpp`), the tone mapping stage alone runs at ~29 ns/pixel single-threaded and ~8.4 ns/pixel across 8 threads.
>
> Because the target is capped to the window, the cost that matters is the size the video is *drawn* at, not the size it was encoded at. End to end — decode plus conversion, per frame, on that 4K source:
>
> | Drawn at | Per frame | Rate |
> | --- | --- | --- |
> | 1024×576 (default window) | 14 ms | 74 fps |
> | 1600×900 | 24 ms | 41 fps |
> | 1920×1080 | 30 ms | 34 fps |
> | 2560×1440 | 50 ms | 20 fps |
> | 3840×2160 (full screen) | 82 ms | 12 fps |
>
> So 4K HDR plays in real time in any window up to roughly 1080p. **Full-screen 4K HDR still will not sustain 24 fps on a 4-core machine** — there the window genuinely wants every pixel and there is nothing to cap away. Drop the resolution selector below native, or turn tone mapping off in the HDR panel (`C`) and accept the uncorrected picture.

#### FFmpeg Version Portability
Two different FFmpegs are in play. Windows and Linux x86_64 use a current bundled build (n8.x) downloaded by CMake; **Linux ARM64 deliberately links the distro's system FFmpeg**, because that is what carries the V4L2 M2M hardware decoding a Raspberry Pi needs. A Pi OS or Ubuntu of that generation can be several major versions behind.

`src/core/FFmpegCompat.hpp` is the single place every version threshold is stated. Each guarded feature degrades to the behaviour that predates it rather than failing to build:

| Feature | Needs | Without it |
| --- | --- | --- |
| `AVPacket::opaque` seek tagging | 6.0 | Pre-seek packets reach the codec, as before the optimisation |
| `codecpar` side data / framerate | 7.0 | Metadata comes from the bitstream probe; frame rate from the stream |
| Dolby Vision side data / L1 levels | 5.0 / 7.0 | Still tone mapped, but statically and unlabelled |
| HDR10+ dynamic metadata | 4.3 | Static tone mapping |
| Threaded swscale | 7.1 | Single-threaded unpack (95 ms vs ~27 ms at 4K) |
| `AVChannelLayout` API | 5.1 | Falls back to the `uint64_t` layout mask |
| `const AVCodec**` in `av_find_best_stream` | 5.0 | Non-const pointer type |

The whole application compiles cleanly against both FFmpeg 4.4 and 8.1, which is what lets Linux ARM64 link whatever the distro ships.

Note that AVX2 is x86-only by construction: the runtime check compiles out entirely on ARM64, which simply runs the baseline tone mapper.

#### Reading Decoder State Without Stalling
The video thread holds `m_videoDecoderMutex` for the whole of `decodeNextFrame()` **and** `convertFrame()` — tens of milliseconds on 4K HDR content, since the tone mapping happens inside that call. Anything on the render thread that takes the same mutex therefore waits on the decoder rather than on itself.

That is exactly what the diagnostics HUD does: it reads five video properties (`getColorInfo()`, `isVideoHardware()`, `getVideoPixelFormat()`, `getVideoWidth()`/`getVideoHeight()`) on every rendered frame. Modelled at a 27 ms conversion, those five reads blocked the render thread for a **median of 80 ms per frame against a 16.7 ms budget** — enabling the overlay visibly dropped the frame rate, which is a poor property for a diagnostic tool.

Those accessors now use `try_lock` and return the last known value whenever the decoder is busy, backed by a separate, briefly-held cache mutex. In the same model the median cost falls to **0.00 ms**, with 595 of 600 reads served from cache. They are display-only figures that change rarely or never during playback, so a value one frame old is invisible — while blocking the render thread is not. Any UI accessor added later should follow the same rule rather than taking the decoder mutex directly.

#### Startup Preroll and A/V Sync
Audio is the master clock, so it must not start running before there is a picture to match it against. `PlayerController::prerollVideo()` holds the audio clock until the video pipeline has produced its first frame, bounded by a two-second ceiling so a stream whose video never decodes still plays its audio.

This matters more than a startup detail because the offset it prevents is permanent. The demuxer is paced by audio consumption -- it blocks once the audio queue is full -- so during playback the video path receives packets at real time and cannot decode ahead. Whatever gap exists when audio starts is carried for the whole file. Before the preroll, a 4K HDR clip whose first frame took 1.10 s to decode ran 1.15 s behind the clock forever, which made every frame register as late and left the player showing one frame in nine.

The audio packet queue is sized to make the wait possible: capacity is counted in packets but what matters is the time it holds, and that varies by two orders of magnitude between codecs (an AAC packet is ~23 ms, a TrueHD access unit under 1 ms), so a file with video gets as many packets as it takes to hold three seconds of its own audio. Too small, and the demuxer blocks on audio during the preroll and starves video of the very packets it is waiting for.

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
     │   (a 0-sample result here means the resampler is still
     │    buffering — decode another frame, never emit silence)
     ▼
DSP Chain  ── Parametric EQ (5-band biquad, freq/Q/gain all adjustable)
           ──► Noise Gate (downward expander below threshold)
           ──► Compressor (soft-knee, linked multichannel)
           ──► Multiband Compressor (3 bands, 2 crossover splits)
           ──► Limiter (lookahead delay line + fast attack, hard-ceiling backstop)
           ──► Bass-Management Crossover (Linkwitz-Riley 4th-order:
               LFE lowpass, optional highpass + redirect on other channels)
     │
     ▼
Loudness Normalization (EBU R128; real-time smoothed gain, a
whole-file prescan, or a ReplayGain/R128 tag primes the gain)
     │
     ▼
3D Surround synthesis ──► Stereo Widener ──► Balance (final output-width/pan stage)
     │
     ▼
Spectrum Analyzer (display tap -- reads the signal, never modifies it)
     │
     ▼
Final safety limiter (always-on backstop)
     │
     ▼
TPDF Dither → device-format truncation (16-bit int / 32-bit int / 32-bit float) → SDL3 device output
```

- **Float-internal, dither only at the end:** `swr_convert` outputs `AV_SAMPLE_FMT_FLT`, not the device format — every DSP stage runs at full float precision, with quantization noise (triangular/TPDF dither) introduced exactly once, at the final truncation. 32-bit float output skips that truncation/dither step entirely (see [Selectable Output Format & Device](#key-features) above).
- **Zero cost when disabled:** default settings (0 dB EQ, gate/compressor/multiband ratio 1:1, 0 dB limiter ceiling, loudness off) make each stage a true no-op — enabling the chain doesn't change the sound, or add measurable overhead, until something is actually configured.
- **True parametric EQ:** each of the 5 bands has independently adjustable center frequency, Q (bandwidth), and gain — not just gain around a fixed set of frequencies.
- **Noise gate:** a downward expander below threshold (the mirror of the compressor), with fast-open/slow-close attack-release and its own short detector-smoothing stage so a sustained tone's own zero-crossings don't chatter the gate.
- **Multiband compressor:** splits into low/mid/high bands via two Linkwitz-Riley crossover points and compresses each independently, so taming one frequency range doesn't drag the others down with it the way the single full-band compressor does.
- **Lookahead limiter:** holds a short (a few milliseconds) internal delay line so the gain-reduction envelope has time to react *before* a fast transient reaches the output, rather than only clamping it after the fact once it's already passed through at full level.
- **True bass management, optionally:** the crossover's LFE lowpass is always available; enabling bass redirect additionally highpasses every non-LFE channel and sums exactly the content removed from each into the LFE channel, for setups where the main/surround speakers can't reproduce bass well.
- **Live, thread-safe control:** `AudioDecoder::applyDspSettings()` lets the UI thread update every parameter (EQ bands, gate, compressor, multiband, limiter, crossover, loudness target, surround/widener/balance) while the SDL audio callback thread concurrently processes audio — both sides are guarded by one short-held mutex, so switching presets never blocks or glitches playback.
- **EBU R128 via FFmpeg, not a hand-rolled meter:** loudness measurement uses FFmpeg's own `ebur128` `libavfilter` filter (a minimal `abuffer → ebur128 → abuffersink` graph, reading momentary/integrated LUFS back via frame metadata) rather than a custom K-weighting/gating implementation — spec-compliance for loudness numbers matters in a way DSP *effects* don't.
- **Two-pass loudness priming:** `naikav::dsp::prescanIntegratedLufs()` decodes a file's whole audio stream once (no video, no device output) to get a stable whole-file LUFS figure before playback begins, which primes the real-time normalizer to the correct gain immediately — avoiding both the multi-second startup ramp and the reset-to-zero-gain a plain real-time meter would otherwise hit on every seek.
- **ReplayGain/R128 tag priming, when present:** `naikav::dsp::readTaggedLoudnessAsLufs()` checks for `R128_TRACK_GAIN`/`REPLAYGAIN_TRACK_GAIN` (and their `_ALBUM_` fallbacks) before falling back to the decode-based prescan above — no decoding needed at all when the tag is already there.
- **Spectrum analyzer:** `SpectrumAnalyzer` downmixes the final signal to mono, accumulates it into a ring buffer, and runs a hand-rolled 1024-point radix-2 FFT every full block (~21ms at 48kHz), Hann-windowed and frame-to-frame smoothed for a stable display. A read-only tap (via a self-synchronized snapshot getter, no shared locking needed) -- it never touches the signal it's analyzing.
- **Presets & persistence:** Flat / Music / Cinema / Night / Podcast / Gaming / Live / Bass Boost / Vocal Boost presets (Audio Processing panel, `A` key) apply a canned combination of every parameter in one step, optionally auto-selected by a file's genre tag; all settings persist to `player_settings.txt` across sessions.

> [!NOTE]
> **Not implemented (deliberately scoped out):**
> - **HRTF/binaural rendering** — the existing "3D Surround"/Virtual Surround features are hand-rolled ambience/positional-cue synthesis, explicitly *not* real HRTF (no licensed decoder or measured head-related impulse response data involved, and this project doesn't vendor one). Real HRTF rendering needs an actual measured HRIR dataset (e.g. MIT KEMAR, SADIE, CIPIC) to convolve against, which is a data/licensing decision for a future contribution, not something fabricated here.
> - **Gapless playback & crossfade** — a real playlist/track-queue engine now exists (see [Playlist & Auto-Advance](#playlist--auto-advance) below), but true sample-accurate gapless transitions and crossfade need cross-instance `AudioDecoder` handoff and a crossfade DSP stage on top of it, which is a larger architectural addition than the rest of the DSP chain above and is left for a future iteration. Today's playlist auto-advance opens and starts the next file the same way manually opening a new file does (brief re-init gap, no crossfade).

### Dedicated Audio-Only Playback & Real-Time Visualizer

NaikAVPlayer provides a dedicated audio-only playback mode that activates automatically whenever a media file contains audio streams without a motion video stream (including MP3, AAC, FLAC, OGG, WAV):
- **Cover Art Filtering (`AV_DISPOSITION_ATTACHED_PIC`)**: Embedded album artwork (ID3 APIC / attached pictures) is filtered out from video stream indexing so audio-only files immediately engage the visualizer rather than stalling the video decoder pipeline.
- **Timebase-Accurate Audio Clock**: Directly converts decoded audio frame timestamps from the stream timebase (`m_timeBase`) into seconds for sample-accurate clock progression.
- **Hardware-Accelerated Reactive Visualizer**: Renders 4 distinct visualization styles (Neon Equalizer Bars with falling peak physics and reflections, Smooth Flowing Waveform, Radial Audio Disc, Mirrored Stereo Spectrum) across 4 color palettes (Cyberpunk, Sunset Fire, Mint Emerald, Electric Violet) using Dear ImGui `ImDrawList` primitives.

### Subtitle Pipeline (Internal & External)

- **Embedded Subtitle Detection**: Demuxes container-embedded subtitle streams (SRT, ASS/SSA, WebVTT, MOV Text) into bounded queues synchronized to video presentation timestamps.
- **External Sidecar Loading**: Supports loading standalone subtitle files (`.srt`, `.vtt`, `.ass`, `.ssa`, `.sub`) with automatic adjacent-file discovery and instant random seeking via in-memory event caching.
- **Text Sanitization & Contrast Rendering**: Strips raw ASS override styling tags and dialogue headers while rendering clean, centered text over a translucent dark rounded backdrop box (`rgba(5, 5, 8, 0.78)`).
- **Millisecond-Accurate Sync Offset**: Real-time delay adjustment in 50ms increments via `G` / `H` hotkeys or the `[CC]` controls menu.

### Multiple Audio Track Switching & External Audio

- **Multi-Stream Audio Discovery**: Discovers all embedded audio streams in MP4/MKV containers, exposing language tags, titles, codecs, channel layouts, and sample rates.
- **Runtime Track Switching**: Hot-swaps the active audio stream via `B` or the `[Audio]` menu without dropping video frames or interrupting video playback.
- **External Audio Demuxing**: Concurrently demuxes external audio files (`.m4a`, `.aac`, `.ac3`, `.mp3`, `.wav`, `.flac`, `.ogg`, `.opus`, `.wma`, `.mka`) via a dedicated secondary demuxer thread synchronized with the master video timeline.

### Frame Screenshot Export & Usability Pipeline

- **PNG Screenshot Capture (`FrameExporter`)**: Extracts the current presentation `AVFrame`, converts it to RGB24 via `sws_scale`, and encodes it to a PNG image using FFmpeg's native PNG encoder. Output files are saved into `screenshots/NaikAVPlayer_<basename>_<YYYYMMDD_HHMMSS>_<time>.png`.
- **Toast Notifications**: Floating on-screen feedback toasts at the bottom/top of the screen for track changes, delay adjustments, screenshot exports, loop toggling, and errors.
- **Auto-Hide Controls & Cursor**: Controls dock and mouse cursor fade out after 2.5 seconds of inactivity during video playback and restore upon any mouse or keyboard input.

### Playlist & Auto-Advance

`PlayerController` owns a header-only `naikav::playlist::Playlist` (`src/playlist/`, no SDL/FFmpeg/ImGui dependency) tracking the queued items, `RepeatMode` (`Off`/`All`/`One`), and shuffle state. The current item is tracked by a stable id assigned on insert rather than a raw index, so it survives reordering and removal of other items without index-arithmetic bookkeeping.

- **Auto-advance**: `PlayerController::pollPlaylistAutoAdvance()`, called once per frame from the main loop, checks for the `ENDED` state (see [State Machine Transitions](#state-machine-transitions) below) and, if reached, advances via `Playlist::next()` and opens/plays the result the same way any other file open does — never touching the packet queues directly. `next()`/`previous()` automatically skip entries marked invalid (e.g. a file referenced by a loaded M3U that's since been moved or deleted) instead of getting stuck on them.
- **Loop interaction**: the per-file `[Loop]` toggle is unchanged and independent — playback never reaches `ENDED` while it's on (it wraps via `instantSeek(0.0)` first), so the playlist's own repeat mode only takes effect once `[Loop]` is off.
- **Opening a file resets the playlist to just that file**: `openFile(path, resetPlaylist = true)` is the default for the CLI argument, a single drag-and-drop, and the "Open File" dialog, so what's "playing" and what's "in the playlist" never disagree. Playlist-driven navigation calls `openFile(path, false)` instead.
- **Multi-file drag-and-drop**: SDL3 delivers one drop event per dropped file within the same poll batch; these are now collected and, if more than one, appended to the playlist as a batch (dropping exactly one file keeps the original single-file-open behavior).
- **Persistence**: playlist contents save as `playlist.m3u8` (standard M3U8, so it also opens correctly in other players); the current index, repeat mode, and shuffle flag are three additional `key=value` lines in `player_settings.txt`, following that file's existing forward/backward-compatible format. Both save immediately on every playlist mutation, and load once at startup without auto-playing.

### State Machine Transitions
* **`UNINITIALIZED`**: Initial state. Loading media starts background demuxing and transitions to `OPENED`.
* **`OPENED`**: Media metadata loaded; decoders initialized; initial frame rendered. Triggering `play()` transitions to `PLAYING`.
* **`PLAYING`**: Audio output unpaused; main loop syncs video frames to the master clock.
* **`PAUSED`**: Audio device paused; current clock frozen.
* **`ENDED`**: Demuxer hits EOF and packet queues drain. If **Loop Mode** is enabled, reaching EOF directly invokes `seek(0.0)` to restart playback continuously; otherwise, if a playlist item follows (per its repeat/shuffle state), `pollPlaylistAutoAdvance()` opens and plays it on the next frame.
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
> **`libasound2-dev` / `libpipewire-0.3-dev` are required, not optional.** SDL3 detects available audio backends via `pkg-config` at CMake configure time. If neither package (nor `libpulse-dev`) is installed, SDL3 silently builds with only its `dummy`/`disk` audio drivers — the build succeeds, but the app fails at launch with `Could not initialize SDL3: No available audio device`. This is a Linux-only failure mode: Windows audio (WASAPI) ships inside the OS SDK, so there is no equivalent dev package to forget. CMake now checks for this at configure time (see [Troubleshooting in help.md](help.md#9-troubleshooting)) and fails fast with instructions instead of producing a silently broken build.

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

> **Advanced Build Flags**: Additional CMake options are supported for analysis across build configurations:
> `-DENABLE_SANITIZERS=ON` (ASan/UBSan, default `OFF`), `-DENABLE_TSAN=ON` (ThreadSanitizer, default `OFF`),
> `-DTREAT_WARNINGS_AS_ERRORS=OFF` (`-Werror`, default `ON`), `-DENABLE_COVERAGE=OFF` (gcov/lcov
> instrumentation, default `ON`), `-DNAIKAV_FORCE_BUNDLED_FFMPEG=OFF` (use the system FFmpeg instead of the
> downloaded prebuilt one), plus the `RelWithDebInfo` and `MinSizeRel` build types.

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

**Launch with console log output (Windows):**
```bash
./build/NaikAVPlayer.exe --console "/home/user/Videos/video.mp4"
```
The Windows build is a GUI-subsystem executable with no console of its own, so
its log output is discarded by default. `--console` attaches it to the terminal
that launched it. Opt-in by design: automatic console attachment at startup
matches a console-hiding pattern that generic antivirus heuristics flag. No
effect when launched by double-click, and ignored on Linux.

### Keyboard Controls & Gestures

| Key / Gesture | Action |
| :--- | :--- |
| **`Spacebar`** | Toggle Play / Pause |
| **`F11`** / **`Alt+Enter`** | Toggle Fullscreen Mode |
| **`Double-Click`** (on video) | Toggle Fullscreen Mode |
| **`M`** | Toggle Audio Mute / Unmute |
| **`Up Arrow`** / **`Down Arrow`** | Increase / Decrease Volume (±5%) |
| **`Mouse Wheel`** (over video) | Adjust Volume Up / Down (±5%) |
| **`S`** | Capture Screenshot / Export Current Video Frame as PNG |
| **`B`** | Cycle Audio Tracks (Off -> Embedded -> External -> Off) |
| **`V`** | Cycle Subtitle Tracks (Off -> Embedded -> External -> Off) |
| **`G`** / **`H`** | Adjust Subtitle Synchronization Delay (-50ms / +50ms) |
| **`Left Arrow`** / **`Right Arrow`** | Seek backward / forward by 10 seconds |
| **`[`** / **`]`** | Decrease / Increase playback speed by 0.25x (0.25x – 2.0x) |
| **`Backspace`** | Reset playback speed to normal (1.0x) |
| **`L`** | Toggle Continuous Loop Mode |
| **`P`** | Toggle Playlist Panel |
| **`Delete`** (in Playlist panel) | Remove selected item from playlist |
| **`D`** | Toggle Diagnostics HUD & Telemetry Metrics |
| **`A`** | Toggle Audio Processing Panel (EQ, Noise Gate, Compressor, Multiband, Limiter, Crossover, Loudness, Surround, Balance, Channel/Device/Format Selection) |
| **`Escape`** | Exit Fullscreen (if in fullscreen) or Exit Application |



---

## Release Packaging & Compliance

Release packages generated and published by the CI/CD pipeline (`NaikAVPlayer-windows-x64`) are validated for full open-source redistribution compliance.

The Windows release is built with the **native MSVC toolchain on a Windows runner**, not MinGW cross-compiled from Linux: MinGW PE binaries trip generic antivirus heuristics, while MSVC output trips far fewer and supports Control Flow Guard. It links the static CRT (`/MT`), so neither a VC++ redistributable nor MinGW's `libwinpthread` is bundled.

*(Note: Linux release binary artifact uploads are currently suspended until a portable AppImage/containerized build process is available. Windows release executables continue to be published — uploads run only for `release*` branches).*

Every published release package archive includes:
- **Executable**: `NaikAVPlayer.exe` (or local native `NaikAVPlayer`)
- **Dynamic Libraries**: Bundled shared libraries (`.dll` or `lib/*.so*`)
- **Licenses Directory**: Complete `licenses/` and `LICENSES/` folder containing project and third-party license text files:
  - Project `LICENSE` (MIT License)
  - `LICENSE.lgpl-3` & `FFMPEG_CREDITS.txt` (FFmpeg LGPL v3 -- this build uses `--enable-version3`; also credits the bundled libsoxr resampler and the `avfilter`/`ebur128` loudness metering it ships)
  - `LICENSE.sdl3` (SDL3 Zlib License)
  - `LICENSE.imgui` (Dear ImGui MIT License)
  - `LICENSE.nfd` (nativefiledialog-extended Zlib License)
  - `LICENSE.winpthread` (MinGW Winpthread License — retained for locally MinGW-built binaries; not applicable to the MSVC release build above)
- **Documentation**: `README.md` and `help.md`
- **Assets**: Fonts and icons in `assets/`

---

## Frequently Asked Questions (FAQ)

### What makes NaikAVPlayer different from standard media players like VLC or MPV?
NaikAVPlayer is designed as both a standalone, lightweight media player and a high-performance **native C++ reference engine**. It avoids heavy scripting layers and multi-process overhead by utilizing direct [FFmpeg](https://ffmpeg.org/) C APIs, direct GPU texture memory streaming via [SDL3](https://www.libsdl.org/), and an in-process 64-bit float DSP audio processing architecture with zero-allocation real-time loops.

### How does NaikAVPlayer achieve sub-10ms Audio/Video synchronization?
NaikAVPlayer utilizes an **Audio-Master Clock Reference**. The playback clock is reconstructed sample-accurately directly from the SDL3 audio stream consumer offset, accounting for hardware queue depth and resampling latency. The video thread paces frames against this microsecond-accurate timebase, and drops one only when it is both late *and* the packet queue is backed up — lateness alone does not mean the pipeline is behind, since a constant offset cannot be recovered by dropping anything (the demuxer is paced by audio, so video never gets the chance to decode ahead). Playback also prerolls the first video frame before starting the audio clock, so that offset does not arise in the first place.

### How does EBU R128 loudness normalization work?
Instead of simple peak normalization that distorts dynamics, NaikAVPlayer implements standard **ITU-R BS.1770-4 / EBU R128** loudness measurement. It supports both real-time gated LUFS tracking and whole-file prescan (`prescanIntegratedLufs()`), instantly applying smooth gain corrections to match your target loudness (e.g. -23 LUFS / -16 LUFS) without clipping or distortion.

### Does NaikAVPlayer support hardware video acceleration on my system?
Yes. NaikAVPlayer dynamically probes and utilizes platform-specific hardware acceleration:
- **Windows:** Direct3D 11 Video Acceleration (`D3D11VA`), `DXVA2`, Intel Quick Sync Video (`QSV`), and NVIDIA `NVDEC`/`CUVID`.
- **Linux:** Video Acceleration API (`VA-API`), `V4L2M2M`, Intel `QSV`, and NVIDIA `NVDEC`.
If hardware context allocation fails or a codec is unsupported, it automatically falls back to multithreaded software decoding without dropping playback state.

---

## External References & Standards

- **Official Project Repository:** [GitHub - sanjeev-naik/NaikAVPlayer](https://github.com/sanjeev-naik/NaikAVPlayer)
- **Multimedia Engine Core:** [FFmpeg Official Documentation](https://ffmpeg.org/documentation.html)
- **Windowing & Audio Backend:** [Simple DirectMedia Layer 3 (SDL3)](https://wiki.libsdl.org/SDL3/FrontPage)
- **Graphical User Interface:** [Dear ImGui Repository](https://github.com/ocornut/imgui)
- **Loudness Standards:** [EBU Recommendation R128](https://tech.ebu.ch/loudness) & [ITU-R BS.1770-4](https://www.itu.int/rec/R-REC-BS.1770)
- **Audio Resampling Engine:** [SoX Resampler Library (libsoxr)](https://sourceforge.net/projects/soxr/)
- **Microsoft Direct3D Video:** [Direct3D 11 Video APIs (Microsoft Learn)](https://learn.microsoft.com/en-us/windows/win32/medfound/direct3d-11-video-apis)
- **Linux Video Acceleration:** [Intel VA-API Linux Drivers & Specification](https://github.com/intel/libva)

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
