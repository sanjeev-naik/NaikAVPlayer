---
name: Performance Issue ⚡
about: Report high CPU/GPU usage, seek latency delays, or audio-video clock drift.
title: '[Performance] '
labels: performance
assignees: ''
---

<!-- 
Please verify that your application was built in Release mode (e.g. CMAKE_BUILD_TYPE=Release) before reporting performance bottlenecks.
-->

## Performance Bottleneck Description

*Describe the performance issue. What degrades? (e.g., high memory consumption, high frame drop rate, seek lag, CPU spikes).*

---

## Test Environment

*Please specify the context of playback when the bottleneck occurs:*
- **OS & Graphics Driver**: [e.g. Windows 11, NVIDIA Driver 546.33]
- **CPU & GPU**: [e.g. AMD Ryzen 7 5800X, Intel UHD Graphics 770]
- **Media Resolution & Framerate**: [e.g., 4K @ 60fps, 1080p @ 120fps]
- **Video & Audio Codecs**: [e.g., H.264 (AVC) + AAC, HEVC + FLAC]
- **Hardware Decoding Status**: [e.g., D3D11VA active, VAAPI active, or Software-only decoding]

---

## Performance Metrics

*Please supply measurements relative to the project KPIs, if possible:*
- **Seek Latency (Target: < 80ms)**: [e.g., Seeking takes ~250ms]
- **Audio-Video Drift (Target: < 10ms)**: [e.g., Audio-Video sync drifts to 45ms after 5 minutes]
- **CPU Usage**: [e.g., 45% CPU core usage]
- **Memory Footprint**: [e.g., RAM usage grows steadily up to 1.5 GB]
- **GPU Usage / Copy Overhead**: [e.g., YUV-to-RGB upload limits UI rendering]

---

## Steps to Reproduce

*List the setup steps to trigger the degradation:*
- Open the player.
- Ingest a file of type `<specify-format>`.
- Perform actions like `<rapid seeks / loop playback / resizing window>`.
- Observe the lag/metrics.

---

## Profiling and Benchmark Data

*Attach any profiles, call stacks, trace logs, or benchmark results (e.g., from Valgrind/Callgrind, Tracy, RenderDoc, Visual Studio Profiler, or gprof):*

```text
[Paste profile logs or benchmark dumps here]
```

---

## Proposed Optimizations (Optional)

*If you have suggestions to solve the performance issue, please share them (e.g. increasing DecodedFrameQueue capacity, using a different SDL texture format, utilizing direct GPU memory maps).*
