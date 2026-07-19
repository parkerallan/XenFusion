#include "render/SceneRenderer.h"

#include "core/Log.h"
#include "project/ProjectIO.h"
#include "render/Shader.h"
#include "render/ShaderCompiler.h"
#include "state/EngineState.h"

#include "imgui.h"
#include "ImGuizmo.h"

#include <windows.h> // GetModuleFileNameW
#include <Xinput.h>
#include "input/XInputPoll.h"
#include "input/ControllerMapping.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace
{
    IDirect3DTexture9* CreateSolidTexture(IDirect3DDevice9* dev, D3DCOLOR argb)
    {
        IDirect3DTexture9* t = nullptr;
        if (FAILED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)))
            return nullptr;
        D3DLOCKED_RECT r;
        if (SUCCEEDED(t->LockRect(0, &r, nullptr, 0)))
        {
            *static_cast<D3DCOLOR*>(r.pBits) = argb;
            t->UnlockRect(0);
        }
        return t;
    }

    // Read CPU-side collision geometry from a baked GpuMesh by locking its managed
    // vertex/index buffers (positions = first 3 floats of each MeshVertex). Indices
    // are only needed for exact (triangle-mesh) colliders. One-time cost at Play.
    bool ExtractMeshGeometry(GpuMesh* gm, std::vector<float>& pos,
                             std::vector<unsigned int>& idx, bool wantIndices)
    {
        if (!gm || !gm->vb || gm->vertexCount == 0)
            return false;
        void* vp = nullptr;
        if (FAILED(gm->vb->Lock(0, 0, &vp, D3DLOCK_READONLY)))
            return false;
        const unsigned char* base = static_cast<const unsigned char*>(vp);
        pos.resize((std::size_t)gm->vertexCount * 3);
        for (uint32_t v = 0; v < gm->vertexCount; ++v)
        {
            const float* fp = reinterpret_cast<const float*>(base + (std::size_t)v * sizeof(MeshVertex));
            pos[v * 3 + 0] = fp[0]; pos[v * 3 + 1] = fp[1]; pos[v * 3 + 2] = fp[2];
        }
        gm->vb->Unlock();

        if (wantIndices && gm->ib && gm->indexCount > 0)
        {
            void* ip = nullptr;
            if (SUCCEEDED(gm->ib->Lock(0, 0, &ip, D3DLOCK_READONLY)))
            {
                const unsigned int* src = static_cast<const unsigned int*>(ip);
                idx.assign(src, src + gm->indexCount);
                gm->ib->Unlock();
            }
        }
        return true;
    }
}

// ---------------------------------------------------------------------------
// Minimal matrix helpers (left-handed, row-major to match D3D9). Hand-rolled
// so we don't pull in the legacy d3dx9 helper library.
// ---------------------------------------------------------------------------
namespace
{
    struct Vec3 { float x, y, z; };

    Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    Vec3 Normalize(const Vec3& v)
    {
        float len = std::sqrt(Dot(v, v));
        if (len < 1e-6f) return {0, 0, 0};
        return {v.x / len, v.y / len, v.z / len};
    }

    D3DMATRIX LookAtLH(const Vec3& eye, const Vec3& at, const Vec3& up)
    {
        Vec3 z = Normalize(Sub(at, eye));
        Vec3 x = Normalize(Cross(up, z));
        Vec3 y = Cross(z, x);

        D3DMATRIX m = {};
        m._11 = x.x; m._12 = y.x; m._13 = z.x; m._14 = 0.0f;
        m._21 = x.y; m._22 = y.y; m._23 = z.y; m._24 = 0.0f;
        m._31 = x.z; m._32 = y.z; m._33 = z.z; m._34 = 0.0f;
        m._41 = -Dot(x, eye); m._42 = -Dot(y, eye); m._43 = -Dot(z, eye); m._44 = 1.0f;
        return m;
    }

    D3DMATRIX PerspectiveFovLH(float fovY, float aspect, float zn, float zf)
    {
        float yScale = 1.0f / std::tan(fovY * 0.5f);
        float xScale = yScale / aspect;

        D3DMATRIX m = {};
        m._11 = xScale;
        m._22 = yScale;
        m._33 = zf / (zf - zn);
        m._34 = 1.0f;
        m._43 = -zn * zf / (zf - zn);
        return m;
    }

    D3DMATRIX Identity()
    {
        D3DMATRIX m = {};
        m._11 = m._22 = m._33 = m._44 = 1.0f;
        return m;
    }

    D3DMATRIX Multiply(const D3DMATRIX& a, const D3DMATRIX& b)
    {
        D3DMATRIX r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] +
                            a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        return r;
    }

    D3DMATRIX Translation(float x, float y, float z)
    {
        D3DMATRIX m = Identity();
        m._41 = x; m._42 = y; m._43 = z;
        return m;
    }

    D3DMATRIX Scaling(float x, float y, float z)
    {
        D3DMATRIX m = Identity();
        m._11 = x; m._22 = y; m._33 = z;
        return m;
    }

    D3DMATRIX RotationX(float a)
    {
        D3DMATRIX m = Identity();
        const float c = std::cos(a), s = std::sin(a);
        m._22 = c; m._23 = s; m._32 = -s; m._33 = c;
        return m;
    }
    D3DMATRIX RotationY(float a)
    {
        D3DMATRIX m = Identity();
        const float c = std::cos(a), s = std::sin(a);
        m._11 = c; m._13 = -s; m._31 = s; m._33 = c;
        return m;
    }
    D3DMATRIX RotationZ(float a)
    {
        D3DMATRIX m = Identity();
        const float c = std::cos(a), s = std::sin(a);
        m._11 = c; m._12 = s; m._21 = -s; m._22 = c;
        return m;
    }

    // World = Scale * Rot(x,y,z) * Translation (row-vector convention).
    D3DMATRIX ComposeWorld(const SceneObject& o)
    {
        const float d2r = 3.14159265f / 180.0f;
        const D3DMATRIX rot = Multiply(Multiply(RotationX(o.rotation[0] * d2r),
                                                RotationY(o.rotation[1] * d2r)),
                                       RotationZ(o.rotation[2] * d2r));
        return Multiply(Multiply(Scaling(o.scale[0], o.scale[1], o.scale[2]), rot),
                        Translation(o.position[0], o.position[1], o.position[2]));
    }

    // Transpose the rotation 3x3 (translation zeroed) — the inverse of a rotation.
    D3DMATRIX Transpose3(const D3DMATRIX& m)
    {
        D3DMATRIX r = Identity();
        r._11 = m._11; r._12 = m._21; r._13 = m._31;
        r._21 = m._12; r._22 = m._22; r._23 = m._32;
        r._31 = m._13; r._32 = m._23; r._33 = m._33;
        return r;
    }

    // Transform a point (row vector, w = 1) by a matrix.
    Vec3 TransformPoint(const Vec3& p, const D3DMATRIX& m)
    {
        return { p.x * m._11 + p.y * m._21 + p.z * m._31 + m._41,
                 p.x * m._12 + p.y * m._22 + p.z * m._32 + m._42,
                 p.x * m._13 + p.y * m._23 + p.z * m._33 + m._43 };
    }
}

void SceneRenderer::Initialize(IDirect3DDevice9* device)
{
    m_device = device;
    m_meshes.Init(device);
    m_shaders.Init(device);
    BuildGrid();

    // The engine ships its built-in shader source in <exe>/shaders. It's
    // compiled into each project's own shaders/ folder (with the custom shaders)
    // by the Reload-shaders action.
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    m_standard_src = std::filesystem::path(std::wstring(exe, n)).parent_path() / "shaders" / "standard.hlsl";

    // Vertex declaration matching MeshVertex (pos/normal/tangent/uv).
    const D3DVERTEXELEMENT9 elems[] = {
        {0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
        {0, 24, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT,  0},
        {0, 36, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    device->CreateVertexDeclaration(elems, &m_mesh_decl);

    // Fallback textures for meshes missing a map.
    m_def_white  = CreateSolidTexture(device, D3DCOLOR_ARGB(255, 255, 255, 255)); // diffuse
    m_def_normal = CreateSolidTexture(device, D3DCOLOR_ARGB(255, 128, 128, 255)); // flat normal (0,0,1)
    m_def_black  = CreateSolidTexture(device, D3DCOLOR_ARGB(255, 0, 0, 0));       // no specular

    // Unit quad (XY plane, uv 0..1, v=0 at the bottom) for standalone shaders.
    // MeshVertex layout: pos, normal, tangent, uv. MANAGED so it survives resets.
    const MeshVertex quad[4] = {
        {-0.5f, -0.5f, 0.0f,  0,0,-1,  1,0,0,  0.0f, 0.0f},
        { 0.5f, -0.5f, 0.0f,  0,0,-1,  1,0,0,  1.0f, 0.0f},
        { 0.5f,  0.5f, 0.0f,  0,0,-1,  1,0,0,  1.0f, 1.0f},
        {-0.5f,  0.5f, 0.0f,  0,0,-1,  1,0,0,  0.0f, 1.0f},
    };
    const uint32_t quad_idx[6] = {0, 1, 2, 0, 2, 3};
    if (SUCCEEDED(device->CreateVertexBuffer(sizeof(quad), 0, 0, D3DPOOL_MANAGED, &m_quad_vb, nullptr)))
    {
        void* p = nullptr;
        if (SUCCEEDED(m_quad_vb->Lock(0, sizeof(quad), &p, 0))) { memcpy(p, quad, sizeof(quad)); m_quad_vb->Unlock(); }
    }
    if (SUCCEEDED(device->CreateIndexBuffer(sizeof(quad_idx), 0, D3DFMT_INDEX32, D3DPOOL_MANAGED, &m_quad_ib, nullptr)))
    {
        void* p = nullptr;
        if (SUCCEEDED(m_quad_ib->Lock(0, sizeof(quad_idx), &p, 0))) { memcpy(p, quad_idx, sizeof(quad_idx)); m_quad_ib->Unlock(); }
    }

    // Unit cube [-0.5, 0.5]^3 for "volume" shaders (raymarched in local space).
    // Only position matters; the volume pixel shader ignores normal/tangent/uv.
    MeshVertex cube[8];
    const float c = 0.5f;
    const float cx[8] = {-c,  c,  c, -c, -c,  c,  c, -c};
    const float cy[8] = {-c, -c,  c,  c, -c, -c,  c,  c};
    const float cz[8] = {-c, -c, -c, -c,  c,  c,  c,  c};
    for (int k = 0; k < 8; ++k)
        cube[k] = { cx[k], cy[k], cz[k],  0,0,0,  0,0,0,  0,0 };
    const uint32_t cube_idx[36] = {
        0,1,2, 0,2,3,  // -z
        5,4,7, 5,7,6,  // +z
        4,0,3, 4,3,7,  // -x
        1,5,6, 1,6,2,  // +x
        4,5,1, 4,1,0,  // -y
        3,2,6, 3,6,7,  // +y
    };
    if (SUCCEEDED(device->CreateVertexBuffer(sizeof(cube), 0, 0, D3DPOOL_MANAGED, &m_cube_vb, nullptr)))
    {
        void* p = nullptr;
        if (SUCCEEDED(m_cube_vb->Lock(0, sizeof(cube), &p, 0))) { memcpy(p, cube, sizeof(cube)); m_cube_vb->Unlock(); }
    }
    if (SUCCEEDED(device->CreateIndexBuffer(sizeof(cube_idx), 0, D3DFMT_INDEX32, D3DPOOL_MANAGED, &m_cube_ib, nullptr)))
    {
        void* p = nullptr;
        if (SUCCEEDED(m_cube_ib->Lock(0, sizeof(cube_idx), &p, 0))) { memcpy(p, cube_idx, sizeof(cube_idx)); m_cube_ib->Unlock(); }
    }
}

// Load the already-compiled standard shader from the project's shaders/ folder.
// No compiling here — that only happens via CompileShaders (the button).
void SceneRenderer::LoadStandardShader()
{
    if (m_project_root.empty())
        return;
    const std::filesystem::path out = m_project_root / "shaders";
    IDirect3DVertexShader9* vs = shader::LoadVS(m_device, out / "standard_vs.cso");
    IDirect3DPixelShader9*  ps = shader::LoadPS(m_device, out / "standard_ps.cso");
    if (vs && ps)
    {
        if (m_vs) m_vs->Release();
        if (m_ps) m_ps->Release();
        m_vs = vs;
        m_ps = ps;
    }
    else
    {
        if (vs) vs->Release();
        if (ps) ps->Release();
        applog::Warn("Shaders not compiled yet — press 'Compile shaders' in Settings");
    }
}

// "Compile shaders" action: compile every shader into <project>/shaders (the
// actual work lives in shadercompiler), then reload the results.
void SceneRenderer::CompileShaders()
{
    if (m_project_root.empty())
    {
        applog::Warn("Open a project to compile shaders");
        return;
    }
    shadercompiler::CompileAll(m_standard_src, m_project_root);
    LoadStandardShader(); // reload standard from the fresh .cso
    m_shaders.Clear();    // custom shaders reload from the fresh .cso on next use
}

void SceneRenderer::Shutdown()
{
    if (m_cube_ib)    { m_cube_ib->Release();    m_cube_ib = nullptr; }
    if (m_cube_vb)    { m_cube_vb->Release();    m_cube_vb = nullptr; }
    if (m_quad_ib)    { m_quad_ib->Release();    m_quad_ib = nullptr; }
    if (m_quad_vb)    { m_quad_vb->Release();    m_quad_vb = nullptr; }
    if (m_def_black)  { m_def_black->Release();  m_def_black = nullptr; }
    if (m_def_normal) { m_def_normal->Release(); m_def_normal = nullptr; }
    if (m_def_white)  { m_def_white->Release();  m_def_white = nullptr; }
    if (m_mesh_decl)  { m_mesh_decl->Release();  m_mesh_decl = nullptr; }
    if (m_ps)         { m_ps->Release();         m_ps = nullptr; }
    if (m_vs)         { m_vs->Release();         m_vs = nullptr; }
    m_shaders.Shutdown();
    m_meshes.Shutdown();
    OnDeviceLost();
    m_device = nullptr;
}

void SceneRenderer::OnDeviceLost()
{
    if (m_depthMS)   { m_depthMS->Release();   m_depthMS = nullptr; }
    if (m_rtMS)      { m_rtMS->Release();      m_rtMS = nullptr; }
    if (m_depth)     { m_depth->Release();     m_depth = nullptr; }
    if (m_rtSurface) { m_rtSurface->Release(); m_rtSurface = nullptr; }
    if (m_rt)        { m_rt->Release();        m_rt = nullptr; }
    m_width = m_height = 0;
}

void SceneRenderer::BeginFrame()
{
    m_renderRequested = false;
}

// --- UI phase: runs inside the ImGui frame -------------------------------
void SceneRenderer::RenderUi(EngineState& state)
{
    ImGuizmo::BeginFrame();

    const ImVec2 avail = ImGui::GetContentRegionAvail();

    // No project -> nothing to render; show an empty viewport.
    if (!state.HasProject())
    {
        const ImVec2 q0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##noproj", ImVec2((std::max)(avail.x, 1.0f), (std::max)(avail.y, 1.0f)));
        const char* msg = "No project open";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(q0.x + (avail.x - ts.x) * 0.5f, q0.y + (avail.y - ts.y) * 0.5f),
            IM_COL32(130, 130, 130, 255), msg);
        return; // m_renderRequested stays false -> RenderGpu no-ops
    }

    // On project open/change, load the compiled shaders. Compiling is only done
    // by the Compile-shaders button (CompileShaders), never automatically.
    if (m_project_root != state.project_root)
    {
        m_project_root = state.project_root;
        LoadStandardShader();
    }
    m_meshes.SetProjectRoot(state.project_root);  // clears cache on project change
    m_shaders.SetProjectRoot(state.project_root);

    // Compile-shaders button: compile every shader (standard + all custom).
    if (state.compile_shaders_requested)
    {
        CompileShaders();
        state.compile_shaders_requested = false;
    }

    // Capture the models + standalone shaders to draw from the selected scene,
    // plus the active "Camera" attribute (first wins) for the look-through view
    // and the scene lights (first directional + first four point lights).
    bool  have_cam = false;
    float cam_pos[3] = {}, cam_rot[3] = {};
    float cam_fov = 45.0f, cam_near = 0.5f, cam_far = 100.0f;
    bool  have_dir = false;
    int   point_count = 0;
    std::memset(m_point_pos, 0, sizeof(m_point_pos));
    std::memset(m_point_col, 0, sizeof(m_point_col));
    m_draw_items.clear();
    m_shader_items.clear();
    m_phys_debug.clear(); // rebuilt from the scene each frame (authoring wireframes)
    if (SceneFile* scene = state.SelectedScene())
    {
        for (int i = 0; i < (int)scene->objects.size(); ++i)
        {
            const SceneObject& o = scene->objects[i];
            if (!o.visible)
                continue;
            const bool selected = (i == state.selected_object);

            std::string model_path, shader_path;
            for (const ObjectAttribute& a : o.attributes)
            {
                if (a.type == "3D Model" && !a.model_path.empty() && model_path.empty())
                    model_path = a.model_path;
                else if (a.type == "Shader" && !a.shader_path.empty() && shader_path.empty())
                    shader_path = a.shader_path;
                else if (a.type == "Camera" && a.cam_active && !have_cam)
                {
                    for (int k = 0; k < 3; ++k) { cam_pos[k] = o.position[k]; cam_rot[k] = o.rotation[k]; }
                    cam_fov = a.cam_fov; cam_near = a.cam_near; cam_far = a.cam_far;
                    have_cam = true;
                }
                else if (a.type == "Directional Light" && !have_dir)
                {
                    // Direction = the object's forward (+Z through its rotation).
                    const float d2r = 3.14159265f / 180.0f;
                    const D3DMATRIX r = Multiply(Multiply(RotationX(o.rotation[0] * d2r),
                                                          RotationY(o.rotation[1] * d2r)),
                                                 RotationZ(o.rotation[2] * d2r));
                    m_light_dir[0] = r._31; m_light_dir[1] = r._32; m_light_dir[2] = r._33;
                    for (int k = 0; k < 3; ++k) m_light_col[k] = a.light_color[k] * a.light_intensity;
                    have_dir = true;
                }
                else if (a.type == "Point Light" && point_count < 4)
                {
                    const float range = (std::max)(a.light_range, 0.1f);
                    for (int k = 0; k < 3; ++k)
                    {
                        m_point_pos[point_count][k] = o.position[k];
                        m_point_col[point_count][k] = a.light_color[k] * a.light_intensity;
                    }
                    m_point_pos[point_count][3] = 1.0f / (range * range);
                    ++point_count;
                }
                else if (a.type == "Rigid Body" || a.type == "Trigger Volume")
                {
                    // Collider wireframe, shown always (not just while playing) so
                    // colliders can be sized against the mesh. Uses the authored
                    // transform; RenderGpu overrides with the simulated pose while
                    // playing. Rotation+translation only (collider ignores scale).
                    const bool trig = (a.type == "Trigger Volume");
                    PhysDebug pd;
                    pd.shape = trig ? a.trig_shape : a.phys_shape;
                    const float* sz = trig ? a.trig_size : a.phys_size;
                    pd.half[0] = sz[0]; pd.half[1] = sz[1]; pd.half[2] = sz[2];
                    pd.object_index = i;
                    pd.trigger = trig;
                    const float d2r = 3.14159265f / 180.0f;
                    const D3DMATRIX rot = Multiply(Multiply(RotationX(o.rotation[0] * d2r),
                                                            RotationY(o.rotation[1] * d2r)),
                                                   RotationZ(o.rotation[2] * d2r));
                    pd.base = Multiply(rot, Translation(o.position[0], o.position[1], o.position[2]));
                    m_phys_debug.push_back(pd);
                }
            }

            // A "//@geometry model" shader takes over the mesh, replacing the
            // standard material; anything else is standalone geometry.
            bool shader_owns_mesh = false;
            if (!shader_path.empty())
            {
                CustomShader* cs = m_shaders.Get(shader_path);
                shader_owns_mesh = cs && cs->state.geometry == ShaderState::Model;

                ShaderItem si;
                si.shader_path = shader_path;
                si.model_path  = model_path;
                si.pos[0] = o.position[0]; si.pos[1] = o.position[1]; si.pos[2] = o.position[2];
                si.rot[0] = o.rotation[0]; si.rot[1] = o.rotation[1]; si.rot[2] = o.rotation[2];
                si.scale[0] = o.scale[0]; si.scale[1] = o.scale[1]; si.scale[2] = o.scale[2];
                si.selected = selected;
                m_shader_items.push_back(si);
            }

            if (!model_path.empty() && !shader_owns_mesh)
            {
                DrawItem di;
                di.model_path   = model_path;
                di.world        = ComposeWorld(o);
                di.selected     = selected;
                di.object_index = i;
                di.scale[0] = o.scale[0]; di.scale[1] = o.scale[1]; di.scale[2] = o.scale[2];
                m_draw_items.push_back(di);
            }
        }
    }

    // Physics preview: build the Bullet world on the Play rising edge, tear it
    // down on Stop (or when the scene selection goes away). The button that
    // toggles state.physics_playing is drawn in the viewport overlay below.
    {
        SceneFile* pscene = state.SelectedScene();
        if (state.physics_playing && pscene)
        {
            if (!m_phys_on)
                StartPhysics(*pscene);
        }
        else if (m_phys_on)
        {
            StopPhysics();
            state.physics_playing = false;
        }
    }

    // While playing, poll the controller, then overlay keyboard/mouse per the
    // editor's Mapping panel (state.controller_mapping). Done here in RenderUi
    // where the ImGui key/mouse state is live; RenderGpu's script Update reads it.
    if (m_phys_on)
    {
        input::PollXInput(m_input);
        input::ApplyMapping(state.controller_mapping, m_input);
    }

    // No directional light authored: keep the legacy fixed sun — white when the
    // scene has no lights at all (old scenes render unchanged), black when point
    // lights exist (the author owns the lighting; only c0's direction, which
    // custom shaders read, stays meaningful).
    if (!have_dir)
    {
        const Vec3 ld = Normalize({-0.4f, -1.0f, -0.5f});
        m_light_dir[0] = ld.x; m_light_dir[1] = ld.y; m_light_dir[2] = ld.z;
        const float w = (point_count == 0) ? 1.0f : 0.0f;
        m_light_col[0] = m_light_col[1] = m_light_col[2] = w;
    }

    // Focus-key ("F") target: the selected object.
    m_has_focus = false;
    if (SceneObject* sel = state.SelectedObject())
    {
        m_focus_pos[0] = sel->position[0];
        m_focus_pos[1] = sel->position[1];
        m_focus_pos[2] = sel->position[2];
        m_focus_scale  = (std::max)({sel->scale[0], sel->scale[1], sel->scale[2]});
        m_has_focus = true;
    }

    const int w = (int)avail.x;
    const int h = (int)avail.y;

    // Size the target now, while the panel size is known, so the texture id
    // recorded below is valid and correctly sized for this frame.
    const bool ready = EnsureTarget(w, h);

    // Capture state for the GPU pass.
    auto to8 = [](float v) { int i = (int)(v * 255.0f + 0.5f); return i < 0 ? 0 : (i > 255 ? 255 : i); };
    m_background = D3DCOLOR_ARGB(255, to8(state.clear_color[0]),
                                 to8(state.clear_color[1]), to8(state.clear_color[2]));

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (ready && m_rt)
    {
        ImGui::Image((ImTextureID)(intptr_t)m_rt, avail);
        m_renderRequested = true; // tell RenderGpu to fill it this frame
        HandleCameraInput();      // must follow the Image item (uses IsItemHovered)

        // Play/Stop the physics preview (top-left overlay).
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 8.0f, p0.y + 8.0f));
        if (ImGui::Button(state.physics_playing ? "Stop" : "Play"))
            state.physics_playing = !state.physics_playing;

        // Look-through toggle (only offered while the scene has an active camera).
        if (have_cam)
        {
            ImGui::SetCursorScreenPos(ImVec2(p0.x + 8.0f, p0.y + 40.0f));
            ImGui::Checkbox("Camera view", &m_look_through);
        }

        // Camera matrices (also used by RenderGpu, so they stay in sync).
        if (have_cam && m_look_through)
        {
            // Through the active scene camera: basis from the object's rotation
            // (same Rot(x,y,z) order as ComposeWorld; rotation 0 looks down +Z),
            // projection from its fov/near/far — matching the 360 runtime.
            const float d2r = 3.14159265f / 180.0f;
            const D3DMATRIX rot = Multiply(Multiply(RotationX(cam_rot[0] * d2r),
                                                    RotationY(cam_rot[1] * d2r)),
                                           RotationZ(cam_rot[2] * d2r));
            Vec3 fwd = { rot._31, rot._32, rot._33 };
            Vec3 up  = { rot._21, rot._22, rot._23 };
            Vec3 eye = { cam_pos[0], cam_pos[1], cam_pos[2] };
            m_eye[0] = eye.x; m_eye[1] = eye.y; m_eye[2] = eye.z;
            m_view = LookAtLH(eye, {eye.x + fwd.x, eye.y + fwd.y, eye.z + fwd.z}, up);
            m_proj = PerspectiveFovLH(cam_fov * d2r, (float)m_width / (float)m_height,
                                      cam_near, cam_far);
        }
        else
        {
            Vec3 cdir = { std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch),
                          std::cos(m_pitch) * std::cos(m_yaw) };
            Vec3 cat  = { m_target[0], m_target[1], m_target[2] };
            Vec3 ceye = { cat.x + cdir.x * m_distance, cat.y + cdir.y * m_distance,
                          cat.z + cdir.z * m_distance };
            m_eye[0] = ceye.x; m_eye[1] = ceye.y; m_eye[2] = ceye.z;
            m_view = LookAtLH(ceye, cat, {0.0f, 1.0f, 0.0f});
            m_proj = PerspectiveFovLH(3.14159265f / 4.0f, (float)m_width / (float)m_height, 0.1f, 200.0f);
        }

        // Translation gizmo for the selected object.
        if (SceneObject* sel = state.SelectedObject())
        {
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist();
            ImGuizmo::SetRect(p0.x, p0.y, avail.x, avail.y);
            D3DMATRIX model = ComposeWorld(*sel);
            ImGuizmo::Manipulate(&m_view.m[0][0], &m_proj.m[0][0],
                                 ImGuizmo::TRANSLATE, ImGuizmo::WORLD, &model.m[0][0]);
            if (ImGuizmo::IsUsing())
            {
                sel->position[0] = model.m[3][0];
                sel->position[1] = model.m[3][1];
                sel->position[2] = model.m[3][2];
                if (SceneFile* s = state.SelectedScene()) s->dirty = true;
                m_gizmo_editing = true;
            }
            else if (m_gizmo_editing)
            {
                if (SceneFile* s = state.SelectedScene()) project::SaveScene(*s);
                m_gizmo_editing = false;
            }
        }
    }
    else
    {
        ImGui::InvisibleButton("##viewport_surface",
                               ImVec2((std::max)(avail.x, 1.0f), (std::max)(avail.y, 1.0f)));
        ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 12.0f, p0.y + 12.0f),
                                            IM_COL32(140, 150, 130, 255), "Preparing scene...");
    }
}

// Orbit camera controls: right-drag rotates, middle-drag pans, wheel zooms.
void SceneRenderer::HandleCameraInput()
{
    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();

    // Start a drag only when it begins over the viewport; keep tracking it
    // (even off the image) until the button is released.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))  m_rotating = true;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) m_panning  = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))  m_rotating = false;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) m_panning  = false;

    if (m_rotating)
    {
        m_yaw   -= io.MouseDelta.x * 0.01f;
        m_pitch -= io.MouseDelta.y * 0.01f;
        const float lim = 1.55f; // avoid flipping over the poles
        m_pitch = std::clamp(m_pitch, -lim, lim);
    }

    if (m_panning && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
    {
        // Move the target across the camera's right / up plane so the scene
        // follows the cursor ("grab and drag").
        Vec3 dir     = { std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch),
                         std::cos(m_pitch) * std::cos(m_yaw) };
        Vec3 forward = { -dir.x, -dir.y, -dir.z };
        Vec3 right   = Normalize(Cross(Vec3{0, 1, 0}, forward));
        Vec3 camUp   = Cross(forward, right);

        const float s = m_distance * 0.0015f;
        m_target[0] += (right.x * -io.MouseDelta.x + camUp.x * io.MouseDelta.y) * s;
        m_target[1] += (right.y * -io.MouseDelta.x + camUp.y * io.MouseDelta.y) * s;
        m_target[2] += (right.z * -io.MouseDelta.x + camUp.z * io.MouseDelta.y) * s;
    }

    if (hovered && io.MouseWheel != 0.0f)
    {
        m_distance *= std::pow(0.9f, io.MouseWheel);
        m_distance = std::clamp(m_distance, 2.0f, 200.0f);
    }

    // Focus key: frame the selected object (Unity-style "F").
    if (hovered && m_has_focus && ImGui::IsKeyPressed(ImGuiKey_F))
    {
        m_target[0] = m_focus_pos[0];
        m_target[1] = m_focus_pos[1];
        m_target[2] = m_focus_pos[2];
        m_distance  = std::clamp(m_focus_scale * 3.0f + 3.0f, 3.0f, 100.0f);
    }
}

// --- GPU phase: runs after ImGui::Render(), before the back-buffer scene ---
void SceneRenderer::RenderGpu(float dt)
{
    if (!m_device || !m_renderRequested || !m_rtSurface)
        return;

    m_time += dt; // drives animated custom shaders (gTime)

    // Physics preview: step Bullet, then override this frame's draw items with the
    // simulated poses (world = Scale * pose, keeping authored scale). Trigger
    // enter/exit events go to the Log. Same code the 360 runtime runs.
    if (m_phys_on)
    {
        // Input was polled in RenderUi (controller + Mapping-panel keyboard/mouse).
        m_script.Update(dt); // scripts set impulses/velocity/transforms for this step
        m_phys.Step(dt);
        m_phys.ReadPoses(m_phys_poses);
        for (size_t pi = 0; pi < m_phys_poses.size(); ++pi)
        {
            const phys::Pose& p = m_phys_poses[pi];
            D3DMATRIX pose;
            std::memcpy(&pose.m[0][0], p.matrix, sizeof(float) * 16);
            for (size_t di = 0; di < m_draw_items.size(); ++di)
            {
                if (m_draw_items[di].object_index != p.objectIndex)
                    continue;
                const float* s = m_draw_items[di].scale;
                m_draw_items[di].world = Multiply(Scaling(s[0], s[1], s[2]), pose);
            }
        }

        // Dispatch trigger enter events to scripts (no automatic engine logging —
        // a script's own on_trigger + log() is the only trigger output).
        std::vector<phys::TriggerEvent> events;
        m_phys.DrainTriggerEvents(events);
        for (size_t ei = 0; ei < events.size(); ++ei)
            if (events[ei].entered)
                m_script.FireTrigger(events[ei].triggerObjectIndex, events[ei].otherObjectIndex);
    }

    // Redirect rendering to the offscreen target, remembering the back buffer.
    IDirect3DSurface9* prevRt    = nullptr;
    IDirect3DSurface9* prevDepth = nullptr;
    m_device->GetRenderTarget(0, &prevRt);
    m_device->GetDepthStencilSurface(&prevDepth);

    // Render into the multisampled target when available (for alpha-to-coverage);
    // it is resolved into m_rt after EndScene.
    IDirect3DSurface9* sceneRt    = (m_msaa != D3DMULTISAMPLE_NONE) ? m_rtMS    : m_rtSurface;
    IDirect3DSurface9* sceneDepth = (m_msaa != D3DMULTISAMPLE_NONE) ? m_depthMS : m_depth;
    m_device->SetRenderTarget(0, sceneRt);
    m_device->SetDepthStencilSurface(sceneDepth);
    m_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, m_background, 1.0f, 0);

    if (SUCCEEDED(m_device->BeginScene()))
    {
        // Camera matrices were computed in RenderUi (shared with the gizmo).
        D3DMATRIX world = Identity();
        m_device->SetTransform(D3DTS_WORLD, &world);
        m_device->SetTransform(D3DTS_VIEW, &m_view);
        m_device->SetTransform(D3DTS_PROJECTION, &m_proj);

        // --- Grid (fixed-function line list) ---
        m_device->SetVertexShader(nullptr);
        m_device->SetPixelShader(nullptr);
        m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
        m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        m_device->SetTexture(0, nullptr);
        m_device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        if (!m_grid.empty())
        {
            m_device->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)(m_grid.size() / 2),
                                      m_grid.data(), sizeof(Vertex));
        }

        // --- Scene models (shader: diffuse / normal / specular) ---
        if (!m_draw_items.empty() && m_vs && m_ps && m_mesh_decl)
        {
            m_device->SetVertexDeclaration(m_mesh_decl);
            m_device->SetVertexShader(m_vs);
            m_device->SetPixelShader(m_ps);
            m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
            for (DWORD s = 0; s < 3; ++s)
            {
                m_device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                m_device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            }

            // Pixel-shader constants: light direction, camera position, ambient.
            const float cam_pos[4]   = { m_eye[0], m_eye[1], m_eye[2], 0.0f };
            const float ambient[4]   = { 0.22f, 0.22f, 0.25f, 0.0f };
            m_device->SetPixelShaderConstantF(0, m_light_dir, 1);
            m_device->SetPixelShaderConstantF(1, cam_pos, 1);
            m_device->SetPixelShaderConstantF(2, ambient, 1);
            m_device->SetPixelShaderConstantF(6, m_light_col, 1);
            m_device->SetPixelShaderConstantF(7, &m_point_pos[0][0], 4);
            m_device->SetPixelShaderConstantF(11, &m_point_col[0][0], 4);

            const D3DMATRIX vp = Multiply(m_view, m_proj);
            // Per-item transform + buffers, then each material subset binds its
            // own textures and draws its index range (multi-material meshes).
            auto set_item = [&](GpuMesh* gm, const DrawItem& item)
            {
                const D3DMATRIX wvp = Multiply(item.world, vp);
                m_device->SetVertexShaderConstantF(0, &wvp.m[0][0], 4);
                m_device->SetVertexShaderConstantF(4, &item.world.m[0][0], 4);
                m_device->SetStreamSource(0, gm->vb, 0, sizeof(MeshVertex));
                m_device->SetIndices(gm->ib);
            };
            // Bump-offset strength: max UV shift across the height field packed
            // in a normal map's alpha (tune by eye; keep in sync with
            // SceneRuntime.cpp). Subsets without a height field upload exactly
            // 0, so their UVs — and the cutout alpha they feed — are untouched.
            const float kBumpScale = 0.08f;
            auto draw_subset = [&](GpuMesh* gm, const GpuSubset& s)
            {
                const float bump[4] = { s.normalHasHeight ? kBumpScale : 0.0f, 0.0f, 0.0f, 0.0f };
                m_device->SetPixelShaderConstantF(5, bump, 1);
                m_device->SetTexture(0, s.diffuse  ? s.diffuse  : m_def_white);
                m_device->SetTexture(1, s.normal   ? s.normal   : m_def_normal);
                m_device->SetTexture(2, s.specular ? s.specular : m_def_black);
                m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                               gm->vertexCount, s.indexStart, s.indexCount / 3);
            };

            // Pass 1 - opaque + cutout subsets (both write depth, no blending).
            // Cutout (a masked alpha: hair cards / foliage) uses the diffuse's
            // alpha: alpha-to-coverage when the GPU supports it, so the mask edge
            // is anti-aliased and depth-correct with no sorting; otherwise a hard
            // alpha test as a fallback.
            m_device->SetRenderState(D3DRS_ALPHAREF,  128); // ~0.5
            m_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
            for (const DrawItem& item : m_draw_items)
            {
                GpuMesh* gm = m_meshes.Get(item.model_path);
                if (!gm)
                    continue;
                bool item_bound = false;
                for (const GpuSubset& s : gm->subsets)
                {
                    if (s.alpha == AlphaKind::Blend)
                        continue; // smooth translucency -> pass 2
                    if (!item_bound) { set_item(gm, item); item_bound = true; }
                    const bool cutout = s.alpha == AlphaKind::Cutout;
                    if (cutout && m_hasA2C)
                    {
                        // Alpha test at the mask's mid-point drops the fully-
                        // transparent background AND the faint low-alpha haze
                        // between strands (which otherwise renders as a pale
                        // veil/rim); A2C anti-aliases the remaining solid strand
                        // edges via MSAA coverage.
                        m_device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
                        m_device->SetRenderState(D3DRS_ALPHAREF, 170);
                        m_device->SetRenderState(m_a2cState, m_a2cOn);
                    }
                    else
                    {
                        m_device->SetRenderState(D3DRS_ALPHATESTENABLE, cutout ? TRUE : FALSE);
                        m_device->SetRenderState(D3DRS_ALPHAREF, 128);
                    }
                    draw_subset(gm, s);
                    if (cutout && m_hasA2C)
                        m_device->SetRenderState(m_a2cState, m_a2cOff);
                }
            }
            m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);

            // Pass 2 - alpha-blended (glass etc.), no depth write.
            m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
            m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            for (const DrawItem& item : m_draw_items)
            {
                GpuMesh* gm = m_meshes.Get(item.model_path);
                if (!gm)
                    continue;
                bool item_bound = false;
                for (const GpuSubset& s : gm->subsets)
                {
                    if (s.alpha != AlphaKind::Blend)
                        continue;
                    if (!item_bound) { set_item(gm, item); item_bound = true; }
                    draw_subset(gm, s);
                }
            }
            m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);

            // --- Selection outline (fixed-function orange wireframe) ---
            m_device->SetVertexShader(nullptr);
            m_device->SetPixelShader(nullptr);
            m_device->SetTransform(D3DTS_VIEW, &m_view);
            m_device->SetTransform(D3DTS_PROJECTION, &m_proj);
            m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
            m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
            m_device->SetTexture(0, nullptr);
            m_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
            m_device->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, 230, 138, 40));
            for (const DrawItem& item : m_draw_items)
            {
                if (!item.selected)
                    continue;
                GpuMesh* gm = m_meshes.Get(item.model_path);
                if (!gm)
                    continue;
                m_device->SetTransform(D3DTS_WORLD, &item.world);
                m_device->SetStreamSource(0, gm->vb, 0, sizeof(MeshVertex));
                m_device->SetIndices(gm->ib);
                m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                               gm->vertexCount, 0, gm->indexCount / 3);
            }
            m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
            m_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        }

        // --- Standalone shader objects: each is drawn by DrawShaderItem. ---
        if (!m_shader_items.empty() && m_quad_vb && m_quad_ib && m_mesh_decl)
        {
            m_device->SetVertexDeclaration(m_mesh_decl);
            const D3DMATRIX vp = Multiply(m_view, m_proj);
            for (const ShaderItem& si : m_shader_items)
            {
                CustomShader* cs = m_shaders.Get(si.shader_path);
                if (cs && cs->Valid())
                    DrawShaderItem(si, *cs, vp);
            }
            // Restore fixed-function defaults for the grid/mesh next frame.
            m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
            m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        }

        // --- Physics collider wireframes (green rigid, cyan trigger) ---
        // Drawn whenever the scene has colliders — during authoring (from the
        // authored transform, so you can size them against the mesh) and while
        // playing (RenderGpu's pose override makes them track the simulation).
        if (!m_phys_debug.empty())
        {
            std::vector<Vertex> lines;
            lines.reserve(m_phys_debug.size() * 48);
            for (size_t i = 0; i < m_phys_debug.size(); ++i)
            {
                const PhysDebug& pd = m_phys_debug[i];
                // Simulated pose if this object has one, else the authored base.
                D3DMATRIX m = pd.base;
                for (size_t pi = 0; pi < m_phys_poses.size(); ++pi)
                    if (m_phys_poses[pi].objectIndex == pd.object_index)
                    {
                        std::memcpy(&m.m[0][0], m_phys_poses[pi].matrix, sizeof(float) * 16);
                        break;
                    }
                const D3DCOLOR col = pd.trigger ? D3DCOLOR_ARGB(255, 90, 200, 230)
                                                : D3DCOLOR_ARGB(255, 90, 220, 110);
                AppendColliderWire(pd, m, col, lines);
            }
            if (!lines.empty())
            {
                D3DMATRIX ident = Identity();
                m_device->SetVertexShader(nullptr);
                m_device->SetPixelShader(nullptr);
                m_device->SetTransform(D3DTS_WORLD, &ident);
                m_device->SetTransform(D3DTS_VIEW, &m_view);
                m_device->SetTransform(D3DTS_PROJECTION, &m_proj);
                m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
                m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
                m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
                m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                m_device->SetTexture(0, nullptr);
                m_device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)(lines.size() / 2),
                                          lines.data(), sizeof(Vertex));
            }
        }

        m_device->EndScene();
    }

    // Resolve the multisampled scene into the single-sampled texture ImGui shows.
    if (m_msaa != D3DMULTISAMPLE_NONE && m_rtMS && m_rtSurface)
        m_device->StretchRect(m_rtMS, nullptr, m_rtSurface, nullptr, D3DTEXF_NONE);

    // Restore the back buffer as the active target.
    m_device->SetRenderTarget(0, prevRt);
    m_device->SetDepthStencilSurface(prevDepth);
    if (prevRt)    prevRt->Release();
    if (prevDepth) prevDepth->Release();
}

// Draw one standalone shader: pick geometry, build its transform + uniforms,
// set its parsed render states (raw values), and draw. The whole "how a shader
// draws" story is here in one place.
void SceneRenderer::DrawShaderItem(const ShaderItem& item, const CustomShader& shader, const D3DMATRIX& viewProj)
{
    const ShaderState& s = shader.state;
    const float d2r = 3.14159265f / 180.0f;

    const D3DMATRIX rot = Multiply(Multiply(RotationX(item.rot[0] * d2r),
                                            RotationY(item.rot[1] * d2r)),
                                   RotationZ(item.rot[2] * d2r));
    D3DMATRIX w = Multiply(Multiply(Scaling(item.scale[0], item.scale[1], item.scale[2]), rot),
                           Translation(item.pos[0], item.pos[1], item.pos[2]));

    IDirect3DVertexBuffer9* vb = m_quad_vb;
    IDirect3DIndexBuffer9*  ib = m_quad_ib;
    UINT verts = 4, prims = 2;

    if (s.geometry == ShaderState::Volume && m_cube_vb && m_cube_ib)
    {
        vb = m_cube_vb; ib = m_cube_ib; verts = 8; prims = 12;
        // Camera in the cube's local space, so the pixel shader can raymarch it.
        const D3DMATRIX invW =
            Multiply(Multiply(Translation(-item.pos[0], -item.pos[1], -item.pos[2]), Transpose3(rot)),
                     Scaling(1.0f / item.scale[0], 1.0f / item.scale[1], 1.0f / item.scale[2]));
        const Vec3 camObj = TransformPoint({ m_eye[0], m_eye[1], m_eye[2] }, invW);
        const float camObj4[4] = { camObj.x, camObj.y, camObj.z, 0.0f };
        m_device->SetPixelShaderConstantF(4, camObj4, 1); // gCamObj (PS c4)
    }
    GpuMesh* model_gm = nullptr; // "model" geometry: drawn per material subset
    if (s.geometry == ShaderState::Model)
    {
        // The shader is the mesh's material: draw the model, bind its textures.
        model_gm = m_meshes.Get(item.model_path);
        if (!model_gm)
            return; // no mesh to draw on
        vb = model_gm->vb; ib = model_gm->ib;
        verts = model_gm->vertexCount; prims = model_gm->indexCount / 3;
        for (DWORD t = 0; t < 3; ++t)
        {
            m_device->SetSamplerState(t, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(t, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(t, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            m_device->SetSamplerState(t, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        }
    }

    // Parsed render states — flat, no switching.
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, s.alphaBlend);
    m_device->SetRenderState(D3DRS_SRCBLEND,  s.srcBlend);
    m_device->SetRenderState(D3DRS_DESTBLEND, s.destBlend);
    m_device->SetRenderState(D3DRS_ZENABLE,      s.zEnable);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, s.zWrite);
    m_device->SetRenderState(D3DRS_CULLMODE,     s.cull);

    // Frame inputs any custom shader may use (same registers as the standard
    // material), so a "model" shader can light/texture the mesh like standard.
    const float* lightDir   = m_light_dir; // the scene's directional light
    const float camPos[4]   = { m_eye[0], m_eye[1], m_eye[2], 0.0f };
    const float ambient[4]  = { 0.22f, 0.22f, 0.25f, 0.0f };
    const float time4[4]    = { m_time, 0.0f, 0.0f, 0.0f };

    const D3DMATRIX wvp = Multiply(w, viewProj);
    m_device->SetVertexShader(shader.vs);
    m_device->SetPixelShader(shader.ps);
    m_device->SetVertexShaderConstantF(0, &wvp.m[0][0], 4);
    m_device->SetVertexShaderConstantF(4, &w.m[0][0], 4);
    m_device->SetPixelShaderConstantF(0, lightDir, 1);
    m_device->SetPixelShaderConstantF(1, camPos, 1);
    m_device->SetPixelShaderConstantF(2, ambient, 1);
    m_device->SetPixelShaderConstantF(3, time4, 1); // gTime
    m_device->SetStreamSource(0, vb, 0, sizeof(MeshVertex));
    m_device->SetIndices(ib);
    if (model_gm)
    {
        // "model" geometry: one draw per material subset, its textures bound.
        for (const GpuSubset& sub : model_gm->subsets)
        {
            m_device->SetTexture(0, sub.diffuse  ? sub.diffuse  : m_def_white);
            m_device->SetTexture(1, sub.normal   ? sub.normal   : m_def_normal);
            m_device->SetTexture(2, sub.specular ? sub.specular : m_def_black);
            m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, verts,
                                           sub.indexStart, sub.indexCount / 3);
        }
    }
    else
        m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, verts, 0, prims);
}

void SceneRenderer::BuildGrid()
{
    m_grid.clear();

    const int     half  = 10;
    const float   extent = (float)half;
    const D3DCOLOR line  = D3DCOLOR_ARGB(255, 60, 60, 70);
    const D3DCOLOR xAxis = D3DCOLOR_ARGB(255, 170, 70, 70);  // along +X (z = 0)
    const D3DCOLOR zAxis = D3DCOLOR_ARGB(255, 70, 90, 170);  // along +Z (x = 0)
    const D3DCOLOR yAxis = D3DCOLOR_ARGB(255, 90, 170, 80);  // up

    for (int i = -half; i <= half; ++i)
    {
        const float f = (float)i;
        D3DCOLOR c = (i == 0) ? xAxis : line;
        m_grid.push_back({-extent, 0.0f, f, c});
        m_grid.push_back({ extent, 0.0f, f, c});
        c = (i == 0) ? zAxis : line;
        m_grid.push_back({f, 0.0f, -extent, c});
        m_grid.push_back({f, 0.0f,  extent, c});
    }

    m_grid.push_back({0.0f, 0.0f, 0.0f, yAxis});
    m_grid.push_back({0.0f, extent * 0.5f, 0.0f, yAxis});
}

// --- Physics preview ---------------------------------------------------------

void SceneRenderer::StartPhysics(const SceneFile& scene)
{
    std::vector<phys::BodyDesc> descs;
    m_phys_names.assign(scene.objects.size(), std::string());

    // Heap-owned mesh geometry kept alive until Build() copies what it needs.
    std::vector<std::vector<float>*>        geomPos;
    std::vector<std::vector<unsigned int>*> geomIdx;

    for (int i = 0; i < (int)scene.objects.size(); ++i)
    {
        const SceneObject& o = scene.objects[i];
        m_phys_names[i] = o.name;

        // The object's 3D Model, used by mesh-shape colliders.
        std::string model_path;
        for (const ObjectAttribute& ma : o.attributes)
            if (ma.type == "3D Model" && !ma.model_path.empty()) { model_path = ma.model_path; break; }

        for (const ObjectAttribute& a : o.attributes)
        {
            const bool rigid = (a.type == "Rigid Body");
            const bool trig  = (a.type == "Trigger Volume");
            if (!rigid && !trig)
                continue;

            phys::BodyDesc d;
            d.objectIndex = i;
            for (int k = 0; k < 3; ++k) { d.pos[k] = o.position[k]; d.rotEulerDeg[k] = o.rotation[k]; }
            if (rigid)
            {
                d.isTrigger  = false;
                d.kind       = a.phys_kind;
                d.shape      = a.phys_shape;
                for (int k = 0; k < 3; ++k) d.halfExtents[k] = a.phys_size[k];
                d.mass        = a.phys_mass;
                d.linDamping  = a.phys_lin_damping;
                d.angDamping  = a.phys_ang_damping;
                d.restitution  = a.phys_restitution;
                d.friction     = a.phys_friction;
                d.gravity      = a.phys_gravity;
                d.gravityScale = a.phys_gravity_scale;
            }
            else
            {
                d.isTrigger = true;
                d.kind      = phys::BodyDesc::Static;
                d.shape     = a.trig_shape;
                for (int k = 0; k < 3; ++k) d.halfExtents[k] = a.trig_size[k];
            }

            // Mesh colliders: pull CPU geometry from the (baked) GpuMesh.
            if (d.shape == phys::BodyDesc::MeshConvex || d.shape == phys::BodyDesc::MeshExact)
            {
                const bool exact = (d.shape == phys::BodyDesc::MeshExact);
                GpuMesh* gm = model_path.empty() ? 0 : m_meshes.Get(model_path);
                std::vector<float>*        pv = new std::vector<float>();
                std::vector<unsigned int>* iv = new std::vector<unsigned int>();
                if (ExtractMeshGeometry(gm, *pv, *iv, exact) && !pv->empty())
                {
                    // Bake the object's scale into the collision vertices so the
                    // collider matches the SCALED visual mesh (primitives are sized
                    // explicitly instead; the body transform carries no scale).
                    for (std::size_t vi = 0; vi + 2 < pv->size(); vi += 3)
                    {
                        (*pv)[vi + 0] *= o.scale[0];
                        (*pv)[vi + 1] *= o.scale[1];
                        (*pv)[vi + 2] *= o.scale[2];
                    }
                    geomPos.push_back(pv);
                    geomIdx.push_back(iv);
                    d.meshVerts     = &(*pv)[0];
                    d.meshVertCount = (int)(pv->size() / 3);
                    if (exact && !iv->empty())
                    {
                        d.meshIndices    = &(*iv)[0];
                        d.meshIndexCount = (int)iv->size();
                    }
                }
                else
                {
                    delete pv; delete iv; // PhysicsWorld falls back to a box
                    applog::Warn("Physics: mesh collider on \"" + o.name +
                                 "\" has no baked mesh; using a box");
                }
            }

            descs.push_back(d);
        }
    }

    m_phys.Build(descs);

    for (std::size_t g = 0; g < geomPos.size(); ++g) delete geomPos[g];
    for (std::size_t g = 0; g < geomIdx.size(); ++g) delete geomIdx[g];

    // Lua scripts: load each object's .lua and run its on_start.
    m_script.Begin(&m_phys, this);
    int scriptCount = 0;
    for (int i = 0; i < (int)scene.objects.size(); ++i)
    {
        const SceneObject& o = scene.objects[i];
        for (const ObjectAttribute& a : o.attributes)
        {
            if (a.type != "Script" || a.script_path.empty())
                continue;
            std::ifstream in(m_project_root / a.script_path, std::ios::binary);
            if (!in) { applog::Error("Script not found: " + a.script_path); continue; }
            std::ostringstream ss; ss << in.rdbuf();
            m_script.LoadScript(i, ss.str(),
                                std::filesystem::path(a.script_path).filename().string());
            ++scriptCount;
        }
    }
    m_script.Start();

    m_phys_poses.clear();
    m_phys_on = true;
    applog::Info("Physics: play (" + std::to_string((int)descs.size()) + " bodies, " +
                 std::to_string(scriptCount) + " scripts)");
}

void SceneRenderer::StopPhysics()
{
    m_script.Clear();
    m_phys.Clear();
    m_phys_poses.clear();
    m_phys_on = false;
    applog::Info("Physics: stop");
    // m_phys_debug is rebuilt from the scene each RenderUi (authoring wireframes).
}

// --- script::ScriptHost (editor) ---
bool  SceneRenderer::InputButton(const char* name) { return m_input.Button(name); }
float SceneRenderer::InputAxis(const char* name)   { return m_input.Axis(name); }
void  SceneRenderer::Log(const char* msg)          { applog::Script(msg); }        // [LOG]
void  SceneRenderer::LogError(const char* msg)     { applog::Error(msg); }          // [ERROR]
int   SceneRenderer::FindObject(const char* name)
{
    for (std::size_t i = 0; i < m_phys_names.size(); ++i)
        if (m_phys_names[i] == name)
            return (int)i;
    return -1;
}
const char* SceneRenderer::ObjectName(int index)
{
    if (index >= 0 && index < (int)m_phys_names.size())
        return m_phys_names[index].c_str();
    return "";
}

void SceneRenderer::AppendColliderWire(const PhysDebug& pd, const D3DMATRIX& m,
                                       D3DCOLOR col, std::vector<Vertex>& out) const
{
    // Local point (row vector) * m -> world.
    struct L
    {
        static Vertex P(float x, float y, float z, const D3DMATRIX& m, D3DCOLOR c)
        {
            Vertex v;
            v.x = x * m._11 + y * m._21 + z * m._31 + m._41;
            v.y = x * m._12 + y * m._22 + z * m._32 + m._42;
            v.z = x * m._13 + y * m._23 + z * m._33 + m._43;
            v.color = c;
            return v;
        }
    };

    if (pd.shape >= 4) // Mesh (convex/exact): the mesh itself is the visual
        return;

    const int seg = 20;
    const float TAU = 6.28318531f, PI = 3.14159265f;

    if (pd.shape == 1) // Sphere: three axis-aligned circles of radius half[0]
    {
        const float r = pd.half[0];
        for (int plane = 0; plane < 3; ++plane)
            for (int s = 0; s < seg; ++s)
            {
                const float a0 = (float)s / seg * TAU;
                const float a1 = (float)(s + 1) / seg * TAU;
                const float c0 = std::cos(a0) * r, s0 = std::sin(a0) * r;
                const float c1 = std::cos(a1) * r, s1 = std::sin(a1) * r;
                float p0[3] = {0, 0, 0}, p1[3] = {0, 0, 0};
                if      (plane == 0) { p0[0] = c0; p0[1] = s0; p1[0] = c1; p1[1] = s1; } // XY
                else if (plane == 1) { p0[1] = c0; p0[2] = s0; p1[1] = c1; p1[2] = s1; } // YZ
                else                 { p0[0] = c0; p0[2] = s0; p1[0] = c1; p1[2] = s1; } // XZ
                out.push_back(L::P(p0[0], p0[1], p0[2], m, col));
                out.push_back(L::P(p1[0], p1[1], p1[2], m, col));
            }
    }
    else if (pd.shape == 2 || pd.shape == 3) // Capsule / Cylinder (Y-up)
    {
        const float r = pd.half[0];   // radius
        const float h = pd.half[1];   // half-height of the cylindrical section
        // Two rings (XZ plane) at y = +h and y = -h.
        for (int ring = 0; ring < 2; ++ring)
        {
            const float y = (ring == 0) ? h : -h;
            for (int s = 0; s < seg; ++s)
            {
                const float a0 = (float)s / seg * TAU, a1 = (float)(s + 1) / seg * TAU;
                out.push_back(L::P(std::cos(a0) * r, y, std::sin(a0) * r, m, col));
                out.push_back(L::P(std::cos(a1) * r, y, std::sin(a1) * r, m, col));
            }
        }
        // Four vertical connectors.
        const float dx[4] = {r, -r, 0, 0}, dz[4] = {0, 0, r, -r};
        for (int i = 0; i < 4; ++i)
        {
            out.push_back(L::P(dx[i],  h, dz[i], m, col));
            out.push_back(L::P(dx[i], -h, dz[i], m, col));
        }
        if (pd.shape == 2) // Capsule: hemispherical end caps (XY + ZY arcs)
        {
            const int hs = seg / 2;
            for (int s = 0; s < hs; ++s)
            {
                const float a0 = (float)s / hs * PI, a1 = (float)(s + 1) / hs * PI;
                const float c0 = std::cos(a0) * r, n0 = std::sin(a0) * r;
                const float c1 = std::cos(a1) * r, n1 = std::sin(a1) * r;
                out.push_back(L::P(c0,  h + n0, 0, m, col)); out.push_back(L::P(c1,  h + n1, 0, m, col)); // top XY
                out.push_back(L::P(0,   h + n0, c0, m, col)); out.push_back(L::P(0,   h + n1, c1, m, col)); // top ZY
                out.push_back(L::P(c0, -h - n0, 0, m, col)); out.push_back(L::P(c1, -h - n1, 0, m, col)); // bot XY
                out.push_back(L::P(0,  -h - n0, c0, m, col)); out.push_back(L::P(0,  -h - n1, c1, m, col)); // bot ZY
            }
        }
    }
    else // Box: 12 edges of the half-extent box
    {
        const float hx = pd.half[0], hy = pd.half[1], hz = pd.half[2];
        const float cx[8] = {-hx, hx, hx, -hx, -hx, hx, hx, -hx};
        const float cy[8] = {-hy, -hy, hy, hy, -hy, -hy, hy, hy};
        const float cz[8] = {-hz, -hz, -hz, -hz, hz, hz, hz, hz};
        const int e[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
        for (int i = 0; i < 12; ++i)
        {
            out.push_back(L::P(cx[e[i][0]], cy[e[i][0]], cz[e[i][0]], m, col));
            out.push_back(L::P(cx[e[i][1]], cy[e[i][1]], cz[e[i][1]], m, col));
        }
    }
}

bool SceneRenderer::EnsureTarget(int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;
    if (m_rt && width == m_width && height == m_height)
        return true;

    OnDeviceLost(); // release any existing target before recreating

    // Resolved, single-sampled texture that ImGui samples.
    if (FAILED(m_device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                       D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_rt, nullptr)))
        return false;
    if (FAILED(m_rt->GetSurfaceLevel(0, &m_rtSurface)))
    {
        OnDeviceLost();
        return false;
    }

    // Pick the best MSAA level this GPU supports for both color and depth, and
    // detect the vendor's alpha-to-coverage toggle (for hair/foliage cutout).
    m_msaa   = D3DMULTISAMPLE_NONE;
    m_hasA2C = false;
    if (IDirect3D9* d3d = nullptr; SUCCEEDED(m_device->GetDirect3D(&d3d)) && d3d)
    {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        m_device->GetCreationParameters(&cp);

        const D3DMULTISAMPLE_TYPE want[] = { D3DMULTISAMPLE_4_SAMPLES, D3DMULTISAMPLE_2_SAMPLES };
        for (D3DMULTISAMPLE_TYPE t : want)
        {
            DWORD qc = 0, qd = 0;
            if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_A8R8G8B8, TRUE, t, &qc)) &&
                SUCCEEDED(d3d->CheckDeviceMultiSampleType(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_D16,      TRUE, t, &qd)))
            {
                m_msaa = t;
                break;
            }
        }

        auto fourcc = [](char a, char b, char c, char d) -> DWORD {
            return (DWORD)(BYTE)a | ((DWORD)(BYTE)b << 8) | ((DWORD)(BYTE)c << 16) | ((DWORD)(BYTE)d << 24); };
        D3DDISPLAYMODE mode{};
        d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode);
        if (SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format, 0,
                                             D3DRTYPE_SURFACE, (D3DFORMAT)fourcc('A','2','M','1'))))
        {   // NVIDIA-style A2C
            m_hasA2C = true; m_a2cState = D3DRS_ADAPTIVETESS_Y;
            m_a2cOn = fourcc('A','2','M','1'); m_a2cOff = fourcc('A','2','M','0');
        }
        else if (SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format, 0,
                                                  D3DRTYPE_SURFACE, (D3DFORMAT)fourcc('A','T','O','C'))))
        {   // AMD/ATI-style A2C
            m_hasA2C = true; m_a2cState = D3DRS_POINTSIZE;
            m_a2cOn = fourcc('A','2','M','1'); m_a2cOff = 0;
        }
        d3d->Release();
    }
    if (m_msaa == D3DMULTISAMPLE_NONE)
        m_hasA2C = false; // A2C needs a multisampled target to do anything

    // Multisampled color + matching depth; the scene renders here, then resolves
    // into m_rt. Fall back to single-sampled if any of it fails.
    if (m_msaa != D3DMULTISAMPLE_NONE)
    {
        if (FAILED(m_device->CreateRenderTarget(width, height, D3DFMT_A8R8G8B8, m_msaa, 0, FALSE, &m_rtMS, nullptr)) ||
            FAILED(m_device->CreateDepthStencilSurface(width, height, D3DFMT_D16, m_msaa, 0, FALSE, &m_depthMS, nullptr)))
        {
            if (m_rtMS)    { m_rtMS->Release();    m_rtMS = nullptr; }
            if (m_depthMS) { m_depthMS->Release(); m_depthMS = nullptr; }
            m_msaa = D3DMULTISAMPLE_NONE;
            m_hasA2C = false;
        }
    }
    if (m_msaa == D3DMULTISAMPLE_NONE)
    {
        if (FAILED(m_device->CreateDepthStencilSurface(width, height, D3DFMT_D16,
                                                       D3DMULTISAMPLE_NONE, 0, TRUE, &m_depth, nullptr)))
        {
            OnDeviceLost();
            return false;
        }
    }

    m_width  = width;
    m_height = height;
    return true;
}
