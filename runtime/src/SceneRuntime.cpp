#include "SceneRuntime.h"
#include "Endian.h"
#include "RtMath.h"
#include "SpakFormat.h"
#include "XboxRenderer.h"
#include "input/XInputPoll.h" // XInput is available via xtl.h (included by the header)

#include <stdio.h>
#include <string.h>

using namespace rmath;

// ---------------------------------------------------------------------------
// Shared frame inputs (same registers / values as the editor's standard material)
// ---------------------------------------------------------------------------
namespace
{
    const float kPi = 3.14159265f;

    // MeshVertex layout: pos(3) normal(3) tangent(3) uv(2) = 11 floats, 44 bytes.
    void PutVertex(unsigned char* p,
                   float px, float py, float pz,
                   float nx, float ny, float nz,
                   float tx, float ty, float tz,
                   float u, float v)
    {
        float f[11] = { px, py, pz, nx, ny, nz, tx, ty, tz, u, v };
        memcpy(p, f, sizeof(f)); // native-endian, matching the converted mesh buffers
    }
}

bool SceneRuntime::Init(IDirect3DDevice9* device, const std::string& contentRoot)
{
    m_device = device;
    m_root   = contentRoot;

    m_mesh_decl = NULL;
    m_def_white = m_def_normal = m_def_black = NULL;
    m_quad_vb = NULL; m_quad_ib = NULL; m_cube_vb = NULL; m_cube_ib = NULL;

    // Fixed camera: the editor's default orbit framing (no input on the console).
    m_yaw = 0.6f; m_pitch = 0.5f; m_distance = 20.0f;
    m_target[0] = m_target[1] = m_target[2] = 0.0f;
    m_eye[0] = m_eye[1] = m_eye[2] = 0.0f;
    m_has_cam = false;
    m_cam_object = m_cam_attr = m_cam_target = -1;
    m_cam_fov = 45.0f; m_cam_near = 0.5f; m_cam_far = 100.0f;
    m_track_dist = m_track_speed = 0.0f;
    m_follow_smooth.Reset();
    m_time = 0.0f;

    m_content.Init(device, contentRoot);

    // Streaming: open game.spak once and drive it through the residency cache.
    // Absent/failed open is fine — every lookup then misses and ResolveMesh falls
    // back to the raw loader.
    m_pak.Open(m_content.Resolve("game.spak").c_str());
    m_cache.Init(device, &m_pak); // default 128 MB residency budget

    // Vertex declaration matching MeshVertex (pos/normal/tangent/uv).
    const D3DVERTEXELEMENT9 elems[] = {
        {0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
        {0, 24, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT,  0},
        {0, 36, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    if (FAILED(m_device->CreateVertexDeclaration(elems, &m_mesh_decl)))
        return false;

    m_def_white  = SolidTexture(D3DCOLOR_ARGB(255, 255, 255, 255)); // diffuse
    m_def_normal = SolidTexture(D3DCOLOR_ARGB(255, 128, 128, 255)); // flat normal
    m_def_black  = SolidTexture(D3DCOLOR_ARGB(255, 0, 0, 0));       // no specular

    if (!BuildGeometry())
        return false;

    m_content.LoadStandard();

    // Load the startup scene: <root>\game.proj -> startupScene, else Main.scene.
    std::string startup = scenedata::ReadStartupScene(m_content.Resolve("game.proj"));
    if (startup.empty())
        startup = "scenes/Main.scene";
    if (!scenedata::LoadScene(m_content.Resolve(startup), m_scene))
        OutputDebugStringA("scene: failed to load startup scene\n");

    BuildDrawLists();
    BuildPhysics();
    BuildScripts();
    return true;
}

// --- script::ScriptHost (runtime) ---
bool  SceneRuntime::InputButton(const char* name) { return m_input.Button(name); }
float SceneRuntime::InputAxis(const char* name)   { return m_input.Axis(name); }
void  SceneRuntime::Log(const char* msg)
{
    OutputDebugStringA("[script] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}
int SceneRuntime::FindObject(const char* name)
{
    for (size_t i = 0; i < m_scene.objects.size(); ++i)
        if (m_scene.objects[i].name == name)
            return (int)i;
    return -1;
}
const char* SceneRuntime::ObjectName(int index)
{
    if (index >= 0 && index < (int)m_scene.objects.size())
        return m_scene.objects[index].name.c_str();
    return "";
}

// Load each object's .lua from the deployed content and run its on_start.
void SceneRuntime::BuildScripts()
{
    m_script.Begin(&m_phys, this);
    for (size_t i = 0; i < m_scene.objects.size(); ++i)
    {
        const RtObject& o = m_scene.objects[i];
        for (size_t a = 0; a < o.attributes.size(); ++a)
        {
            const RtAttribute& at = o.attributes[a];
            if (at.type != "Script" || at.script_path.empty())
                continue;
            // Scripts ship loose (source) — read the whole file from game:\.
            std::string full = m_content.Resolve(at.script_path);
            FILE* f = fopen(full.c_str(), "rb");
            if (!f) { OutputDebugStringA(("script not found: " + at.script_path + "\n").c_str()); continue; }
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            std::string src;
            if (n > 0) { src.resize((size_t)n); size_t got = fread(&src[0], 1, (size_t)n, f); src.resize(got); }
            fclose(f);
            m_script.LoadScript((int)i, src, at.script_path);
        }
    }
    m_script.Start();
}

// Create the Bullet world from the scene's Rigid Body / Trigger attributes. The
// same neutral descriptors the editor builds, so the console simulates identically.
bool SceneRuntime::LoadPakMeshGeometry(const std::string& relPath, std::vector<float>& pos,
                                       std::vector<unsigned int>& idx, bool wantIndices)
{
    if (relPath.empty() || !m_pak.IsOpen())
        return false;
    const unsigned int hash = spak::NameHash(relPath.c_str());
    const SpakEntry* e = m_pak.Find(hash);
    if (!e || e->type != spak::kTypeMesh)
        return false;

    std::vector<BYTE> blob;
    if (!m_pak.ReadBlob(e, blob) || blob.size() < spak::kMeshHeaderBytes)
        return false;

    const unsigned char* p = &blob[0];
    if (endian::LoadU32BE(p + 0) != spak::kMeshMagic)
        return false;
    const unsigned int vcount      = endian::LoadU32BE(p + 4);
    const unsigned int icount      = endian::LoadU32BE(p + 8);
    const unsigned int subsetCount = endian::LoadU32BE(p + 12);
    const size_t subBytes = (size_t)subsetCount * spak::kMeshSubsetBytes;
    const size_t vbytes   = (size_t)vcount * spak::kMeshVertexBytes;
    const size_t ibytes   = (size_t)icount * 4;
    if ((size_t)spak::kMeshHeaderBytes + subBytes + vbytes + ibytes > blob.size())
        return false;
    const size_t bufOff = (size_t)spak::kMeshHeaderBytes + subBytes;

    // VB/IB are baked big-endian = native on the console — copy straight out.
    pos.resize((size_t)vcount * 3);
    for (unsigned int v = 0; v < vcount; ++v)
        memcpy(&pos[(size_t)v * 3], p + bufOff + (size_t)v * spak::kMeshVertexBytes, sizeof(float) * 3);
    if (wantIndices && icount > 0)
    {
        idx.resize(icount);
        memcpy(&idx[0], p + bufOff + vbytes, ibytes);
    }
    return true;
}

void SceneRuntime::BuildPhysics()
{
    std::vector<phys::BodyDesc> descs;

    // Heap-owned mesh geometry kept alive until Build() copies what it needs.
    std::vector<std::vector<float>*>        geomPos;
    std::vector<std::vector<unsigned int>*> geomIdx;

    for (size_t i = 0; i < m_scene.objects.size(); ++i)
    {
        const RtObject& o = m_scene.objects[i];

        // The object's 3D Model, used by mesh-shape colliders.
        std::string model_path;
        for (size_t a = 0; a < o.attributes.size(); ++a)
            if (o.attributes[a].type == "3D Model" && !o.attributes[a].model_path.empty())
            { model_path = o.attributes[a].model_path; break; }

        for (size_t a = 0; a < o.attributes.size(); ++a)
        {
            const RtAttribute& at = o.attributes[a];
            const bool rigid = (at.type == "Rigid Body");
            const bool trig  = (at.type == "Trigger Volume");
            if (!rigid && !trig)
                continue;

            phys::BodyDesc d;
            d.objectIndex = (int)i;
            for (int k = 0; k < 3; ++k) { d.pos[k] = o.position[k]; d.rotEulerDeg[k] = o.rotation[k]; }
            if (rigid)
            {
                d.isTrigger  = false;
                d.kind       = at.phys_kind;
                d.shape      = at.phys_shape;
                for (int k = 0; k < 3; ++k) d.halfExtents[k] = at.phys_size[k];
                d.mass        = at.phys_mass;
                d.linDamping  = at.phys_lin_damping;
                d.angDamping  = at.phys_ang_damping;
                d.restitution  = at.phys_restitution;
                d.friction     = at.phys_friction;
                d.gravity      = at.phys_gravity;
                d.gravityScale = at.phys_gravity_scale;
            }
            else
            {
                d.isTrigger = true;
                d.kind      = phys::BodyDesc::Static;
                d.shape     = at.trig_shape;
                for (int k = 0; k < 3; ++k) d.halfExtents[k] = at.trig_size[k];
            }

            // Mesh colliders: pull CPU geometry from the pak blob.
            if (d.shape == phys::BodyDesc::MeshConvex || d.shape == phys::BodyDesc::MeshExact)
            {
                const bool exact = (d.shape == phys::BodyDesc::MeshExact);
                std::vector<float>*        pv = new std::vector<float>();
                std::vector<unsigned int>* iv = new std::vector<unsigned int>();
                if (LoadPakMeshGeometry(model_path, *pv, *iv, exact) && !pv->empty())
                {
                    // Bake the object's scale into the collision vertices so the
                    // collider matches the SCALED visual mesh (see the editor path).
                    for (size_t vi = 0; vi + 2 < pv->size(); vi += 3)
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
                }
            }

            descs.push_back(d);
        }
    }

    m_phys.Build(descs);

    for (size_t g = 0; g < geomPos.size(); ++g) delete geomPos[g];
    for (size_t g = 0; g < geomIdx.size(); ++g) delete geomIdx[g];
}

// Streamed mesh first (game.spak via the async residency cache); fall back to the
// raw D3DX loader for anything not cooked into the pak. A mesh that's in the pak
// but still streaming in returns NULL and is skipped this frame (no stall) rather
// than falling back to a synchronous load. Both paths ship until the phase-5 cutover.
RtMesh* SceneRuntime::ResolveMesh(const std::string& relPath)
{
    bool inPak = false;
    RtMesh* m = m_cache.GetMesh(relPath, &inPak);
    if (m)     return m;                    // resident
    if (inPak) return NULL;                 // streaming in — skip this frame
    return m_content.GetMesh(relPath);      // not in the pak — raw fallback
}

void SceneRuntime::InitBloom(XboxRenderer& renderer)
{
    if (m_content.LoadBuiltin("bloom_bright",  m_bloom_bright) &&
        m_content.LoadBuiltin("bloom_blur",    m_bloom_blur) &&
        m_content.LoadBuiltin("bloom_combine", m_bloom_combine))
    {
        renderer.SetBloomShaders(m_bloom_bright.vs,  m_bloom_bright.ps,
                                 m_bloom_blur.vs,    m_bloom_blur.ps,
                                 m_bloom_combine.vs, m_bloom_combine.ps);
    }
    // else: glow stays off — the scene still renders (older deploys).
}

void SceneRuntime::Shutdown()
{
    m_bloom_combine.Release();
    m_bloom_blur.Release();
    m_bloom_bright.Release();
    m_cache.Shutdown();
    m_pak.Close();
    if (m_cube_ib)   { m_cube_ib->Release();   m_cube_ib = NULL; }
    if (m_cube_vb)   { m_cube_vb->Release();   m_cube_vb = NULL; }
    if (m_quad_ib)   { m_quad_ib->Release();   m_quad_ib = NULL; }
    if (m_quad_vb)   { m_quad_vb->Release();   m_quad_vb = NULL; }
    if (m_def_black) { m_def_black->Release(); m_def_black = NULL; }
    if (m_def_normal){ m_def_normal->Release();m_def_normal = NULL; }
    if (m_def_white) { m_def_white->Release(); m_def_white = NULL; }
    if (m_mesh_decl) { m_mesh_decl->Release(); m_mesh_decl = NULL; }
    m_content.Shutdown();
    m_device = NULL;
}

IDirect3DTexture9* SceneRuntime::SolidTexture(D3DCOLOR argb)
{
    IDirect3DTexture9* t = NULL;
    if (FAILED(m_device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL)))
        return NULL;
    D3DLOCKED_RECT r;
    if (SUCCEEDED(t->LockRect(0, &r, NULL, 0)))
    {
        *(D3DCOLOR*)r.pBits = argb;
        t->UnlockRect(0);
    }
    return t;
}

bool SceneRuntime::BuildGeometry()
{
    // Unit quad (XY plane, uv 0..1) — for "quad" custom shaders.
    const UINT quadVBytes = 4 * 44;
    if (FAILED(m_device->CreateVertexBuffer(quadVBytes, 0, 0, D3DPOOL_MANAGED, &m_quad_vb, NULL)))
        return false;
    {
        unsigned char* p = NULL;
        m_quad_vb->Lock(0, 0, (void**)&p, 0);
        PutVertex(p +   0, -0.5f, -0.5f, 0.0f,  0,0,-1,  1,0,0,  0.0f, 0.0f);
        PutVertex(p +  44,  0.5f, -0.5f, 0.0f,  0,0,-1,  1,0,0,  1.0f, 0.0f);
        PutVertex(p +  88,  0.5f,  0.5f, 0.0f,  0,0,-1,  1,0,0,  1.0f, 1.0f);
        PutVertex(p + 132, -0.5f,  0.5f, 0.0f,  0,0,-1,  1,0,0,  0.0f, 1.0f);
        m_quad_vb->Unlock();
    }
    if (FAILED(m_device->CreateIndexBuffer(6 * 4, 0, D3DFMT_INDEX32, D3DPOOL_MANAGED, &m_quad_ib, NULL)))
        return false;
    {
        unsigned int idx[6] = { 0, 1, 2, 0, 2, 3 };
        void* dst = NULL;
        m_quad_ib->Lock(0, 0, &dst, 0);
        memcpy(dst, idx, sizeof(idx)); // native-endian indices
        m_quad_ib->Unlock();
    }

    // Unit cube [-0.5,0.5]^3 — for "volume" (raymarched) custom shaders.
    const float c = 0.5f;
    const float cx[8] = {-c,  c,  c, -c, -c,  c,  c, -c};
    const float cy[8] = {-c, -c,  c,  c, -c, -c,  c,  c};
    const float cz[8] = {-c, -c, -c, -c,  c,  c,  c,  c};
    if (FAILED(m_device->CreateVertexBuffer(8 * 44, 0, 0, D3DPOOL_MANAGED, &m_cube_vb, NULL)))
        return false;
    {
        unsigned char* p = NULL;
        m_cube_vb->Lock(0, 0, (void**)&p, 0);
        for (int k = 0; k < 8; ++k)
            PutVertex(p + k * 44, cx[k], cy[k], cz[k],  0,0,0,  0,0,0,  0,0);
        m_cube_vb->Unlock();
    }
    if (FAILED(m_device->CreateIndexBuffer(36 * 4, 0, D3DFMT_INDEX32, D3DPOOL_MANAGED, &m_cube_ib, NULL)))
        return false;
    {
        unsigned int idx[36] = {
            0,1,2, 0,2,3,  5,4,7, 5,7,6,  4,0,3, 4,3,7,
            1,5,6, 1,6,2,  4,5,1, 4,1,0,  3,2,6, 3,6,7,
        };
        void* dst = NULL;
        m_cube_ib->Lock(0, 0, &dst, 0);
        memcpy(dst, idx, sizeof(idx)); // native-endian indices
        m_cube_ib->Unlock();
    }
    return true;
}

// Capture the models + standalone shaders to draw from the loaded scene. Static
// on the console (no editing), so this runs once at Init.
void SceneRuntime::BuildDrawLists()
{
    m_draw_items.clear();
    m_shader_items.clear();
    m_has_cam = false;

    bool have_dir = false;
    int  point_count = 0;
    memset(m_light_dir, 0, sizeof(m_light_dir));
    memset(m_light_col, 0, sizeof(m_light_col));
    memset(m_point_pos, 0, sizeof(m_point_pos));
    memset(m_point_col, 0, sizeof(m_point_col));

    for (size_t i = 0; i < m_scene.objects.size(); ++i)
    {
        const RtObject& o = m_scene.objects[i];
        if (!o.visible)
            continue;

        std::string model_path, shader_path;
        for (size_t a = 0; a < o.attributes.size(); ++a)
        {
            const RtAttribute& at = o.attributes[a];
            if (at.type == "3D Model" && !at.model_path.empty() && model_path.empty())
                model_path = at.model_path;
            else if (at.type == "Shader" && !at.shader_path.empty() && shader_path.empty())
                shader_path = at.shader_path;
            else if (at.type == "Camera" && at.cam_active && !m_has_cam)
            {
                // Record which camera; the view itself is resolved per frame in
                // Render so Follow/Track see live physics/script motion.
                m_cam_object = (int)i;
                m_cam_attr   = (int)a;
                m_cam_fov  = at.cam_fov;
                m_cam_near = at.cam_near;
                m_cam_far  = at.cam_far;
                m_cam_target = -1;
                if (at.cam_type == camr::CamFollow && !at.cam_follow_target.empty())
                    for (size_t t = 0; t < m_scene.objects.size(); ++t)
                        if (m_scene.objects[t].name == at.cam_follow_target) { m_cam_target = (int)t; break; }
                m_has_cam  = true;
            }
            else if (at.type == "Directional Light" && !have_dir)
            {
                // Direction = the object's forward (+Z through its rotation).
                const float d2r = kPi / 180.0f;
                const D3DMATRIX r = Multiply(Multiply(RotationX(o.rotation[0] * d2r),
                                                      RotationY(o.rotation[1] * d2r)),
                                             RotationZ(o.rotation[2] * d2r));
                m_light_dir[0] = r._31; m_light_dir[1] = r._32; m_light_dir[2] = r._33;
                for (int k = 0; k < 3; ++k) m_light_col[k] = at.light_color[k] * at.light_intensity;
                have_dir = true;
            }
            else if (at.type == "Point Light" && point_count < 4)
            {
                const float range = at.light_range > 0.1f ? at.light_range : 0.1f;
                for (int k = 0; k < 3; ++k)
                {
                    m_point_pos[point_count][k] = o.position[k];
                    m_point_col[point_count][k] = at.light_color[k] * at.light_intensity;
                }
                m_point_pos[point_count][3] = 1.0f / (range * range);
                ++point_count;
            }
        }

        bool shader_owns_mesh = false;
        if (!shader_path.empty())
        {
            RtShader* cs = m_content.GetShader(shader_path);
            shader_owns_mesh = cs && cs->state.geometry == RtShaderState::Model;

            ShaderItem si;
            si.shader_path = shader_path;
            si.model_path  = model_path;
            for (int k = 0; k < 3; ++k) { si.pos[k] = o.position[k]; si.rot[k] = o.rotation[k]; si.scale[k] = o.scale[k]; }
            m_shader_items.push_back(si);
        }

        if (!model_path.empty() && !shader_owns_mesh)
        {
            DrawItem di;
            di.model_path = model_path;
            di.world = ComposeWorld(o.position, o.rotation, o.scale);
            di.object_index = (int)i;
            for (int k = 0; k < 3; ++k) di.scale[k] = o.scale[k];
            m_draw_items.push_back(di);
        }
    }

    // No directional light authored: keep the legacy fixed sun — white when the
    // scene has no lights at all (old scenes render unchanged), black when point
    // lights exist (the author owns the lighting; only c0's direction, which
    // custom shaders read, stays meaningful).
    if (!have_dir)
    {
        Vec3 lraw = { -0.4f, -1.0f, -0.5f };
        Vec3 ln = Normalize(lraw);
        m_light_dir[0] = ln.x; m_light_dir[1] = ln.y; m_light_dir[2] = ln.z;
        const float w = (point_count == 0) ? 1.0f : 0.0f;
        m_light_col[0] = m_light_col[1] = m_light_col[2] = w;
    }
}

void SceneRuntime::Render(float dt)
{
    if (!m_device)
        return;

    m_time += dt;

    // Register a budgeted batch of finished streaming loads into live resources
    // (the worker thread did the read + decompress off this thread).
    m_cache.Update(8);

    // Poll the controller, then run scripts before the physics step so their
    // impulses/velocity/transforms are integrated this frame (no-op if none).
    input::PollXInput(m_input);
    m_script.Update(dt);

    // Physics: step Bullet, then override the draw items whose objects are
    // simulated (world = Scale * pose). Same code path as the editor preview.
    if (!m_phys.Empty())
    {
        m_phys.Step(dt);
        m_phys.ReadPoses(m_phys_poses);
        for (size_t pi = 0; pi < m_phys_poses.size(); ++pi)
        {
            const phys::Pose& p = m_phys_poses[pi];
            D3DMATRIX pose;
            memcpy(&pose.m[0][0], p.matrix, sizeof(float) * 16);
            for (size_t di = 0; di < m_draw_items.size(); ++di)
            {
                if (m_draw_items[di].object_index != p.objectIndex)
                    continue;
                const float* s = m_draw_items[di].scale;
                m_draw_items[di].world = Multiply(Scaling(s[0], s[1], s[2]), pose);
            }
        }

        // Dispatch trigger enter events to scripts (no automatic engine logging).
        std::vector<phys::TriggerEvent> events;
        m_phys.DrainTriggerEvents(events);
        for (size_t ei = 0; ei < events.size(); ++ei)
            if (events[ei].entered)
                m_script.FireTrigger(events[ei].triggerObjectIndex, events[ei].otherObjectIndex);
    }

    // View through the scene's active "Camera" attribute when it has one, else
    // the fixed orbit framing baked at Init (no input on the console either way).
    D3DMATRIX view, proj;
    if (m_has_cam)
    {
        // Per-frame resolve through the shared camera/CameraResolve.h (the same
        // math the editor's look-through uses). A simulated pose stands in for
        // the authored transform when the object has one.
        const RtObject&    co = m_scene.objects[m_cam_object];
        const RtAttribute& ca = co.attributes[m_cam_attr];
        const float d2r = kPi / 180.0f;

        const phys::Pose* cam_pose = 0;
        const phys::Pose* tgt_pose = 0;
        for (size_t pi = 0; pi < m_phys_poses.size(); ++pi)
        {
            if (m_phys_poses[pi].objectIndex == m_cam_object) cam_pose = &m_phys_poses[pi];
            if (m_cam_target >= 0 && m_phys_poses[pi].objectIndex == m_cam_target) tgt_pose = &m_phys_poses[pi];
        }

        camr::View v;
        if (cam_pose)
        {
            const float* pm = cam_pose->matrix;
            v.pos[0] = pm[12]; v.pos[1] = pm[13]; v.pos[2] = pm[14];
            v.fwd[0] = pm[8];  v.fwd[1] = pm[9];  v.fwd[2] = pm[10];
            v.up[0]  = pm[4];  v.up[1]  = pm[5];  v.up[2]  = pm[6];
        }
        else
            camr::ResolveFixed(co.position, co.rotation, v);

        if (ca.cam_type == camr::CamFollow && m_cam_target >= 0)
        {
            float tpos[3];
            if (tgt_pose)
            { tpos[0] = tgt_pose->matrix[12]; tpos[1] = tgt_pose->matrix[13]; tpos[2] = tgt_pose->matrix[14]; }
            else
                for (int k = 0; k < 3; ++k) tpos[k] = m_scene.objects[m_cam_target].position[k];
            camr::ResolveFollow(co.position, co.rotation, tpos,
                                ca.cam_follow_offset, ca.cam_follow_orbit,
                                ca.cam_follow_rot_offset, ca.cam_follow_lock, v);
            camr::SmoothFollow(v, m_follow_smooth, ca.cam_follow_smoothing, dt);
        }
        else if (ca.cam_type == camr::CamTrack && ca.cam_track_points.size() >= 6)
        {
            const float* pts = &ca.cam_track_points[0];
            const int    n   = (int)(ca.cam_track_points.size() / 3);
            const float total = camr::TrackTotalLength(pts, n);
            camr::AdvanceTrack(m_track_dist, m_track_speed, ca.cam_track_speed,
                               ca.cam_track_accel, dt, total);
            camr::ResolveTrack(pts, n, m_track_dist, ca.cam_track_rot_offset, v);
        }

        Vec3 eye = { v.pos[0], v.pos[1], v.pos[2] };
        Vec3 at  = { eye.x + v.fwd[0], eye.y + v.fwd[1], eye.z + v.fwd[2] };
        Vec3 up  = { v.up[0], v.up[1], v.up[2] };
        m_eye[0] = eye.x; m_eye[1] = eye.y; m_eye[2] = eye.z;
        view = LookAtLH(eye, at, up);
        proj = PerspectiveFovLH(m_cam_fov * d2r, 16.0f / 9.0f, m_cam_near, m_cam_far);
    }
    else
    {
        Vec3 dir = { cosf(m_pitch) * sinf(m_yaw), sinf(m_pitch), cosf(m_pitch) * cosf(m_yaw) };
        Vec3 at  = { m_target[0], m_target[1], m_target[2] };
        Vec3 eye = { at.x + dir.x * m_distance, at.y + dir.y * m_distance, at.z + dir.z * m_distance };
        m_eye[0] = eye.x; m_eye[1] = eye.y; m_eye[2] = eye.z;
        Vec3 up = { 0.0f, 1.0f, 0.0f };
        view = LookAtLH(eye, at, up);
        proj = PerspectiveFovLH(kPi / 4.0f, 16.0f / 9.0f, 0.5f, 100.0f);
    }
    const D3DMATRIX vp = Multiply(view, proj);

    const float* lightDir   = m_light_dir; // scene lights captured in BuildDrawLists
    const float camPos[4]   = { m_eye[0], m_eye[1], m_eye[2], 0.0f };
    const float ambient[4]  = { 0.22f, 0.22f, 0.25f, 0.0f };

    m_device->SetVertexDeclaration(m_mesh_decl);

    // Common state (the 360 has no fixed-function pipeline; everything is shaded).
    // The 360 doesn't guarantee PC render-state defaults, so set them explicitly.
    m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    m_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    // Colour writes default to RGB only: the tile target's ALPHA channel is the
    // emissive glow mask (the bloom source XboxRenderer's glow chain reads from
    // the resolved frame). Only opaque material subsets write it — see DrawMesh.
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);

    // --- Scene models (standard diffuse / normal / specular / emissive material) ---
    RtShader* std_mat = m_content.Standard();
    if (!m_draw_items.empty() && std_mat && std_mat->Valid())
    {
        m_device->SetVertexShader(std_mat->vs);
        m_device->SetPixelShader(std_mat->ps);
        for (DWORD s = 0; s < 4; ++s)
        {
            m_device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            m_device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        }
        m_device->SetPixelShaderConstantF(0, lightDir, 1);
        m_device->SetPixelShaderConstantF(1, camPos, 1);
        m_device->SetPixelShaderConstantF(2, ambient, 1);
        m_device->SetPixelShaderConstantF(6, m_light_col, 1);
        m_device->SetPixelShaderConstantF(7, &m_point_pos[0][0], 4);
        m_device->SetPixelShaderConstantF(11, &m_point_col[0][0], 4);

        // Pass 1 — opaque + cutout subsets (both write depth). Cutout (hair cards,
        // foliage) is a masked alpha: the per-subset states (alpha test + Xenos
        // alpha-to-coverage) are set inside DrawMesh, per material subset.
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        for (size_t i = 0; i < m_draw_items.size(); ++i)
        {
            RtMesh* gm = ResolveMesh(m_draw_items[i].model_path);
            if (gm)
                DrawMesh(gm, m_draw_items[i].world, vp, std_mat, false /*opaque+cutout*/);
        }
        m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ALPHATOMASKENABLE, FALSE);

        // Pass 2 — alpha-blended (glass, water), after all opaque depth. Depth TEST
        // ON so opaque geometry in front correctly occludes it; depth WRITE OFF so
        // the transparent surface doesn't populate the depth buffer. Verified in
        // Xenia: the glass renders correctly and is occluded by nearer geometry (the
        // old ZENABLE-off workaround was compensating for a non-existent quirk).
        m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
        m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        for (size_t i = 0; i < m_draw_items.size(); ++i)
        {
            RtMesh* gm = ResolveMesh(m_draw_items[i].model_path);
            if (gm)
                DrawMesh(gm, m_draw_items[i].world, vp, std_mat, true /*blend*/);
        }
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        m_device->SetRenderState(D3DRS_COLORWRITEENABLE,   // back to RGB-only:
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    }

    // --- Standalone custom-shader objects ---
    if (!m_shader_items.empty())
    {
        m_device->SetVertexDeclaration(m_mesh_decl);
        for (size_t i = 0; i < m_shader_items.size(); ++i)
        {
            RtShader* cs = m_content.GetShader(m_shader_items[i].shader_path);
            if (cs && cs->Valid())
                DrawShaderItem(m_shader_items[i], *cs, vp);
        }
        // Restore defaults for next frame.
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    }
}

// Draw one standard-material mesh's subsets for a pass: upload its transform
// (via the constant table so packing is correct), then per material subset bind
// its textures, set its cutout states (opaque pass only), and draw its index
// range. Pass-level states (blend / depth) are set by the caller.
void SceneRuntime::DrawMesh(RtMesh* gm, const D3DMATRIX& world, const D3DMATRIX& vp, RtShader* mat, bool blendPass)
{
    // Skip the transform upload when nothing in this mesh belongs to the pass.
    bool any = false;
    for (size_t i = 0; i < gm->subsets.size(); ++i)
        if ((gm->subsets[i].alpha == RtBlend) == blendPass) { any = true; break; }
    if (!any)
        return;

    const D3DMATRIX wvp = Multiply(world, vp);
    if (mat->vsConstants)
    {
        if (mat->hWVP)   mat->vsConstants->SetMatrix(m_device, mat->hWVP,   (const D3DXMATRIX*)&wvp);
        if (mat->hWorld) mat->vsConstants->SetMatrix(m_device, mat->hWorld, (const D3DXMATRIX*)&world);
    }
    m_device->SetStreamSource(0, gm->vb, 0, 44);
    m_device->SetIndices(gm->ib);

    for (size_t i = 0; i < gm->subsets.size(); ++i)
    {
        const RtSubset& s = gm->subsets[i];
        if ((s.alpha == RtBlend) != blendPass)
            continue;
        if (!blendPass)
        {
            if (s.alpha == RtCutout)
            {
                // Alpha test at the mask's mid-point drops the fully-transparent
                // background AND the faint low-alpha haze between strands (the pale
                // rim); alpha-to-coverage (Xenos native) anti-aliases the remaining
                // solid strand edges via MSAA coverage.
                m_device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
                m_device->SetRenderState(D3DRS_ALPHAREF, 170);
                m_device->SetRenderState(D3DRS_ALPHATOMASKENABLE, TRUE);
            }
            else
            {
                m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                m_device->SetRenderState(D3DRS_ALPHATOMASKENABLE, FALSE);
            }
        }
        // Bump-offset strength (PS c5): max UV shift across the height field
        // packed in a normal map's alpha (keep in sync with the editor's
        // SceneRenderer.cpp). Subsets without a height field upload exactly 0,
        // so their UVs — and the cutout alpha they feed — are untouched.
        const float bump[4] = { s.normalHasHeight ? 0.08f : 0.0f, 0.0f, 0.0f, 0.0f };
        m_device->SetPixelShaderConstantF(5, bump, 1);
        // Opaque subsets export their emissive strength in the target's alpha —
        // the glow chain's bloom source. Cutout/blend keep the diffuse's alpha
        // for masking/blending and must not touch the mask channel.
        const bool glowMask = (s.alpha == RtOpaque);
        const float maskc[4] = { glowMask ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
        m_device->SetPixelShaderConstantF(15, maskc, 1);
        m_device->SetRenderState(D3DRS_COLORWRITEENABLE, glowMask
            ? D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
              D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA
            : D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        m_device->SetTexture(0, s.diffuse  ? s.diffuse  : m_def_white);
        m_device->SetTexture(1, s.normal   ? s.normal   : m_def_normal);
        m_device->SetTexture(2, s.specular ? s.specular : m_def_black);
        m_device->SetTexture(3, s.emissive ? s.emissive : m_def_black);
        m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, gm->vertexCount,
                                       s.indexStart, s.indexCount / 3);
    }
}

// Draw one standalone shader — geometry, transform, uniforms, parsed render
// states, then the draw. Mirrors the editor's SceneRenderer::DrawShaderItem.
void SceneRuntime::DrawShaderItem(const ShaderItem& item, RtShader& shader, const D3DMATRIX& viewProj)
{
    const RtShaderState& s = shader.state;
    const float d2r = kPi / 180.0f;

    const D3DMATRIX rot = Multiply(Multiply(RotationX(item.rot[0] * d2r),
                                            RotationY(item.rot[1] * d2r)),
                                   RotationZ(item.rot[2] * d2r));
    D3DMATRIX w = Multiply(Multiply(Scaling(item.scale[0], item.scale[1], item.scale[2]), rot),
                           Translation(item.pos[0], item.pos[1], item.pos[2]));

    IDirect3DVertexBuffer9* vb = m_quad_vb;
    IDirect3DIndexBuffer9*  ib = m_quad_ib;
    UINT verts = 4, prims = 2;

    if (s.geometry == RtShaderState::Volume && m_cube_vb && m_cube_ib)
    {
        vb = m_cube_vb; ib = m_cube_ib; verts = 8; prims = 12;
        // Camera in the cube's local space, so the pixel shader can raymarch it.
        const D3DMATRIX invW =
            Multiply(Multiply(Translation(-item.pos[0], -item.pos[1], -item.pos[2]), Transpose3(rot)),
                     Scaling(1.0f / item.scale[0], 1.0f / item.scale[1], 1.0f / item.scale[2]));
        const Vec3 e = { m_eye[0], m_eye[1], m_eye[2] };
        const Vec3 camObj = TransformPoint(e, invW);
        const float camObj4[4] = { camObj.x, camObj.y, camObj.z, 0.0f };
        m_device->SetPixelShaderConstantF(4, camObj4, 1); // gCamObj (PS c4)
    }
    RtMesh* model_gm = NULL; // "model" geometry: drawn per material subset
    if (s.geometry == RtShaderState::Model)
    {
        model_gm = ResolveMesh(item.model_path);
        if (!model_gm)
            return;
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

    const float* lightDir   = m_light_dir; // the scene's directional light
    const float camPos[4]   = { m_eye[0], m_eye[1], m_eye[2], 0.0f };
    const float ambient[4]  = { 0.22f, 0.22f, 0.25f, 0.0f };
    const float time4[4]    = { m_time, 0.0f, 0.0f, 0.0f };

    const D3DMATRIX wvp = Multiply(w, viewProj);
    m_device->SetVertexShader(shader.vs);
    m_device->SetPixelShader(shader.ps);
    if (shader.vsConstants)
    {
        // Custom shaders may declare only gWVP (e.g. Fire.hlsl has no gWorld); the
        // handle is 0 when absent, and SetMatrix with a null handle faults on the 360.
        if (shader.hWVP)   shader.vsConstants->SetMatrix(m_device, shader.hWVP,   (const D3DXMATRIX*)&wvp);
        if (shader.hWorld) shader.vsConstants->SetMatrix(m_device, shader.hWorld, (const D3DXMATRIX*)&w);
    }
    m_device->SetPixelShaderConstantF(0, lightDir, 1);
    m_device->SetPixelShaderConstantF(1, camPos, 1);
    m_device->SetPixelShaderConstantF(2, ambient, 1);
    m_device->SetPixelShaderConstantF(3, time4, 1); // gTime
    m_device->SetStreamSource(0, vb, 0, 44);
    m_device->SetIndices(ib);
    if (model_gm)
    {
        // "model" geometry: one draw per material subset, its textures bound.
        for (size_t i = 0; i < model_gm->subsets.size(); ++i)
        {
            const RtSubset& sub = model_gm->subsets[i];
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
