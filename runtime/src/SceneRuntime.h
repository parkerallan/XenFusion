#pragma once

#include "Content.h"
#include "SceneData.h"
#include "StreamCache.h"
#include "camera/CameraResolve.h"
#include "physics/PhysicsWorld.h"
#include "script/ScriptVM.h"
#include "script/ScriptTypes.h"
#include "input/InputState.h"

#include <xtl.h>

// The stripped-down runtime renderer: the console analogue of the editor's
// SceneRenderer, minus everything editor-only. There is no grid, no gizmo, no
// selection highlight, and no keyboard/mouse camera — just the scene's meshes
// and custom-shader objects drawn from a fixed camera, straight to the back
// buffer. It reuses the editor's binary formats (.mesh / .scene) and material
// (standard.hlsl), so a scene authored in the editor renders the same here.
class SceneRuntime : public script::ScriptHost
{
public:
    // script::ScriptHost — input is stubbed until the input system lands; log
    // goes to OutputDebugString; find resolves a scene object name to its index.
    bool  InputButton(const char* name);
    float InputAxis(const char* name);
    void  Log(const char* msg);
    int   FindObject(const char* name);
    const char* ObjectName(int index);

    // contentRoot is the deployed game root, e.g. "game:\". Loads the standard
    // material, the startup scene, and builds shared geometry. Returns false if
    // the device objects can't be created.
    bool Init(IDirect3DDevice9* device, const std::string& contentRoot);
    void Shutdown();

    // Draw the whole scene for one frame (device scene already begun by the
    // caller). dt advances gTime for animated custom shaders.
    void Render(float dt);

private:
    struct DrawItem
    {
        std::string model_path;
        D3DMATRIX   world;
        int         object_index; // for physics pose override
        float       scale[3];     // authored scale (collider is sized separately)
    };
    struct ShaderItem
    {
        std::string shader_path;
        std::string model_path;
        float pos[3];
        float rot[3];
        float scale[3];
    };

    void BuildDrawLists();
    void BuildPhysics(); // create the Bullet world from Rigid Body / Trigger attributes
    void BuildScripts(); // load each object's .lua and run on_start
    // Extract CPU collision geometry (positions, + indices for exact) for a mesh
    // collider straight from the pak's MSH2 blob. Native-endian on the console.
    bool LoadPakMeshGeometry(const std::string& relPath, std::vector<float>& pos,
                             std::vector<unsigned int>& idx, bool wantIndices);
    void DrawMesh(RtMesh* gm, const D3DMATRIX& world, const D3DMATRIX& vp, RtShader* mat,
                  bool blendPass); // false = opaque+cutout subsets, true = blend subsets
    void DrawShaderItem(const ShaderItem& item, RtShader& shader, const D3DMATRIX& viewProj);
    IDirect3DTexture9* SolidTexture(D3DCOLOR argb);
    bool BuildGeometry();

    // Resolve a scene mesh: streamed from game.spak via the cache first, falling
    // back to the raw D3DX loader while both are shipped (pre phase-5 cutover).
    RtMesh* ResolveMesh(const std::string& relPath);

    IDirect3DDevice9*            m_device;
    std::string                  m_root;
    Content                      m_content;
    RtScene                      m_scene;

    IDirect3DVertexDeclaration9* m_mesh_decl;
    IDirect3DTexture9*           m_def_white;
    IDirect3DTexture9*           m_def_normal;
    IDirect3DTexture9*           m_def_black;

    // Unit quad + unit cube (mesh vertex layout) for standalone shaders.
    IDirect3DVertexBuffer9*      m_quad_vb;
    IDirect3DIndexBuffer9*       m_quad_ib;
    IDirect3DVertexBuffer9*      m_cube_vb;
    IDirect3DIndexBuffer9*       m_cube_ib;

    // Fixed orbit camera (the editor's default framing; no input on the console).
    // Used only when the scene has no active "Camera" attribute.
    float m_yaw;
    float m_pitch;
    float m_distance;
    float m_target[3];
    float m_eye[3];

    // Active scene camera ("Camera" attribute with active=true, first wins).
    // BuildDrawLists records WHICH camera (object/attribute index + target
    // object index); Render resolves the view per frame through the shared
    // camera/CameraResolve.h so Follow/Track see live physics/script motion.
    bool  m_has_cam;
    int   m_cam_object;      // index into m_scene.objects
    int   m_cam_attr;        // index into that object's attributes
    int   m_cam_target;      // follow target object index (-1 = none)
    float m_cam_fov;  // vertical FOV, degrees
    float m_cam_near;
    float m_cam_far;

    // Per-frame camera state: track advancement + follow smoothing.
    float m_track_dist;
    float m_track_speed;
    camr::FollowSmooth m_follow_smooth;

    // Scene lights captured in BuildDrawLists — ready-made PS constant payloads
    // (c0 / c6 / c7-c10 / c11-c14), mirroring the editor's SceneRenderer. The
    // legacy fixed sun is used when the scene has no light attributes at all.
    float m_light_dir[4];
    float m_light_col[4];
    float m_point_pos[4][4]; // xyz + w = 1/range^2
    float m_point_col[4][4]; // rgb * intensity; zero = unused slot

    float m_time;

    std::vector<DrawItem>   m_draw_items;
    std::vector<ShaderItem> m_shader_items;

    // Streaming: the pak is opened once and driven through the residency cache.
    StreamPak   m_pak;
    StreamCache m_cache;

    // Physics (Bullet) — same wrapper the editor preview uses. Built in Init from
    // the scene's Rigid Body / Trigger attributes, stepped each Render(dt).
    phys::PhysicsWorld      m_phys;
    std::vector<phys::Pose> m_phys_poses;

    // Lua scripting — same VM the editor uses. Built in Init, stepped before m_phys.
    script::ScriptVM        m_script;

    // Controller snapshot, polled from XInput each frame; read by the ScriptHost.
    input::InputState       m_input;
};
