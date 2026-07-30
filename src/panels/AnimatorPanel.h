#pragma once

#include "anim/AnimatorController.h"
#include "render/AnimatorPreviewRenderer.h"

#include <d3d9.h>

#include <filesystem>
#include <string>
#include <vector>

struct EngineState;

class AnimatorPanel
{
public:
    void Render(EngineState& state);

    // Preview-renderer lifecycle hooks, forwarded by the application loop next
    // to the ViewportPanel's (mirrors that panel's ownership of SceneRenderer).
    void InitPreview(IDirect3DDevice9* device) { preview_.Initialize(device); }
    void OnPreviewDeviceLost()                 { preview_.OnDeviceLost(); }
    void BeginPreviewFrame()                   { preview_.BeginFrame(); }
    void RenderPreviewGpuPass(float dt)        { preview_.RenderGpu(dt); }
    void ShutdownPreview()                     { preview_.Shutdown(); }

private:
    void RefreshControllers(const EngineState& state);
    void OpenController(const std::filesystem::path& path, EngineState& state);
    void NewController();
    bool SaveController(EngineState& state);
    void RenderClips(EngineState& state);
    void RenderStates();
    void RenderTransitions();
    void RenderBoneModifiers(EngineState& state);
    void RenderControllerEditor(EngineState& state);
    void RenderPreviewViewport(EngineState& state);

    std::vector<std::filesystem::path> controller_paths_;
    std::filesystem::path scanned_directory_;
    std::filesystem::path loaded_path_;
    AnimatorController controller_;
    bool loaded_ = false;
    bool dirty_ = false;
    char new_name_[128] = "NewAnimator";
    std::vector<std::string> bone_modifier_text_;
    bool preview_playing_ = false;
    bool preview_mesh_ = true;
    bool preview_skeleton_ = true;
    bool preview_texture_ = true;
    int preview_clip_index_ = 0;
    float preview_time_ = 0.0f;
    AnimatorPreviewRenderer preview_;
};
