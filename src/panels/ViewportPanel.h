#pragma once

#include "render/SceneRenderer.h"

#include <d3d9.h>

struct EngineState;

// Center panel. Owns and drives the scene renderer (mirrors how WorkspacePanel
// owns SceneViewportRenderer in the reference engine). The application loop
// calls the BeginFrame / Render / RenderSceneGpuPass hooks at the right points.
class ViewportPanel
{
public:
    void Init(IDirect3DDevice9* device) { scene_renderer_.Initialize(device); }
    void Shutdown()                     { scene_renderer_.Shutdown(); }
    void OnDeviceLost()                 { scene_renderer_.OnDeviceLost(); }

    void BeginFrame()                   { scene_renderer_.BeginFrame(); }
    void Render(EngineState& state);
    void RenderSceneGpuPass(float dt)   { scene_renderer_.RenderGpu(dt); }

private:
    SceneRenderer scene_renderer_;
};
