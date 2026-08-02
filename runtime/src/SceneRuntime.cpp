#include "SceneRuntime.h"
#include "Endian.h"
#include "RtMath.h"
#include "SpakFormat.h"
#include "XboxRenderer.h"
#include "camera/EnvCubeViews.h"
#include "input/XInputPoll.h" // XInput is available via xtl.h (included by the header)

#include <xgraphics.h> // GPU_EDRAM_TILE_SIZE (env-capture EDRAM placement)

#include <stdio.h>
#include <string.h>

using namespace rmath;

SceneRuntime::SceneRuntime()
    : m_audio(&m_pak)
{
}

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

    m_mesh_decl = m_skin_mesh_decl = NULL;
    m_def_white = m_def_normal = m_def_black = NULL;
    m_def_envcube = NULL; m_env = NULL;
    m_envRT = NULL; m_envDepth = NULL; m_envDynCube = NULL; m_envBlurCube = NULL;
    m_envBlurTmp = NULL; m_env_captured = false;
    m_quad_vb = NULL; m_quad_ib = NULL; m_cube_vb = NULL; m_cube_ib = NULL;
    m_beam_vb = NULL; m_spot_beam_count = 0;

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

    // Light payload defaults, in case Render runs before BuildDrawLists: legacy
    // ambient, muted spot slots with a valid cone pair (see BuildDrawLists).
    memset(m_spot_pos, 0, sizeof(m_spot_pos));
    memset(m_spot_dir, 0, sizeof(m_spot_dir));
    memset(m_spot_col, 0, sizeof(m_spot_col));
    m_spot_dir[0][2] = m_spot_dir[1][2] = 1.0f;
    m_spot_dir[0][3] = m_spot_dir[1][3] = 1.0f;
    m_ambient[0] = 0.22f; m_ambient[1] = 0.22f; m_ambient[2] = 0.25f; m_ambient[3] = 0.0f;

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
    const D3DVERTEXELEMENT9 skinElems[] = {
        {0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
        {0, 24, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0},
        {0, 36, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {1,  0, D3DDECLTYPE_UBYTE4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0},
        {1,  4, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0},
        D3DDECL_END()
    };
    if (FAILED(m_device->CreateVertexDeclaration(skinElems, &m_skin_mesh_decl)))
        return false;

    m_def_white  = SolidTexture(D3DCOLOR_ARGB(255, 255, 255, 255)); // diffuse
    m_def_normal = SolidTexture(D3DCOLOR_ARGB(255, 128, 128, 255)); // flat normal
    m_def_black  = SolidTexture(D3DCOLOR_ARGB(255, 0, 0, 0));       // no specular / metal

    // Black 1x1 cube: the env-map fallback (metal reflects nothing).
    if (SUCCEEDED(m_device->CreateCubeTexture(1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m_def_envcube, NULL)))
    {
        for (int f = 0; f < 6; ++f)
        {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(m_def_envcube->LockRect((D3DCUBEMAP_FACES)f, 0, &lr, NULL, 0)))
            {
                *(DWORD*)lr.pBits = D3DCOLOR_ARGB(255, 0, 0, 0);
                m_def_envcube->UnlockRect((D3DCUBEMAP_FACES)f, 0);
            }
        }
    }

    if (!BuildGeometry())
        return false;

    m_content.LoadStandard();
    m_env = m_content.LoadEnvCube(); // static fallback; the dynamic capture wins

    // Dynamic environment capture resources: a small EDRAM target + depth at
    // Base 0 (aliasing the tile targets — the capture runs before BeginTiling)
    // and the cube texture the faces resolve into. Failure just disables the
    // capture; the static/black env fallback still binds.
    {
        D3DSURFACE_PARAMETERS sp;
        ZeroMemory(&sp, sizeof(sp));
        sp.Base = 0;
        if (SUCCEEDED(m_device->CreateRenderTarget(128, 128, D3DFMT_A8R8G8B8,
                                                   D3DMULTISAMPLE_NONE, 0, FALSE, &m_envRT, &sp)))
        {
            sp.Base = m_envRT->Size / GPU_EDRAM_TILE_SIZE;
            sp.HierarchicalZBase = 0;
            m_device->CreateDepthStencilSurface(128, 128, D3DFMT_D24S8,
                                                D3DMULTISAMPLE_NONE, 0, FALSE, &m_envDepth, &sp);
        }
        m_device->CreateCubeTexture(128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_envDynCube, NULL);
        // Blurred copy the roughness chain samples (standard.hlsl texCUBElod).
        // Separate from the capture so a blur level never reads the resource it
        // is writing; on failure the sharp capture still binds at max mip 0.
        m_device->CreateCubeTexture(128, envcube::kMips, 0, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, &m_envBlurCube, NULL);
        m_device->CreateCubeTexture(128, envcube::kMips, 0, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, &m_envBlurTmp, NULL);
    }

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

void SceneRuntime::AnimatorSetFloat(int objectIndex, const char* name, float value)
{
    for (size_t index = 0; index < m_draw_items.size(); ++index)
        if (m_draw_items[index].object_index == objectIndex)
            m_draw_items[index].animator.SetFloat(name, value);
}

void SceneRuntime::AnimatorSetBool(int objectIndex, const char* name, bool value)
{
    for (size_t index = 0; index < m_draw_items.size(); ++index)
        if (m_draw_items[index].object_index == objectIndex)
            m_draw_items[index].animator.SetBool(name, value);
}

void SceneRuntime::AnimatorSetTrigger(int objectIndex, const char* name)
{
    for (size_t index = 0; index < m_draw_items.size(); ++index)
        if (m_draw_items[index].object_index == objectIndex)
            m_draw_items[index].animator.SetTrigger(name);
}

void SceneRuntime::AnimatorSetState(int objectIndex, const char* name)
{
    for (size_t index = 0; index < m_draw_items.size(); ++index)
        if (m_draw_items[index].object_index == objectIndex)
            m_draw_items[index].animator.SetState(name);
}

// Lua audio.*: transient overrides on the object's Audio attribute (the
// parsed scene keeps its authored values).
void SceneRuntime::AudioSetPlaying(int objectIndex, bool play)
{
    const char* name = ObjectName(objectIndex);
    if (!name || !*name)
        return;
    const bool runningNow = AudioIsPlaying(objectIndex);
    AudioOverride& ov = m_audio_overrides[name];
    ov.play = play ? 1 : 0;
    if (play && !runningNow)
        ++ov.gen; // restart from the top
}
bool SceneRuntime::AudioIsPlaying(int objectIndex)
{
    const char* name = ObjectName(objectIndex);
    if (!name || !*name)
        return false;
    std::map<std::string, AudioOverride>::const_iterator ov = m_audio_overrides.find(name);
    for (size_t i = 0; i < m_audio_items.size(); ++i)
        if (m_audio_items[i].object == name)
        {
            if (ov != m_audio_overrides.end() && ov->second.play == 0)
                return false;
            const std::string key = AudioKeyFor(m_audio_items[i]);
            if (m_audio.HasStream(key))
                return m_audio.IsPlaying(key);
            if (ov != m_audio_overrides.end() && ov->second.play == 1)
                return true;
            return m_audio_items[i].play &&
                   (ov == m_audio_overrides.end() || ov->second.play != 0);
        }
    return false;
}
void SceneRuntime::AudioSetVolume(int objectIndex, float volume)
{
    const char* name = ObjectName(objectIndex);
    if (name && *name)
        m_audio_overrides[name].volume = volume < 0.0f ? 0.0f : volume;
}
void SceneRuntime::AudioSetPitch(int objectIndex, float pitch)
{
    const char* name = ObjectName(objectIndex);
    if (name && *name)
        m_audio_overrides[name].pitch = pitch < 0.1f ? 0.1f : pitch;
}
void SceneRuntime::AudioSetLoop(int objectIndex, bool loop)
{
    const char* name = ObjectName(objectIndex);
    if (name && *name)
        m_audio_overrides[name].loop = loop ? 1 : 0;
}

    void SceneRuntime::TextSetValue(int objectIndex, const char* value)
    {
        const char* name = ObjectName(objectIndex);
        if (name && *name)
        m_text_overrides[name] = value ? value : "";
    }

// Lua video.play/stop: an override on the object's Video attribute play mode,
// consulted when RenderOverlay builds the wanted set each frame. The
// parsed scene keeps its authored value. play on a video that isn't currently
// running bumps the restart generation (new stream key -> decode from the
// top); play while running only updates the mode (Once <-> Loop, live).
void SceneRuntime::VideoSetPlaying(int objectIndex, bool play, bool loop)
{
    const char* name = ObjectName(objectIndex);
    if (!name || !*name)
        return;
    const bool runningNow = VideoIsPlaying(objectIndex);
    VideoOverride& ov = m_video_overrides[name];
    if (play)
    {
        ov.mode = loop ? vid::PlayLoop : vid::PlayOnce;
        if (!runningNow)
            ++ov.gen; // restart from the top
    }
    else
        ov.mode = vid::PlayOff;
}
bool SceneRuntime::VideoIsPlaying(int objectIndex)
{
    const char* name = ObjectName(objectIndex);
    if (!name || !*name)
        return false;
    // Streams reconcile at draw time, so a play/stop issued this same script
    // tick is answered from the override; an existing stream (at the current
    // effective key) still decides Once-finished.
    std::map<std::string, VideoOverride>::const_iterator ov = m_video_overrides.find(name);
    for (size_t i = 0; i < m_video_items.size(); ++i)
        if (m_video_items[i].object == name)
        {
            if (ov != m_video_overrides.end() && ov->second.mode == (int)vid::PlayOff)
                return false;
            const std::string key = VideoKeyFor(m_video_items[i]);
            if (m_video.HasStream(key))
                return m_video.IsPlaying(key);
            // play() issued; the stream opens at this frame's draw.
            return ov != m_video_overrides.end();
        }
    return false;
}

// --- gui::HostAssets ------------------------------------------------------
// Ids are handed out once per path and never change; the POINTER behind an id
// is resolved every frame, because the StreamCache serves a placeholder while
// a load is in flight and can evict under LRU pressure.

int SceneRuntime::AcquireTexture(const char* relPath)
{
    if (!relPath || !*relPath)
        return -1;
    const std::string key = relPath;
    std::map<std::string, int>::const_iterator cached = m_gui_texture_ids.find(key);
    if (cached != m_gui_texture_ids.end())
        return cached->second;
    GuiAsset asset;
    asset.path = key;
    asset.isFont = false;
    const int id = (int)m_gui_assets.size();
    m_gui_assets.push_back(asset);
    m_gui_texture_ids[key] = id;
    return id;
}

IDirect3DTexture9* SceneRuntime::GuiTexture(int textureId)
{
    if (textureId < 0 || textureId >= (int)m_gui_assets.size())
        return NULL;
    const GuiAsset& asset = m_gui_assets[textureId];
    if (!asset.isFont)
        return m_cache.GetTexture(asset.path);
    IDirect3DTexture9* atlas = NULL;
    m_cache.GetFont(asset.path, &atlas);
    return atlas;
}

bool SceneRuntime::TextureSize(int textureId, int& outWidth, int& outHeight)
{
    IDirect3DTexture9* texture = GuiTexture(textureId);
    D3DSURFACE_DESC desc;
    if (!texture || FAILED(texture->GetLevelDesc(0, &desc)))
        return false;
    outWidth = (int)desc.Width;
    outHeight = (int)desc.Height;
    return true;
}

const text::FontMetrics* SceneRuntime::AcquireFont(const char* relPath)
{
    if (!relPath || !*relPath)
        return NULL;
    IDirect3DTexture9* atlas = NULL;
    const text::CookedFont* font = m_cache.GetFont(relPath, &atlas);
    // The atlas is half the answer: metrics without it would lay glyphs out
    // against a texture that is not there yet.
    if (!font || !atlas)
        return NULL;
    return &font->metrics;
}

int SceneRuntime::AcquireFontAtlas(const char* relPath)
{
    if (!relPath || !*relPath)
        return -1;
    IDirect3DTexture9* atlas = NULL;
    if (!m_cache.GetFont(relPath, &atlas) || !atlas)
        return -1;
    const std::string key = relPath;
    std::map<std::string, int>::const_iterator cached = m_gui_font_atlas_ids.find(key);
    if (cached != m_gui_font_atlas_ids.end())
        return cached->second;
    GuiAsset asset;
    asset.path = key;
    asset.isFont = true;
    const int id = (int)m_gui_assets.size();
    m_gui_assets.push_back(asset);
    m_gui_font_atlas_ids[key] = id;
    return id;
}

// Load each object's .lua from the deployed content and run its on_start.
void SceneRuntime::BuildScripts()
{
    m_gui.Begin(this);
    m_script.Begin(&m_phys, this, &m_gui);
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
    const unsigned int magic = endian::LoadU32BE(p + 0);
    const bool skinned = magic == spak::kSkinMeshMagic;
    if (magic != spak::kMeshMagic && !skinned)
        return false;
    const size_t headerBytes = skinned ? spak::kSkinMeshHeaderBytes : spak::kMeshHeaderBytes;
    if (blob.size() < headerBytes)
        return false;
    const unsigned int vcount      = endian::LoadU32BE(p + 4);
    const unsigned int icount      = endian::LoadU32BE(p + 8);
    const unsigned int subsetCount = endian::LoadU32BE(p + 12);
    const size_t subBytes = (size_t)subsetCount * spak::kMeshSubsetBytes;
    const size_t vbytes   = (size_t)vcount * spak::kMeshVertexBytes;
    const size_t skinBytes = skinned ? (size_t)vcount * spak::kSkinInfluenceBytes : 0;
    const size_t ibytes   = (size_t)icount * 4;
    if (headerBytes + subBytes + vbytes + skinBytes + ibytes > blob.size())
        return false;
    const size_t bufOff = headerBytes + subBytes;

    // VB/IB are baked big-endian = native on the console — copy straight out.
    pos.resize((size_t)vcount * 3);
    for (unsigned int v = 0; v < vcount; ++v)
        memcpy(&pos[(size_t)v * 3], p + bufOff + (size_t)v * spak::kMeshVertexBytes, sizeof(float) * 3);
    if (wantIndices && icount > 0)
    {
        idx.resize(icount);
        memcpy(&idx[0], p + bufOff + vbytes + skinBytes, ibytes);
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
                for (int axis = 0; axis < 3; ++axis)
                    d.lockRotation[axis] = at.phys_lock_rotation[axis];
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

    // Spot volumetric beam shader. Optional the same way: missing .cso just
    // means no light shafts (older deploys).
    // Roughness reflection chain. Optional: without it the capture still binds
    // sharp (max mip 0), so reflections stay correct, just unblurred.
    m_content.LoadBuiltin("env_blur", m_env_blur);

    m_content.LoadBuiltin("beam", m_beam);
    m_content.LoadBuiltin("image", m_image_shader); // Image attribute overlay (optional)
    m_content.LoadBuiltin("text", m_text_shader);   // Text attribute overlay (optional)
    m_content.LoadBuiltin("gui", m_gui_shader);     // Lua-scriptable GUI (optional)
    m_content.LoadBuiltin("video", m_video_shader); // Video attribute overlay (optional)
}

// One capture face: frame constants for the face's point of view, then both
// material passes over the draw items via DrawMesh, skipping the captured
// object (a mirror can't mirror itself).
void SceneRuntime::DrawModelsForEnv(const D3DMATRIX& vp, const float* eye, int skipItem)
{
    RtShader* mat = m_content.Standard();

    const float cam[4]     = { eye[0], eye[1], eye[2], 0.0f };
    // s5 here is the single-level black cube (no recursion), so roughness must
    // not reach for a mip that does not exist.
    const float zero4[4]   = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_device->SetPixelShaderConstantF(22, zero4, 1);
    m_device->SetPixelShaderConstantF(0, m_light_dir, 1);
    m_device->SetPixelShaderConstantF(1, cam, 1);
    m_device->SetPixelShaderConstantF(2, m_ambient, 1);
    m_device->SetPixelShaderConstantF(6, m_light_col, 1);
    m_device->SetPixelShaderConstantF(7, &m_point_pos[0][0], 4);
    m_device->SetPixelShaderConstantF(11, &m_point_col[0][0], 4);
    m_device->SetPixelShaderConstantF(16, &m_spot_pos[0][0], 2);
    m_device->SetPixelShaderConstantF(18, &m_spot_dir[0][0], 2);
    m_device->SetPixelShaderConstantF(20, &m_spot_col[0][0], 2);

    // Pass 1 — opaque + cutout; pass 2 — blended (same order as Render).
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    for (size_t i = 0; i < m_draw_items.size(); ++i)
    {
        if ((int)i == skipItem) continue;
        RtMesh* gm = ResolveMesh(m_draw_items[i].model_path);
        if (gm) DrawMesh(gm, m_draw_items[i].world, vp, mat, false, &m_draw_items[i].skin_palette);
    }
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHATOMASKENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    for (size_t i = 0; i < m_draw_items.size(); ++i)
    {
        if ((int)i == skipItem) continue;
        RtMesh* gm = ResolveMesh(m_draw_items[i].model_path);
        if (gm) DrawMesh(gm, m_draw_items[i].world, vp, mat, true, &m_draw_items[i].skin_palette);
    }
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
}

void SceneRuntime::RenderEnvCapture()
{
    m_env_captured = false;
    if (!m_device || !m_envRT || !m_envDepth || !m_envDynCube)
        return;
    RtShader* mat = m_content.Standard();
    if (!mat || !mat->Valid() || m_draw_items.empty())
        return;

    // The first draw item with a reflective subset is the capture point.
    // Roughness counts alongside metal and clearcoat: a matte object alone in a
    // scene must still reflect the room, not fall through to the painted cube.
    int   skip = -1;
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    // Two passes: metal first, so scenes that already had a capture keep
    // capturing from exactly the object they did before; rough/clearcoat only
    // supply a capture point when there is no metal at all.
    for (int pass = 0; pass < 2 && skip < 0; ++pass)
        for (size_t i = 0; i < m_draw_items.size(); ++i)
        {
            RtMesh* gm = ResolveMesh(m_draw_items[i].model_path);
            if (!gm) continue;
            bool reflective = false;
            for (size_t s = 0; s < gm->subsets.size(); ++s)
                if (pass == 0 ? (gm->subsets[s].metallic != NULL)
                              : (gm->subsets[s].roughness || gm->subsets[s].clearcoat))
                { reflective = true; break; }
            if (!reflective) continue;
            skip = (int)i;
            pos[0] = m_draw_items[i].world._41;
            pos[1] = m_draw_items[i].world._42;
            pos[2] = m_draw_items[i].world._43;
            break;
        }
    if (skip < 0)
        return; // nothing reflective in the scene -> nothing needs reflections

    m_device->SetRenderTarget(0, m_envRT);
    m_device->SetDepthStencilSurface(m_envDepth);
    D3DVIEWPORT9 vp;
    vp.X = 0; vp.Y = 0; vp.Width = 128; vp.Height = 128;
    vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    m_device->SetViewport(&vp);

    m_device->SetVertexDeclaration(m_mesh_decl);
    m_device->SetVertexShader(mat->vs);
    m_device->SetPixelShader(mat->ps);
    m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    m_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    for (DWORD st = 0; st < 5; ++st)
    {
        m_device->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        m_device->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    }
    // Metal inside a reflection reflects nothing (no recursion). The clearcoat
    // slot is bound per subset by DrawMesh; its env term is zero here anyway.
    m_device->SetTexture(5, (IDirect3DBaseTexture9*)m_def_envcube);

    float pm[16];
    envcube::FaceProj(0.2f, 100.0f, pm);
    D3DMATRIX proj;
    memcpy(&proj, pm, sizeof(pm));

    const D3DRECT faceRect = { 0, 0, 128, 128 };
    for (int f = 0; f < 6; ++f)
    {
        m_device->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                        D3DCOLOR_ARGB(255, 24, 24, 28), 1.0f, 0);
        float vm[16];
        envcube::FaceView(f, pos, vm);
        D3DMATRIX view;
        memcpy(&view, vm, sizeof(vm));
        DrawModelsForEnv(Multiply(view, proj), pos, skip);
        m_device->Resolve(D3DRESOLVE_RENDERTARGET0, &faceRect, m_envDynCube,
                          NULL, 0, (UINT)f, NULL, 0.0f, 0, NULL);
    }
    m_env_captured = true;

    BuildEnvBlurChain();
}

// Fill m_envBlurCube from the sharp capture by running env_blur.hlsl over every
// face of every level. Mirrors SceneRenderer::BuildEnvBlurChain, including the
// two rules it depends on: the two cubes take turns as source and destination,
// and levels run outermost because the taps cross face boundaries.
//
// Called from RenderEnvCapture, so it stays OUTSIDE the tiling bracket and the
// EDRAM target at Base 0 remains legal.
void SceneRuntime::BuildEnvBlurChain()
{
    if (!m_envRT || !m_envBlurCube || !m_envBlurTmp || !m_envDynCube || !m_env_blur.Valid())
        return;

    struct QuadVtx { float x, y, z, u, v; };
    static const QuadVtx quad[4] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
    };

    m_device->SetRenderTarget(0, m_envRT);
    m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    m_device->SetVertexDeclaration(NULL);
    m_device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    m_device->SetVertexShader(m_env_blur.vs);
    m_device->SetPixelShader(m_env_blur.ps);
    m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    // POINT: the shader's explicit LOD must land on exactly the level we built,
    // not blend in one that was never written.
    m_device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);

    for (int level = 0; level < envcube::kMips; ++level)
    {
        const int   size    = envcube::kSize >> level;
        const int   passes  = envcube::BlurPasses(level);
        const float half[4] = { 1.0f / (float)size, 1.0f / (float)size, 0.0f, 0.0f };
        const D3DRECT lvlRect = { 0, 0, size, size };
        D3DVIEWPORT9 lvp;
        lvp.X = 0; lvp.Y = 0; lvp.Width = (DWORD)size; lvp.Height = (DWORD)size;
        lvp.MinZ = 0.0f; lvp.MaxZ = 1.0f;
        m_device->SetViewport(&lvp);
        m_device->SetVertexShaderConstantF(0, half, 1);

        // Pass 0 halves resolution, the rest re-filter in place. The cubes
        // alternate and the count is even, so the last write always lands in
        // m_envBlurCube — what s5 binds and what the next level reads.
        for (int pass = 0; pass < passes; ++pass)
        {
            const bool toBlur = (level == 0) || (pass & 1);
            IDirect3DCubeTexture9* dst = toBlur ? m_envBlurCube : m_envBlurTmp;
            IDirect3DCubeTexture9* src = (level == 0) ? m_envDynCube
                                       : (toBlur ? m_envBlurTmp : m_envBlurCube);
            const float srcLod = (pass == 0) ? (float)(level - 1) : (float)level;
            const float step[4] = { (pass == 0) ? envcube::TapStep(level)
                                               : envcube::TapStepAt(level),
                                    (level == 0) ? 0.0f : srcLod,
                                    envcube::EdgeFixup(level), 0.0f };
            m_device->SetTexture(0, (IDirect3DBaseTexture9*)src);
            m_device->SetPixelShaderConstantF(3, step, 1);
            for (int f = 0; f < 6; ++f)
            {
                float r3[3], u3[3], f3[3];
                envcube::FaceBasis(f, r3, u3, f3);
                const float faceR[4] = { r3[0], r3[1], r3[2], 0.0f };
                const float faceU[4] = { u3[0], u3[1], u3[2], 0.0f };
                const float faceF[4] = { f3[0], f3[1], f3[2], 0.0f };
                m_device->SetPixelShaderConstantF(0, faceR, 1);
                m_device->SetPixelShaderConstantF(1, faceU, 1);
                m_device->SetPixelShaderConstantF(2, faceF, 1);
                m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVtx));
                m_device->Resolve(D3DRESOLVE_RENDERTARGET0, &lvlRect, dst,
                                  NULL, (UINT)level, (UINT)f, NULL, 0.0f, 0, NULL);
            }
        }
    }
    m_device->SetTexture(0, NULL);
    m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
}

void SceneRuntime::Shutdown()
{
    m_audio.Shutdown();
    m_video.Shutdown();
    for (size_t i = 0; i < m_video_tex.size(); ++i)
        for (int s = 0; s < 2; ++s)
        {
            if (m_video_tex[i].y[s])  m_video_tex[i].y[s]->Release();
            if (m_video_tex[i].cb[s]) m_video_tex[i].cb[s]->Release();
            if (m_video_tex[i].cr[s]) m_video_tex[i].cr[s]->Release();
        }
    m_video_tex.clear();
    m_image_shader.Release();
    m_text_shader.Release();
    m_gui_shader.Release();
    m_video_shader.Release();
    m_bloom_combine.Release();
    m_bloom_blur.Release();
    m_bloom_bright.Release();
    m_beam.Release();
    RuntimeAnimator::ClearSharedResources();
    m_cache.Shutdown();
    m_pak.Close();
    if (m_beam_vb)   { m_beam_vb->Release();   m_beam_vb = NULL; }
    if (m_cube_ib)   { m_cube_ib->Release();   m_cube_ib = NULL; }
    if (m_cube_vb)   { m_cube_vb->Release();   m_cube_vb = NULL; }
    if (m_quad_ib)   { m_quad_ib->Release();   m_quad_ib = NULL; }
    if (m_quad_vb)   { m_quad_vb->Release();   m_quad_vb = NULL; }
    if (m_def_black) { m_def_black->Release(); m_def_black = NULL; }
    if (m_def_normal){ m_def_normal->Release();m_def_normal = NULL; }
    if (m_envBlurTmp)  { m_envBlurTmp->Release();  m_envBlurTmp = NULL; }
    if (m_envBlurCube) { m_envBlurCube->Release(); m_envBlurCube = NULL; }
    if (m_envDynCube)  { m_envDynCube->Release();  m_envDynCube = NULL; }
    if (m_envDepth)    { m_envDepth->Release();    m_envDepth = NULL; }
    if (m_envRT)       { m_envRT->Release();       m_envRT = NULL; }
    if (m_env)         { m_env->Release();         m_env = NULL; }
    if (m_def_envcube) { m_def_envcube->Release(); m_def_envcube = NULL; }
    if (m_def_white) { m_def_white->Release(); m_def_white = NULL; }
    if (m_skin_mesh_decl) { m_skin_mesh_decl->Release(); m_skin_mesh_decl = NULL; }
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

    // Unit cone for the spot volumetric beam (beam.hlsl): apex at the origin
    // opening down +Z, ring radius 1 at z = 1 (radius = z) — the world matrix's
    // (r, r, range) scale shapes it into any spot's outer cone. Side surface
    // only (the far end fades to zero in the shader). Smooth ring normals
    // ((cos, sin, -1)/sqrt2); the apex takes its face's mid-angle normal.
    // uv.x = t along the beam (0 = apex, 1 = end); mirrors the editor's
    // SceneRenderer::BuildBeamCone.
    {
        const int   seg = 24;
        const float TAU = 6.28318531f;
        const float inv = 0.70710678f; // 1/sqrt2
        if (FAILED(m_device->CreateVertexBuffer(seg * 3 * 44, 0, 0, D3DPOOL_MANAGED, &m_beam_vb, NULL)))
            return false;
        unsigned char* p = NULL;
        m_beam_vb->Lock(0, 0, (void**)&p, 0);
        for (int s = 0; s < seg; ++s)
        {
            const float a0 = (float)s / seg * TAU;
            const float a1 = (float)(s + 1) / seg * TAU;
            const float am = (a0 + a1) * 0.5f;
            PutVertex(p + (s * 3 + 0) * 44, 0, 0, 0,
                      cosf(am) * inv, sinf(am) * inv, -inv,  0,0,0,  0.0f, 0.0f);
            PutVertex(p + (s * 3 + 1) * 44, cosf(a0), sinf(a0), 1.0f,
                      cosf(a0) * inv, sinf(a0) * inv, -inv,  0,0,0,  1.0f, 0.0f);
            PutVertex(p + (s * 3 + 2) * 44, cosf(a1), sinf(a1), 1.0f,
                      cosf(a1) * inv, sinf(a1) * inv, -inv,  0,0,0,  1.0f, 0.0f);
        }
        m_beam_vb->Unlock();
    }
    return true;
}

// Capture the models + standalone shaders to draw from the loaded scene. Static
// on the console (no editing), so this runs once at Init.
void SceneRuntime::BuildDrawLists()
{
    m_draw_items.clear();
    m_shader_items.clear();
    m_image_items.clear();
    m_text_items.clear();
    m_video_items.clear();
    m_audio_items.clear();
    m_has_cam = false;

    bool have_dir = false;
    bool have_env = false;
    int  point_count = 0;
    int  spot_count = 0;
    memset(m_light_dir, 0, sizeof(m_light_dir));
    memset(m_light_col, 0, sizeof(m_light_col));
    memset(m_point_pos, 0, sizeof(m_point_pos));
    memset(m_point_col, 0, sizeof(m_point_col));
    memset(m_spot_pos, 0, sizeof(m_spot_pos));
    memset(m_spot_dir, 0, sizeof(m_spot_dir));
    memset(m_spot_col, 0, sizeof(m_spot_col));
    // Unused spot slots keep a valid cone pair (inner cos 1, outer cos 0) so the
    // shader's smoothstep edges never collapse; zero color mutes them anyway.
    m_spot_dir[0][2] = m_spot_dir[1][2] = 1.0f;
    m_spot_dir[0][3] = m_spot_dir[1][3] = 1.0f;
    m_ambient[0] = m_ambient[1] = m_ambient[2] = m_ambient[3] = 0.0f;
    m_spot_beam_count = 0;
    int overlay_sequence = 0;

    for (size_t i = 0; i < m_scene.objects.size(); ++i)
    {
        const RtObject& o = m_scene.objects[i];
        if (!o.visible)
            continue;

        std::string model_path, shader_path;
        const RtAttribute* animator_attribute = NULL;
        for (size_t a = 0; a < o.attributes.size(); ++a)
        {
            const RtAttribute& at = o.attributes[a];
            if (at.type == "3D Model" && !at.model_path.empty() && model_path.empty())
                model_path = at.model_path;
            else if (at.type == "Animator" && !at.animator_controller_path.empty() && !animator_attribute)
                animator_attribute = &at;
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
            else if (at.type == "Audio" && !at.audio_path.empty())
            {
                AudioItem ai;
                char keybuf[16];
                sprintf_s(keybuf, sizeof(keybuf), "#%u", (unsigned int)a);
                ai.key    = o.name + keybuf;
                ai.object = o.name;
                ai.path   = at.audio_path;
                ai.object_index = (int)i;
                ai.play     = at.audio_play != 0;
                ai.loop     = at.audio_loop;
                ai.volume   = at.audio_volume;
                ai.pitch    = at.audio_pitch;
                ai.audio_class = at.audio_class;
                ai.priority = at.audio_priority;
                ai.load_mode = at.audio_load_mode;
                ai.spatial  = at.audio_spatial;
                ai.min_dist = at.audio_min_dist;
                ai.max_dist = at.audio_max_dist;
                ai.doppler  = at.audio_doppler;
                m_audio_items.push_back(ai);
            }
            else if (at.type == "Image" && !at.image_path.empty())
            {
                ImageItem image;
                image.path = at.image_path;
                image.x = at.image_x; image.y = at.image_y;
                image.w = at.image_w; image.h = at.image_h;
                image.stretch = at.image_stretch;
                image.lock_aspect = at.image_lock_aspect;
                for (int k = 0; k < 3; ++k) image.tint[k] = at.image_tint[k];
                image.alpha = at.image_alpha;
                image.priority = at.image_priority;
                image.sequence = overlay_sequence++;
                m_image_items.push_back(image);
            }
            else if (at.type == "Text" && !at.text_font_path.empty() && !at.text_value.empty())
            {
                TextItem textItem;
                textItem.object = o.name;
                textItem.fontPath = at.text_font_path;
                textItem.value = at.text_value;
                textItem.x = at.text_x; textItem.y = at.text_y;
                textItem.w = at.text_w; textItem.h = at.text_h;
                textItem.fontSize = at.text_font_size;
                for (int k = 0; k < 3; ++k) textItem.color[k] = at.text_color[k];
                textItem.alpha = at.text_alpha;
                textItem.lockAspect = at.text_lock_aspect;
                textItem.priority = at.text_priority;
                textItem.sequence = overlay_sequence++;
                m_text_items.push_back(textItem);
            }
            else if (at.type == "Video" && !at.video_path.empty())
            {
                VideoItem vi;
                char keybuf[16];
                sprintf_s(keybuf, sizeof(keybuf), "#%u", (unsigned int)a);
                vi.key    = o.name + keybuf;
                vi.object = o.name;
                vi.path   = at.video_path;
                vi.x = at.video_x; vi.y = at.video_y;
                vi.w = at.video_w; vi.h = at.video_h;
                vi.stretch     = at.video_stretch;
                vi.lock_aspect = at.video_lock_aspect;
                for (int k = 0; k < 3; ++k) vi.tint[k] = at.video_tint[k];
                vi.alpha     = at.video_alpha;
                vi.priority  = at.video_priority;
                vi.sequence  = overlay_sequence++;
                vi.play_mode = at.video_play_mode;
                vi.volume    = at.video_volume;
                vi.muted     = at.video_muted;
                m_video_items.push_back(vi);
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
            else if (at.type == "Spot Light" && spot_count < 2)
            {
                // Position + beam direction (+Z forward) from the transform.
                const float d2r = kPi / 180.0f;
                const D3DMATRIX r = Multiply(Multiply(RotationX(o.rotation[0] * d2r),
                                                      RotationY(o.rotation[1] * d2r)),
                                             RotationZ(o.rotation[2] * d2r));
                const float range = at.light_range > 0.1f ? at.light_range : 0.1f;
                // Keep the smoothstep edges apart: outer strictly wider.
                float inner = at.light_inner_deg;
                if (inner < 0.1f)  inner = 0.1f;
                if (inner > 89.0f) inner = 89.0f;
                float outer = at.light_outer_deg;
                if (outer < inner + 0.1f) outer = inner + 0.1f;
                m_spot_pos[spot_count][0] = o.position[0];
                m_spot_pos[spot_count][1] = o.position[1];
                m_spot_pos[spot_count][2] = o.position[2];
                m_spot_pos[spot_count][3] = 1.0f / range;
                m_spot_dir[spot_count][0] = r._31;
                m_spot_dir[spot_count][1] = r._32;
                m_spot_dir[spot_count][2] = r._33;
                m_spot_dir[spot_count][3] = cosf(inner * d2r);
                for (int k = 0; k < 3; ++k) m_spot_col[spot_count][k] = at.light_color[k] * at.light_intensity;
                m_spot_col[spot_count][3] = cosf(outer * d2r);
                ++spot_count;
                if (at.light_volumetric && at.light_volumetric_intensity > 0.0f)
                {
                    // Beam brightness: additive and double-sided, so a small
                    // gain reads as haze rather than a solid (tune by eye;
                    // keep in sync with SceneRenderer.cpp).
                    const float kBeamGain = 0.10f;
                    const float tano = tanf(outer * d2r);
                    const float rr = tano * range;
                    SpotBeam& b = m_spot_beams[m_spot_beam_count++];
                    b.world = Multiply(Multiply(Scaling(rr, rr, range), r),
                                       Translation(o.position[0], o.position[1], o.position[2]));
                    for (int k = 0; k < 3; ++k)
                        b.color[k] = at.light_color[k] * at.light_intensity *
                                     at.light_volumetric_intensity * kBeamGain;
                    b.color[3] = 0.0f;
                    b.apex[0] = o.position[0]; b.apex[1] = o.position[1];
                    b.apex[2] = o.position[2]; b.apex[3] = 0.0f;
                    b.axis[0] = r._31; b.axis[1] = r._32; b.axis[2] = r._33;
                    b.axis[3] = tano;
                }
            }
            else if (at.type == "Environment Light")
            {
                // Every instance sums into the one ambient term (c2).
                for (int k = 0; k < 3; ++k) m_ambient[k] += at.light_color[k] * at.light_intensity;
                have_env = true;
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
            if (animator_attribute)
                di.animator.Load(&m_pak, &m_cache, animator_attribute->animator_controller_path,
                                 animator_attribute->animator_initial_state,
                                 animator_attribute->animator_playback_speed,
                                 animator_attribute->animator_auto_play);
            m_draw_items.push_back(di);
        }
    }

    // No directional light authored: keep the legacy fixed sun — white when the
    // scene has no direct lights at all (old scenes render unchanged), black
    // when point/spot lights exist (the author owns the lighting; only c0's
    // direction, which custom shaders read, stays meaningful).
    if (!have_dir)
    {
        Vec3 lraw = { -0.4f, -1.0f, -0.5f };
        Vec3 ln = Normalize(lraw);
        m_light_dir[0] = ln.x; m_light_dir[1] = ln.y; m_light_dir[2] = ln.z;
        const float w = (point_count == 0 && spot_count == 0) ? 1.0f : 0.0f;
        m_light_col[0] = m_light_col[1] = m_light_col[2] = w;
    }
    // No Environment Light authored: keep the legacy fixed ambient, so every
    // existing scene renders unchanged. Authored ones replace it entirely (an
    // Environment Light is only ever an ambient booster — it does not touch the
    // legacy-sun rule above).
    if (!have_env)
    {
        m_ambient[0] = 0.22f; m_ambient[1] = 0.22f; m_ambient[2] = 0.25f;
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

    // A menu freezes animation along with physics (gui.set_paused).
    const float animDt = m_gui.Paused() ? 0.0f : dt;
    for (size_t index = 0; index < m_draw_items.size(); ++index)
    {
        DrawItem& item = m_draw_items[index];
        if (!item.animator.IsValid()) continue;
        item.animator.Update(animDt);
    }

    // Poll the controller, then run scripts before the physics step so their
    // impulses/velocity/transforms are integrated this frame (no-op if none).
    // The GUI reads the same snapshot first, turning it into focus/confirm
    // events that ScriptVM::Update drains before any on_update runs.
    input::PollXInput(m_input);
    m_gui.Update(dt, m_input);
    m_script.Update(dt);

    // Physics: step Bullet, then override the draw items whose objects are
    // simulated (world = Scale * pose). Same code path as the editor preview.
    // gui.set_paused(true) freezes the step but still reads poses back, so
    // simulated objects hold where they stopped instead of snapping to their
    // authored transforms.
    if (!m_phys.Empty())
    {
        if (!m_gui.Paused())
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
            {
                const char* boneName = NULL;
                if (events[ei].boneHash)
                    for (size_t drawIndex = 0; drawIndex < m_draw_items.size() && !boneName; ++drawIndex)
                        if (m_draw_items[drawIndex].object_index == events[ei].triggerObjectIndex)
                            boneName = m_draw_items[drawIndex].animator.BoneName(events[ei].boneHash);
                m_script.FireTrigger(events[ei].triggerObjectIndex,
                                     events[ei].otherObjectIndex, boneName);
            }
    }

    for (size_t index = 0; index < m_draw_items.size(); ++index)
    {
        DrawItem& item = m_draw_items[index];
        if (!item.animator.IsValid()) continue;
        RtMesh* mesh = ResolveMesh(item.model_path);
        if (!mesh || !mesh->IsSkinned()) continue;
        item.animator.BuildPalette(*mesh, &item.world.m[0][0], item.skin_palette);
        item.animator.GetBoneColliders(*mesh, item.bone_collider_poses);
        if (item.bone_collider_handles.size() != item.bone_collider_poses.size())
        {
            item.bone_collider_handles.clear();
            for (size_t colliderIndex = 0; colliderIndex < item.bone_collider_poses.size(); ++colliderIndex)
            {
                RuntimeAnimator::BoneColliderPose& pose = item.bone_collider_poses[colliderIndex];
                float scaledExtents[3] = {pose.halfExtents[0] * fabsf(item.scale[0]),
                                          pose.halfExtents[1] * fabsf(item.scale[1]),
                                          pose.halfExtents[2] * fabsf(item.scale[2])};
                item.bone_collider_handles.push_back(m_phys.AddBoneCollider(
                    item.object_index, pose.boneHash, scaledExtents));
            }
        }
        for (size_t colliderIndex = 0; colliderIndex < item.bone_collider_poses.size(); ++colliderIndex)
        {
            D3DMATRIX boneModel;
            memcpy(&boneModel.m[0][0], item.bone_collider_poses[colliderIndex].modelMatrix,
                   sizeof(item.bone_collider_poses[colliderIndex].modelMatrix));
            const D3DMATRIX colliderWorld = Multiply(boneModel, item.world);
            m_phys.UpdateBoneCollider(item.bone_collider_handles[colliderIndex],
                                      &colliderWorld.m[0][0]);
        }
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

    // --- Scene models (standard diffuse / normal / specular / emissive /
    //     metallic material + environment cube) ---
    RtShader* std_mat = m_content.Standard();
    if (!m_draw_items.empty() && std_mat && std_mat->Valid())
    {
        m_device->SetVertexShader(std_mat->vs);
        m_device->SetPixelShader(std_mat->ps);
        for (DWORD s = 0; s < 5; ++s)
        {
            m_device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            m_device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        }
        // s5 = environment cube (bound once per frame); s6 = clearcoat mask,
        // s7 = roughness. MIPFILTER is what makes roughness blur anything:
        // D3D9 defaults it to D3DTEXF_NONE, which pins texCUBElod to level 0.
        m_device->SetSamplerState(5, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(5, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(5, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(5, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        m_device->SetSamplerState(5, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        m_device->SetSamplerState(5, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
        for (DWORD s = 6; s <= 7; ++s)
        {
            m_device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            m_device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        }
        // Only the blurred chain has levels to read; the painted cube and the
        // black default are single-level, so roughness gets max mip 0 there.
        const bool envBlurred = m_env_captured && m_envBlurCube && m_env_blur.Valid();
        m_device->SetTexture(5, envBlurred      ? (IDirect3DBaseTexture9*)m_envBlurCube
                              : m_env_captured  ? (IDirect3DBaseTexture9*)m_envDynCube
                              : m_env           ? (IDirect3DBaseTexture9*)m_env
                                                : (IDirect3DBaseTexture9*)m_def_envcube);
        const float roughCfg[4] = { envBlurred ? envcube::kMaxMip : 0.0f, 0.0f, 0.0f, 0.0f };
        m_device->SetPixelShaderConstantF(22, roughCfg, 1);
        m_device->SetPixelShaderConstantF(0, lightDir, 1);
        m_device->SetPixelShaderConstantF(1, camPos, 1);
        m_device->SetPixelShaderConstantF(2, m_ambient, 1);
        m_device->SetPixelShaderConstantF(6, m_light_col, 1);
        m_device->SetPixelShaderConstantF(7, &m_point_pos[0][0], 4);
        m_device->SetPixelShaderConstantF(11, &m_point_col[0][0], 4);
        m_device->SetPixelShaderConstantF(16, &m_spot_pos[0][0], 2);
        m_device->SetPixelShaderConstantF(18, &m_spot_dir[0][0], 2);
        m_device->SetPixelShaderConstantF(20, &m_spot_col[0][0], 2);

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
                DrawMesh(gm, m_draw_items[i].world, vp, std_mat, false /*opaque+cutout*/,
                         &m_draw_items[i].skin_palette);
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
                DrawMesh(gm, m_draw_items[i].world, vp, std_mat, true /*blend*/,
                         &m_draw_items[i].skin_palette);
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

    // --- Spot volumetric beams (additive cone mesh, beam.hlsl) ---
    // Last of the translucents: additive is order-independent, depth test keeps
    // geometry occluding the shaft, no depth write. The color-write mask is
    // already RGB-only here, so the ONE+ONE blend can't disturb the glow mask.
    if (m_spot_beam_count > 0 && m_beam.Valid() && m_beam_vb)
    {
        m_device->SetVertexDeclaration(m_mesh_decl);
        m_device->SetVertexShader(m_beam.vs);
        m_device->SetPixelShader(m_beam.ps);
        m_device->SetStreamSource(0, m_beam_vb, 0, 44);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_ONE);
        m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        m_device->SetPixelShaderConstantF(1, camPos, 1);
        for (int i = 0; i < m_spot_beam_count; ++i)
        {
            const SpotBeam& b = m_spot_beams[i];
            const D3DMATRIX wvp = Multiply(b.world, vp);
            if (m_beam.vsConstants)
            {
                if (m_beam.hWVP)   m_beam.vsConstants->SetMatrix(m_device, m_beam.hWVP,   (const D3DXMATRIX*)&wvp);
                if (m_beam.hWorld) m_beam.vsConstants->SetMatrix(m_device, m_beam.hWorld, (const D3DXMATRIX*)&b.world);
            }
            m_device->SetPixelShaderConstantF(0, b.color, 1);
            m_device->SetPixelShaderConstantF(2, b.apex, 1);
            m_device->SetPixelShaderConstantF(3, b.axis, 1);
            m_device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 24);
        }
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    }

    // --- Image/video overlays (screen-space, on top of everything) ---
    // Drawn inside the tiled pass — a clip-space quad replays fine per band.
    RenderOverlay(dt);

    // --- Lua-scriptable GUI (menus), last so it sits over the overlays ---
    RenderGui();

    // --- Audio attribute sources (the console always plays) ---
    UpdateAudio(dt, view);
}

// Draw the GUI tree. Runs inside the tiling bracket, right after the overlay:
// the geometry is clip-space, so replaying it per tile band is correct, and the
// separate-alpha blend below drives the frame's ALPHA (the emissive glow mask
// the post-resolve bloom reads) to zero underneath the menu — without it, a
// glowing object behind a pause screen would bleed through it.
void SceneRuntime::RenderGui()
{
    if (!m_device || !m_gui_shader.Valid())
        return;
    const gui::DrawList& list = m_gui.Emit();
    if (list.Empty())
        return;

    m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    m_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    m_device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO);
    m_device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    m_device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    const float halfTexel[4] = { 1.0f / 1280.0f, 1.0f / 720.0f, 0.0f, 0.0f };
    m_device->SetVertexShader(m_gui_shader.vs);
    m_device->SetPixelShader(m_gui_shader.ps);
    m_device->SetVertexShaderConstantF(0, halfTexel, 1);

    struct QuadVtx { float x, y, z, u, v; };
    std::vector<QuadVtx> vertices;

    for (size_t b = 0; b < list.batches.size(); ++b)
    {
        const gui::Batch& batch = list.batches[b];
        IDirect3DTexture9* texture = batch.textureId >= 0
            ? GuiTexture(batch.textureId) : m_def_white;
        // A textured/glyph batch with nothing resident is still streaming in —
        // skip it rather than draw it wrong. A solid quad's sample is multiplied
        // out by the shader's kind select, so it draws fine with nothing bound.
        if (!texture && batch.kind != gui::QuadSolid)
            continue;

        // Kind select, matching gui.hlsl: Solid (0,0) Texture (0,1) Glyph (1,0).
        float mode[4];
        mode[0] = batch.kind == gui::QuadGlyph   ? 1.0f : 0.0f;
        mode[1] = batch.kind == gui::QuadTexture ? 1.0f : 0.0f;
        mode[2] = 0.0f;
        mode[3] = 0.0f;

        vertices.clear();
        vertices.reserve((size_t)batch.count * 6);
        for (int i = 0; i < batch.count; ++i)
        {
            const gui::Quad& q = list.quads[batch.first + i];
            // 1280x720 reference space -> NDC, top-left origin. Same mapping the
            // Image/Text overlays use, so a GUI and an authored overlay line up.
            const float x0 = q.x0 / gui::kRefWidth * 2.0f - 1.0f;
            const float x1 = q.x1 / gui::kRefWidth * 2.0f - 1.0f;
            const float y0 = 1.0f - q.y0 / gui::kRefHeight * 2.0f;
            const float y1 = 1.0f - q.y1 / gui::kRefHeight * 2.0f;
            QuadVtx tri[6];
            tri[0].x = x0; tri[0].y = y0; tri[0].z = 0.0f; tri[0].u = q.u0; tri[0].v = q.v0;
            tri[1].x = x1; tri[1].y = y0; tri[1].z = 0.0f; tri[1].u = q.u1; tri[1].v = q.v0;
            tri[2].x = x0; tri[2].y = y1; tri[2].z = 0.0f; tri[2].u = q.u0; tri[2].v = q.v1;
            tri[3].x = x0; tri[3].y = y1; tri[3].z = 0.0f; tri[3].u = q.u0; tri[3].v = q.v1;
            tri[4].x = x1; tri[4].y = y0; tri[4].z = 0.0f; tri[4].u = q.u1; tri[4].v = q.v0;
            tri[5].x = x1; tri[5].y = y1; tri[5].z = 0.0f; tri[5].u = q.u1; tri[5].v = q.v1;
            vertices.insert(vertices.end(), tri, tri + 6);
        }
        if (vertices.empty())
            continue;

        m_device->SetPixelShaderConstantF(0, batch.rgba, 1);
        m_device->SetPixelShaderConstantF(1, mode, 1);
        m_device->SetTexture(0, texture);
        m_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)batch.count * 2,
                                  &vertices[0], sizeof(QuadVtx));
    }

    // Leave the states the overlay's own epilogue leaves them in.
    m_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    m_device->SetTexture(0, NULL);
}

// The Audio attribute reconciler: every playing attribute becomes a wanted
// stream, fed from the pak's raw AUDI entry (byte window through the player's
// own handle) or the loose file in dev runs. The listener is the resolved
// camera; spatial emitters ride the object's live physics pose.
// The item's stream key with the script override's restart generation folded
// in (audio.play on a stopped clip decodes a fresh stream from the top).
std::string SceneRuntime::AudioKeyFor(const AudioItem& item) const
{
    std::map<std::string, AudioOverride>::const_iterator ov = m_audio_overrides.find(item.object);
    if (ov == m_audio_overrides.end() || ov->second.gen == 0)
        return item.key;
    char buf[16];
    sprintf_s(buf, sizeof(buf), "#g%u", ov->second.gen);
    return item.key + buf;
}

void SceneRuntime::UpdateAudio(float dt, const D3DMATRIX& view)
{
    std::vector<aud::Want> wants;
    for (size_t i = 0; i < m_audio_items.size(); ++i)
    {
        const AudioItem& item = m_audio_items[i];
        // Script overrides win over the authored attribute state.
        std::map<std::string, AudioOverride>::const_iterator ov =
            m_audio_overrides.find(item.object);
        const bool play = (ov != m_audio_overrides.end() && ov->second.play >= 0)
                              ? ov->second.play != 0 : item.play;
        if (!play)
            continue;
        aud::Want w;
        w.key     = AudioKeyFor(item);
        w.loop    = item.loop;
        w.volume  = item.volume;
        w.pitch   = item.pitch;
        w.audioClass = item.audio_class;
        w.priority = item.priority;
        w.loadMode = item.load_mode;
        if (ov != m_audio_overrides.end())
        {
            if (ov->second.loop   >= 0)    w.loop   = ov->second.loop != 0;
            if (ov->second.volume >= 0.0f) w.volume = ov->second.volume;
            if (ov->second.pitch  >= 0.0f) w.pitch  = ov->second.pitch;
        }
        w.spatial = item.spatial;
        w.minDist = item.min_dist;
        w.maxDist = item.max_dist;
        w.doppler = item.doppler;
        if (item.object_index >= 0 && item.object_index < (int)m_scene.objects.size())
            for (int k = 0; k < 3; ++k)
                w.pos[k] = m_scene.objects[item.object_index].position[k];
        for (size_t pi = 0; pi < m_phys_poses.size(); ++pi)
            if (m_phys_poses[pi].objectIndex == item.object_index)
            {
                w.pos[0] = m_phys_poses[pi].matrix[12];
                w.pos[1] = m_phys_poses[pi].matrix[13];
                w.pos[2] = m_phys_poses[pi].matrix[14];
                break;
            }
        const SpakEntry* e = m_pak.IsOpen() ? m_pak.Find(spak::NameHash(item.path.c_str())) : NULL;
        if (e && e->type == spak::kTypeAudio && spak::CodecOf(e->flags) == spak::kCodecNone)
        {
            w.path   = m_content.Resolve("game.spak");
            w.offset = e->diskOffset;
            w.length = e->compressedSize; // raw entry: on-disk bytes ARE the .mp2
        }
        else
            w.path = m_content.Resolve(item.path);
        wants.push_back(w);
    }

    // Listener = the resolved camera: LookAtLH's basis lives in the view
    // matrix columns (col 2 = forward, col 1 = up); position is m_eye.
    aud::ListenerState listener;
    listener.pos[0] = m_eye[0]; listener.pos[1] = m_eye[1]; listener.pos[2] = m_eye[2];
    listener.fwd[0] = view._13; listener.fwd[1] = view._23; listener.fwd[2] = view._33;
    listener.up[0]  = view._12; listener.up[1]  = view._22; listener.up[2]  = view._32;

    m_audio.Update(wants.empty() ? NULL : &wants[0], (int)wants.size(),
                   dt, &listener);
}

// Copy one decoded plane into a linear L8 texture (LockRect is a plain
// pitch-strided row copy for LIN_ formats; L8 is bytes, so no endian work).
static void UploadPlaneL8(IDirect3DTexture9* tex, const unsigned char* src, int w, int h)
{
    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, NULL, 0)))
        return;
    for (int row = 0; row < h; ++row)
        memcpy((unsigned char*)lr.pBits + row * lr.Pitch, src + row * w, w);
    tex->UnlockRect(0);
}

// Create (or resize) the double-buffered L8 plane textures for a stream and
// upload the frame when its id changed. Alternating sets keeps the CPU off
// the textures the GPU may still reference from the previous frame's replay.
SceneRuntime::RtVideoTex* SceneRuntime::EnsureVideoTex(const std::string& key, const vid::Frame& frame)
{
    RtVideoTex* t = NULL;
    for (size_t i = 0; i < m_video_tex.size(); ++i)
        if (m_video_tex[i].key == key) { t = &m_video_tex[i]; break; }
    if (!t)
    {
        m_video_tex.push_back(RtVideoTex());
        t = &m_video_tex.back();
        t->key = key;
    }

    if (t->y[0] && (t->yW != frame.yW || t->yH != frame.yH))
    {
        for (int s = 0; s < 2; ++s)
        {
            if (t->y[s])  { t->y[s]->Release();  t->y[s] = NULL; }
            if (t->cb[s]) { t->cb[s]->Release(); t->cb[s] = NULL; }
            if (t->cr[s]) { t->cr[s]->Release(); t->cr[s] = NULL; }
        }
    }
    bool created = false;
    if (!t->y[0])
    {
        for (int s = 0; s < 2; ++s)
        {
            if (FAILED(m_device->CreateTexture(frame.yW, frame.yH, 1, 0, D3DFMT_LIN_L8,
                                               D3DPOOL_MANAGED, &t->y[s], NULL)) ||
                FAILED(m_device->CreateTexture(frame.cW, frame.cH, 1, 0, D3DFMT_LIN_L8,
                                               D3DPOOL_MANAGED, &t->cb[s], NULL)) ||
                FAILED(m_device->CreateTexture(frame.cW, frame.cH, 1, 0, D3DFMT_LIN_L8,
                                               D3DPOOL_MANAGED, &t->cr[s], NULL)))
                return NULL; // partial sets are released by the vanish reconcile
        }
        t->yW = frame.yW; t->yH = frame.yH;
        t->cW = frame.cW; t->cH = frame.cH;
        t->cur = 0;
        created = true;
    }

    if (created || t->lastFrame != frame.frameId)
    {
        if (!created)
            t->cur ^= 1;
        UploadPlaneL8(t->y[t->cur],  frame.y,  frame.yW, frame.yH);
        UploadPlaneL8(t->cb[t->cur], frame.cb, frame.cW, frame.cH);
        UploadPlaneL8(t->cr[t->cur], frame.cr, frame.cW, frame.cH);
        t->lastFrame = frame.frameId;
    }
    return t;
}

// The Video overlays: reconcile the shared player's streams with this frame's
// items (in-pak range when the VIDE entry exists, else the loose file), then
// draw each as an alpha-blended screen quad, higher priority first (= behind).
// The separate alpha blend drives the tile target's glow mask toward zero
// under the video, so emissive geometry behind it can't bloom through.
// The item's stream key with the script override's restart generation folded
// in — bumping the generation changes the key, so the player closes the old
// stream and decodes a fresh one from the top (that's how video.play replays
// a finished/stopped video).
std::string SceneRuntime::VideoKeyFor(const VideoItem& item) const
{
    std::map<std::string, VideoOverride>::const_iterator ov = m_video_overrides.find(item.object);
    if (ov == m_video_overrides.end() || ov->second.gen == 0)
        return item.key;
    char buf[16];
    sprintf_s(buf, sizeof(buf), "#g%u", ov->second.gen);
    return item.key + buf;
}
int SceneRuntime::VideoModeFor(const VideoItem& item) const
{
    std::map<std::string, VideoOverride>::const_iterator ov = m_video_overrides.find(item.object);
    return ov != m_video_overrides.end() ? ov->second.mode : item.play_mode;
}

void SceneRuntime::RenderOverlay(float dt)
{
    // Keys/modes go through the script override here — after this frame's
    // scripts ran — so a video.play/stop lands on the same frame's draw.
    std::vector<std::string> keys;
    std::vector<vid::Want> wants;
    for (size_t i = 0; i < m_video_items.size(); ++i)
    {
        const VideoItem& item = m_video_items[i];
        keys.push_back(VideoKeyFor(item));
        vid::Want w;
        w.key      = keys.back();
        w.playMode = VideoModeFor(item);
        w.audible  = true; // the console always plays for real
        w.volume   = item.volume;
        w.muted    = item.muted;
        const SpakEntry* e = m_pak.IsOpen() ? m_pak.Find(spak::NameHash(item.path.c_str())) : NULL;
        if (e && e->type == spak::kTypeVideo && spak::CodecOf(e->flags) == spak::kCodecNone)
        {
            w.path   = m_content.Resolve("game.spak");
            w.offset = e->diskOffset;
            w.length = e->compressedSize; // raw entry: on-disk bytes ARE the .mpg
        }
        else
            w.path = m_content.Resolve(item.path);
        wants.push_back(w);
    }
    m_video.Update(wants.empty() ? NULL : &wants[0], (int)wants.size(), dt);

    // Drop plane textures for streams that vanished.
    for (size_t i = 0; i < m_video_tex.size(); ++i)
        m_video_tex[i].used = false;
    for (size_t i = 0; i < m_video_items.size(); ++i)
        for (size_t t = 0; t < m_video_tex.size(); ++t)
            if (m_video_tex[t].key == keys[i]) { m_video_tex[t].used = true; break; }
    for (size_t i = 0; i < m_video_tex.size(); )
    {
        if (!m_video_tex[i].used)
        {
            RtVideoTex& t = m_video_tex[i];
            for (int s = 0; s < 2; ++s)
            {
                if (t.y[s])  t.y[s]->Release();
                if (t.cb[s]) t.cb[s]->Release();
                if (t.cr[s]) t.cr[s]->Release();
            }
            m_video_tex.erase(m_video_tex.begin() + i);
        }
        else
            ++i;
    }

    if (m_image_items.empty() && m_text_items.empty() && m_video_items.empty())
        return;

    struct OverlayRef
    {
        int kind; // 0=image, 1=text, 2=video
        int index;
        int priority;
        int sequence;
        OverlayRef(int itemKind, int itemIndex, int itemPriority, int itemSequence)
            : kind(itemKind), index(itemIndex), priority(itemPriority), sequence(itemSequence) {}
    };
    std::vector<OverlayRef> order;
    for (int i = 0; i < (int)m_image_items.size(); ++i)
    {
        OverlayRef ref(0, i, m_image_items[i].priority, m_image_items[i].sequence);
        int j = 0;
        while (j < (int)order.size() &&
               (order[j].priority > ref.priority ||
            (order[j].priority == ref.priority && order[j].sequence <= ref.sequence))) ++j;
        order.insert(order.begin() + j, ref);
    }
    for (int i = 0; i < (int)m_text_items.size(); ++i)
    {
        OverlayRef ref(1, i, m_text_items[i].priority, m_text_items[i].sequence);
        int j = 0;
        while (j < (int)order.size() &&
               (order[j].priority > ref.priority ||
            (order[j].priority == ref.priority && order[j].sequence <= ref.sequence))) ++j;
        order.insert(order.begin() + j, ref);
    }
    for (int i = 0; i < (int)m_video_items.size(); ++i)
    {
        OverlayRef ref(2, i, m_video_items[i].priority, m_video_items[i].sequence);
        int j = 0;
        while (j < (int)order.size() &&
               (order[j].priority > ref.priority ||
            (order[j].priority == ref.priority && order[j].sequence <= ref.sequence))) ++j;
        order.insert(order.begin() + j, ref);
    }

    struct QuadVtx { float x, y, z, u, v; };
    bool stateSet = false;
    for (size_t oi = 0; oi < order.size(); ++oi)
    {
        const OverlayRef& ref = order[oi];
        if (ref.kind == 0)
        {
            if (!m_image_shader.Valid())
                continue;
            const ImageItem& item = m_image_items[ref.index];
            IDirect3DTexture9* texture = m_cache.GetTexture(item.path);
            if (!texture)
                continue;
            D3DSURFACE_DESC desc;
            if (FAILED(texture->GetLevelDesc(0, &desc)))
                continue;

            if (!stateSet)
            {
                m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
                m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                m_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
                m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
                    D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                    D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
                m_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
                m_device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO);
                m_device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
                m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
                m_device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
                for (DWORD st = 0; st < 3; ++st)
                {
                    m_device->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                    m_device->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                    m_device->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                    m_device->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                }
                stateSet = true;
            }

            float x0 = -1.0f, y0 = 1.0f, x1 = 1.0f, y1 = -1.0f;
            if (!item.stretch)
            {
                const float width = item.w;
                const float height = item.lock_aspect && desc.Width > 0
                    ? width * (float)desc.Height / (float)desc.Width : item.h;
                x0 = item.x / 1280.0f * 2.0f - 1.0f;
                x1 = (item.x + width) / 1280.0f * 2.0f - 1.0f;
                y0 = 1.0f - item.y / 720.0f * 2.0f;
                y1 = 1.0f - (item.y + height) / 720.0f * 2.0f;
            }
            const QuadVtx quad[4] = {
                { x0, y0, 0.0f, 0.0f, 0.0f },
                { x1, y0, 0.0f, 1.0f, 0.0f },
                { x0, y1, 0.0f, 0.0f, 1.0f },
                { x1, y1, 0.0f, 1.0f, 1.0f },
            };
            const float halfTexel[4] = { 1.0f / 1280.0f, 1.0f / 720.0f, 0.0f, 0.0f };
            const float tint[4] = { item.tint[0], item.tint[1], item.tint[2], item.alpha };
            m_device->SetVertexShader(m_image_shader.vs);
            m_device->SetPixelShader(m_image_shader.ps);
            m_device->SetVertexShaderConstantF(0, halfTexel, 1);
            m_device->SetPixelShaderConstantF(0, tint, 1);
            m_device->SetTexture(0, texture);
            m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVtx));
            continue;
        }

        if (ref.kind == 1)
        {
            if (!m_text_shader.Valid())
                continue;
            const TextItem& item = m_text_items[ref.index];
            IDirect3DTexture9* atlas = NULL;
            const text::CookedFont* font = m_cache.GetFont(item.fontPath, &atlas);
            if (!font || !atlas)
                continue;
            text::Layout layout;
            const float wrapWidth = item.lockAspect ? 0.0f : item.w;
            std::map<std::string, std::string>::const_iterator overrideValue =
                m_text_overrides.find(item.object);
            const std::string& value = overrideValue != m_text_overrides.end()
                ? overrideValue->second : item.value;
            if (!text::BuildLayout(font->metrics, value, item.fontSize, wrapWidth, layout) ||
                layout.glyphs.empty())
                continue;

            if (!stateSet)
            {
                m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
                m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                m_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
                m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
                    D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                    D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
                m_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
                m_device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO);
                m_device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
                m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
                m_device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
                m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                stateSet = true;
            }

            float fitScale = 1.0f;
            if (item.lockAspect && layout.width > 0.0f && layout.height > 0.0f)
            {
                const float sx = item.w / layout.width;
                const float sy = item.h / layout.height;
                fitScale = sx < sy ? sx : sy;
            }
            const float glyphScale = item.fontSize / font->metrics.sourcePixelSize * fitScale;
            std::vector<QuadVtx> vertices;
            vertices.reserve(layout.glyphs.size() * 6);
            for (size_t glyphIndex = 0; glyphIndex < layout.glyphs.size(); ++glyphIndex)
            {
                const text::PositionedGlyph& positioned = layout.glyphs[glyphIndex];
                const text::Glyph& glyph = font->metrics.glyphs[positioned.glyphIndex];
                const float px0 = item.x + positioned.x * fitScale;
                const float py0 = item.y + positioned.y * fitScale;
                const float px1 = px0 + glyph.width * glyphScale;
                const float py1 = py0 + glyph.height * glyphScale;
                const float x0 = px0 / 1280.0f * 2.0f - 1.0f;
                const float x1 = px1 / 1280.0f * 2.0f - 1.0f;
                const float y0 = 1.0f - py0 / 720.0f * 2.0f;
                const float y1 = 1.0f - py1 / 720.0f * 2.0f;
                const QuadVtx quad[6] = {
                    {x0,y0,0,glyph.u0,glyph.v0}, {x1,y0,0,glyph.u1,glyph.v0}, {x0,y1,0,glyph.u0,glyph.v1},
                    {x0,y1,0,glyph.u0,glyph.v1}, {x1,y0,0,glyph.u1,glyph.v0}, {x1,y1,0,glyph.u1,glyph.v1}
                };
                vertices.insert(vertices.end(), quad, quad + 6);
            }
            const float halfTexel[4] = { 1.0f / 1280.0f, 1.0f / 720.0f, 0.0f, 0.0f };
            const float tint[4] = { item.color[0], item.color[1], item.color[2], item.alpha };
            m_device->SetVertexShader(m_text_shader.vs);
            m_device->SetPixelShader(m_text_shader.ps);
            m_device->SetVertexShaderConstantF(0, halfTexel, 1);
            m_device->SetPixelShaderConstantF(0, tint, 1);
            m_device->SetTexture(0, atlas);
            m_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)layout.glyphs.size() * 2,
                                      &vertices[0], sizeof(QuadVtx));
            continue;
        }

        if (!m_video_shader.Valid())
            continue;
        const int videoIndex = ref.index;
        const VideoItem& item = m_video_items[videoIndex];
        vid::Frame f;
        if (!m_video.GetFrame(keys[videoIndex], f))
            continue;
        RtVideoTex* t = EnsureVideoTex(keys[videoIndex], f);
        if (!t)
            continue;

        if (!stateSet)
        {
            m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
            m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
            m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            // RGBA writes: colour blends normally while the separate alpha
            // blend scales the glow mask by (1 - video alpha) — opaque video
            // leaves alpha 0, so the bloom chain sees nothing under it.
            m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
                D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
            m_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
            m_device->SetRenderState(D3DRS_SRCBLENDALPHA,  D3DBLEND_ZERO);
            m_device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
            m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
            m_device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
            for (DWORD st = 0; st < 3; ++st)
            {
                m_device->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                m_device->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            }
            m_device->SetVertexShader(m_video_shader.vs);
            m_device->SetPixelShader(m_video_shader.ps);
            const float halfTexel[4] = { 1.0f / 1280.0f, 1.0f / 720.0f, 0.0f, 0.0f };
            m_device->SetVertexShaderConstantF(0, halfTexel, 1);
            stateSet = true;
        }

        // Quad rect in clip space from the 1280x720 reference mapping.
        float x0 = -1.0f, y0 = 1.0f, x1 = 1.0f, y1 = -1.0f;
        if (!item.stretch)
        {
            const float w = item.w;
            const float h = (item.lock_aspect && f.width > 0)
                                ? w * (float)f.height / (float)f.width
                                : item.h;
            x0 = item.x / 1280.0f * 2.0f - 1.0f;
            x1 = (item.x + w) / 1280.0f * 2.0f - 1.0f;
            y0 = 1.0f - item.y / 720.0f * 2.0f;
            y1 = 1.0f - (item.y + h) / 720.0f * 2.0f;
        }
        // The planes are macroblock-padded; the picture is the top-left crop.
        const float uMax = (float)f.width  / (float)f.yW;
        const float vMax = (float)f.height / (float)f.yH;
        const QuadVtx quad[4] = {
            { x0, y0, 0.0f, 0.0f, 0.0f },
            { x1, y0, 0.0f, uMax, 0.0f },
            { x0, y1, 0.0f, 0.0f, vMax },
            { x1, y1, 0.0f, uMax, vMax },
        };

        const float halfTexel[4] = { 1.0f / 1280.0f, 1.0f / 720.0f, 0.0f, 0.0f };
        const float tint[4] = { item.tint[0], item.tint[1], item.tint[2], item.alpha };
        m_device->SetVertexShader(m_video_shader.vs);
        m_device->SetPixelShader(m_video_shader.ps);
        m_device->SetVertexShaderConstantF(0, halfTexel, 1);
        m_device->SetPixelShaderConstantF(0, tint, 1);
        m_device->SetTexture(0, t->y[t->cur]);
        m_device->SetTexture(1, t->cb[t->cur]);
        m_device->SetTexture(2, t->cr[t->cur]);
        m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVtx));
    }

    if (stateSet)
    {
        m_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
            D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        m_device->SetTexture(0, NULL);
        m_device->SetTexture(1, NULL);
        m_device->SetTexture(2, NULL);
    }
}

// Draw one standard-material mesh's subsets for a pass: upload its transform
// (via the constant table so packing is correct), then per material subset bind
// its textures, set its cutout states (opaque pass only), and draw its index
// range. Pass-level states (blend / depth) are set by the caller.
void SceneRuntime::DrawMesh(RtMesh* gm, const D3DMATRIX& world, const D3DMATRIX& vp, RtShader* mat,
                            bool blendPass, const std::vector<float>* skinPalette)
{
    // Skip the transform upload when nothing in this mesh belongs to the pass.
    bool any = false;
    for (size_t i = 0; i < gm->subsets.size(); ++i)
        if ((gm->subsets[i].alpha == RtBlend) == blendPass) { any = true; break; }
    if (!any)
        return;

    const D3DMATRIX wvp = Multiply(world, vp);
    const bool useSkin = gm->IsSkinned() && m_skin_mesh_decl && mat->skinVs;
    LPD3DXCONSTANTTABLE constants = useSkin ? mat->skinVsConstants : mat->vsConstants;
    if (constants)
    {
        D3DXHANDLE hWVP = useSkin ? mat->hSkinWVP : mat->hWVP;
        D3DXHANDLE hWorld = useSkin ? mat->hSkinWorld : mat->hWorld;
        if (hWVP)   constants->SetMatrix(m_device, hWVP,   (const D3DXMATRIX*)&wvp);
        if (hWorld) constants->SetMatrix(m_device, hWorld, (const D3DXMATRIX*)&world);
    }
    m_device->SetVertexDeclaration(useSkin ? m_skin_mesh_decl : m_mesh_decl);
    m_device->SetVertexShader(useSkin ? mat->skinVs : mat->vs);
    m_device->SetStreamSource(0, gm->vb, 0, 44);
    m_device->SetStreamSource(1, useSkin ? gm->skinVb : NULL, 0,
                              useSkin ? spak::kSkinInfluenceBytes : 0);
    m_device->SetIndices(gm->ib);
    if (useSkin)
    {
        float palette[spak::kMaxSkinJoints * 12];
        const float* source = NULL;
        if (skinPalette && skinPalette->size() == gm->joints.size() * 12)
            source = &(*skinPalette)[0];
        else
        {
            memset(palette, 0, sizeof(palette));
            for (size_t joint = 0; joint < gm->joints.size(); ++joint)
            {
                float* columns = palette + joint * 12;
                columns[0] = 1.0f;
                columns[5] = 1.0f;
                columns[10] = 1.0f;
            }
            source = palette;
        }
        m_device->SetVertexShaderConstantF(8, source, (UINT)gm->joints.size() * 3);
    }

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
        m_device->SetTexture(4, s.metallic ? s.metallic : m_def_black);
        m_device->SetTexture(6, s.clearcoat ? s.clearcoat : m_def_black);
        m_device->SetTexture(7, s.roughness ? s.roughness : m_def_black);
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
    m_device->SetPixelShaderConstantF(2, m_ambient, 1);
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
