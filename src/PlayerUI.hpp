#pragma once

#include "PlayerController.hpp"
#include <string>
#include <functional>
#include <deque>

#include <imgui.h>

struct ImFont;

class PlayerUI {
public:
    enum class IconType {
        Folder,
        Play,
        Pause,
        Stop,
        SeekBackward,
        SeekForward,
        VolumeMute,
        VolumeHigh
    };

private:
    PlayerController& m_controller;
    std::function<std::string()> m_fileDialogCallback;
    
    // UI state variables
    float m_uiVolume;
    bool m_isMuted;
    float m_savedVolume;
    bool m_showDiagnostics;
    
    // Auto-hide controls timing
    double m_lastMouseMoveTime;
    bool m_controlsVisible;
    
    // Modal state
    bool m_showLoadFileDialog;
    char m_filePathBuffer[512];

    // Loaded fonts
    ImFont* m_mainFont;
    ImFont* m_titleFont;
    ImFont* m_hudFont;

    // FPS tracking
    std::deque<double> m_videoFrameTimes;
    double m_videoFPS;

    std::string formatTime(double seconds) const;
    void applyTheme();

    // Modular drawing helpers
    void drawWelcomeHUD(int windowWidth, int windowHeight);
    void drawTitleBar(int windowWidth, int windowHeight);
    void drawControlsBar(int windowWidth, int windowHeight);
    void drawDiagnosticsHUD(int windowWidth, int windowHeight);

    bool drawIconButton(const char* str_id, IconType icon, ImVec2 size);

public:
    explicit PlayerUI(PlayerController& controller);
    ~PlayerUI();

    void init();
    
    void registerVideoFrameRendered(double currentSystemTime);
    
    void setFileDialogCallback(std::function<std::string()> callback) {
        m_fileDialogCallback = callback;
    }
    
    // Draw the UI overlays. Called once per frame in the render loop.
    void draw(int windowWidth, int windowHeight, double currentSystemTime);

    // Notify UI that mouse moved to keep controls active
    void notifyMouseActivity(double currentSystemTime);

    // Get visibility state
    bool areControlsVisible() const { return m_controlsVisible; }
};
