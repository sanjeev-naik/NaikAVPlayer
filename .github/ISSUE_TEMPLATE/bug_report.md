---
name: Bug Report 🐛
about: Report an unexpected behavior, crash, or freeze in NaikAVPlayer.
title: '[Bug] '
labels: bug
assignees: ''
---

<!-- 
Before opening a bug report, please ensure you are running the latest version of the main branch and have checked existing issue reports. 
-->

## Bug Description

*A clear and concise description of what the bug is.*

---

## Environment Details

*Please fill in the system configuration details below:*
- **OS**: [e.g., Windows 11, Ubuntu 22.04 LTS, macOS Sonoma]
- **NaikAVPlayer Version / Commit Hash**: [e.g., v1.2.0 or commit hash 9a8b7c]
- **FFmpeg Version**: [e.g., FFmpeg 5.x, 6.0, 7.0]
- **SDL Version**: [e.g., SDL 2.28.5 or 3.x]
- **GPU & Drivers**: [e.g., NVIDIA RTX 4070 Driver 546.33, Intel Iris Xe]
- **Hardware Decoder configuration**: [e.g., Enabled (QSV, CUVID, DXVA2), or Software fallback only]

---

## Steps to Reproduce

*List the precise steps to reproduce the behavior:*
1. Start `NaikAVPlayer`
2. Load a media file: [e.g., a specific .mp4 / .mkv / custom stream - please specify specs (4K, 1080p, H264, HEVC) if relevant]
3. Perform action: [e.g., Seek to 00:30, toggle Loop Mode, mute audio]
4. Observe the failure...

---

## Expected Behavior

*A clear and concise description of what you expected to happen.*

---

## Actual Behavior

*A clear and concise description of what actually happens (e.g., frame freezes, application crashes with stack trace, audio-video out of sync by >100ms).*

---

## Root Cause (Optional)

*If you have analyzed the codebase and know the root cause, please explain it here (e.g. queue capacity lock in ThreadSafeQueue, missing texture resource freeing).*

---

## Proposed Solution (Optional)

*Outline a suggested code change or patch to fix the bug.*

---

## Logs and Media (Optional)

*If applicable, please attach:*
- Terminal/console output log files.
- Screenshots or screen recordings showing the issue.
- Link to a sample media file that reproduces the crash.
