#include "ui/PlayerUI.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <imgui.h>
#include <iostream>

PlayerUI::PlayerUI(PlayerController &controller)
    : m_controller(controller), m_uiVolume(5.0f), m_isMuted(false),
      m_seekDragActive(false), m_seekDragValue(0.0f), m_savedVolume(5.0f),
      m_showDiagnostics(false), m_lastMouseMoveTime(0.0),
      m_controlsVisible(true), m_showLoadFileDialog(false), m_mainFont(nullptr),
      m_titleFont(nullptr), m_hudFont(nullptr), m_videoFPS(0.0) {
  m_filePathBuffer[0] = '\0';
}

PlayerUI::~PlayerUI() {}

void PlayerUI::init() {
  ImGuiIO &io = ImGui::GetIO();

  // Candidate paths to search for the bundled Noto Sans open-source fonts
  std::string regularPaths[] = {
      "assets/fonts/NotoSans-Regular.ttf",
      "../assets/fonts/NotoSans-Regular.ttf",
      "../../assets/fonts/NotoSans-Regular.ttf",
      "./NotoSans-Regular.ttf"
#ifdef __linux__
      ,
      "/usr/local/share/NaikAVPlayer/fonts/NotoSans-Regular.ttf",
      "/usr/share/NaikAVPlayer/fonts/NotoSans-Regular.ttf"
#endif
  };

  std::string boldPaths[] = {
      "assets/fonts/NotoSans-Bold.ttf",
      "../assets/fonts/NotoSans-Bold.ttf",
      "../../assets/fonts/NotoSans-Bold.ttf",
      "./NotoSans-Bold.ttf"
#ifdef __linux__
      ,
      "/usr/local/share/NaikAVPlayer/fonts/NotoSans-Bold.ttf",
      "/usr/share/NaikAVPlayer/fonts/NotoSans-Bold.ttf"
#endif
  };

  std::string foundRegularPath = "";
  auto regularIt =
      std::find_if(std::begin(regularPaths), std::end(regularPaths),
                   [](const std::string &path) {
                     if (FILE *f = std::fopen(path.c_str(), "rb")) {
                       std::fclose(f);
                       return true;
                     }
                     return false;
                   });
  if (regularIt != std::end(regularPaths)) {
    foundRegularPath = *regularIt;
  }

  std::string foundBoldPath = "";
  auto boldIt = std::find_if(std::begin(boldPaths), std::end(boldPaths),
                             [](const std::string &path) {
                               if (FILE *f = std::fopen(path.c_str(), "rb")) {
                                 std::fclose(f);
                                 return true;
                               }
                               return false;
                             });
  if (boldIt != std::end(boldPaths)) {
    foundBoldPath = *boldIt;
  }

  // Setup font config for maximum sharpness and subpixel alignment
  ImFontConfig fontConfig;
  fontConfig.OversampleH = 3;
  fontConfig.OversampleV = 3;
  fontConfig.PixelSnapH =
      true; // Align characters to pixel boundaries for sharp rendering

  // Load Noto Sans Regular if found, which is SIL Open Font licensed
  if (!foundRegularPath.empty()) {
    m_mainFont = io.Fonts->AddFontFromFileTTF(foundRegularPath.c_str(), 20.0f,
                                              &fontConfig);
    m_hudFont = io.Fonts->AddFontFromFileTTF(foundRegularPath.c_str(), 16.0f,
                                             &fontConfig);
  }

  // Load Noto Sans Bold if found, otherwise fall back to regular
  if (!foundBoldPath.empty()) {
    m_titleFont =
        io.Fonts->AddFontFromFileTTF(foundBoldPath.c_str(), 28.0f, &fontConfig);
  } else if (!foundRegularPath.empty()) {
    m_titleFont = io.Fonts->AddFontFromFileTTF(foundRegularPath.c_str(), 28.0f,
                                               &fontConfig);
  }

  applyTheme();
}

void PlayerUI::openMediaFileDialog() {
  if (m_controller.getState() == PlayerState::PLAYING) {
    m_controller.pause();
  }

  if (m_fileDialogCallback) {
    std::string path = m_fileDialogCallback();
    if (!path.empty()) {
      if (m_controller.openFile(path)) {
        m_controller.play();
      }
    }
  } else {
    m_showLoadFileDialog = true;
  }
}

std::string PlayerUI::formatTime(double seconds) {
  int s = static_cast<int>(seconds);
  int h = s / 3600;
  int m = (s % 3600) / 60;
  int sec = s % 60;
  char buf[64];
  if (h > 0) {
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, sec);
  } else {
    std::snprintf(buf, sizeof(buf), "%02d:%02d", m, sec);
  }
  return std::string(buf);
}

void PlayerUI::notifyMouseActivity(double currentSystemTime) {
  m_lastMouseMoveTime = currentSystemTime;
  m_controlsVisible = true;
}

void PlayerUI::registerVideoFrameRendered(double currentSystemTime) {
  m_videoFrameTimes.push_back(currentSystemTime);
}

void PlayerUI::draw(int windowWidth, int windowHeight,
                    double currentSystemTime) {
  // Update video FPS calculation
  while (!m_videoFrameTimes.empty() &&
         currentSystemTime - m_videoFrameTimes.front() > 1.0) {
    m_videoFrameTimes.pop_front();
  }
  if (m_videoFrameTimes.size() > 1) {
    m_videoFPS = (m_videoFrameTimes.size() - 1) /
                 (m_videoFrameTimes.back() - m_videoFrameTimes.front());
  } else {
    m_videoFPS = 0.0;
  }

  PlayerState state = m_controller.getState();
  bool isPlaying = (state == PlayerState::PLAYING);
  bool imguiWantsMouse = ImGui::GetIO().WantCaptureMouse;

  // Add clock offset samples at 10 Hz (every 100ms)
  if (m_showDiagnostics && state == PlayerState::PLAYING &&
      !m_controller.isSeeking() && !m_controller.isCatchingUp()) {
    static double lastSampleTime = 0.0;
    if (currentSystemTime - lastSampleTime >= 0.1) {
      lastSampleTime = currentSystemTime;

      double masterClock = m_controller.getCurrentTime();
      float audioOffset = 0.0f;
      float videoOffset = 0.0f;

      if (m_controller.hasAudio()) {
        audioOffset = static_cast<float>((m_controller.getAudioClock() - masterClock) * 1000.0);
      }
      if (m_controller.hasVideo()) {
        videoOffset = static_cast<float>((m_controller.getVideoClock() - masterClock) * 1000.0);
      }

      ClockOffsetSample sample = {currentSystemTime, audioOffset, videoOffset};
      m_offsetHistory.push_back(sample);

      // Keep last 20 seconds of history (200 samples at 10 Hz)
      while (m_offsetHistory.size() > 200) {
        m_offsetHistory.pop_front();
      }
    }
  }

  // Keep controls visible if playback is paused or user is interacting with the
  // GUI
  if (!isPlaying || imguiWantsMouse || m_showLoadFileDialog) {
    m_controlsVisible = true;
  } else {
    // Hide controls after 2.5 seconds of mouse inactivity
    if (currentSystemTime - m_lastMouseMoveTime > 2.5) {
      m_controlsVisible = false;
    }
  }

  // Apply main font if loaded
  if (m_mainFont)
    ImGui::PushFont(m_mainFont);

  // 1. Welcome / Instruction Screen (empty state)
  if (state == PlayerState::UNINITIALIZED) {
    drawWelcomeHUD(windowWidth, windowHeight);
  }

  // 1b. Real-time Audio Visualizer (automatically shown for audio-only media)
  if (state != PlayerState::UNINITIALIZED && m_controller.hasAudio() && !m_controller.hasVideo()) {
    drawAudioVisualizer(windowWidth, windowHeight, currentSystemTime);
  }

  // 2. Top Title Bar HUD
  if (state != PlayerState::UNINITIALIZED && m_controlsVisible) {
    drawTitleBar(windowWidth, windowHeight);
  }

  // 3. Diagnostics Info HUD
  if (state != PlayerState::UNINITIALIZED && m_showDiagnostics) {
    drawDiagnosticsHUD(windowWidth, windowHeight);
  }

  // 3b. Audio Processing Panel (EQ/compressor/limiter/crossover/loudness)
  if (state != PlayerState::UNINITIALIZED && m_showAudioSettings) {
    drawAudioSettingsPanel(windowWidth, windowHeight);
  }

  // 4. Bottom Controls Bar Dock
  if (state != PlayerState::UNINITIALIZED && m_controlsVisible) {
    drawControlsBar(windowWidth, windowHeight);
  }

  // 4b. On-screen Toast Notification Banner (for screenshots, mute, etc.)
  drawToastNotification(windowWidth, windowHeight, currentSystemTime);

  if (m_mainFont)
    ImGui::PopFont();

  // 5. File selection modal popup (always accessible)
  if (m_showLoadFileDialog) {
    ImGui::OpenPopup("Load File Modal");
  }

  if (ImGui::BeginPopupModal("Load File Modal", &m_showLoadFileDialog,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Enter the path to the media file:");
    ImGui::Spacing();

    ImGui::InputText("File Path", m_filePathBuffer, sizeof(m_filePathBuffer));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Load & Play", ImVec2(120, 0))) {
      if (m_controller.openFile(m_filePathBuffer)) {
        m_controller.play();
        m_showLoadFileDialog = false;
      } else {
        ImGui::OpenPopup("Load Error");
      }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      m_showLoadFileDialog = false;
    }

    // Mini warning nested popup on load failures
    if (ImGui::BeginPopup("Load Error")) {
      ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                         "Failed to load file!");
      ImGui::Text("Please verify the file path is correct.");
      if (ImGui::Button("Close")) {
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }

    ImGui::EndPopup();
  }
}

bool PlayerUI::drawIconButton(const char *str_id, IconType icon, ImVec2 size) {
  ImGui::PushID(str_id);
  bool clicked = ImGui::Button("##btn", size);

  ImVec2 minPos = ImGui::GetItemRectMin();
  ImVec2 maxPos = ImGui::GetItemRectMax();
  ImVec2 center =
      ImVec2((minPos.x + maxPos.x) * 0.5f, (minPos.y + maxPos.y) * 0.5f);

  ImU32 color;
  if (ImGui::IsItemActive()) {
    color = ImGui::GetColorU32(ImGuiCol_ButtonActive);
  } else if (ImGui::IsItemHovered()) {
    color = ImGui::GetColorU32(
        ImVec4(0.00f, 0.83f, 0.88f, 1.00f)); // Neon cyan hover glow
  } else {
    color = ImGui::GetColorU32(ImGuiCol_Text);
  }

  ImDrawList *drawList = ImGui::GetWindowDrawList();
  float padding = 6.0f;
  float w = (maxPos.x - minPos.x) - padding * 2.0f;
  float h = (maxPos.y - minPos.y) - padding * 2.0f;
  float sz = std::min(w, h);

  switch (icon) {
  case IconType::Play: {
    float r = sz * 0.5f;
    ImVec2 p1(center.x - r * 0.35f, center.y - r * 0.6f);
    ImVec2 p2(center.x - r * 0.35f, center.y + r * 0.6f);
    ImVec2 p3(center.x + r * 0.55f, center.y);
    drawList->AddTriangleFilled(p1, p2, p3, color);
    break;
  }
  case IconType::Pause: {
    float r = sz * 0.5f;
    float barW = r * 0.3f;
    float barH = r * 1.1f;
    float gap = r * 0.35f;
    ImVec2 p1_min(center.x - gap * 0.5f - barW, center.y - barH * 0.5f);
    ImVec2 p1_max(center.x - gap * 0.5f, center.y + barH * 0.5f);
    ImVec2 p2_min(center.x + gap * 0.5f, center.y - barH * 0.5f);
    ImVec2 p2_max(center.x + gap * 0.5f + barW, center.y + barH * 0.5f);
    drawList->AddRectFilled(p1_min, p1_max, color, 1.5f);
    drawList->AddRectFilled(p2_min, p2_max, color, 1.5f);
    break;
  }
  case IconType::Stop: {
    float r = sz * 0.5f;
    float stopSz = r * 0.9f;
    ImVec2 p_min(center.x - stopSz * 0.5f, center.y - stopSz * 0.5f);
    ImVec2 p_max(center.x + stopSz * 0.5f, center.y + stopSz * 0.5f);
    drawList->AddRectFilled(p_min, p_max, color, 1.5f);
    break;
  }
  case IconType::SeekBackward: {
    float r = sz * 0.5f;
    ImVec2 p1(center.x - r * 0.8f, center.y);
    ImVec2 p2(center.x - r * 0.1f, center.y - r * 0.5f);
    ImVec2 p3(center.x - r * 0.1f, center.y + r * 0.5f);
    ImVec2 p4(center.x - r * 0.1f, center.y);
    ImVec2 p5(center.x + r * 0.6f, center.y - r * 0.5f);
    ImVec2 p6(center.x + r * 0.6f, center.y + r * 0.5f);
    drawList->AddTriangleFilled(p1, p2, p3, color);
    drawList->AddTriangleFilled(p4, p5, p6, color);
    break;
  }
  case IconType::SeekForward: {
    float r = sz * 0.5f;
    ImVec2 p1(center.x - r * 0.6f, center.y - r * 0.5f);
    ImVec2 p2(center.x - r * 0.6f, center.y + r * 0.5f);
    ImVec2 p3(center.x + r * 0.1f, center.y);
    ImVec2 p4(center.x - r * 0.1f, center.y - r * 0.5f);
    ImVec2 p5(center.x - r * 0.1f, center.y + r * 0.5f);
    ImVec2 p6(center.x + r * 0.6f, center.y);
    drawList->AddTriangleFilled(p1, p2, p3, color);
    drawList->AddTriangleFilled(p4, p5, p6, color);
    break;
  }
  case IconType::Folder: {
    float r = sz * 0.5f;
    float folderW = r * 1.3f;
    float folderH = r * 0.9f;
    ImVec2 p_min(center.x - folderW * 0.5f, center.y - folderH * 0.2f);
    ImVec2 p_max(center.x + folderW * 0.5f, center.y + folderH * 0.6f);
    drawList->AddRectFilled(p_min, p_max, color, 1.5f);
    ImVec2 tab_min(center.x - folderW * 0.5f, center.y - folderH * 0.5f);
    ImVec2 tab_max(center.x - folderW * 0.1f, center.y - folderH * 0.2f);
    drawList->AddRectFilled(tab_min, tab_max, color, 1.0f);
    break;
  }
  case IconType::VolumeMute: {
    float r = sz * 0.5f;
    float spkH = r * 0.55f;
    drawList->AddRectFilled(ImVec2(center.x - r * 0.6f, center.y - spkH * 0.5f),
                            ImVec2(center.x - r * 0.3f, center.y + spkH * 0.5f),
                            color, 1.0f);
    ImVec2 pts[4] = {ImVec2(center.x - r * 0.3f, center.y - spkH * 0.5f),
                     ImVec2(center.x, center.y - r * 0.55f),
                     ImVec2(center.x, center.y + r * 0.55f),
                     ImVec2(center.x - r * 0.3f, center.y + spkH * 0.5f)};
    drawList->AddConvexPolyFilled(pts, 4, color);
    drawList->AddLine(ImVec2(center.x + r * 0.25f, center.y - r * 0.25f),
                      ImVec2(center.x + r * 0.65f, center.y + r * 0.25f), color,
                      1.5f);
    drawList->AddLine(ImVec2(center.x + r * 0.65f, center.y - r * 0.25f),
                      ImVec2(center.x + r * 0.25f, center.y + r * 0.25f), color,
                      1.5f);
    break;
  }
  case IconType::VolumeHigh: {
    float r = sz * 0.5f;
    float spkH = r * 0.55f;
    drawList->AddRectFilled(ImVec2(center.x - r * 0.7f, center.y - spkH * 0.5f),
                            ImVec2(center.x - r * 0.4f, center.y + spkH * 0.5f),
                            color, 1.0f);
    ImVec2 pts[4] = {ImVec2(center.x - r * 0.4f, center.y - spkH * 0.5f),
                     ImVec2(center.x - r * 0.1f, center.y - r * 0.55f),
                     ImVec2(center.x - r * 0.1f, center.y + r * 0.55f),
                     ImVec2(center.x - r * 0.4f, center.y + spkH * 0.5f)};
    drawList->AddConvexPolyFilled(pts, 4, color);
    drawList->AddLine(ImVec2(center.x + r * 0.15f, center.y - r * 0.25f),
                      ImVec2(center.x + r * 0.3f, center.y), color, 1.5f);
    drawList->AddLine(ImVec2(center.x + r * 0.3f, center.y),
                      ImVec2(center.x + r * 0.15f, center.y + r * 0.25f), color,
                      1.5f);
    drawList->AddLine(ImVec2(center.x + r * 0.4f, center.y - r * 0.45f),
                      ImVec2(center.x + r * 0.6f, center.y), color, 1.5f);
    drawList->AddLine(ImVec2(center.x + r * 0.6f, center.y),
                      ImVec2(center.x + r * 0.4f, center.y + r * 0.45f), color,
                      1.5f);
    break;
  }
  case IconType::Loop: {
    constexpr float kPi = 3.14159265358979323846f;
    float r = sz * 0.42f;
    float thickness = std::max(1.5f, sz * 0.12f);
    float startAngle = -kPi * 0.15f;
    float endAngle = kPi * 1.65f;
    drawList->PathArcTo(center, r, startAngle, endAngle, 24);
    drawList->PathStroke(color, ImDrawFlags_None, thickness);

    // Arrowhead at the leading end of the arc, tangent to the circle
    ImVec2 tipPos(center.x + r * cosf(endAngle), center.y + r * sinf(endAngle));
    ImVec2 dir(-sinf(endAngle), cosf(endAngle));
    ImVec2 perp(-dir.y, dir.x);
    float headSize = r * 0.55f;
    ImVec2 tip(tipPos.x + dir.x * headSize * 0.5f, tipPos.y + dir.y * headSize * 0.5f);
    ImVec2 baseCenter(tipPos.x - dir.x * headSize * 0.5f, tipPos.y - dir.y * headSize * 0.5f);
    ImVec2 base1(baseCenter.x + perp.x * headSize * 0.5f, baseCenter.y + perp.y * headSize * 0.5f);
    ImVec2 base2(baseCenter.x - perp.x * headSize * 0.5f, baseCenter.y - perp.y * headSize * 0.5f);
    drawList->AddTriangleFilled(tip, base1, base2, color);
    break;
  }
  }

  ImGui::PopID();
  return clicked;
}

void PlayerUI::drawWelcomeHUD(int windowWidth, int windowHeight) {
  // Centered modern onboarding panel
  float cardWidth = 650.0f;
  float cardHeight = 360.0f;
  ImGui::SetNextWindowPos(ImVec2((windowWidth - cardWidth) * 0.5f,
                                 (windowHeight - cardHeight) * 0.5f));
  ImGui::SetNextWindowSize(ImVec2(cardWidth, cardHeight));

  ImGui::Begin("Welcome HUD", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoSavedSettings);

  // Draw vector icon (Play button inside a sleek glowing circle)
  ImDrawList *drawList = ImGui::GetWindowDrawList();
  ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();

  ImVec2 iconCenter =
      ImVec2(cursorScreenPos.x + cardWidth * 0.5f, cursorScreenPos.y + 70.0f);
  float iconRadius = 36.0f;

  // Circle border
  drawList->AddCircle(iconCenter, iconRadius, IM_COL32(0, 212, 224, 200), 64,
                      3.0f);
  // Outer soft glow
  drawList->AddCircle(iconCenter, iconRadius + 4.0f, IM_COL32(0, 212, 224, 50),
                      64, 1.5f);

  // Triangle (Play symbol) inside the circle
  ImVec2 p1 = ImVec2(iconCenter.x - 10.0f, iconCenter.y - 16.0f);
  ImVec2 p2 = ImVec2(iconCenter.x - 10.0f, iconCenter.y + 16.0f);
  ImVec2 p3 = ImVec2(iconCenter.x + 18.0f, iconCenter.y);
  drawList->AddTriangleFilled(p1, p2, p3, IM_COL32(30, 136, 229, 255));

  ImGui::Dummy(ImVec2(0.0f, 120.0f)); // Push cursor below the drawing

  // Headline text
  if (m_titleFont)
    ImGui::PushFont(m_titleFont);
  const char *titleText = "NaikAVPlayer";
  float titleWidth = ImGui::CalcTextSize(titleText).x;
  ImGui::SetCursorPosX((cardWidth - titleWidth) * 0.5f);
  ImGui::TextColored(ImVec4(0.00f, 0.83f, 0.88f, 1.00f), "%s", titleText);
  if (m_titleFont)
    ImGui::PopFont();

  ImGui::Spacing();

  // Subtext description
  const char *subtext =
      "Drag & Drop video files here or browse to start playing";
  float subTextWidth = ImGui::CalcTextSize(subtext).x;
  ImGui::SetCursorPosX((cardWidth - subTextWidth) * 0.5f);
  ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.75f, 0.90f), "%s", subtext);

  ImGui::Spacing();
  ImGui::Spacing();

  // Large Browse Button
  float btnWidth = 180.0f;
  float btnHeight = 40.0f;
  ImGui::SetCursorPosX((cardWidth - btnWidth) * 0.5f);
  if (ImGui::Button("Open Media File", ImVec2(btnWidth, btnHeight))) {
    openMediaFileDialog();
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // Shortcuts cheat-sheet
  if (m_hudFont)
    ImGui::PushFont(m_hudFont);

  auto renderKey = [](const char *key, const char *desc) {
    ImGui::TextDisabled(" [");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(ImVec4(0.00f, 0.83f, 0.88f, 0.90f), "%s", key);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextDisabled("] ");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Text("%s   ", desc);
    ImGui::SameLine(0.0f, 0.0f);
  };

  // Centering the shortcut display
  float rowWidth =
      ImGui::CalcTextSize(
          " [Space] Play/Pause    [<- / ->] Seek 10s    [ [ / ] ] Speed    [F11] Fullscreen    [M] Mute    [Up/Down] Vol    [S] Screenshot    [L] Loop    [Esc] Exit")
          .x;
  ImGui::SetCursorPosX((cardWidth - rowWidth) * 0.5f);

  renderKey("Space", "Play/Pause");
  renderKey("<- / ->", "Seek 10s");
  renderKey("[ / ]", "Speed -/+");
  renderKey("F11", "Fullscreen");
  renderKey("M", "Mute");
  renderKey("Up/Down", "Vol");
  renderKey("S", "Screenshot");
  renderKey("L", "Loop");
  renderKey("Esc", "Exit");

  ImGui::NewLine(); // terminate the SameLine loop

  if (m_hudFont)
    ImGui::PopFont();

  ImGui::End();
}

void PlayerUI::drawAudioVisualizer(int windowWidth, int windowHeight, double currentSystemTime) {
  // Ensure visualizer state buffers are properly sized
  if (m_visualizerSmoothBands.size() != static_cast<size_t>(kNumVisualizerBands)) {
    m_visualizerSmoothBands.assign(kNumVisualizerBands, 0.0f);
    m_visualizerPeakCaps.assign(kNumVisualizerBands, 0.0f);
    m_visualizerPeakVelocities.assign(kNumVisualizerBands, 0.0f);
  }

  // Calculate delta time for physics / animation
  float dt = 0.016f;
  if (m_visualizerLastTime > 0.0 && currentSystemTime > m_visualizerLastTime) {
    dt = static_cast<float>(currentSystemTime - m_visualizerLastTime);
    dt = std::clamp(dt, 0.001f, 0.1f);
  }
  m_visualizerLastTime = currentSystemTime;

  bool isPlaying = (m_controller.getState() == PlayerState::PLAYING);

  // Fetch real-time spectrum and waveform from PlayerController / SpectrumAnalyzer
  std::vector<float> spectrumDb = m_controller.getSpectrumMagnitudesDb();
  std::vector<float> waveform = m_controller.getWaveformSamples();

  // Logarithmic frequency binning: map 512 raw FFT bins into kNumVisualizerBands
  const size_t numBins = spectrumDb.size();
  for (int i = 0; i < kNumVisualizerBands; ++i) {
    float targetLevel = 0.0f;
    if (isPlaying && numBins > 0) {
      float fracStart = std::pow(static_cast<float>(i) / kNumVisualizerBands, 2.2f);
      float fracEnd = std::pow(static_cast<float>(i + 1) / kNumVisualizerBands, 2.2f);
      size_t bStart = std::min(static_cast<size_t>(fracStart * numBins), numBins - 1);
      size_t bEnd = std::clamp(static_cast<size_t>(fracEnd * numBins) + 1, bStart + 1, numBins);

      float maxDb = naikav::dsp::SpectrumAnalyzer::kFloorDb;
      for (size_t b = bStart; b < bEnd; ++b) {
        if (spectrumDb[b] > maxDb) {
          maxDb = spectrumDb[b];
        }
      }

      // Normalize from [-75dB, 0dB] to [0.0, 1.0]
      constexpr float kFloor = -75.0f;
      float norm = std::clamp((maxDb - kFloor) / (-kFloor), 0.0f, 1.0f);
      // Gentle frequency equalization curve: slight boost on low bass and high air
      float freqCurve = 1.0f + 0.15f * std::cos(static_cast<float>(i) / kNumVisualizerBands * 3.14159f);
      targetLevel = std::clamp(norm * freqCurve, 0.0f, 1.0f);
    }

    // Smooth response with fast attack and natural decay
    float attackSpeed = 26.0f;
    float decaySpeed = 10.0f;
    float speed = (targetLevel > m_visualizerSmoothBands[i]) ? attackSpeed : decaySpeed;
    m_visualizerSmoothBands[i] += (targetLevel - m_visualizerSmoothBands[i]) * std::clamp(dt * speed, 0.0f, 1.0f);

    // Peak cap physics (falling gravity)
    if (m_visualizerSmoothBands[i] >= m_visualizerPeakCaps[i]) {
      m_visualizerPeakCaps[i] = m_visualizerSmoothBands[i];
      m_visualizerPeakVelocities[i] = 0.0f;
    } else {
      m_visualizerPeakVelocities[i] += 1.8f * dt;
      m_visualizerPeakCaps[i] -= m_visualizerPeakVelocities[i] * dt;
      if (m_visualizerPeakCaps[i] < 0.0f) m_visualizerPeakCaps[i] = 0.0f;
    }
  }

  // Calculate aggregate energy for bass and brightness
  const int bassBandsCount = std::min(8, kNumVisualizerBands);
  float bassEnergy = std::accumulate(m_visualizerSmoothBands.begin(),
                                     m_visualizerSmoothBands.begin() + bassBandsCount,
                                     0.0f) / static_cast<float>(bassBandsCount);


  // Palette color interpolator
  auto getThemeColor = [this](float t, float alpha = 1.0f) -> ImVec4 {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (m_visualizerTheme) {
    case VisualizerTheme::SunsetFire:
      // Golden yellow (1.0, 0.75, 0.1) -> Fiery Orange (1.0, 0.35, 0.05) -> Hot Pink (1.0, 0.1, 0.45)
      if (t < 0.5f) {
        float f = t * 2.0f;
        return ImVec4(1.0f, 0.75f - f * 0.40f, 0.10f - f * 0.05f, alpha);
      } else {
        float f = (t - 0.5f) * 2.0f;
        return ImVec4(1.0f, 0.35f - f * 0.25f, 0.05f + f * 0.40f, alpha);
      }
    case VisualizerTheme::NeonEmerald:
      // Mint (0.2, 1.0, 0.7) -> Electric Green (0.0, 0.9, 0.35) -> Deep Teal (0.0, 0.6, 0.65)
      if (t < 0.5f) {
        float f = t * 2.0f;
        return ImVec4(0.20f - f * 0.20f, 1.00f - f * 0.10f, 0.70f - f * 0.35f, alpha);
      } else {
        float f = (t - 0.5f) * 2.0f;
        return ImVec4(0.0f, 0.90f - f * 0.30f, 0.35f + f * 0.30f, alpha);
      }
    case VisualizerTheme::ElectricViolet:
      // Electric Aqua (0.3, 0.8, 1.0) -> Violet (0.65, 0.3, 1.0) -> Vivid Purple (0.9, 0.15, 1.0)
      if (t < 0.5f) {
        float f = t * 2.0f;
        return ImVec4(0.30f + f * 0.35f, 0.80f - f * 0.50f, 1.0f, alpha);
      } else {
        float f = (t - 0.5f) * 2.0f;
        return ImVec4(0.65f + f * 0.25f, 0.30f - f * 0.15f, 1.0f, alpha);
      }
    case VisualizerTheme::Cyberpunk:
    default:
      // Cyan (0.0, 0.88, 0.95) -> Deep Blue (0.3, 0.4, 1.0) -> Magenta/Pink (0.98, 0.15, 0.7)
      if (t < 0.5f) {
        float f = t * 2.0f;
        return ImVec4(f * 0.30f, 0.88f - f * 0.48f, 0.95f + f * 0.05f, alpha);
      } else {
        float f = (t - 0.5f) * 2.0f;
        return ImVec4(0.30f + f * 0.68f, 0.40f - f * 0.25f, 1.0f - f * 0.30f, alpha);
      }
    }
  };

  // Setup full-viewport canvas for visualizer drawing
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(windowWidth), static_cast<float>(windowHeight)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::Begin("AudioVisualizerCanvas", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
               ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
               ImGuiWindowFlags_NoSavedSettings);

  ImDrawList* drawList = ImGui::GetWindowDrawList();
  const float cx = windowWidth * 0.5f;
  const float cy = windowHeight * 0.5f;

  // 1. Ambient Background Glow reacting to bass
  float glowRadius = 160.0f + bassEnergy * 150.0f;
  ImVec4 glowColor = getThemeColor(0.2f, 0.08f + bassEnergy * 0.12f);
  drawList->AddCircleFilled(ImVec2(cx, cy), glowRadius, ImGui::GetColorU32(glowColor), 48);

  // 2. Render Selected Visualizer Mode
  switch (m_visualizerMode) {
  case VisualizerMode::NeonBars: {
    // Mode 0: Modern Equalizer Bars with falling peak caps and reflections
    const float availableW = std::max(200.0f, static_cast<float>(windowWidth) - 100.0f);
    const float barSpacing = std::clamp(availableW / kNumVisualizerBands, 4.0f, 18.0f);
    const float barW = std::max(2.0f, barSpacing - 2.0f);
    const float totalW = kNumVisualizerBands * barSpacing;
    const float startX = (windowWidth - totalW) * 0.5f;
    const float baseY = cy + std::min(130.0f, windowHeight * 0.22f);
    const float maxHeight = std::min(240.0f, windowHeight * 0.40f);

    for (int i = 0; i < kNumVisualizerBands; ++i) {
      float tFrac = static_cast<float>(i) / (kNumVisualizerBands - 1);
      float barX = startX + i * barSpacing;
      float barH = std::max(3.0f, m_visualizerSmoothBands[i] * maxHeight);

      ImVec4 colTop = getThemeColor(tFrac, 1.0f);
      ImVec4 colBottom = getThemeColor(tFrac * 0.4f, 0.35f);
      ImU32 u32Top = ImGui::GetColorU32(colTop);
      ImU32 u32Bottom = ImGui::GetColorU32(colBottom);

      // Main Equalizer Bar (multi-color gradient)
      drawList->AddRectFilledMultiColor(
          ImVec2(barX, baseY - barH),
          ImVec2(barX + barW, baseY),
          u32Top, u32Top, u32Bottom, u32Bottom);

      // Top glowing cap highlight
      drawList->AddRectFilled(
          ImVec2(barX, baseY - barH),
          ImVec2(barX + barW, baseY - barH + 2.0f),
          ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.90f)), 1.0f);

      // Falling Peak Cap
      float peakH = m_visualizerPeakCaps[i] * maxHeight;
      if (peakH > barH + 2.0f) {
        drawList->AddRectFilled(
            ImVec2(barX, baseY - peakH - 2.0f),
            ImVec2(barX + barW, baseY - peakH),
            ImGui::GetColorU32(ImVec4(colTop.x, colTop.y, colTop.z, 0.95f)), 1.0f);
      }

      // Floor Reflection (downward fading gradient)
      float reflH = barH * 0.35f;
      ImU32 u32ReflStart = ImGui::GetColorU32(ImVec4(colTop.x, colTop.y, colTop.z, 0.25f));
      ImU32 u32ReflEnd = ImGui::GetColorU32(ImVec4(colTop.x, colTop.y, colTop.z, 0.0f));
      drawList->AddRectFilledMultiColor(
          ImVec2(barX, baseY + 2.0f),
          ImVec2(barX + barW, baseY + 2.0f + reflH),
          u32ReflStart, u32ReflStart, u32ReflEnd, u32ReflEnd);
    }

    // Horizon line under bars
    drawList->AddLine(
        ImVec2(startX - 20.0f, baseY + 1.0f),
        ImVec2(startX + totalW + 20.0f, baseY + 1.0f),
        ImGui::GetColorU32(ImVec4(0.3f, 0.35f, 0.45f, 0.4f)), 1.0f);
    break;
  }

  case VisualizerMode::SmoothWave: {
    // Mode 1: Glowing Continuous Waveform
    const float leftX = 60.0f;
    const float rightX = windowWidth - 60.0f;
    const float waveW = rightX - leftX;
    const float waveY = cy + 40.0f;
    const float waveAmp = std::min(150.0f, windowHeight * 0.25f);
    const size_t waveCount = waveform.empty() ? kNumVisualizerBands : std::min(waveform.size(), size_t(256));

    std::vector<ImVec2> pts;
    pts.reserve(waveCount);

    for (size_t i = 0; i < waveCount; ++i) {
      float t = static_cast<float>(i) / (waveCount - 1);
      float x = leftX + t * waveW;
      float sample = 0.0f;
      if (isPlaying) {
        if (!waveform.empty()) {
          sample = waveform[i % waveform.size()];
        } else {
          int bandIdx = static_cast<int>(t * (kNumVisualizerBands - 1));
          sample = m_visualizerSmoothBands[bandIdx] * std::sin(t * 12.0f + static_cast<float>(currentSystemTime) * 4.0f);
        }
      }
      float y = waveY - sample * waveAmp;
      pts.push_back(ImVec2(x, y));
    }

    // Draw translucent gradient fill under the wave
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
      float t = static_cast<float>(i) / pts.size();
      ImVec4 c = getThemeColor(t, 0.20f);
      ImU32 u32C = ImGui::GetColorU32(c);
      ImU32 u32Fade = ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.0f));
      drawList->AddTriangleFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, waveY + waveAmp * 0.5f), u32Fade);
      drawList->AddTriangleFilled(pts[i], ImVec2(pts[i + 1].x, waveY + waveAmp * 0.5f), ImVec2(pts[i].x, waveY + waveAmp * 0.5f), u32C);
    }

    // Draw glowing polyline
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
      float t = static_cast<float>(i) / pts.size();
      ImVec4 cGlow = getThemeColor(t, 0.40f);
      ImVec4 cCore = getThemeColor(t, 0.95f);
      // Outer glow line
      drawList->AddLine(pts[i], pts[i + 1], ImGui::GetColorU32(cGlow), 5.0f);
      // Core bright line
      drawList->AddLine(pts[i], pts[i + 1], ImGui::GetColorU32(cCore), 2.0f);
    }
    break;
  }

  case VisualizerMode::RadialDisc: {
    // Mode 2: Circular 360-degree Radial Visualizer
    const float radiusBase = std::min(80.0f, windowHeight * 0.16f);
    const float maxSpike = std::min(130.0f, windowHeight * 0.22f);
    constexpr float kTwoPi = 6.2831853f;

    // Draw rotating vinyl disc grooves
    float spinAngle = static_cast<float>(currentSystemTime) * (isPlaying ? 0.8f : 0.0f);
    for (int r = 1; r <= 4; ++r) {
      float gr = radiusBase * (0.3f + r * 0.16f);
      drawList->AddCircle(ImVec2(cx, cy), gr, ImGui::GetColorU32(ImVec4(0.2f, 0.22f, 0.28f, 0.35f)), 36, 1.0f);
    }

    // Center pulsating disc
    float pulseR = radiusBase + bassEnergy * 15.0f;
    drawList->AddCircleFilled(ImVec2(cx, cy), pulseR, ImGui::GetColorU32(ImVec4(0.08f, 0.09f, 0.12f, 0.90f)), 48);
    drawList->AddCircle(ImVec2(cx, cy), pulseR, ImGui::GetColorU32(getThemeColor(0.5f, 0.85f)), 48, 2.5f);

    // Radiating 360-degree frequency spikes
    for (int i = 0; i < kNumVisualizerBands; ++i) {
      float angle = spinAngle + (static_cast<float>(i) / kNumVisualizerBands) * kTwoPi;
      float cosA = std::cos(angle);
      float sinA = std::sin(angle);

      float spikeLen = m_visualizerSmoothBands[i] * maxSpike;
      float p1x = cx + cosA * (pulseR + 4.0f);
      float p1y = cy + sinA * (pulseR + 4.0f);
      float p2x = cx + cosA * (pulseR + 4.0f + spikeLen);
      float p2y = cy + sinA * (pulseR + 4.0f + spikeLen);

      float t = static_cast<float>(i) / kNumVisualizerBands;
      ImVec4 col = getThemeColor(t, 0.90f);
      drawList->AddLine(ImVec2(p1x, p1y), ImVec2(p2x, p2y), ImGui::GetColorU32(col), 2.5f);
    }

    // Center music note / visual icon
    drawList->AddCircleFilled(ImVec2(cx, cy), 18.0f, ImGui::GetColorU32(getThemeColor(0.2f, 0.3f)), 24);
    drawList->AddCircle(ImVec2(cx, cy), 18.0f, ImGui::GetColorU32(getThemeColor(0.8f, 0.9f)), 24, 2.0f);
    break;
  }

  case VisualizerMode::MirroredBars: {
    // Mode 3: Symmetrical Top-and-Bottom Mirrored Spectrum
    const float availableW = std::max(200.0f, static_cast<float>(windowWidth) - 100.0f);
    const float barSpacing = std::clamp(availableW / kNumVisualizerBands, 4.0f, 18.0f);
    const float barW = std::max(2.0f, barSpacing - 2.0f);
    const float totalW = kNumVisualizerBands * barSpacing;
    const float startX = (windowWidth - totalW) * 0.5f;
    const float horizonY = cy + 30.0f;
    const float maxBarH = std::min(130.0f, windowHeight * 0.22f);

    for (int i = 0; i < kNumVisualizerBands; ++i) {
      float tFrac = static_cast<float>(i) / (kNumVisualizerBands - 1);
      float barX = startX + i * barSpacing;
      float barH = std::max(2.0f, m_visualizerSmoothBands[i] * maxBarH);

      ImVec4 col = getThemeColor(tFrac, 0.90f);
      ImU32 u32Col = ImGui::GetColorU32(col);
      ImU32 u32Fade = ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.20f));

      // Upper bar (reaching up)
      drawList->AddRectFilledMultiColor(
          ImVec2(barX, horizonY - barH),
          ImVec2(barX + barW, horizonY),
          u32Col, u32Col, u32Fade, u32Fade);

      // Lower mirrored bar (reaching down)
      drawList->AddRectFilledMultiColor(
          ImVec2(barX, horizonY),
          ImVec2(barX + barW, horizonY + barH),
          u32Fade, u32Fade, u32Col, u32Col);
    }

    // Glowing horizon line
    drawList->AddLine(
        ImVec2(startX - 20.0f, horizonY),
        ImVec2(startX + totalW + 20.0f, horizonY),
        ImGui::GetColorU32(getThemeColor(0.5f, 0.85f)), 2.0f);
    break;
  }
  default:
    break;
  }

  ImGui::End();
  ImGui::PopStyleVar(2);

  // 3. Interactive Floating Track Info & Visualizer Controls Header (when controls visible)
  {
    float cardW = std::min(600.0f, static_cast<float>(windowWidth) - 40.0f);
    float cardH = 76.0f;
    float cardX = (windowWidth - cardW) * 0.5f;
    float cardY = 60.0f;

    ImGui::SetNextWindowPos(ImVec2(cardX, cardY));
    ImGui::SetNextWindowSize(ImVec2(cardW, cardH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.10f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.22f, 0.28f, 0.70f));

    ImGuiWindowFlags infoFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings;
    if (!m_controlsVisible) {
      infoFlags |= ImGuiWindowFlags_NoInputs;
    }

    if (ImGui::Begin("AudioVisualizerHeader", nullptr, infoFlags)) {
      // Audio title basename
      std::string fullPath = m_controller.getFilename();
      std::string baseName = fullPath;
      size_t lastSlash = fullPath.find_last_of("/\\");
      if (lastSlash != std::string::npos) {
        baseName = fullPath.substr(lastSlash + 1);
      }

      ImGui::TextColored(ImVec4(0.00f, 0.88f, 0.95f, 1.0f), "AUDIO PLAYBACK");
      ImGui::SameLine();
      ImGui::TextDisabled("|");
      ImGui::SameLine();
      ImGui::TextUnformatted(baseName.c_str());

      // Audio stream spec pill badges & Style switcher on the second line
      std::string codecStr = m_controller.getAudioCodecName();
      std::string layoutStr = m_controller.getAudioChannelLayoutName();
      ImGui::TextColored(ImVec4(0.70f, 0.72f, 0.78f, 1.0f), "[ %s ]  [ %s ]",
                         codecStr.c_str(), layoutStr.c_str());

      // Visualizer Mode Selector Buttons (interactive)
      ImGui::SameLine(cardW - 270.0f);
      auto drawModeBtn = [this](const char* label, VisualizerMode mode) {
        bool active = (m_visualizerMode == mode);
        if (active) {
          ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.00f, 0.65f, 0.75f, 0.80f));
        }
        if (ImGui::SmallButton(label)) {
          m_visualizerMode = mode;
        }
        if (active) {
          ImGui::PopStyleColor();
        }
      };

      drawModeBtn("Bars", VisualizerMode::NeonBars);
      ImGui::SameLine();
      drawModeBtn("Wave", VisualizerMode::SmoothWave);
      ImGui::SameLine();
      drawModeBtn("Radial", VisualizerMode::RadialDisc);
      ImGui::SameLine();
      drawModeBtn("Mirror", VisualizerMode::MirroredBars);

      // Theme toggle button
      ImGui::SameLine();
      const char* themeNames[] = { "CYBER", "SUNSET", "MINT", "VIOLET" };
      int currentTheme = static_cast<int>(m_visualizerTheme);
      if (ImGui::SmallButton(themeNames[currentTheme])) {
        m_visualizerTheme = static_cast<VisualizerTheme>((currentTheme + 1) % 4);
      }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
  }
}

void PlayerUI::drawTitleBar(int windowWidth, int windowHeight) {
  (void)windowHeight;
  // Slim header bar floating at the top of the viewport
  float barHeight = 45.0f;
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(windowWidth), barHeight));

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f, 10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.06f, 0.65f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

  ImGui::Begin("TitleBarHUD", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoSavedSettings);

  std::string titleText = "NaikAVPlayer";
  std::string filePath = m_controller.getFilename();
  if (!filePath.empty()) {
    size_t lastSlash = filePath.find_last_of("/\\");
    std::string fileName = (lastSlash == std::string::npos)
                               ? filePath
                               : filePath.substr(lastSlash + 1);
    titleText += "  |  " + fileName;
  }

  if (m_hudFont)
    ImGui::PushFont(m_hudFont);

  // Centered title string
  float textWidth = ImGui::CalcTextSize(titleText.c_str()).x;
  ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
  ImGui::SetCursorPosY(11.0f);
  ImGui::Text("%s", titleText.c_str());

  // Right-aligned Info/Diagnostics toggle button
  float btnWidth = 100.0f;
  ImGui::SetCursorPosX(windowWidth - btnWidth - 15.0f);
  ImGui::SetCursorPosY(8.0f);

  if (m_showDiagnostics) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.53f, 0.90f, 0.80f));
    if (ImGui::Button("Hide Info", ImVec2(btnWidth, 28.0f))) {
      setDiagnosticsVisible(false);
    }
    ImGui::PopStyleColor();
  } else {
    if (ImGui::Button("Show Info", ImVec2(btnWidth, 28.0f))) {
      setDiagnosticsVisible(true);
    }
  }

  if (m_hudFont)
    ImGui::PopFont();

  ImGui::End();

  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);
}

void PlayerUI::drawControlsBar(int windowWidth, int windowHeight) {
  // Floating centered dock design.
  //
  // 940 rather than 880: the right-hand group (resolution / EQ / mute /
  // volume) is right-anchored and the playback buttons are centered, so at
  // 880 the two collide once the right group reserves its true width. The
  // extra 60px restores a clear gap between them and keeps the volume
  // slider fully inside the dock.
  float barWidth = std::min(940.0f, windowWidth * 0.95f);
  float barHeight = 85.0f;
  float posX = (windowWidth - barWidth) * 0.5f;
  float posY = windowHeight - barHeight - 20.0f;

  ImGui::SetNextWindowPos(ImVec2(posX, posY));
  ImGui::SetNextWindowSize(ImVec2(barWidth, barHeight));

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 10.0f));

  ImGui::Begin("ControlsDock", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoSavedSettings);

  PlayerState state = m_controller.getState();
  double currentTime = m_controller.getCurrentTime();
  double duration = m_controller.getDuration();

  // Row 1: Timeline Seeker Bar
  ImGui::SetCursorPosY(12.0f);

  // Left current time. While actively dragging the seek bar, show the drag
  // target instead of the live playback clock - otherwise the numeric label
  // keeps counting up with real playback while the slider handle sits
  // wherever the user dragged it, which looks broken/inconsistent.
  double displayTime =
      m_seekDragActive ? static_cast<double>(m_seekDragValue) : currentTime;
  std::string timeCurrentStr = formatTime(displayTime);
  ImGui::Text("%s", timeCurrentStr.c_str());
  ImGui::SameLine(0.0f, 10.0f);

  // Calculate space available for slider
  float durationTextWidth = ImGui::CalcTextSize(formatTime(duration).c_str()).x;
  float currentTextWidth = ImGui::CalcTextSize(timeCurrentStr.c_str()).x;
  float sliderWidth = barWidth - currentTextWidth - durationTextWidth - 65.0f;

  ImGui::PushItemWidth(sliderWidth);

  // While the user is actively dragging, keep showing/using the value
  // they're dragging to - do NOT resync from currentTime, which keeps
  // advancing during playback and will otherwise fight the drag.
  float seekTarget =
      m_seekDragActive ? m_seekDragValue : static_cast<float>(currentTime);

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
  ImGui::SliderFloat("##seeker", &seekTarget, 0.0f,
                     static_cast<float>(duration), "");

  if (ImGui::IsItemActivated()) {
    m_seekDragActive = true;
  }
  if (m_seekDragActive) {
    m_seekDragValue = seekTarget;
  }
  if (ImGui::IsItemDeactivated()) {
    // Commit the seek once the drag/click ends, whether or not ImGui's
    // own value-changed bookkeeping flagged an edit (that check can miss
    // cases here since the backing value is a live, ticking clock rather
    // than a static settings value).
    m_controller.seek(seekTarget);
    m_seekDragActive = false;
  }
  ImGui::PopStyleVar();
  ImGui::PopItemWidth();

  // Right duration
  ImGui::SameLine(0.0f, 10.0f);
  std::string timeDurationStr = formatTime(duration);
  ImGui::Text("%s", timeDurationStr.c_str());

  // Row 2: Controls and Volume
  ImGui::SetCursorPosY(46.0f);

  // Left Group: Open File Button
  if (drawIconButton("##browse", IconType::Folder, ImVec2(36, 28))) {
    openMediaFileDialog();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Browse Media File");
  }

  // Middle Group: Playback buttons
  float centerButtonsGroupWidth = 268.0f;
  ImGui::SameLine((barWidth - centerButtonsGroupWidth) * 0.5f);

  // Seek back button (<<)
  // Relative seeks are based on the seek reference time so that repeated
  // presses stack onto a catch-up that is still in flight.
  if (drawIconButton("##seek_back", IconType::SeekBackward, ImVec2(36, 28))) {
    m_controller.seek(m_controller.getSeekReferenceTime() - 10.0);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Seek Backward 10s");
  }
  ImGui::SameLine(0.0f, 8.0f);

  // Play/Pause button
  bool isPlaying = (state == PlayerState::PLAYING);
  if (isPlaying) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.53f, 0.90f, 0.75f));
    if (drawIconButton("##pause", IconType::Pause, ImVec2(40, 28))) {
      m_controller.pause();
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Pause Playback");
    }
  } else {
    if (drawIconButton("##play", IconType::Play, ImVec2(40, 28))) {
      if (state != PlayerState::UNINITIALIZED) {
        m_controller.play();
      } else {
        openMediaFileDialog();
      }
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Start Playback");
    }
  }
  ImGui::SameLine(0.0f, 8.0f);

  // Stop button
  if (drawIconButton("##stop", IconType::Stop, ImVec2(36, 28))) {
    m_controller.stop();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Stop Playback");
  }
  ImGui::SameLine(0.0f, 8.0f);

  // Seek forward button (>>)
  if (drawIconButton("##seek_forward", IconType::SeekForward, ImVec2(36, 28))) {
    m_controller.seek(m_controller.getSeekReferenceTime() + 10.0);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Seek Forward 10s");
  }
  ImGui::SameLine(0.0f, 8.0f);

  // Loop toggle button
  bool loopEnabled = m_controller.isLoopEnabled();
  if (loopEnabled) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.53f, 0.90f, 0.75f));
  }
  if (drawIconButton("##loop", IconType::Loop, ImVec2(36, 28))) {
    m_controller.setLoopEnabled(!loopEnabled);
  }
  if (loopEnabled) {
    ImGui::PopStyleColor();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(loopEnabled ? "Loop: On" : "Loop: Off");
  }
  ImGui::SameLine(0.0f, 8.0f);

  // Playback Speed button
  float currentSpeed = m_controller.getPlaybackSpeed();
  char speedBtnLabel[16];
  if (std::fabs(currentSpeed - std::round(currentSpeed)) < 0.01f) {
    std::snprintf(speedBtnLabel, sizeof(speedBtnLabel), "%.0fx##speed", currentSpeed);
  } else {
    std::snprintf(speedBtnLabel, sizeof(speedBtnLabel), "%.2fx##speed", currentSpeed);
  }

  bool isNonDefaultSpeed = (std::fabs(currentSpeed - 1.0f) > 0.01f);
  if (isNonDefaultSpeed) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.53f, 0.90f, 0.75f));
  }
  if (ImGui::Button(speedBtnLabel, ImVec2(44, 28))) {
    ImGui::OpenPopup("##speed_popup");
  }
  if (isNonDefaultSpeed) {
    ImGui::PopStyleColor();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Playback Speed: %.2fx (Hotkeys: [ / ] or Backspace)", currentSpeed);
  }

  if (ImGui::BeginPopup("##speed_popup")) {
    ImGui::TextColored(ImVec4(0.00f, 0.83f, 0.88f, 1.00f), "Playback Speed");
    ImGui::Separator();
    const float speeds[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
    for (float s : speeds) {
      char sLabel[32];
      if (std::fabs(s - 1.0f) < 0.01f) {
        std::snprintf(sLabel, sizeof(sLabel), "1.0x (Normal)%s", (std::fabs(currentSpeed - s) < 0.01f) ? "  [OK]" : "");
      } else {
        std::snprintf(sLabel, sizeof(sLabel), "%.2fx%s", s, (std::fabs(currentSpeed - s) < 0.01f) ? "  [OK]" : "");
      }
      if (ImGui::Selectable(sLabel, std::fabs(currentSpeed - s) < 0.01f)) {
        m_controller.setPlaybackSpeed(s);
      }
    }
    ImGui::EndPopup();
  }

  // Right Group: Resolution, Volume and Mute
  //
  // This group is right-anchored, so the offset below must reserve room for
  // EVERY item in it: the resolution combo, the EQ and mute buttons, the
  // volume slider, the 8px gaps between them, and the window's right padding.
  // Deriving the reserve from the actual item widths (rather than a hand-
  // totalled constant) keeps the trailing volume slider inside the window --
  // under-counting here pushes it past the content edge, where it is clipped.
  const float groupItemSpacing = 8.0f;
  const float resolutionGroupWidth = 120.0f;
  const float eqButtonWidth = 36.0f;
  const float muteButtonWidth = 36.0f;
  const float volumeSliderWidth = 100.0f;
  const float barRightPadding = 20.0f; // matches the WindowPadding.x pushed above

  float rightGroupWidth = resolutionGroupWidth + eqButtonWidth +
                          muteButtonWidth + volumeSliderWidth +
                          groupItemSpacing * 3.0f;
  ImGui::SameLine(barWidth - rightGroupWidth - barRightPadding);

  // Resolution Dropdown
  const char* resolutionNames[] = {
      "Original",
      "360p",
      "480p",
      "720p",
      "1080p",
      "1440p",
      "4K"
  };
  ResolutionOption currentOpt = m_controller.getResolutionOption();
  int currentItem = static_cast<int>(currentOpt);

  ImGui::PushItemWidth(resolutionGroupWidth);
  if (ImGui::BeginCombo("##resolution", resolutionNames[currentItem])) {
      for (int i = 0; i < static_cast<int>(ResolutionOption::COUNT); i++) {
          bool isSelected = (currentItem == i);
          if (ImGui::Selectable(resolutionNames[i], isSelected)) {
              m_controller.setResolutionOption(static_cast<ResolutionOption>(i));
          }
          if (isSelected) {
              ImGui::SetItemDefaultFocus();
          }
      }
      ImGui::EndCombo();
  }
  ImGui::PopItemWidth();
  if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Select Output Playback Resolution");
  }

  ImGui::SameLine(0.0f, groupItemSpacing);
  // Capture before the button, not re-checked after: toggleAudioSettings()
  // mutates m_showAudioSettings on click, so re-reading it to decide
  // whether to Pop would mismatch the earlier Push whenever the button is
  // actually clicked (push decided by the old value, pop by the new one).
  bool eqButtonHighlighted = m_showAudioSettings;
  if (eqButtonHighlighted) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.53f, 0.90f, 0.80f));
  }
  if (ImGui::Button("EQ", ImVec2(eqButtonWidth, 28))) {
    toggleAudioSettings();
  }
  if (eqButtonHighlighted) {
    ImGui::PopStyleColor();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Audio Processing (EQ, Compressor, Loudness) - [A]");
  }

  ImGui::SameLine(0.0f, groupItemSpacing);

  // Mute button
  if (m_isMuted) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.00f, 0.20f, 0.20f, 0.80f));
    if (drawIconButton("##unmute", IconType::VolumeMute, ImVec2(muteButtonWidth, 28))) {
      m_isMuted = false;
      m_uiVolume = m_savedVolume;
      m_controller.setVolume(m_uiVolume / 100.0f);
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Unmute Audio");
    }
  } else {
    if (drawIconButton("##mute", IconType::VolumeHigh, ImVec2(muteButtonWidth, 28))) {
      m_savedVolume = m_uiVolume;
      m_isMuted = true;
      m_uiVolume = 0.0f;
      m_controller.setVolume(0.0f);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Mute Audio");
    }
  }

  ImGui::SameLine(0.0f, groupItemSpacing);
  ImGui::PushItemWidth(volumeSliderWidth);
  if (ImGui::SliderFloat("##volume", &m_uiVolume, 0.0f, 100.0f,
                         "Vol: %.0f%%")) {
    m_isMuted = (m_uiVolume == 0.0f);
    m_controller.setVolume(m_uiVolume / 100.0f);
  }
  ImGui::PopItemWidth();

  ImGui::End();
  ImGui::PopStyleVar(2);
}

void PlayerUI::drawDiagnosticsHUD(int windowWidth, int windowHeight) {
  if (!m_showDiagnostics)
    return;

  // Floating stats card on the top right
  float cardWidth = 340.0f;
  float cardHeight = windowHeight - 80.0f;
  if (cardHeight < 720.0f) cardHeight = 720.0f;
  if (cardHeight > 1050.0f) cardHeight = 1050.0f;

  ImGui::SetNextWindowPos(ImVec2(windowWidth - cardWidth - 20.0f, 60.0f));
  ImGui::SetNextWindowSize(ImVec2(cardWidth, cardHeight));

  ImGui::Begin("Diagnostics HUD", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings);

  if (m_hudFont)
    ImGui::PushFont(m_hudFont);

  ImGui::TextColored(ImVec4(0.00f, 0.83f, 0.88f, 1.00f),
                     "System Info & Diagnostics");
  ImGui::Separator();
  ImGui::Spacing();

  PlayerState state = m_controller.getState();
  const char *stateStr = "Unknown";
  ImVec4 stateColor = ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
  switch (state) {
  case PlayerState::UNINITIALIZED:
    stateStr = "Uninitialized";
    stateColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    break;
  case PlayerState::OPENED:
    stateStr = "Ready (Paused)";
    stateColor = ImVec4(0.12f, 0.53f, 0.90f, 1.00f);
    break;
  case PlayerState::PLAYING:
    stateStr = "Playing";
    stateColor = ImVec4(0.0f, 0.83f, 0.4f, 1.0f);
    break;
  case PlayerState::PAUSED:
    stateStr = "Paused";
    stateColor = ImVec4(0.9f, 0.7f, 0.0f, 1.0f);
    break;
  case PlayerState::ENDED:
    stateStr = "Ended";
    stateColor = ImVec4(0.5f, 0.5f, 0.8f, 1.0f);
    break;
  case PlayerState::ERROR_STATE:
    stateStr = "Error";
    stateColor = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
    break;
  }

  ImGui::Text("State: ");
  ImGui::SameLine();
  ImGui::TextColored(stateColor, "%s", stateStr);

  ImGui::SameLine(0.0f, 16.0f);
  ImGui::Text("Speed: ");
  ImGui::SameLine();
  ImGui::TextColored(ImVec4(0.00f, 0.83f, 0.88f, 1.00f), "%.2fx", m_controller.getPlaybackSpeed());

  ImGui::Spacing();

  if (m_controller.hasVideo()) {
    ImGui::Text("Native Res: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.00f), "%dx%d",
                       m_controller.getVideoWidth(),
                       m_controller.getVideoHeight());
    ImGui::Spacing();
    ImGui::Text("Playback Res: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.00f), "%dx%d",
                       m_controller.getPlaybackWidth(),
                       m_controller.getPlaybackHeight());
    ImGui::Spacing();
    ImGui::Text("Pixel Format: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.00f), "%s",
                       m_controller.getVideoPixelFormat().c_str());
    ImGui::Spacing();
    ImGui::Text("Decoder Type: ");
    ImGui::SameLine();
    bool isHW = m_controller.isVideoHardware();
    ImGui::TextColored(isHW ? ImVec4(0.0f, 0.83f, 0.4f, 1.0f)
                            : ImVec4(0.9f, 0.7f, 0.0f, 1.0f),
                       "%s", isHW ? "Hardware" : "Software");

    ColorPipelineInfo colorInfo = m_controller.getColorInfo();

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.00f, 0.83f, 0.88f, 1.00f), "Color & HDR Pipeline");
    ImGui::Separator();

    ImGui::Text("Chroma & Depth: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.00f), "%s (%d-bit, %s)",
                       colorInfo.pixelFormat.c_str(),
                       colorInfo.bitDepth,
                       colorInfo.chromaSubsampling.c_str());

    ImGui::Text("Color Space: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.00f), "%s",
                       colorInfo.colorSpace.c_str());

    ImGui::Text("Primaries / TRC: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.00f), "%s / %s",
                       colorInfo.colorPrimaries.c_str(),
                       colorInfo.transferChar.c_str());

    ImGui::Text("Color Range: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.00f), "%s",
                       colorInfo.colorRange.c_str());

    ImGui::Text("HDR Standard: ");
    ImGui::SameLine();
    ImVec4 hdrColor = colorInfo.isHDR ? ImVec4(1.0f, 0.75f, 0.0f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
    ImGui::TextColored(hdrColor, "%s", colorInfo.hdrType.c_str());
  } else {
    ImGui::Text("Media Type: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.00f), "Audio Only");
  }

  ImGui::Spacing();

  ImGui::Text("Time: ");
  ImGui::SameLine();
  ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.00f), "%s / %s",
                     formatTime(m_controller.getCurrentTime()).c_str(),
                     formatTime(m_controller.getDuration()).c_str());

  ImGui::Spacing();

  ImGui::Text("Audio Output: ");
  ImGui::SameLine();
  if (m_controller.hasAudio()) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Active (%s, %s) @ %.2fs",
                  m_controller.getAudioCodecName().c_str(),
                  m_controller.getAudioChannelLayoutName().c_str(),
                  m_controller.getAudioClock());
    ImGui::TextColored(ImVec4(0.0f, 0.83f, 0.4f, 1.0f), "%s", buf);
  } else {
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "None");
  }

  ImGui::Text("Video Output: ");
  ImGui::SameLine();
  if (m_controller.hasVideo()) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Active (%s) @ %.2fs",
                  m_controller.getVideoCodecName().c_str(),
                  m_controller.getVideoClock());
    ImGui::TextColored(ImVec4(0.0f, 0.83f, 0.4f, 1.0f), "%s", buf);
  } else {
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "None");
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.00f, 0.83f, 0.88f, 1.00f), "Pipeline Queue Depths");
  ImGui::Spacing();

  // Helper lambda for rendering queue progress bars
  auto drawQueueDepth = [](const char* label, size_t size, size_t capacity, const char* extraInfo) {
    float fraction = capacity > 0 ? static_cast<float>(size) / capacity : 0.0f;
    ImVec4 color = ImVec4(0.0f, 0.83f, 0.4f, 1.0f); // Green
    
    // Customize health indicators based on label type
    if (std::strcmp(label, "Video Frame Q") == 0) {
      if (size == 0) color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); // Red
      else if (size <= 2) color = ImVec4(0.9f, 0.7f, 0.0f, 1.0f); // Yellow
    /* } else if (std::strcmp(label, "Audio Frame Q") == 0) {
      float ms = (size * 1000.0f) / 48000.0f;
      if (ms < 50.0f) color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); // Red
      else if (ms < 150.0f) color = ImVec4(0.9f, 0.7f, 0.0f, 1.0f); // Yellow */
    } else { // Packets Q
      if (size < 5) color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); // Red
      else if (size < 20) color = ImVec4(0.9f, 0.7f, 0.0f, 1.0f); // Yellow
    }

    ImGui::Text("%s: %d/%d", label, static_cast<int>(size), static_cast<int>(capacity));
    if (extraInfo) {
      ImGui::SameLine();
      ImGui::TextDisabled(" (%s)", extraInfo);
    }
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    char pctBuf[32];
    std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", fraction * 100.0f);
    ImGui::ProgressBar(fraction, ImVec2(-1, 14), pctBuf);
    ImGui::PopStyleColor();
    ImGui::Spacing();
  };

  if (m_controller.hasVideo()) {
    drawQueueDepth("Video Packet Q", m_controller.getVideoPacketQueueSize(), m_controller.getVideoPacketQueueCapacity(), nullptr);
    drawQueueDepth("Video Frame Q", m_controller.getVideoFrameQueueSize(), m_controller.getVideoFrameQueueCapacity(), nullptr);
  }
  if (m_controller.hasAudio()) {
    drawQueueDepth("Audio Packet Q", m_controller.getAudioPacketQueueSize(), m_controller.getAudioPacketQueueCapacity(), nullptr);
    
    /* size_t audioFrmSize = m_controller.getAudioFrameQueueSize();
    float queuedMs = (audioFrmSize * 1000.0f) / 48000.0f;
    char audioInfo[32];
    std::snprintf(audioInfo, sizeof(audioInfo), "%.0f ms buf", queuedMs);
    drawQueueDepth("Audio Frame Q", audioFrmSize, m_controller.getAudioFrameQueueCapacity(), audioInfo); */
  }

  // Subtitle Queue: hardcoded to disabled since it's not supported by core
  /* ImGui::Text("Subtitle Q: N/A");
  ImGui::SameLine();
  ImGui::TextDisabled(" (Disabled)");
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
  ImGui::ProgressBar(0.0f, ImVec2(-1, 14), "0.0%");
  ImGui::PopStyleColor(); */

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.00f, 0.83f, 0.88f, 1.00f), "Decode & Render Timings");
  ImGui::Spacing();

  // Helper lambda for rendering timing budget progress bars
  auto drawTimingBar = [](const char* label, double timeMs, double budgetMs) {
    float fraction = budgetMs > 0.0 ? static_cast<float>(timeMs / budgetMs) : 0.0f;
    ImVec4 color = ImVec4(0.0f, 0.83f, 0.4f, 1.0f); // Green

    if (std::strcmp(label, "Frame Pacing") == 0) {
      // For Frame Pacing, the ideal time is exactly the target budget (100%).
      // We warn if there is a significant deviation from the target budget.
      float deviation = std::abs(fraction - 1.0f);
      if (deviation > 0.30f) {
        color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); // Red
      } else if (deviation > 0.15f) {
        color = ImVec4(0.9f, 0.7f, 0.0f, 1.0f); // Yellow
      }
    } else if (std::strcmp(label, "Present/VSync") == 0) {
      // High Present/VSync time means high headroom (Good -> Green).
      // Low Present/VSync time means we are running out of time (Bad -> Yellow/Red).
      if (fraction < 0.05f) {
        color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); // Red
      } else if (fraction < 0.15f) {
        color = ImVec4(0.9f, 0.7f, 0.0f, 1.0f); // Yellow
      }
    } else {
      // Processing times (Video/Audio Decode, Video Render): smaller is better.
      if (fraction > 0.9f) {
        color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); // Red
      } else if (fraction > 0.5f) {
        color = ImVec4(0.9f, 0.7f, 0.0f, 1.0f); // Yellow
      }
    }
    ImGui::Text("%s: %.2f ms", label, timeMs);
    ImGui::SameLine();
    ImGui::TextDisabled(" (%.1f%% budget)", fraction * 100.0f);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    ImGui::ProgressBar(std::min(fraction, 1.0f), ImVec2(-1, 14), "");
    ImGui::PopStyleColor();
    ImGui::Spacing();
  };

  double targetBudget = (m_videoFPS > 1.0) ? (1000.0 / m_videoFPS) : 33.33;

  if (m_controller.hasVideo()) {
    drawTimingBar("Video Decode", m_controller.getVideoDecodeTimeMs(), targetBudget);
  }
  if (m_controller.hasAudio()) {
    drawTimingBar("Audio Decode", m_controller.getAudioDecodeTimeMs(), targetBudget);
  }
  if (m_controller.hasVideo()) {
    drawTimingBar("Video Render", m_controller.getVideoRenderTimeMs(), targetBudget);
  }
  drawTimingBar("Present/VSync", m_controller.getPresentTimeMs(), targetBudget);
  if (m_controller.hasVideo()) {
    drawTimingBar("Frame Pacing", m_controller.getFramePacingMs(), targetBudget);
  }

  ImGui::Spacing();
  ImGui::Separator();

  // Draw Rolling Sync Graph
  {
    ImGui::TextColored(ImVec4(0.00f, 0.83f, 0.88f, 1.00f), "Clock Synchronization");
    ImGui::Spacing();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    float height = 110.0f;

    // Draw background
    drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(20, 20, 22, 255), 4.0f);
    drawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(60, 60, 65, 255), 4.0f);

    if (m_offsetHistory.empty()) {
      // Draw centered waiting message
      const char* msg = "Waiting for playback data...";
      ImVec2 textSize = ImGui::CalcTextSize(msg);
      drawList->AddText(ImVec2(pos.x + (width - textSize.x) * 0.5f, pos.y + (height - textSize.y) * 0.5f),
                        IM_COL32(150, 150, 155, 180), msg);
      
      // Draw Y axis labels with standard baseline
      drawList->AddText(ImVec2(pos.x + 5, pos.y + 2), IM_COL32(100, 100, 105, 120), "+50ms");
      drawList->AddText(ImVec2(pos.x + 5, pos.y + height - 15), IM_COL32(100, 100, 105, 120), "-50ms");
      drawList->AddText(ImVec2(pos.x + 5, pos.y + height * 0.5f - 6), IM_COL32(100, 100, 105, 120), "0ms");
      drawList->AddLine(ImVec2(pos.x, pos.y + height * 0.5f), ImVec2(pos.x + width, pos.y + height * 0.5f), IM_COL32(100, 100, 105, 80), 1.0f);

      ImGui::Dummy(ImVec2(width, height));
      ImGui::Spacing();
    } else {
      // Determine auto-scaling Y limits
      float maxVal = 50.0f; // minimum scale of ±50ms
      for (const auto& sample : m_offsetHistory) {
        float a = std::abs(sample.audioOffsetMs);
        float v = std::abs(sample.videoOffsetMs);
        if (a > maxVal) maxVal = a;
        if (v > maxVal) maxVal = v;
      }
      // Round maxVal up for visual padding
      maxVal = std::ceil(maxVal / 10.0f) * 10.0f;

      // Draw grid lines
      float centerY = pos.y + height * 0.5f;
      // 0ms baseline
      drawList->AddLine(ImVec2(pos.x, centerY), ImVec2(pos.x + width, centerY), IM_COL32(100, 100, 105, 120), 1.0f);

      // Top/bottom grid lines
      float gridY1 = centerY - height * 0.25f;
      float gridY2 = centerY + height * 0.25f;
      drawList->AddLine(ImVec2(pos.x, gridY1), ImVec2(pos.x + width, gridY1), IM_COL32(60, 60, 65, 80), 1.0f);
      drawList->AddLine(ImVec2(pos.x, gridY2), ImVec2(pos.x + width, gridY2), IM_COL32(60, 60, 65, 80), 1.0f);

      // Draw Y axis labels
      char labelBuf[32];
      std::snprintf(labelBuf, sizeof(labelBuf), "+%.0fms", maxVal);
      drawList->AddText(ImVec2(pos.x + 5, pos.y + 2), IM_COL32(200, 200, 205, 200), labelBuf);
      std::snprintf(labelBuf, sizeof(labelBuf), "-%.0fms", maxVal);
      drawList->AddText(ImVec2(pos.x + 5, pos.y + height - 15), IM_COL32(200, 200, 205, 200), labelBuf);
      drawList->AddText(ImVec2(pos.x + 5, centerY - 6), IM_COL32(150, 150, 155, 180), "0ms");

      // Plot offset curves
      int numSamples = m_offsetHistory.size();
      if (numSamples > 1) {
        for (int i = 0; i < numSamples - 1; ++i) {
          float x1 = pos.x + (static_cast<float>(i) / 199.0f) * width;
          float x2 = pos.x + (static_cast<float>(i + 1) / 199.0f) * width;

          // Clip coordinates to grid boundaries
          x1 = std::clamp(x1, pos.x, pos.x + width);
          x2 = std::clamp(x2, pos.x, pos.x + width);

          if (m_controller.hasAudio()) {
            float ay1 = centerY - (m_offsetHistory[i].audioOffsetMs / maxVal) * height * 0.5f;
            float ay2 = centerY - (m_offsetHistory[i+1].audioOffsetMs / maxVal) * height * 0.5f;
            ay1 = std::clamp(ay1, pos.y, pos.y + height);
            ay2 = std::clamp(ay2, pos.y, pos.y + height);
            drawList->AddLine(ImVec2(x1, ay1), ImVec2(x2, ay2), IM_COL32(236, 72, 153, 255), 1.5f); // Pink/Magenta
          }

          if (m_controller.hasVideo()) {
            float vy1 = centerY - (m_offsetHistory[i].videoOffsetMs / maxVal) * height * 0.5f;
            float vy2 = centerY - (m_offsetHistory[i+1].videoOffsetMs / maxVal) * height * 0.5f;
            vy1 = std::clamp(vy1, pos.y, pos.y + height);
            vy2 = std::clamp(vy2, pos.y, pos.y + height);
            drawList->AddLine(ImVec2(x1, vy1), ImVec2(x2, vy2), IM_COL32(6, 182, 212, 255), 1.5f); // Cyan
          }
        }
      }

      // Advance cursor past the graph drawing area
      ImGui::Dummy(ImVec2(width, height));
      ImGui::Spacing();
    }

    // Legends below graph
    if (m_controller.hasVideo()) {
      ImGui::TextColored(ImVec4(0.02f, 0.71f, 0.83f, 1.0f), "Video Offset (Cyan)");
      if (m_controller.hasAudio()) ImGui::SameLine(180.0f);
    }
    if (m_controller.hasAudio()) {
      ImGui::TextColored(ImVec4(0.92f, 0.28f, 0.60f, 1.0f), "Audio Offset (Magenta)");
    }
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  ImGui::Text("GUI Render FPS: ");
  ImGui::SameLine();
  ImGui::TextColored(ImVec4(0.00f, 0.83f, 0.88f, 1.00f), "%.1f",
                     ImGui::GetIO().Framerate);

  if (m_controller.hasVideo()) {
    ImGui::Text("Video Playback FPS: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.0f, 0.83f, 0.4f, 1.0f), "%.1f", m_videoFPS);
  }

  if (m_hudFont)
    ImGui::PopFont();

  ImGui::End();
}

void PlayerUI::drawAudioSettingsPanel(int windowWidth, int windowHeight) {
  if (!m_showAudioSettings)
    return;
  (void)windowWidth;

  float panelWidth = 380.0f;
  float panelHeight = windowHeight - 80.0f;
  if (panelHeight < 560.0f) panelHeight = 560.0f;
  if (panelHeight > 780.0f) panelHeight = 780.0f;

  ImGui::SetNextWindowPos(ImVec2(20.0f, 60.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(panelWidth, panelHeight), ImGuiCond_FirstUseEver);

  if (!ImGui::Begin("Audio Processing", &m_showAudioSettings,
                     ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::End();
    return;
  }

  if (m_hudFont)
    ImGui::PushFont(m_hudFont);

  naikav::dsp::AudioDspSettings s = m_controller.getAudioDspSettings();
  bool changed = false;

  const ImVec4 sectionColor(0.00f, 0.83f, 0.88f, 1.00f);

  const ImVec4 warnColor(0.9f, 0.55f, 0.0f, 1.0f);
  const ImVec4 grayColor(0.6f, 0.6f, 0.6f, 1.0f);
  const ImVec4 greenColor(0.0f, 0.83f, 0.4f, 1.0f);
  const ImVec4 spectrumColor(1.0f, 0.75f, 0.15f, 1.0f);

  ImGui::TextColored(sectionColor, "Spectrum Analyzer");
  ImGui::Separator();
  changed |= ImGui::Checkbox("Enable Spectrum Analyzer", &s.spectrumAnalyzerEnabled);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Real-time magnitude spectrum of the final processed signal,\n"
        "computed via a 1024-point FFT. Display only -- never modifies\n"
        "the audio, and costs nothing while disabled.");
  }
  if (s.spectrumAnalyzerEnabled && m_controller.hasAudio()) {
    std::vector<float> spectrumMags = m_controller.getSpectrumMagnitudesDb();
    if (!spectrumMags.empty()) {
      float plotWidth = ImGui::GetContentRegionAvail().x;
      ImGui::PushStyleColor(ImGuiCol_PlotLines, spectrumColor);
      ImGui::PlotLines("##spectrum", spectrumMags.data(), static_cast<int>(spectrumMags.size()), 0, nullptr,
                        naikav::dsp::SpectrumAnalyzer::kFloorDb, 0.0f, ImVec2(plotWidth, 120.0f));
      ImGui::PopStyleColor();
    }
  }
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  int outputChannels = m_controller.getAudioChannelCount();
  int deviceNativeChannels = m_controller.getAudioDeviceNativeChannels();
  bool possibleSilentDownmix = m_controller.hasAudio() && deviceNativeChannels > 0 &&
                                deviceNativeChannels < outputChannels;

  ImGui::Text("Channel Layout: ");
  ImGui::SameLine();
  if (m_controller.hasAudio()) {
    ImGui::TextColored(greenColor, "%s (%dch)",
                       m_controller.getAudioChannelLayoutName().c_str(), outputChannels);
  } else {
    ImGui::TextColored(grayColor, "No audio");
  }

  ImGui::Text("Device reports: ");
  ImGui::SameLine();
  if (!m_controller.hasAudio()) {
    ImGui::TextColored(grayColor, "N/A");
  } else if (deviceNativeChannels <= 0) {
    ImGui::TextColored(grayColor, "Unknown");
  } else {
    ImGui::TextColored(possibleSilentDownmix ? warnColor : greenColor, "%dch native", deviceNativeChannels);
  }
  if (possibleSilentDownmix) {
    ImGui::SameLine();
    ImGui::TextColored(warnColor, "(!) likely downmixed by OS");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Your default audio device reports only %d native channel(s).\n"
          "A successful %d-channel device open does not prove %d discrete\n"
          "speakers are connected -- Windows/Linux audio APIs silently\n"
          "downmix in shared mode. What you hear is not necessarily true\n"
          "%s, even though that's the layout being sent.",
          deviceNativeChannels, outputChannels, outputChannels,
          m_controller.getAudioChannelLayoutName().c_str());
    }
  }

  ImGui::Spacing();
  ImGui::Text("Output Channels: ");
  ImGui::SameLine();
  {
    static const char* kChannelOptionNames[] = {"Auto", "Force Stereo", "Virtual Surround"};
    AudioChannelOption currentChOpt = m_controller.getAudioChannelOption();
    int currentChItem = static_cast<int>(currentChOpt);
    ImGui::PushItemWidth(140.0f);
    if (ImGui::BeginCombo("##channeloption", kChannelOptionNames[currentChItem])) {
      for (int i = 0; i < static_cast<int>(AudioChannelOption::COUNT); ++i) {
        bool isSelected = (currentChItem == i);
        if (ImGui::Selectable(kChannelOptionNames[i], isSelected)) {
          m_controller.setAudioChannelOption(static_cast<AudioChannelOption>(i));
        }
        if (isSelected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Applies to the next file you open (not the current one -- changing\n"
        "channel count requires reopening the audio device).\n"
        "Auto: preserve the source's surround layout when it's one\n"
        "  NaikAVPlayer can drive directly (2.1/5.1/7.1).\n"
        "Force Stereo: always downmix to stereo regardless of source.\n"
        "Virtual Surround: preserve the source's surround layout\n"
        "  internally, but always fold it down to stereo with\n"
        "  positional delay/filter cues -- use this on headphones/\n"
        "  stereo speakers to actually hear 5.1/7.1 content spatially\n"
        "  instead of flattened to the middle.");
  }
  if (m_controller.isAudioVirtualSurroundActive()) {
    ImGui::TextColored(greenColor, "Virtual surround active");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "%s is being folded down to a 2-channel stream with positional\n"
          "delay/filter cues, not sent to the device untouched.",
          m_controller.getAudioChannelLayoutName().c_str());
    }
  }

  ImGui::Spacing();
  ImGui::Text("Output Bit Depth: ");
  ImGui::SameLine();
  {
    static const char* kBitDepthNames[] = {"16-bit Integer", "32-bit Integer", "32-bit Float"};
    AudioOutputBitDepth currentDepth = m_controller.getOutputBitDepth();
    int currentDepthItem = static_cast<int>(currentDepth);
    ImGui::PushItemWidth(160.0f);
    if (ImGui::BeginCombo("##outputbitdepth", kBitDepthNames[currentDepthItem])) {
      for (int i = 0; i < static_cast<int>(AudioOutputBitDepth::COUNT); ++i) {
        bool isSelected = (currentDepthItem == i);
        if (ImGui::Selectable(kBitDepthNames[i], isSelected)) {
          m_controller.setOutputBitDepth(static_cast<AudioOutputBitDepth>(i));
        }
        if (isSelected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Applies to the next file you open. The internal pipeline is\n"
        "always float; this only controls the final format handed to\n"
        "the audio device. 32-bit Float skips dithering/truncation\n"
        "entirely (lossless); 32-bit Integer gives more headroom than\n"
        "16-bit for hardware/drivers that prefer integer PCM.");
  }

  ImGui::Spacing();
  ImGui::Text("Output Device: ");
  ImGui::SameLine();
  {
    std::string currentDevice = m_controller.getOutputDeviceName();
    const char* currentLabel = currentDevice.empty() ? "System Default" : currentDevice.c_str();
    ImGui::PushItemWidth(220.0f);
    if (ImGui::BeginCombo("##outputdevice", currentLabel)) {
      bool defaultSelected = currentDevice.empty();
      if (ImGui::Selectable("System Default", defaultSelected)) {
        m_controller.setOutputDeviceName("");
      }
      if (defaultSelected) {
        ImGui::SetItemDefaultFocus();
      }
      for (const std::string& name : AudioDecoder::enumeratePlaybackDeviceNames()) {
        bool isSelected = (currentDevice == name);
        if (ImGui::Selectable(name.c_str(), isSelected)) {
          m_controller.setOutputDeviceName(name);
        }
        if (isSelected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Applies to the next file you open. \"System Default\" follows\nwhatever the OS considers the default playback device.");
  }

  ImGui::Spacing();
  ImGui::Text("Resampler Quality: ");
  ImGui::SameLine();
  {
    static const char* kQualityNames[] = {"Low", "Medium", "High", "Very High"};
    ResamplerQuality currentQuality = m_controller.getResamplerQuality();
    int currentQualityItem = static_cast<int>(currentQuality);
    ImGui::PushItemWidth(140.0f);
    if (ImGui::BeginCombo("##resamplerquality", kQualityNames[currentQualityItem])) {
      for (int i = 0; i < static_cast<int>(ResamplerQuality::COUNT); ++i) {
        bool isSelected = (currentQualityItem == i);
        if (ImGui::Selectable(kQualityNames[i], isSelected)) {
          m_controller.setResamplerQuality(static_cast<ResamplerQuality>(i));
        }
        if (isSelected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "libsoxr resampling precision. Applies to the next file you\n"
        "open. Medium matches this project's original (pre-selector)\n"
        "quality; higher tiers cost more CPU per resampled sample.");
  }

  ImGui::Spacing();
  ImGui::TextColored(sectionColor, "Presets");
  ImGui::Separator();
  {
    struct PresetEntry {
      const char* label;
      naikav::dsp::AudioDspSettings (*make)();
      const char* tooltip;
    };
    static const PresetEntry kPresets[] = {
        {"Flat", naikav::dsp::makeFlatPreset, "No processing -- reference/unmodified source"},
        {"Music", naikav::dsp::makeMusicPreset, "Gentle EQ smile, light dynamics, wider stereo image"},
        {"Cinema", naikav::dsp::makeCinemaPreset, "Dialogue presence, bass management, 3D surround for movies"},
        {"Night", naikav::dsp::makeNightPreset, "Heavy compression so quiet dialogue stays audible at low volume"},
        {"Podcast", naikav::dsp::makePodcastPreset, "Speech clarity and consistent level, rumble cut, mono-safe"},
        {"Gaming", naikav::dsp::makeGamingPreset, "Wide image + light 3D surround for positional audio cues"},
        {"Live", naikav::dsp::makeLivePreset, "Open, spacious \"being there\" feel for live recordings"},
        {"Bass Boost", naikav::dsp::makeBassBoostPreset, "Strong low-end lift with limiter protection"},
        {"Vocal Boost", naikav::dsp::makeVocalBoostPreset, "Push vocals/dialogue forward and centered"},
    };
    constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
    constexpr int kPresetsPerRow = 3;
    constexpr float kPresetButtonWidth = 108.0f;
    for (int i = 0; i < kPresetCount; ++i) {
      if (ImGui::Button(kPresets[i].label, ImVec2(kPresetButtonWidth, 0))) {
        s = kPresets[i].make();
        changed = true;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", kPresets[i].tooltip);
      }
      if ((i + 1) % kPresetsPerRow != 0 && i + 1 < kPresetCount) {
        ImGui::SameLine();
      }
    }

    changed |= ImGui::Checkbox("Auto-select preset by genre", &s.autoGenrePresetEnabled);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "When a file has a genre tag, automatically apply a matching\n"
          "preset above on open (e.g. \"Podcast\"/\"Speech\" -> Podcast,\n"
          "\"Soundtrack\" -> Cinema). Crude keyword matching, not a real\n"
          "genre taxonomy -- falls back to leaving your current settings\n"
          "alone when nothing recognizable matches.");
    }
  }

  ImGui::Spacing();
  ImGui::Spacing();

  ImGui::TextColored(sectionColor, "Master");
  ImGui::Separator();
  changed |= ImGui::Checkbox("Enable Audio Processing", &s.dspEnabled);

  ImGui::BeginDisabled(!s.dspEnabled);

  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, sectionColor);
  bool eqSectionOpen = ImGui::CollapsingHeader("Equalizer (5-band parametric)", ImGuiTreeNodeFlags_DefaultOpen);
  ImGui::PopStyleColor();
  if (eqSectionOpen) {
    static const char* kBandLabels[naikav::dsp::ParametricEQ::kNumBands] = {
        "Band 1 (Bass)", "Band 2 (Low-mid)", "Band 3 (Mid)", "Band 4 (High-mid)", "Band 5 (Treble)"};
    for (int i = 0; i < naikav::dsp::ParametricEQ::kNumBands; ++i) {
      ImGui::PushID(i);
      if (ImGui::TreeNodeEx(kBandLabels[i], ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::SliderFloat("Gain", &s.eqBandGainDb[i], -12.0f, 12.0f, "%.1f dB");
        changed |= ImGui::SliderFloat("Freq", &s.eqBandFreqHz[i], 20.0f, 20000.0f, "%.0f Hz",
                                       ImGuiSliderFlags_Logarithmic);
        changed |= ImGui::SliderFloat("Q", &s.eqBandQ[i], 0.1f, 10.0f, "%.2f");
        ImGui::TreePop();
      }
      ImGui::PopID();
    }
  }

  ImGui::Spacing();
  ImGui::TextColored(sectionColor, "Noise Gate");
  changed |= ImGui::Checkbox("Enable Noise Gate", &s.noiseGateEnabled);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Attenuates room noise/hiss/bleed during quiet passages,\nleaving everything above the threshold untouched.");
  }
  ImGui::BeginDisabled(!s.noiseGateEnabled);
  changed |= ImGui::SliderFloat("Gate Threshold", &s.noiseGateThresholdDb, -80.0f, -20.0f, "%.1f dB");
  changed |= ImGui::SliderFloat("Gate Ratio", &s.noiseGateRatio, 1.0f, 20.0f, "%.1f:1");
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::TextColored(sectionColor, "Compressor");
  changed |= ImGui::Checkbox("Enable Compressor", &s.compressorEnabled);
  ImGui::BeginDisabled(!s.compressorEnabled);
  changed |= ImGui::SliderFloat("Threshold", &s.compressorThresholdDb, -60.0f, 0.0f, "%.1f dB");
  changed |= ImGui::SliderFloat("Ratio", &s.compressorRatio, 1.0f, 20.0f, "%.1f:1");
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::TextColored(sectionColor, "Multiband Compressor");
  changed |= ImGui::Checkbox("Enable Multiband Compressor", &s.multibandEnabled);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Splits into low/mid/high bands (via two crossover points) and\n"
        "compresses each independently, so taming one frequency range\n"
        "doesn't drag the others down with it the way the single\n"
        "full-band Compressor above does.");
  }
  ImGui::BeginDisabled(!s.multibandEnabled);
  changed |= ImGui::SliderFloat("Low/Mid Split", &s.multibandLowMidHz, 60.0f, 1000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
  changed |= ImGui::SliderFloat("Mid/High Split", &s.multibandMidHighHz, 1000.0f, 12000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
  if (ImGui::TreeNodeEx("Low Band", ImGuiTreeNodeFlags_DefaultOpen)) {
    changed |= ImGui::SliderFloat("Threshold##mbLow", &s.multibandLowThresholdDb, -60.0f, 0.0f, "%.1f dB");
    changed |= ImGui::SliderFloat("Ratio##mbLow", &s.multibandLowRatio, 1.0f, 20.0f, "%.1f:1");
    ImGui::TreePop();
  }
  if (ImGui::TreeNodeEx("Mid Band", ImGuiTreeNodeFlags_DefaultOpen)) {
    changed |= ImGui::SliderFloat("Threshold##mbMid", &s.multibandMidThresholdDb, -60.0f, 0.0f, "%.1f dB");
    changed |= ImGui::SliderFloat("Ratio##mbMid", &s.multibandMidRatio, 1.0f, 20.0f, "%.1f:1");
    ImGui::TreePop();
  }
  if (ImGui::TreeNodeEx("High Band", ImGuiTreeNodeFlags_DefaultOpen)) {
    changed |= ImGui::SliderFloat("Threshold##mbHigh", &s.multibandHighThresholdDb, -60.0f, 0.0f, "%.1f dB");
    changed |= ImGui::SliderFloat("Ratio##mbHigh", &s.multibandHighRatio, 1.0f, 20.0f, "%.1f:1");
    ImGui::TreePop();
  }
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::TextColored(sectionColor, "Limiter");
  changed |= ImGui::Checkbox("Enable Limiter", &s.limiterEnabled);
  ImGui::BeginDisabled(!s.limiterEnabled);
  changed |= ImGui::SliderFloat("Ceiling", &s.limiterCeilingDb, -12.0f, 0.0f, "%.1f dB");
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::TextColored(sectionColor, "Bass Crossover (LFE)");
  changed |= ImGui::Checkbox("Enable Crossover", &s.crossoverEnabled);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Only affects a discrete LFE/subwoofer channel (5.1/7.1/2.1 output)");
  }
  ImGui::BeginDisabled(!s.crossoverEnabled);
  changed |= ImGui::SliderFloat("Cutoff", &s.crossoverCutoffHz, 40.0f, 250.0f, "%.0f Hz");
  changed |= ImGui::Checkbox("Redirect Bass to Sub", &s.crossoverBassRedirectEnabled);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "True bass management: highpasses every other channel at the\n"
        "cutoff and sums exactly what was removed into the LFE channel,\n"
        "instead of only tone-shaping an already-present LFE track.\n"
        "Useful when the main/surround speakers can't reproduce bass well.");
  }
  ImGui::EndDisabled();

  ImGui::EndDisabled(); // dspEnabled

  ImGui::Spacing();
  ImGui::TextColored(sectionColor, "Loudness Normalization (EBU R128)");
  ImGui::Separator();
  changed |= ImGui::Checkbox("Enable Loudness Normalization", &s.loudnessEnabled);
  ImGui::BeginDisabled(!s.loudnessEnabled);
  changed |= ImGui::SliderFloat("Target", &s.loudnessTargetLufs, -30.0f, -6.0f, "%.1f LUFS");
  ImGui::EndDisabled();

  if (s.loudnessEnabled && m_controller.hasAudio()) {
    double measured = m_controller.getMeasuredIntegratedLufs();
    ImGui::Spacing();
    if (measured <= -70.0) {
      ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Measuring...");
    } else {
      float appliedGain = m_controller.getCurrentLoudnessGainDb();
      ImGui::Text("Measured: ");
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.0f, 0.83f, 0.4f, 1.0f), "%.1f LUFS", measured);
      ImGui::SameLine();
      ImGui::Text(" | Gain: ");
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.0f, 0.83f, 0.4f, 1.0f), "%+.1f dB", appliedGain);
    }
  }

  ImGui::Spacing();
  ImGui::TextColored(sectionColor, "3D Surround");
  ImGui::Separator();
  changed |= ImGui::Checkbox("Enable 3D Surround", &s.surround3dEnabled);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Synthesizes a wider, more enveloping spatial ambience from any\n"
        "2-channel output -- works on plain stereo sources (unlike Output\n"
        "Channels: Virtual Surround, which needs a real 5.1/7.1 source),\n"
        "and adds extra depth on top when Virtual Surround is also active.\n"
        "Hand-rolled delay/filter based DSP, not a licensed decoder like\n"
        "DTS:X or Dolby Atmos -- no HRTF/measured spatial data involved.");
  }
  ImGui::BeginDisabled(!s.surround3dEnabled);
  changed |= ImGui::SliderFloat("Intensity", &s.surround3dIntensity, 0.0f, 2.0f, "%.2fx");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("1.00x = designed nominal strength, 0.00x = off, >1.00x = more pronounced");
  }
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::TextColored(sectionColor, "Stereo Widener");
  ImGui::Separator();
  changed |= ImGui::Checkbox("Enable Stereo Widener", &s.widenerEnabled);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Mid-side widening for a wider, more spacious stereo image.\n"
        "Only affects 2-channel output -- a no-op on native multichannel\n"
        "passthrough (Output Channels: Auto with real surround hardware).");
  }
  ImGui::BeginDisabled(!s.widenerEnabled);
  changed |= ImGui::SliderFloat("Width", &s.widenerWidth, 0.0f, 2.0f, "%.2fx");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("1.00x = unchanged, 0.00x = mono, >1.00x = wider than the source");
  }
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::TextColored(sectionColor, "Balance");
  ImGui::Separator();
  changed |= ImGui::SliderFloat("L / R Balance", &s.balance, -1.0f, 1.0f, "%.2f");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("0.00 = centered, -1.00 = full left, +1.00 = full right.\nOnly affects 2-channel output.");
  }

  if (changed) {
    // Applied live (thread-safe against the audio callback -- see
    // AudioDecoder::applyDspSettings()) and persisted immediately. A
    // settings-file write on every slider-drag frame is a non-issue here:
    // it only touches the UI thread, never the audio callback thread, so
    // it can't cause an audio dropout.
    m_controller.setAudioDspSettings(s);
    m_controller.persistAudioDspSettings();
  }

  if (m_hudFont)
    ImGui::PopFont();

  ImGui::End();
}

void PlayerUI::applyTheme() {
  ImGuiStyle &style = ImGui::GetStyle();

  // Smooth elements rounding for premium feel
  style.WindowRounding = 14.0f;
  style.FrameRounding = 8.0f;
  style.GrabRounding = 12.0f;
  style.PopupRounding = 10.0f;
  style.WindowBorderSize = 1.0f; // Thin border for frosted glass effect
  style.FrameBorderSize = 0.0f;
  style.PopupBorderSize = 1.0f;
  style.ItemSpacing = ImVec2(10.0f, 8.0f);
  style.WindowPadding = ImVec2(12.0f, 12.0f);

  ImVec4 *colors = style.Colors;

  // Glassmorphic background and borders
  colors[ImGuiCol_WindowBg] =
      ImVec4(0.06f, 0.06f, 0.08f, 0.72f); // Translucent obsidian
  colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  colors[ImGuiCol_PopupBg] =
      ImVec4(0.09f, 0.09f, 0.11f, 0.95f); // Solid dark for popups
  colors[ImGuiCol_Border] =
      ImVec4(0.35f, 0.35f, 0.40f, 0.25f); // Frosted/glowing glass border
  colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

  // Sleek frame controls
  colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.12f, 0.14f, 0.80f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.18f, 0.22f, 0.85f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.22f, 0.26f, 0.90f);

  // Modern button transitions
  colors[ImGuiCol_Button] = ImVec4(0.15f, 0.15f, 0.18f, 0.60f);
  colors[ImGuiCol_ButtonHovered] =
      ImVec4(0.12f, 0.53f, 0.90f, 0.85f); // Neon blue hover
  colors[ImGuiCol_ButtonActive] =
      ImVec4(0.00f, 0.83f, 0.88f, 1.00f); // Neon cyan active

  // Progress / Seeker grab colors
  colors[ImGuiCol_SliderGrab] = ImVec4(0.12f, 0.53f, 0.90f, 1.00f); // Neon blue
  colors[ImGuiCol_SliderGrabActive] =
      ImVec4(0.00f, 0.83f, 0.88f, 1.00f); // Neon cyan

  // Typography
  colors[ImGuiCol_Text] = ImVec4(0.98f, 0.98f, 0.98f, 1.00f);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.60f, 0.65f, 1.00f);

  // Windows Title Bars
  colors[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.06f, 0.08f, 0.80f);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.09f, 0.09f, 0.11f, 0.90f);
  colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.06f, 0.06f, 0.08f, 0.40f);

  // Scrollbars
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0.04f, 0.04f, 0.06f, 0.30f);
  colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.20f, 0.20f, 0.25f, 0.60f);
  colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.25f, 0.25f, 0.30f, 0.80f);
  colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.12f, 0.53f, 0.90f, 0.80f);
}

void PlayerUI::toggleMute() {
  if (m_isMuted) {
    m_isMuted = false;
    m_uiVolume = (m_savedVolume > 0.0f) ? m_savedVolume : 50.0f;
    m_controller.setVolume(m_uiVolume / 100.0f);
    showToast("Audio Unmuted (" + std::to_string(static_cast<int>(std::round(m_uiVolume))) + "%)", false, 2.0);
  } else {
    m_savedVolume = m_uiVolume;
    m_isMuted = true;
    m_uiVolume = 0.0f;
    m_controller.setVolume(0.0f);
    showToast("Audio Muted", false, 2.0);
  }
}

void PlayerUI::adjustVolume(float deltaPercent) {
  float newVol = std::clamp(m_uiVolume + deltaPercent, 0.0f, 100.0f);
  setVolumePercent(newVol);
  showToast("Volume: " + std::to_string(static_cast<int>(std::round(m_uiVolume))) + "%", false, 1.5);
}

void PlayerUI::setVolumePercent(float percent) {
  m_uiVolume = std::clamp(percent, 0.0f, 100.0f);
  if (m_uiVolume > 0.0f) {
    m_isMuted = false;
    m_savedVolume = m_uiVolume;
  } else {
    m_isMuted = true;
  }
  m_controller.setVolume(m_uiVolume / 100.0f);
}

void PlayerUI::showToast(const std::string &message, bool isError, double durationSeconds) {
  m_toast.message = message;
  m_toast.isError = isError;
  m_toast.totalDuration = durationSeconds;
  double now = SDL_GetTicks() / 1000.0;
  m_toast.expiryTime = now + durationSeconds;
}

void PlayerUI::drawToastNotification(int windowWidth, int windowHeight, double currentSystemTime) {
  (void)windowHeight;
  if (m_toast.message.empty() || currentSystemTime >= m_toast.expiryTime) {
    return;
  }

  float remaining = static_cast<float>(m_toast.expiryTime - currentSystemTime);
  float alpha = 1.0f;
  if (remaining < 0.6f) {
    alpha = std::clamp(remaining / 0.6f, 0.0f, 1.0f);
  }

  float cardW = std::min(600.0f, static_cast<float>(windowWidth) - 40.0f);
  float cardH = 46.0f;
  float posX = (windowWidth - cardW) * 0.5f;
  float posY = 58.0f;

  ImGui::SetNextWindowPos(ImVec2(posX, posY));
  ImGui::SetNextWindowSize(ImVec2(cardW, cardH));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 12.0f));

  ImVec4 bgCol = m_toast.isError ? ImVec4(0.18f, 0.06f, 0.07f, 0.92f * alpha)
                                 : ImVec4(0.06f, 0.12f, 0.16f, 0.92f * alpha);
  ImVec4 borderCol = m_toast.isError ? ImVec4(0.95f, 0.25f, 0.25f, 0.90f * alpha)
                                     : ImVec4(0.00f, 0.85f, 0.90f, 0.90f * alpha);

  ImGui::PushStyleColor(ImGuiCol_WindowBg, bgCol);
  ImGui::PushStyleColor(ImGuiCol_Border, borderCol);

  if (ImGui::Begin("ToastNotification", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize)) {
    if (m_hudFont)
      ImGui::PushFont(m_hudFont);

    ImVec4 textCol = m_toast.isError ? ImVec4(1.0f, 0.45f, 0.45f, alpha)
                                     : ImVec4(0.35f, 0.95f, 1.0f, alpha);
    ImGui::TextColored(textCol, "%s  %s", m_toast.isError ? "[!]" : "[*]", m_toast.message.c_str());

    if (m_hudFont)
      ImGui::PopFont();
  }
  ImGui::End();

  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);
}

