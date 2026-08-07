#pragma once

#include "render/MeshCache.h"
#include "render/ShaderCache.h"
#include "anim/AnimationClip.h"
#include "anim/AnimatorController.h"
#include "anim/AnimatorRuntime.h"
#include "render/BoneModifiers.h"
#include "video/VideoPlayer.h"
#include "audio/AudioPlayer.h"
#include "camera/CameraResolve.h"
#include "light/LightSelect.h"
#include "light/LightProbeGrid.h"
#include "physics/PhysicsWorld.h"
#include "script/ScriptVM.h"
#include "script/ScriptTypes.h"
#include "input/InputState.h"
#include "text/TextLayout.h"
#include "gui/GuiContext.h"
#include "gui/GuiHost.h"
#include "state/EngineState.h" // ObjectAttribute, for the live attribute overlay

#include <d3d9.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct EngineState;
struct SceneFile;
struct SceneObject;

struct SceneBoneColliderPose
{
    std::string bone_name;
    float half_extents[3] = {};
    float model_matrix[16] = {};
};

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
class SceneRenderer : public script::ScriptHost, public gui::HostAssets
{
public:
    // script::ScriptHost — input is stubbed until the input system lands; log
    // goes to the editor Log; find resolves a scene object name to its index.
    bool  InputButton(const char* name);
    float InputAxis(const char* name);
    bool  InputButtonPressed(const char* name);
    bool  InputButtonReleased(const char* name);
    void  Log(const char* msg);
    void  LogError(const char* msg);
    int   FindObject(const char* name);
    int   ObjectCount();
    const char* ObjectName(int index);
    void  TextSetValue(int objectIndex, const char* value);
    bool  GetObjectTransform(int objectIndex, float pos[3], float rot[3], float scale[3]);
    void  SetObjectTransform(int objectIndex, const float pos[3], const float rot[3],
                             const float scale[3], int mask);
    bool  GetObjectVisible(int objectIndex);
    void  SetObjectVisible(int objectIndex, bool visible);
    bool  SetActiveCamera(int objectIndex);
    int   GetActiveCamera();
    int   ObjectTagCount(int objectIndex);
    const char* ObjectTag(int objectIndex, int tagIndex);
    bool  AddObjectTag(int objectIndex, const char* tag);
    bool  RemoveObjectTag(int objectIndex, const char* tag);
    unsigned ObjectGeneration(int objectIndex);
    bool  ObjectAlive(int objectIndex);
    int   SpawnObject(int sourceObjectIndex, const char* name, const float pos[3]);
    bool  DestroyObject(int objectIndex);
    bool  LoadScene(const char* name, float minSeconds);
    bool  SceneIsLoading();
    float SceneProgress();
    const char* SceneName();
    bool  AttrGet(int objectIndex, int attrIndex, const char* field, attrfields::Value& out);
    bool  AttrSet(int objectIndex, int attrIndex, const char* field, const attrfields::Value& in);
    int   AttrCount(int objectIndex);
    const char* AttrType(int objectIndex, int attrIndex);

    // gui::HostAssets — the GUI core's only per-target seam. Paths are project
    // relative; the editor resolves them against the project root, loads
    // textures through the shared mesh texture cache, and bakes font atlases
    // live from the source TTF (the same PreviewFont the Text attribute uses).
    int  AcquireTexture(const char* relPath);
    bool TextureSize(int textureId, int& outWidth, int& outHeight);
    const text::FontMetrics* AcquireFont(const char* relPath);
    int  AcquireFontAtlas(const char* relPath);
    void  VideoSetPlaying(int objectIndex, bool play, bool loop); // Lua video.play/stop
    bool  VideoIsPlaying(int objectIndex);
    void  AudioSetPlaying(int objectIndex, bool play); // Lua audio.*
    bool  AudioIsPlaying(int objectIndex);
    void  AudioSetVolume(int objectIndex, float volume);
    void  AudioSetPitch(int objectIndex, float pitch);
    void  AudioSetLoop(int objectIndex, bool loop);
    void  AnimatorSetFloat(int objectIndex, const char* name, float value);
    void  AnimatorSetBool(int objectIndex, const char* name, bool value);
    void  AnimatorSetTrigger(int objectIndex, const char* name);
    void  AnimatorSetState(int objectIndex, const char* name);

    void Initialize(IDirect3DDevice9* device);
    void Shutdown();
    void OnDeviceLost();
    void CompileShaders(); // compile all shaders into <project>/shaders (Settings panel)

    void BeginFrame();
    void RenderUi(EngineState& state);
    void RenderGpu(float dt);
    void InvalidateAnimator(const std::string& controller_path);

private:
    bool EnsureTarget(int width, int height);
    void BuildGrid();
    void HandleCameraInput(); // call right after the viewport Image item
    // Load the already-compiled standard shader from <project>/shaders (no
    // compile — that only happens on the Reload-shaders action).
    void LoadStandardShader();
    void EnsureLightmaps(const SceneFile& scene);
    void ClearLightmaps();
    void RenderShadowMap();
    // Select the static or skinned vertex path and bind both streams. Until
    // animation sampling lands, skinned meshes receive an identity palette so
    // the authored bind pose remains unchanged in edit mode.
    void BindMeshForDraw(GpuMesh* mesh, const std::vector<float>* palette = nullptr);

    struct DrawItem
    {
        std::string model_path;
        D3DMATRIX   world;
        bool        selected = false;
        int         object_index = -1;      // for physics pose override
        float       scale[3] = {1.0f, 1.0f, 1.0f};
        std::string animator_controller_path;
        std::string animator_initial_state;
        float       animator_playback_speed = 1.0f;
        bool        animator_auto_play = true;
        bool        dynamic_lighting = false;
        bool        cast_shadow = false;
        std::vector<float> skin_palette;
    };

    struct LightmapVertex
    {
        MeshVertex base;
        float u1, v1;
    };

    struct LightmapGpuMesh
    {
        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9* ib = nullptr;
        UINT vertexCount = 0;
    };

    struct PickItem
    {
        std::string model_path;
        D3DMATRIX   world;
        int         object_index = -1;
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

    // Follow/Track look-through state (shared resolver camr::*): the track
    // camera advances while look-through is on and resets with it; follow
    // smoothing eases across frames. Keyed to the active camera object index
    // so switching cameras restarts cleanly.
    int               m_cam_key       = -1;
    float             m_track_dist    = 0.0f;
    float             m_track_speed   = 0.0f;
    camr::FollowSmooth m_follow_smooth;

    // Track-point gizmo: preview-then-commit while dragging a control point of
    // the selected object's Track camera (mirrors the object gizmo pattern).
    bool  m_track_preview        = false;
    int   m_track_preview_object = -1; // object index the preview belongs to
    int   m_track_preview_attr   = -1; // attribute index of the Track camera
    int   m_track_preview_index  = -1; // control point index
    float m_track_preview_point[3] = {0.0f, 0.0f, 0.0f};
    int   m_last_selected_object = -1; // to reset point selection on change

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
    // constant payloads (c0 / c2 / c6 / c7-c10 / c11-c14 / c16-c21). The legacy
    // fixed sun/ambient are used when the scene has no light attributes at all.
    float m_light_dir[4]    = {0.0f, 0.0f, 0.0f, 0.0f};
    float m_light_col[4]    = {1.0f, 1.0f, 1.0f, 0.0f};
    std::vector<lsel::PointLight> m_point_lights;
    std::vector<lsel::SpotLight>  m_spot_lights;
    float m_point_pos[4][4] = {}; // xyz + w = 1/range^2
    float m_point_col[4][4] = {}; // rgb * intensity; zero = unused slot
    float m_spot_pos[2][4]  = {}; // xyz + w = 1/range
    float m_spot_dir[2][4]  = {}; // xyz = beam dir (+Z fwd), w = cos(inner half-angle)
    float m_spot_col[2][4]  = {}; // rgb * intensity (zero = unused), w = cos(outer)
    float m_ambient[4]      = {0.22f, 0.22f, 0.25f, 0.0f}; // env-light sum or legacy default
    float m_baked_ambient[4] = {}; // baked env is global and never directionally modulated

    // Bloom post chain: bright-extract (rgb * alpha-mask) into a quarter-res
    // target, separable blur, additive combine. Drawn after the scene resolve;
    // skipped entirely when the shaders/.cso or targets are missing.
    void RenderBloom();

    // Shader-based material pipeline (diffuse / normal / specular).
    IDirect3DVertexShader9*      m_vs        = nullptr;
    IDirect3DPixelShader9*       m_ps        = nullptr;
    IDirect3DVertexShader9*      m_bloom_bright_vs  = nullptr;
    IDirect3DPixelShader9*       m_bloom_bright_ps  = nullptr;
    IDirect3DVertexShader9*      m_bloom_blur_vs    = nullptr;
    IDirect3DPixelShader9*       m_bloom_blur_ps    = nullptr;
    IDirect3DVertexShader9*      m_bloom_combine_vs = nullptr;
    IDirect3DPixelShader9*       m_bloom_combine_ps = nullptr;
    IDirect3DTexture9*           m_bloomA     = nullptr; // quarter-res ping
    IDirect3DSurface9*           m_bloomASurf = nullptr;
    IDirect3DTexture9*           m_bloomB     = nullptr; // quarter-res pong
    IDirect3DSurface9*           m_bloomBSurf = nullptr;
    IDirect3DVertexDeclaration9* m_mesh_decl = nullptr;
    IDirect3DVertexDeclaration9* m_skin_mesh_decl = nullptr;
    IDirect3DVertexDeclaration9* m_lightmap_mesh_decl = nullptr;
    IDirect3DVertexShader9*      m_skin_vs = nullptr;
    IDirect3DVertexShader9*      m_lightmap_vs = nullptr;
        IDirect3DPixelShader9*       m_shadow_ps = nullptr;
    IDirect3DTexture9*           m_def_white  = nullptr; // default diffuse
    IDirect3DTexture9*           m_def_normal = nullptr; // default flat normal
    IDirect3DTexture9*           m_def_black  = nullptr; // default specular / emissive / metallic
    IDirect3DCubeTexture9*       m_def_envcube = nullptr; // black cube: env fallback
    IDirect3DCubeTexture9*       m_env         = nullptr; // static env map (assets/env), fallback

    // Dynamic environment capture: each frame the scene is rendered into this
    // small cube from the first reflective object's position, so reflections
    // show the ACTUAL scene (objects, lit walls) rather than a painted sky.
    IDirect3DCubeTexture9*       m_env_dyn       = nullptr; // D3DPOOL_DEFAULT (device-lost aware)
    IDirect3DSurface9*           m_env_dyn_depth = nullptr;
    // Blurred copy of the capture, ENV_MIPS levels deep — what actually binds to
    // s5. Roughness picks a level (standard.hlsl texCUBElod). Kept separate from
    // m_env_dyn because D3D9 cannot render into level L of a texture while
    // sampling its level 0; filtering every level from the sharp capture also
    // means no ping-pong ordering to get wrong.
    IDirect3DCubeTexture9*       m_env_blur      = nullptr;
    // Scratch chain the blur ping-pongs against: the filter is progressive
    // (level L reads level L-1), and D3D9 cannot sample a texture while
    // rendering into another level of it, so the two cubes take turns.
    IDirect3DCubeTexture9*       m_env_blur_tmp  = nullptr;
    IDirect3DVertexShader9*      m_env_blur_vs   = nullptr;
    IDirect3DPixelShader9*       m_env_blur_ps   = nullptr;
    bool                         m_env_captured  = false;   // captured this frame -> bind m_env_blur
    // One cube face of the capture: a reduced scene pass (models only — no
    // grid/gizmos/outlines/post) from the reflective object's point of view.
    void DrawSceneForEnv(const D3DMATRIX& view, const D3DMATRIX& proj,
                         const float eye[3], int skip_object);
    // Fill m_env_blur from m_env_dyn: level 0 is a straight copy, levels 1+ are
    // direction-space cone filters (env_blur.hlsl). Call inside the capture's
    // BeginScene, after the six faces are drawn.
    void BuildEnvBlurChain();

    // --- Skybox ("Skybox" attribute) ---
    // An equirectangular image drawn as the scene background: a screen quad at
    // the far plane, so geometry occludes it through the depth test rather than
    // through any sorting. Also drawn into each face of the capture above, so
    // reflections show the sky instead of the clear colour. First Skybox in
    // scene order wins; captured in RenderUi, consumed in RenderGpu.
    std::string                  m_sky_path;             // project-relative, empty = no sky
    float                        m_sky_rotation  = 0.0f; // degrees, yaw
    std::string                  m_sky_loaded;           // path m_sky_tex was built from
    IDirect3DTexture9*           m_sky_tex       = nullptr;
    IDirect3DVertexShader9*      m_sky_vs        = nullptr;
    IDirect3DPixelShader9*       m_sky_ps        = nullptr;
    // Draws the sky for the given camera. Leaves depth state as it found it;
    // the caller owns BeginScene.
    void RenderSkybox(const D3DMATRIX& view, const D3DMATRIX& proj, bool depth_test);

    std::filesystem::path        m_standard_src;         // <exe>/standard.hlsl (engine source)
    std::filesystem::path        m_project_root;         // compiled shaders live in <root>/shaders
    std::filesystem::path        m_lightmap_scene;
    std::filesystem::file_time_type m_lightmap_timestamp = {};
    lprobe::Grid                   m_probe_grid;
    IDirect3DTexture9*           m_lightmap0 = nullptr;
    IDirect3DTexture9*           m_lightmap1 = nullptr;
    std::map<int, LightmapGpuMesh> m_lightmap_meshes;
    IDirect3DTexture9*           m_shadow_texture = nullptr;
    IDirect3DSurface9*           m_shadow_surface = nullptr;
    IDirect3DSurface9*           m_shadow_depth = nullptr;
    D3DMATRIX                    m_shadow_matrix = {};
    bool                         m_shadow_enabled = false;

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
    // Non-const because spawn() appends live objects to the scene for the
    // session (see m_authored_object_count); the authored data is never edited.
    void StartPhysics(SceneFile& scene);
    // One object's collider descriptors — shared by the Play-session build and
    // by spawn(), so a spawned body is set up exactly like an authored one.
    void AppendBodyDescs(const SceneFile& scene, int objectIndex,
                         std::vector<phys::BodyDesc>& descs,
                         std::vector<std::vector<float>*>& geomPos,
                         std::vector<std::vector<unsigned int>*>& geomIdx);
    void StartScripts(const SceneFile& scene);
    bool ReadScriptSource(const std::string& relPath, std::string& out);
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

    // Spot-light cone gizmo, shown for the selected object only (display-only,
    // like the collider wires — the Inspector's Range / Inner / Outer values
    // reshape it live; the object gizmo moves/aims it).
    struct SpotGizmo
    {
        D3DMATRIX base;      // rotation + translation, beam down local +Z
        float     range;
        float     inner_deg; // cone half-angles, degrees
        float     outer_deg;
    };
    std::vector<SpotGizmo> m_spot_gizmos; // rebuilt each frame in RenderUi
    void AppendSpotCone(const SpotGizmo& sg, std::vector<Vertex>& out) const;

    // Spot volumetric beams ("Project Light Shaft"): an additive unit-cone mesh
    // (beam.hlsl) scaled to tan(outer) * range by its world matrix. Captured in
    // RenderUi next to the spot slots, drawn after the translucents.
    struct SpotBeam
    {
        D3DMATRIX world;    // unit cone (apex origin, +Z, radius = z) -> world
        float     color[4]; // rgb premultiplied: spot color * intensity * beam * gain
        float     apex[4];  // world apex (PS c2) — the PS rebuilds the cone
        float     axis[4];  // normal analytically: xyz = unit dir, w = tan(outer)
    };
    std::vector<SpotBeam>   m_spot_beams;         // 0..2, rebuilt each frame
    std::vector<MeshVertex> m_beam_cone;          // unit cone, built on first use
    IDirect3DVertexShader9* m_beam_vs = nullptr;  // beam.hlsl (optional, like bloom)
    IDirect3DPixelShader9*  m_beam_ps = nullptr;
    void BuildBeamCone(); // fill m_beam_cone: 24-segment unit-cone triangle fan

    phys::PhysicsWorld           m_phys;
    bool                         m_phys_on = false;
    std::vector<phys::Pose>      m_phys_poses;
    std::vector<std::string>     m_phys_names;   // object names, for trigger logs + find
    std::vector<PhysDebug>       m_phys_debug;

    // Lua scripting: built alongside physics on Play, stepped before m_phys.
    script::ScriptVM             m_script;

    // Controller + keyboard snapshot, polled each Play frame; read by ScriptHost.
    input::InputState            m_input;

    // Models to draw this frame (captured in RenderUi) + the mesh cache that
    // bakes/loads them.
    std::vector<DrawItem>   m_draw_items;
    std::vector<PickItem>   m_pick_items;
    std::vector<ShaderItem> m_shader_items;
    MeshCache               m_meshes;
    ShaderCache             m_shaders;
    float                   m_time = 0.0f; // seconds, for animated custom shaders

    struct ControllerCacheEntry
    {
        AnimatorController controller;
        bool attempted = false;
        bool valid = false;
    };
    struct ClipCacheEntry
    {
        AnimationClip clip;
        bool attempted = false;
        bool valid = false;
    };
    struct AnimatorInstance
    {
        std::string controller_path;
        AnimatorRuntimeState runtime;
        std::map<std::string, BoneModifierPhysicsState> physics_states;
        std::vector<int> bone_collider_handles;
        std::vector<SceneBoneColliderPose> bone_collider_poses;
    };
    std::map<std::string, ControllerCacheEntry> m_animator_controllers;
    std::map<std::string, ClipCacheEntry>       m_animation_clips;
    std::map<int, AnimatorInstance>             m_animator_instances;
    void UpdateAnimations(float dt);

    // Unit quad + unit cube (mesh vertex layout) for standalone shaders
    IDirect3DVertexBuffer9* m_quad_vb = nullptr;
    IDirect3DIndexBuffer9*  m_quad_ib = nullptr;
    IDirect3DVertexBuffer9* m_cube_vb = nullptr;
    IDirect3DIndexBuffer9*  m_cube_ib = nullptr;

    struct ImageItem
    {
        std::string path_abs;
        float x = 0, y = 0, w = 0, h = 0;
        bool  stretch = false;
        bool  lock_aspect = false;
        float tint[3] = {1, 1, 1};
        float alpha = 1.0f;
        int   priority = 1;
        int   sequence = 0;
    };

    struct TextItem
    {
        std::string object;
        std::string font_path_abs;
        std::string value;
        float x = 0, y = 0, w = 0, h = 0;
        float font_size = 32.0f;
        float color[3] = {1, 1, 1};
        float alpha = 1.0f;
        bool  lock_aspect = false;
        int   priority = 1;
        int   sequence = 0;
    };

    struct PreviewFont
    {
        text::FontMetrics metrics;
        IDirect3DTexture9* atlas = nullptr;
        bool attempted = false;
    };
    PreviewFont* EnsurePreviewFont(const std::string& path);

    // --- Image/video overlays ---
    // Captured in RenderUi, drawn by RenderOverlay after the bloom combine:
    // screen-space quads sampling the shared VideoPlayer's YUV planes through
    // the video.hlsl builtin. Edit mode freezes on the first frame (dt = 0);
    // the Play preview advances the clock.
    struct VideoItem
    {
        std::string key;       // stream identity: object name + '#' + attr index
        std::string object;    // owning object's name (Lua video.* lookups)
        std::string path_abs;  // absolute .mpg path
        float x = 0, y = 0, w = 0, h = 0; // 1280x720 reference space
        bool  stretch = false;
        bool  lock_aspect = true;
        float tint[3] = {1, 1, 1};
        float alpha = 1.0f;
        int   priority = 1;    // higher = drawn behind
        int   sequence = 0;    // scene order for equal priorities
        int   play_mode = 2;   // vid::PlayMode
        float volume = 1.0f;   // audio (Play mode only)
        bool  muted = false;
    };

    // The three L8 plane textures for one stream (MANAGED — survive resets).
    struct VideoTex
    {
        std::string        key;
        IDirect3DTexture9* y  = nullptr;
        IDirect3DTexture9* cb = nullptr;
        IDirect3DTexture9* cr = nullptr;
        int      yW = 0, yH = 0, cW = 0, cH = 0;
        unsigned lastFrame = 0; // vid::Frame::frameId last uploaded
        bool     used = false;  // reconcile mark
    };

    void RenderOverlay(float dt);
    VideoTex* EnsureVideoTex(const std::string& key, const vid::Frame& frame);

    // --- Lua-scriptable GUI (the `gui` table; src/gui is shared with the 360) ---
    // The tree lives for the Play session only, like the ScriptVM. Drawn by
    // RenderGui straight after the overlay — i.e. AFTER the bloom combine, so a
    // menu is always crisp and never glows.
    void RenderGui();
    gui::Context m_gui;
    // Texture ids handed to the GUI core are indices into this vector, so they
    // stay stable for the session even as the map behind them grows.
    std::vector<IDirect3DTexture9*>   m_gui_textures;
    std::map<std::string, int>        m_gui_texture_ids;
    std::map<std::string, int>        m_gui_font_atlas_ids;
    IDirect3DTexture9*                m_gui_white = nullptr; // 1x1, for solid quads
    IDirect3DVertexShader9*           m_gui_vs = nullptr;
    IDirect3DPixelShader9*            m_gui_ps = nullptr;
    // The item's stream key / play mode with any script override applied
    // (evaluated at draw time, after this frame's scripts ran).
    std::string VideoKeyFor(const VideoItem& item) const;
    int         VideoModeFor(const VideoItem& item) const;

    // --- Audio ("Audio" attribute; mirrors the Vulkan engine's reconciler) ---
    // Captured in RenderUi, reconciled by UpdateAudio each frame: sounds exist
    // only while the Play preview runs (edit mode passes an empty set, which
    // silences everything — the Vulkan editor's exact behavior). 3D fields are
    // carried now, applied in the spatial phase.
    struct AudioItem
    {
        std::string key;      // object name + '#' + attr index
        std::string object;   // owning object's name (Lua lookups)
        std::string path_abs; // absolute .mp2 path
        bool  play = false;   // playMode On
        bool  loop = false;
        float volume = 1.0f;
        float pitch = 1.0f;
        int   audio_class = aud::AudioEffect;
        int   priority = 0;
        int   load_mode = aud::AudioLoadAuto;
        bool  spatial = true;
        float min_dist = 1.0f;
        float max_dist = 50.0f;
        float doppler = 1.0f;
        float pos[3] = {0, 0, 0}; // authored position (live pose wins in Play)
        int   object_index = -1;  // physics pose lookup
    };
    void UpdateAudio(float dt);
    // Script overrides (object name -> partial state), transient per Play
    // session like the video ones; -1 floats/ints mean "no override". The
    // generation restarts a stopped/finished clip from the top (new key).
    struct AudioOverride
    {
        int      play   = -1; // 0/1
        int      loop   = -1;
        float    volume = -1.0f;
        float    pitch  = -1.0f;
        unsigned gen    = 0;
    };
    // Live per-object scene state for the Play session. Unlike the runtime,
    // which owns its scene outright, the editor is handed the AUTHORED project
    // every frame — so a script's writes land here instead of in SceneObject.
    // Nothing a script does can dirty the project or survive Stop.
    //
    // Sized in StartPhysics (one entry per object, seeded from the authored
    // values) and cleared in StopPhysics. `dirty` stays false until a script
    // actually writes, so the draw loop pays nothing for untouched objects.
    struct LiveObject
    {
        float pos[3]   = {0, 0, 0};
        float rot[3]   = {0, 0, 0};
        float scale[3] = {1, 1, 1};
        bool  visible  = true;
        int   cam_active = -1; // -1 = authored, else camera.set_active override
        bool  dirty    = false;
        std::vector<std::string> tags;
        // Cloned from the authored attributes the first time a script writes a
        // field (obj:set). Empty until then, so the common case costs nothing.
        bool  has_attrs = false;
        std::vector<ObjectAttribute> attrs;
    };
    std::vector<LiveObject> m_live;
    // Object lifetime for the Play session. Slots are append-only with a free
    // list and a per-slot generation, exactly as on the console, so a handle to
    // a destroyed object can't alias whatever spawn reuses its slot.
    std::vector<unsigned>      m_obj_gen;
    std::vector<unsigned char> m_obj_alive;
    std::vector<int>           m_pending_destroy;
    // Spawned objects are appended to the SceneFile so RenderUi (which iterates
    // the authored scene) can draw them at all. The project is NOT marked dirty
    // and StopPhysics truncates them away again, so nothing survives the session.
    SceneFile*                 m_play_scene = nullptr;
    std::size_t                m_authored_object_count = 0;
    std::string                m_pending_scene;   // scene.load, applied on Stop
    float                      m_load_ready_time = 0.0f; // m_time the swap may happen at
    float                      m_load_min_time   = 0.0f; // opt-in hold, 0 = none
    int                        m_load_frames     = 0;    // frames drawn since it began
    void ApplyPendingDestroys();
    // Overlay `authored` with this object's live state, if a script changed it.
    // Returns false when nothing was overridden and `authored` can be used as is.
    bool ApplyLive(int objectIndex, const SceneObject& authored, SceneObject& out) const;

    std::string AudioKeyFor(const AudioItem& item) const;
    std::map<std::string, AudioOverride> m_audio_overrides;
    std::vector<AudioItem> m_audio_items;
    aud::AudioPlayer       m_audio;

    std::vector<VideoItem> m_video_items;
    std::vector<VideoTex>  m_video_tex;
    // Script play/stop overrides (object name -> mode + restart generation),
    // live only for the Play session — the scene's authored play_mode is never
    // touched. The generation is folded into the stream key, so a video.play
    // on a stopped/finished video recreates its stream (restart from the top).
    struct VideoOverride
    {
        int      mode = 0; // vid::PlayMode
        unsigned gen  = 0;
    };
    std::map<std::string, VideoOverride> m_video_overrides;
    std::vector<ImageItem> m_image_items;
    std::vector<TextItem> m_text_items;
    std::map<std::string, std::string> m_text_overrides;
    std::map<std::string, IDirect3DTexture9*> m_image_textures;
    std::map<std::string, PreviewFont> m_text_fonts;
    vid::VideoPlayer       m_video;
    IDirect3DVertexShader9* m_image_vs = nullptr;
    IDirect3DPixelShader9*  m_image_ps = nullptr;
    IDirect3DVertexShader9* m_text_vs = nullptr;
    IDirect3DPixelShader9*  m_text_ps = nullptr;
    IDirect3DVertexShader9* m_video_vs = nullptr;
    IDirect3DPixelShader9*  m_video_ps = nullptr;
};
