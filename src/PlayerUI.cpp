#include "PlayerUI.hpp"
#include <imgui.h>
#include <cstdio>
#include <algorithm>
#include <iostream>

PlayerUI::PlayerUI(PlayerController& controller)
    : m_controller(controller),
      m_uiVolume(5.0f),
      m_isMuted(false),
      m_savedVolume(5.0f),
      m_lastMouseMoveTime(0.0),
      m_controlsVisible(true),
      m_showLoadFileDialog(false) {
    m_filePathBuffer[0] = '\0';
}

PlayerUI::~PlayerUI() {}

void PlayerUI::init() {
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

void PlayerUI::draw(int windowWidth, int windowHeight, double currentSystemTime) {
    PlayerState state = m_controller.getState();
    bool isPlaying = (state == PlayerState::PLAYING);
    bool imguiWantsMouse = ImGui::GetIO().WantCaptureMouse;

    // Keep controls visible if playback is paused or user is interacting with the GUI
    if (!isPlaying || imguiWantsMouse || m_showLoadFileDialog) {
        m_controlsVisible = true;
    } else {
        // Hide controls after 2.5 seconds of mouse inactivity
        if (currentSystemTime - m_lastMouseMoveTime > 2.5) {
            m_controlsVisible = false;
        }
    }

    // 1. Render Centered Instructions when Player is Empty
    if (state == PlayerState::UNINITIALIZED) {
        ImGui::SetNextWindowPos(ImVec2(windowWidth * 0.1f, windowHeight * 0.4f));
        ImGui::SetNextWindowSize(ImVec2(windowWidth * 0.8f, 100.0f));
        ImGui::Begin("InstructionHUD", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
        
        // Centered Text Rendering
        const char* instructionText = "Drag & Drop a video file here to play";
        float textWidth = ImGui::CalcTextSize(instructionText).x;
        ImGui::SetCursorPosX((windowWidth * 0.8f - textWidth) * 0.5f);
        
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.75f, 1.00f));
        ImGui::Text("%s", instructionText);
        ImGui::PopStyleColor();
        
        const char* subtext = "Or click 'Load File' at the bottom controls bar";
        float subTextWidth = ImGui::CalcTextSize(subtext).x;
        ImGui::SetCursorPosX((windowWidth * 0.8f - subTextWidth) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.40f, 0.45f, 0.80f));
        ImGui::Text("%s", subtext);
        ImGui::PopStyleColor();
        
        ImGui::End();
    }

    // 2. Render Developer HUD Diagnostics (Top-Left corner)
    if (state != PlayerState::UNINITIALIZED && m_controlsVisible) {
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("Diagnostics HUD", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        
        const char* stateStr = "Unknown";
        switch (state) {
            case PlayerState::OPENED:  stateStr = "Opened (Paused)"; break;
            case PlayerState::PLAYING: stateStr = "Playing"; break;
            case PlayerState::PAUSED:  stateStr = "Paused"; break;
            case PlayerState::ENDED:   stateStr = "Ended"; break;
            case PlayerState::ERROR_STATE: stateStr = "Error"; break;
            default: break;
        }

        ImGui::Text("State: %s", stateStr);
        if (m_controller.hasVideo()) {
            ImGui::Text("Resolution: %dx%d", m_controller.getVideoWidth(), m_controller.getVideoHeight());
        }
        ImGui::Text("Media Time: %s / %s", 
                    formatTime(m_controller.getCurrentTime()).c_str(), 
                    formatTime(m_controller.getDuration()).c_str());
        
        ImGui::End();
    }

    // 3. Render Bottom Control Bar overlay
    if (m_controlsVisible) {
        ImGui::SetNextWindowPos(ImVec2(0.0f, windowHeight - 75.0f));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(windowWidth), 75.0f));
        
        // Remove window padding for full bleed seeker bar
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
        ImGui::Begin("ControlsBar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
        
        double currentTime = m_controller.getCurrentTime();
        double duration = m_controller.getDuration();
        
        // Time readout overlay strings
        std::string timeDisplay = formatTime(currentTime) + " / " + formatTime(duration);
        
        // A. Full width timeline seeker bar
        ImGui::PushItemWidth(-1.0f);
        float seekTarget = static_cast<float>(currentTime);
        if (ImGui::SliderFloat("##seeker", &seekTarget, 0.0f, static_cast<float>(duration), timeDisplay.c_str())) {
            m_controller.seek(seekTarget);
        }
        ImGui::PopItemWidth();
        
        ImGui::Spacing();

        // B. Control buttons layout (Play, Pause, Stop, Load, Volume)
        if (state == PlayerState::PLAYING) {
            if (ImGui::Button("Pause")) {
                m_controller.pause();
            }
        } else {
            if (ImGui::Button("Play")) {
                if (state != PlayerState::UNINITIALIZED) {
                    m_controller.play();
                } else {
                    m_showLoadFileDialog = true;
                }
            }
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
            m_controller.stop();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Load File")) {
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

        // C. Mute and volume slider
        ImGui::SameLine(0.0f, 20.0f);
        if (m_isMuted) {
            if (ImGui::Button("Unmute")) {
                m_isMuted = false;
                m_uiVolume = m_savedVolume;
                m_controller.setVolume(m_uiVolume / 100.0f);
            }
        } else {
            if (ImGui::Button("Mute")) {
                m_savedVolume = m_uiVolume;
                m_isMuted = true;
                m_uiVolume = 0.0f;
                m_controller.setVolume(0.0f);
            }
        }
        
        ImGui::SameLine();
        ImGui::PushItemWidth(100.0f);
        if (ImGui::SliderFloat("##volume", &m_uiVolume, 0.0f, 100.0f, "Vol: %.0f%%")) {
            m_isMuted = (m_uiVolume == 0.0f);
            m_controller.setVolume(m_uiVolume / 100.0f);
        }
        ImGui::PopItemWidth();
        
        ImGui::End();
        ImGui::PopStyleVar(); // Restore padding
    }

    // 4. File selection modal popup
    if (m_showLoadFileDialog) {
        ImGui::OpenPopup("Load File Modal");
    }

    if (ImGui::BeginPopupModal("Load File Modal", &m_showLoadFileDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
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
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Failed to load file!");
            ImGui::Text("Please verify the file path is correct.");
            if (ImGui::Button("Close")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::EndPopup();
    }
}

void PlayerUI::applyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    
    // Smooth elements rounding
    style.WindowRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 1.0f;
    
    ImVec4* colors = style.Colors;
    
    // Background acrylic dark grey theme
    colors[ImGuiCol_WindowBg]             = ImVec4(0.08f, 0.08f, 0.09f, 0.85f);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.11f, 0.11f, 0.13f, 0.95f);
    colors[ImGuiCol_Border]               = ImVec4(0.20f, 0.20f, 0.23f, 0.60f);
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    
    // Controls foreground items
    colors[ImGuiCol_FrameBg]              = ImVec4(0.15f, 0.15f, 0.18f, 0.90f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.20f, 0.20f, 0.25f, 0.90f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.25f, 0.25f, 0.30f, 0.90f);
    
    // Interactive buttons
    colors[ImGuiCol_Button]               = ImVec4(0.22f, 0.22f, 0.27f, 0.80f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.30f, 0.30f, 0.38f, 0.90f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.00f, 0.50f, 0.85f, 1.00f); // Sleek cyan highlight
    
    // Seeker bar histogram/progress coloring
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.00f, 0.55f, 0.90f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.00f, 0.65f, 1.00f, 1.00f);
    
    // Typography
    colors[ImGuiCol_Text]                 = ImVec4(0.95f, 0.95f, 0.97f, 1.00f);
    colors[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.50f, 0.55f, 1.00f);
    
    // Title headers
    colors[ImGuiCol_TitleBg]              = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.15f, 0.15f, 0.18f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.11f, 0.11f, 0.13f, 0.50f);
}
