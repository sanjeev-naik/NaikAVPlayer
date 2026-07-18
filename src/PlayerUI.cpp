#include "PlayerUI.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

  // 2. Top Title Bar HUD
  if (state != PlayerState::UNINITIALIZED && m_controlsVisible) {
    drawTitleBar(windowWidth, windowHeight);
  }

  // 3. Diagnostics Info HUD
  if (state != PlayerState::UNINITIALIZED && m_showDiagnostics) {
    drawDiagnosticsHUD(windowWidth, windowHeight);
  }

  // 4. Bottom Controls Bar Dock
  if (state != PlayerState::UNINITIALIZED && m_controlsVisible) {
    drawControlsBar(windowWidth, windowHeight);
  }

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
          " [Space] Play/Pause    [<- / ->] Seek 10s    [L] Loop    [Esc] Exit")
          .x;
  ImGui::SetCursorPosX((cardWidth - rowWidth) * 0.5f);

  renderKey("Space", "Play/Pause");
  renderKey("<- / ->", "Seek 10s");
  renderKey("L", "Loop");
  renderKey("Esc", "Exit");

  ImGui::NewLine(); // terminate the SameLine loop

  if (m_hudFont)
    ImGui::PopFont();

  ImGui::End();
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
  // Floating centered dock design
  float barWidth = std::min(880.0f, windowWidth * 0.95f);
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
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Browse Media File");
  }

  // Middle Group: Playback buttons
  float centerButtonsGroupWidth = 216.0f;
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
        m_showLoadFileDialog = true;
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

  // Right Group: Resolution, Volume and Mute
  float volumeGroupWidth = 144.0f;
  float resolutionGroupWidth = 120.0f;
  ImGui::SameLine(barWidth - volumeGroupWidth - resolutionGroupWidth - 28.0f);

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

  ImGui::SameLine(0.0f, 8.0f);

  // Mute button
  if (m_isMuted) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.00f, 0.20f, 0.20f, 0.80f));
    if (drawIconButton("##unmute", IconType::VolumeMute, ImVec2(36, 28))) {
      m_isMuted = false;
      m_uiVolume = m_savedVolume;
      m_controller.setVolume(m_uiVolume / 100.0f);
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Unmute Audio");
    }
  } else {
    if (drawIconButton("##mute", IconType::VolumeHigh, ImVec2(36, 28))) {
      m_savedVolume = m_uiVolume;
      m_isMuted = true;
      m_uiVolume = 0.0f;
      m_controller.setVolume(0.0f);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Mute Audio");
    }
  }

  ImGui::SameLine(0.0f, 8.0f);
  ImGui::PushItemWidth(100.0f);
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
  if (cardHeight < 650.0f) cardHeight = 650.0f;
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
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Active (%s) @ %.2fs",
                  m_controller.getAudioCodecName().c_str(),
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
    } else if (std::strcmp(label, "Audio Frame Q") == 0) {
      float ms = (size * 1000.0f) / 48000.0f;
      if (ms < 50.0f) color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); // Red
      else if (ms < 150.0f) color = ImVec4(0.9f, 0.7f, 0.0f, 1.0f); // Yellow
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
    
    size_t audioFrmSize = m_controller.getAudioFrameQueueSize();
    float queuedMs = (audioFrmSize * 1000.0f) / 48000.0f;
    char audioInfo[32];
    std::snprintf(audioInfo, sizeof(audioInfo), "%.0f ms buf", queuedMs);
    drawQueueDepth("Audio Frame Q", audioFrmSize, m_controller.getAudioFrameQueueCapacity(), audioInfo);
  }

  // Subtitle Queue: hardcoded to disabled since it's not supported by core
  ImGui::Text("Subtitle Q: N/A");
  ImGui::SameLine();
  ImGui::TextDisabled(" (Disabled)");
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
  ImGui::ProgressBar(0.0f, ImVec2(-1, 14), "0.0%");
  ImGui::PopStyleColor();

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.00f, 0.83f, 0.88f, 1.00f), "Decode & Render Timings");
  ImGui::Spacing();

  // Helper lambda for rendering timing budget progress bars
  auto drawTimingBar = [](const char* label, double timeMs, double budgetMs) {
    float fraction = budgetMs > 0.0 ? static_cast<float>(timeMs / budgetMs) : 0.0f;
    ImVec4 color = ImVec4(0.0f, 0.83f, 0.4f, 1.0f); // Green
    if (fraction > 0.9f) {
      color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f); // Red
    } else if (fraction > 0.5f) {
      color = ImVec4(0.9f, 0.7f, 0.0f, 1.0f); // Yellow
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
