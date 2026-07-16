# Recommended GitHub Labels

To effectively manage and triage issues and pull requests, we recommend setting up the following labels in the **NaikAVPlayer** repository.

---

## 1. Type Labels
These categorize the nature of the issue or pull request.

| Label Name | Recommended Color | Description |
| :--- | :---: | :--- |
| `bug` | `#d73a4a` | Something isn't working as expected (crashes, leaks, freezes) |
| `enhancement` | `#a2eeef` | New features, player HUD updates, or functional additions |
| `performance` | `#8f14e9` | High CPU/GPU usage, memory leaks, sync drift, or seek delay |
| `refactor` | `#5319e7` | Code restructurings, queue cleanup, thread organization |
| `documentation` | `#0075ca` | README updates, header documentation, manual/build guides |
| `testing` | `#cfd3d7` | Adding, updating, or fixing tests and coverage configurations |

---

## 2. Platform Labels
Since NaikAVPlayer is a native media engine, platform-specific bugs and compilation requirements arise.

| Label Name | Recommended Color | Description |
| :--- | :---: | :--- |
| `platform:windows` | `#1d76db` | Specific to Windows OS (MSVC, MinGW, D3D11VA, DXVA2) |
| `platform:linux` | `#fef2c0` | Specific to Linux OS (GCC, GTK3, VAAPI, V4L2M2M) |
| `platform:macos` | `#e4e669` | Specific to macOS OS (Apple Clang, Metal, AVFoundation) |
| `platform:rpi` | `#d81b60` | Specific to Raspberry Pi single-board builds |

---

## 3. Subsystem Labels
These help maintainers direct reviews to the correct subsystem experts.

| Label Name | Recommended Color | Description |
| :--- | :---: | :--- |
| `subsystem:decoder` | `#006b75` | Related to AudioDecoder, VideoDecoder, or HW fallback logic |
| `subsystem:player` | `#bfdadc` | Related to PlayerController, AV master clock synchronization |
| `subsystem:ui` | `#fef2c0` | Related to Dear ImGui overlay rendering, vector icons, HUD |
| `subsystem:build` | `#0052cc` | Related to CMake configuration files or third-party deps |

---

## 4. Triage & Status Labels
Use these to organize issues by urgency, contributor accessibility, or state.

| Label Name | Recommended Color | Description |
| :--- | :---: | :--- |
| `good first issue` | `#7057ff` | Good for newcomers (low barrier to entry) |
| `help wanted` | `#008672` | Extra attention is needed from the community |
| `question` | `#d876e3` | Support inquiries or general questions about build/design |
| `duplicate` | `#cfd3d7` | This issue or PR already exists elsewhere |
| `wontfix` | `#ffffff` | Work that will not be pursued (invalid, out of scope, expected) |
| `stale` | `#6c757d` | Inactive issues or PRs that will close automatically |
