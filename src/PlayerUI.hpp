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
        VolumeHigh,
        Loop
    };

private:
    PlayerController& m_controller;
    std::function<std::string()> m_fileDialogCallback;
    
    // UI state variables
    float m_uiVolume;
    bool m_isMuted;

    // Seek bar drag state. While the user is dragging, the slider's value
    // must NOT be re-synced from the live (advancing) playback clock every
    // frame, or the drag position gets fought by the ticking clock.
    bool m_seekDragActive;
    float m_seekDragValue;
    float m_savedVolume;
    bool m_showDiagnostics;
    
    struct ClockOffsetSample {
        double timeStamp;
        float audioOffsetMs;
        float videoOffsetMs;
    };
    std::deque<ClockOffsetSample> m_offsetHistory;
    
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

    static std::string formatTime(double seconds);
    static void applyTheme();

    // Modular drawing helpers
    void drawWelcomeHUD(int windowWidth, int windowHeight);
    void drawTitleBar(int windowWidth, int windowHeight);
    void drawControlsBar(int windowWidth, int windowHeight);
    void drawDiagnosticsHUD(int windowWidth, int windowHeight);

    static bool drawIconButton(const char* str_id, IconType icon, ImVec2 size);

public:
    explicit PlayerUI(PlayerController& controller);
    ~PlayerUI();

    void init();
    
    void toggleDiagnostics() {
        m_showDiagnostics = !m_showDiagnostics;
        m_controller.getPipelineMetrics().setProfilingEnabled(m_showDiagnostics);
    }
    bool isDiagnosticsVisible() const { return m_showDiagnostics; }
    void setDiagnosticsVisible(bool visible) {
        m_showDiagnostics = visible;
        m_controller.getPipelineMetrics().setProfilingEnabled(visible);
    }
    
    void registerVideoFrameRendered(double currentSystemTime);
    
    void setFileDialogCallback(std::function<std::string()> callback) {
        m_fileDialogCallback = callback;
    }
    
    // Draw the UI overlays. Called once per frame in the render loop.
    void draw(int windowWidth, int windowHeight, double currentSystemTime);

    // Notify UI that mouse moved to keep controls active
    void notifyMouseActivity(double currentSystemTime);

    // Get visibility state
};
