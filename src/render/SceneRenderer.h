#pragma once

#include <d3d9.h>

#include <vector>

struct EngineState;

// Standalone scene renderer, owned and driven by the editor's Viewport panel
// (mirrors the Vulkan engine's SceneViewportRenderer owned by WorkspacePanel).
//
// Phased like the reference:
//   BeginFrame()  - per-frame reset, before the ImGui frame
//   RenderUi()    - during the ImGui frame: sizes the target, draws the
//                   ImGui::Image() for the scene, captures camera/background
//   RenderGpu()   - after ImGui::Render(): fills the offscreen target with the
//                   3D scene (its own BeginScene/EndScene)
//
// Rendering goes to a D3DPOOL_DEFAULT render-target texture, so OnDeviceLost()
// releases it before a device Reset; RenderUi lazily recreates it.
class SceneRenderer
{
public:
    void Initialize(IDirect3DDevice9* device);
    void Shutdown();
    void OnDeviceLost();

    void BeginFrame();
    void RenderUi(EngineState& state);
    void RenderGpu(float dt);

private:
    bool EnsureTarget(int width, int height);
    void BuildGrid();
    void HandleCameraInput(); // call right after the viewport Image item

    struct Vertex
    {
        float    x, y, z;
        D3DCOLOR color;
    };

    IDirect3DDevice9*  m_device    = nullptr;
    IDirect3DTexture9* m_rt        = nullptr;
    IDirect3DSurface9* m_rtSurface = nullptr;
    IDirect3DSurface9* m_depth     = nullptr;

    int   m_width  = 0;
    int   m_height = 0;

    // Orbit camera.
    float m_yaw       = 0.6f;  // azimuth (radians)
    float m_pitch     = 0.5f;  // elevation (radians)
    float m_distance  = 20.0f; // distance from the target
    float m_target[3] = {0.0f, 0.0f, 0.0f};
    bool  m_rotating  = false; // right-drag active
    bool  m_panning   = false; // middle-drag active

    // Set by RenderUi, consumed by RenderGpu (the "pending" state pattern).
    bool     m_renderRequested = false;
    D3DCOLOR m_background       = D3DCOLOR_ARGB(255, 24, 24, 28);

    std::vector<Vertex> m_grid;
};
