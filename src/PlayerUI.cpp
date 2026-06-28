#include "PlayerUI.hpp"
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <iostream>

PlayerUI::PlayerUI(PlayerController &controller)
    : m_controller(controller), m_uiVolume(5.0f), m_isMuted(false),
      m_savedVolume(5.0f), m_showDiagnostics(false), m_lastMouseMoveTime(0.0),
      m_controlsVisible(true), m_showLoadFileDialog(false), m_mainFont(nullptr),
      m_titleFont(nullptr), m_hudFont(nullptr) {
  m_filePathBuffer[0] = '\0';
}

PlayerUI::~PlayerUI() {}

void PlayerUI::init() {
  ImGuiIO &io = ImGui::GetIO();

  // Candidate paths to search for the bundled Noto Sans open-source fonts
  std::string regularPaths[] = {"assets/fonts/NotoSans-Regular.ttf",
                                "../assets/fonts/NotoSans-Regular.ttf",
                                "../../assets/fonts/NotoSans-Regular.ttf",
                                "./NotoSans-Regular.ttf"};

  std::string boldPaths[] = {
      "assets/fonts/NotoSans-Bold.ttf", "../assets/fonts/NotoSans-Bold.ttf",
      "../../assets/fonts/NotoSans-Bold.ttf", "./NotoSans-Bold.ttf"};

  std::string foundRegularPath = "";
  for (const auto &path : regularPaths) {
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
      std::fclose(f);
      foundRegularPath = path;
      break;
    }
  }

  std::string foundBoldPath = "";
  for (const auto &path : boldPaths) {
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
      std::fclose(f);
      foundBoldPath = path;
      break;
    }
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

std::string PlayerUI::formatTime(double seconds) const {
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

void PlayerUI::draw(int windowWidth, int windowHeight,
                    double currentSystemTime) {
  PlayerState state = m_controller.getState();
  bool isPlaying = (state == PlayerState::PLAYING);
  bool imguiWantsMouse = ImGui::GetIO().WantCaptureMouse;

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
  ImVec2 center = ImVec2((minPos.x + maxPos.x) * 0.5f, (minPos.y + maxPos.y) * 0.5f);

  ImU32 color;
  if (ImGui::IsItemActive()) {
    color = ImGui::GetColorU32(ImGuiCol_ButtonActive);
  } else if (ImGui::IsItemHovered()) {
    color = ImGui::GetColorU32(ImVec4(0.00f, 0.83f, 0.88f, 1.00f)); // Neon cyan hover glow
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
    ImVec2 pts[4] = {
        ImVec2(center.x - r * 0.3f, center.y - spkH * 0.5f),
        ImVec2(center.x, center.y - r * 0.55f),
        ImVec2(center.x, center.y + r * 0.55f),
        ImVec2(center.x - r * 0.3f, center.y + spkH * 0.5f)
    };
    drawList->AddConvexPolyFilled(pts, 4, color);
    drawList->AddLine(ImVec2(center.x + r * 0.25f, center.y - r * 0.25f),
                      ImVec2(center.x + r * 0.65f, center.y + r * 0.25f),
                      color, 1.5f);
    drawList->AddLine(ImVec2(center.x + r * 0.65f, center.y - r * 0.25f),
                      ImVec2(center.x + r * 0.25f, center.y + r * 0.25f),
                      color, 1.5f);
    break;
  }
  case IconType::VolumeHigh: {
    float r = sz * 0.5f;
    float spkH = r * 0.55f;
    drawList->AddRectFilled(ImVec2(center.x - r * 0.7f, center.y - spkH * 0.5f),
                           ImVec2(center.x - r * 0.4f, center.y + spkH * 0.5f),
                           color, 1.0f);
    ImVec2 pts[4] = {
        ImVec2(center.x - r * 0.4f, center.y - spkH * 0.5f),
        ImVec2(center.x - r * 0.1f, center.y - r * 0.55f),
        ImVec2(center.x - r * 0.1f, center.y + r * 0.55f),
        ImVec2(center.x - r * 0.4f, center.y + spkH * 0.5f)
    };
    drawList->AddConvexPolyFilled(pts, 4, color);
    drawList->AddLine(ImVec2(center.x + r * 0.15f, center.y - r * 0.25f),
                      ImVec2(center.x + r * 0.3f, center.y),
                      color, 1.5f);
    drawList->AddLine(ImVec2(center.x + r * 0.3f, center.y),
                      ImVec2(center.x + r * 0.15f, center.y + r * 0.25f),
                      color, 1.5f);
    drawList->AddLine(ImVec2(center.x + r * 0.4f, center.y - r * 0.45f),
                      ImVec2(center.x + r * 0.6f, center.y),
                      color, 1.5f);
    drawList->AddLine(ImVec2(center.x + r * 0.6f, center.y),
                      ImVec2(center.x + r * 0.4f, center.y + r * 0.45f),
                      color, 1.5f);
    break;
  }
  }

  ImGui::PopID();
  return clicked;
}

void PlayerUI::drawWelcomeHUD(int windowWidth, int windowHeight) {
  // Centered modern onboarding panel
  float cardWidth = 540.0f;
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
  const char *titleText = "Naik AV Player";
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
          " [Space] Play/Pause    [<- / ->] Seek 10s    [Esc] Exit")
          .x;
  ImGui::SetCursorPosX((cardWidth - rowWidth) * 0.5f);

  renderKey("Space", "Play/Pause");
  renderKey("<- / ->", "Seek 10s");
  renderKey("Esc", "Exit");

  ImGui::NewLine(); // terminate the SameLine loop

  if (m_hudFont)
    ImGui::PopFont();

  ImGui::End();
}

void PlayerUI::drawTitleBar(int windowWidth, int windowHeight) {
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
      m_showDiagnostics = false;
    }
    ImGui::PopStyleColor();
  } else {
    if (ImGui::Button("Show Info", ImVec2(btnWidth, 28.0f))) {
      m_showDiagnostics = true;
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

  // Left current time
  std::string timeCurrentStr = formatTime(currentTime);
  ImGui::Text("%s", timeCurrentStr.c_str());
  ImGui::SameLine(0.0f, 10.0f);

  // Calculate space available for slider
  float durationTextWidth = ImGui::CalcTextSize(formatTime(duration).c_str()).x;
  float currentTextWidth = ImGui::CalcTextSize(timeCurrentStr.c_str()).x;
  float sliderWidth = barWidth - currentTextWidth - durationTextWidth - 65.0f;

  ImGui::PushItemWidth(sliderWidth);
  float seekTarget = static_cast<float>(currentTime);

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
  if (ImGui::SliderFloat("##seeker", &seekTarget, 0.0f,
                         static_cast<float>(duration), "")) {
    m_controller.seek(seekTarget);
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
  float centerButtonsGroupWidth = 172.0f;
  ImGui::SameLine((barWidth - centerButtonsGroupWidth) * 0.5f);

  // Seek back button (<<)
  if (drawIconButton("##seek_back", IconType::SeekBackward, ImVec2(36, 28))) {
    m_controller.seek(currentTime - 10.0);
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
    m_controller.seek(currentTime + 10.0);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Seek Forward 10s");
  }

  // Right Group: Volume and Mute
  float volumeGroupWidth = 144.0f;
  ImGui::SameLine(barWidth - volumeGroupWidth - 20.0f);

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
  float cardWidth = 300.0f;
  float cardHeight = 290.0f;

  ImGui::SetNextWindowPos(ImVec2(windowWidth - cardWidth - 20.0f, 60.0f));
  ImGui::SetNextWindowSize(ImVec2(cardWidth, cardHeight));

  ImGui::Begin("Diagnostics HUD", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoResize |
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
    ImGui::Text("Resolution: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.00f), "%dx%d",
                       m_controller.getVideoWidth(),
                       m_controller.getVideoHeight());
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
  ImGui::TextColored(m_controller.hasAudio() ? ImVec4(0.0f, 0.83f, 0.4f, 1.0f)
                                             : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                     m_controller.hasAudio() ? "Active" : "None");

  ImGui::Text("Video Output: ");
  ImGui::SameLine();
  ImGui::TextColored(m_controller.hasVideo() ? ImVec4(0.0f, 0.83f, 0.4f, 1.0f)
                                             : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                     m_controller.hasVideo() ? "Active" : "None");

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  ImGui::TextDisabled("Security Note:");
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.2f, 0.85f));
  ImGui::TextWrapped("Keep external FFmpeg decoders updated to mitigate "
                     "potential media parsing exploits.");
  ImGui::PopStyleColor();

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
