#pragma once

#include "render/MeshCache.h"
#include "anim/AnimationClip.h"

#include "imgui.h"

#include <d3d9.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct EngineState;

// Lightweight preview renderer for the Animator panel's viewport. Owned by the
// AnimatorPanel by value, it mirrors the ViewportPanel -> SceneRenderer pattern:
// a standalone D3D9 op that renders to an offscreen texture ImGui samples, with
// the same phased lifecycle (Initialize / BeginFrame / RenderUi / RenderGpu).
//
// It reuses the existing skinned-mesh pipeline (MeshCache + the compiled
// standard_skin_vs/standard_ps shaders + the same pose math) so the preview
// looks exactly like the in-scene render. The rig is drawn as a 2D screen-space
// overlay (ImGui draw list) on top of the mesh, and clicking a joint selects it.
//
// Rendering goes to a D3DPOOL_DEFAULT render-target texture, so OnDeviceLost()
// releases it before a device Reset; RenderUi lazily recreates it.
class AnimatorPreviewRenderer
{
public:
    void Initialize(IDirect3DDevice9* device);
    void Shutdown();
    void OnDeviceLost();

    void BeginFrame(); // per-frame reset, before the ImGui frame

    // During the ImGui frame: sizes/draws the offscreen image into the canvas
    // rect, handles orbit-camera input, advances playback, evaluates the pose,
    // and draws the bone overlay + click-selection. model_path is the preview
    // model (project-relative); clip_source_model + clip_name select the active
    // animation (empty clip_name = show bind pose). Captures state for RenderGpu.
    void RenderUi(EngineState& state, const std::string& model_path,
                  const std::string& clip_source_model, const std::string& clip_name,
                  const ImVec2& canvas_min, const ImVec2& canvas_max);

    // After ImGui::Render(): fills the offscreen target (own BeginScene/EndScene).
    void RenderGpu(float dt);

    // Toolbar-driven display/playback state.
    void SetShowSkeleton(bool show) { show_skeleton_ = show; }
    void SetShowMesh(bool show)     { show_mesh_ = show; }
    void SetShowTexture(bool show)  { show_texture_ = show; }
    void SetPlaying(bool playing)   { playing_ = playing; }

    // Normalized playback position over the active clip (0..1). Reading it lets
    // the toolbar slider follow playback; writing it scrubs.
    float TimeNormalized() const { return time_normalized_; }
    void  SetTimeNormalized(float t);

    const std::string& SelectedBoneName() const { return selected_bone_name_; }

private:
    bool EnsureTarget(int width, int height);
    void LoadShaders(const std::filesystem::path& project_root);
    IDirect3DTexture9* SolidTexture(D3DCOLOR argb);
    // Evaluate the skeleton at sample_time (clip == nullptr yields the bind
    // pose). Fills out_palette (globals * inverseBind, 12 floats/joint for the
    // skin VS) and out_joint_world (each joint's model-space origin, xyz/joint).
    void EvaluatePose(const GpuMesh& mesh, const AnimationClip* clip, float sample_time,
                      std::vector<float>& out_palette, std::vector<float>& out_joint_world);
    const AnimationClip* GetClip(const std::string& source_model, const std::string& clip_name);

    IDirect3DDevice9*  m_device    = nullptr;
    IDirect3DTexture9* m_rt        = nullptr; // what ImGui samples
    IDirect3DSurface9* m_rtSurface = nullptr;
    IDirect3DSurface9* m_depth     = nullptr;
    int m_width  = 0;
    int m_height = 0;

    // Shaders + vertex declarations (loaded from <project>/shaders on project
    // change). Skinned VS is optional — missing it draws the bind pose.
    IDirect3DVertexShader9*      m_vs       = nullptr;
    IDirect3DVertexShader9*      m_skin_vs  = nullptr;
    IDirect3DPixelShader9*       m_ps       = nullptr;
    IDirect3DVertexDeclaration9* m_mesh_decl = nullptr;
    IDirect3DVertexDeclaration9* m_skin_decl = nullptr;
    IDirect3DTexture9*           m_def_white  = nullptr;
    IDirect3DTexture9*           m_def_normal = nullptr;
    IDirect3DTexture9*           m_def_black  = nullptr;
    IDirect3DCubeTexture9*       m_def_envcube = nullptr;

    std::filesystem::path m_project_root; // compiled shaders live in <root>/shaders
    MeshCache             m_meshes;

    struct ClipCacheEntry
    {
        AnimationClip clip;
        bool attempted = false;
        bool valid = false;
    };
    std::map<std::string, ClipCacheEntry> m_clips; // keyed by "source#name"

    // Orbit camera, auto-framed to the model bounds on load.
    float m_yaw      = 0.6f;
    float m_pitch    = 0.35f;
    float m_distance = 3.0f;
    float m_target[3] = {0.0f, 0.0f, 0.0f};
    bool  m_rotating = false;
    bool  m_panning  = false;
    std::string m_framed_model; // bounds already framed for this model path

    // Playback.
    bool  playing_ = false;
    float current_time_seconds_ = 0.0f;
    float time_normalized_ = 0.0f;
    float active_duration_ = 0.0f; // active clip duration (0 = none)

    // Display toggles.
    bool show_skeleton_ = true;
    bool show_mesh_     = true;
    bool show_texture_  = true;

    // Bone selection (screen-space click on the overlay).
    std::string selected_bone_name_;

    // Set by RenderUi, consumed by RenderGpu (the "pending" state pattern).
    bool        m_renderRequested = false;
    std::string m_pending_model;
    std::vector<float> m_pending_palette;
    D3DMATRIX   m_view = {};
    D3DMATRIX   m_proj = {};
    float       m_eye[3] = {0.0f, 0.0f, 0.0f};
};
