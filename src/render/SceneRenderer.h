#pragma once

#include "render/MeshCache.h"
#include "render/ShaderCache.h"
#include "physics/PhysicsWorld.h"

#include <d3d9.h>

#include <filesystem>
#include <string>
#include <vector>

struct EngineState;
struct SceneFile;

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
    void CompileShaders(); // compile all shaders into <project>/shaders (Settings panel)

    void BeginFrame();
    void RenderUi(EngineState& state);
    void RenderGpu(float dt);

private:
    bool EnsureTarget(int width, int height);
    void BuildGrid();
    void HandleCameraInput(); // call right after the viewport Image item
    // Load the already-compiled standard shader from <project>/shaders (no
    // compile — that only happens on the Reload-shaders action).
    void LoadStandardShader();

    struct DrawItem
    {
        std::string model_path;
        D3DMATRIX   world;
        bool        selected = false;
        int         object_index = -1;      // for physics pose override
        float       scale[3] = {1.0f, 1.0f, 1.0f};
    };

    // A custom-shader object. Drawn on built-in geometry (quad/volume) at its
    // position, or on model_path's mesh when its geometry is "model".
    struct ShaderItem
    {
        std::string shader_path;
        std::string model_path; // the object's 3D Model, for "model" geometry
        float       pos[3]   = {0, 0, 0};
        float       rot[3]   = {0, 0, 0};
        float       scale[3] = {1, 1, 1};
        bool        selected = false;
    };

    // The one place a standalone shader is drawn: geometry, uniforms, and its parsed blend/depth/cull, then the draw.
    void DrawShaderItem(const ShaderItem& item, const CustomShader& shader, const D3DMATRIX& viewProj);

    struct Vertex
    {
        float    x, y, z;
        D3DCOLOR color;
    };

    IDirect3DDevice9*  m_device    = nullptr;
    IDirect3DTexture9* m_rt        = nullptr; // resolved, single-sampled: what ImGui samples
    IDirect3DSurface9* m_rtSurface = nullptr;
    IDirect3DSurface9* m_depth     = nullptr;

    // Multisampled scene target for alpha-to-coverage (smooth hair/foliage cutout
    // edges). The scene renders here, then resolves into m_rt. Falls back to
    // rendering straight into m_rtSurface when MSAA/A2C isn't available.
    IDirect3DSurface9*  m_rtMS    = nullptr;
    IDirect3DSurface9*  m_depthMS = nullptr;
    D3DMULTISAMPLE_TYPE m_msaa    = D3DMULTISAMPLE_NONE;
    bool                m_hasA2C  = false; // alpha-to-coverage supported by this GPU
    D3DRENDERSTATETYPE  m_a2cState = D3DRS_ADAPTIVETESS_Y; // vendor-specific enable
    DWORD               m_a2cOn   = 0;
    DWORD               m_a2cOff  = 0;

    int   m_width  = 0;
    int   m_height = 0;

    // Look-through: view the scene through the active "Camera" attribute
    // instead of the orbit camera (viewport toggle; editor-only state).
    bool m_look_through = false;

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

    // Camera matrices + eye computed in RenderUi (shared with the gizmo/RenderGpu).
    D3DMATRIX m_view = {};
    D3DMATRIX m_proj = {};
    float     m_eye[3] = {0.0f, 0.0f, 0.0f};

    // Scene lights captured in RenderUi, uploaded in RenderGpu — ready-made PS
    // constant payloads (c0 / c6 / c7-c10 / c11-c14). The legacy fixed sun is
    // used when the scene has no light attributes at all.
    float m_light_dir[4]    = {0.0f, 0.0f, 0.0f, 0.0f};
    float m_light_col[4]    = {1.0f, 1.0f, 1.0f, 0.0f};
    float m_point_pos[4][4] = {}; // xyz + w = 1/range^2
    float m_point_col[4][4] = {}; // rgb * intensity; zero = unused slot

    // Shader-based material pipeline (diffuse / normal / specular).
    IDirect3DVertexShader9*      m_vs        = nullptr;
    IDirect3DPixelShader9*       m_ps        = nullptr;
    IDirect3DVertexDeclaration9* m_mesh_decl = nullptr;
    IDirect3DTexture9*           m_def_white  = nullptr; // default diffuse
    IDirect3DTexture9*           m_def_normal = nullptr; // default flat normal
    IDirect3DTexture9*           m_def_black  = nullptr; // default specular
    std::filesystem::path        m_standard_src;         // <exe>/standard.hlsl (engine source)
    std::filesystem::path        m_project_root;         // compiled shaders live in <root>/shaders

    // Focus-key ("F") target: the selected object's position + rough size.
    bool  m_has_focus   = false;
    float m_focus_pos[3] = {0.0f, 0.0f, 0.0f};
    float m_focus_scale = 1.0f;

    bool m_gizmo_editing = false; // gizmo drag in progress (commit on release)

    std::vector<Vertex> m_grid;

    // --- Physics preview (Bullet) ---
    // Built on the Play rising edge from the scene's Rigid Body / Trigger
    // attributes, stepped in RenderGpu; poses override the draw items and the
    // debug wireframes for the frame. Stopped restores authored transforms
    // (the scene is never mutated).
    void StartPhysics(const SceneFile& scene);
    void StopPhysics();

    struct PhysDebug
    {
        int       shape;        // 0=Box, 1=Sphere
        float     half[3];      // collider half-extents / radius
        int       object_index; // pose lookup
        bool      trigger;      // color: rigid vs trigger
        D3DMATRIX base;         // authored rotation+translation (no scale)
    };

    // Append the collider's wireframe (box edges / sphere circles), transformed
    // by m (row-vector rotation+translation), to a fixed-function line list.
    void AppendColliderWire(const PhysDebug& pd, const D3DMATRIX& m,
                            D3DCOLOR col, std::vector<Vertex>& out) const;

    phys::PhysicsWorld           m_phys;
    bool                         m_phys_on = false;
    std::vector<phys::Pose>      m_phys_poses;
    std::vector<std::string>     m_phys_names;   // object names, for trigger logs
    std::vector<PhysDebug>       m_phys_debug;

    // Models to draw this frame (captured in RenderUi) + the mesh cache that
    // bakes/loads them.
    std::vector<DrawItem>   m_draw_items;
    std::vector<ShaderItem> m_shader_items;
    MeshCache               m_meshes;
    ShaderCache             m_shaders;
    float                   m_time = 0.0f; // seconds, for animated custom shaders

    // Unit quad + unit cube (mesh vertex layout) for standalone shaders
    IDirect3DVertexBuffer9* m_quad_vb = nullptr;
    IDirect3DIndexBuffer9*  m_quad_ib = nullptr;
    IDirect3DVertexBuffer9* m_cube_vb = nullptr;
    IDirect3DIndexBuffer9*  m_cube_ib = nullptr;
};
