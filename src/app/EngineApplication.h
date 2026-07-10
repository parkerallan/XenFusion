#pragma once

#include "render/Renderer.h"
#include "state/EngineState.h"

#include "panels/ViewportPanel.h"
#include "panels/InspectorPanel.h"
#include "panels/FilesPanel.h"
#include "panels/VersionControlPanel.h"
#include "panels/AssetsPanel.h"
#include "panels/SettingsPanel.h"
#include "panels/LogPanel.h"
#include "panels/PerformancePanel.h"
#include "panels/EditorPanel.h"

#include <windows.h>

#include "imgui.h"

// Top-level editor application: owns the Win32 window, the D3D9 renderer, the
// ImGui context, the shared EngineState, and every panel. Mirrors the
// reference engine's EngineApplication shell.
class EngineApplication
{
public:
    bool Init();
    void RunLoop();
    void Shutdown();

    // Called by the static WndProc.
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    void ProcessEvents();
    void RenderUI();
    void RenderMainMenuBar();
    void RenderImportModal();
    void RenderRecentModal();
    void OnDropFiles(void* hDrop);            // HDROP from WM_DROPFILES
    void ImportStagedFiles(const char* dest); // dest relative to project root
    void LoadFonts();
    void ApplyStyle();
    void ApplyGrayTheme();
    void BuildDefaultDockLayout(ImGuiID dockspace_id);
    float TickDeltaSeconds();

    HWND     window_  = nullptr;
    Renderer renderer_;
    bool     running_ = false;

    // High-resolution frame timer (used to drive the scene camera before the
    // ImGui frame — where io.DeltaTime isn't available yet).
    LARGE_INTEGER qpc_freq_ = {};
    LARGE_INTEGER qpc_last_ = {};

    EngineState state_;

    ViewportPanel    viewport_panel_; // owns the SceneRenderer
    InspectorPanel   inspector_panel_;
    FilesPanel          files_panel_;
    VersionControlPanel version_control_panel_;
    AssetsPanel      assets_panel_;
    SettingsPanel    settings_panel_;
    LogPanel         log_panel_;
    PerformancePanel performance_panel_;
    EditorPanel      editor_panel_;
};
