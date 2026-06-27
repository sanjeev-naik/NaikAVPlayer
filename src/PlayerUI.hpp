#pragma once

#include "PlayerController.hpp"
#include <string>
#include <functional>

class PlayerUI {
private:
    PlayerController& m_controller;
    std::function<std::string()> m_fileDialogCallback;
    
    // UI state variables
    float m_uiVolume;
    bool m_isMuted;
    float m_savedVolume;
    
    // Auto-hide controls timing
    double m_lastMouseMoveTime;
    bool m_controlsVisible;
    
    // Modal state
    bool m_showLoadFileDialog;
    char m_filePathBuffer[512];

    std::string formatTime(double seconds) const;
    void applyTheme();

public:
    explicit PlayerUI(PlayerController& controller);
    ~PlayerUI();

    void init();
    
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
