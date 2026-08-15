#include "render/AnimatorPreviewRenderer.h"

#include "render/Mesh.h"
#include "render/Shader.h"
#include "state/EngineState.h"
#include "core/Log.h"

#include <assimp/matrix4x4.h>
#include <assimp/quaternion.h>
#include <assimp/vector3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// Minimal matrix helpers (left-handed, row-major to match D3D9). Duplicated
// from SceneRenderer's anonymous namespace so the preview op stays standalone.
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

    // World point -> viewport screen position through a view*proj matrix.
    // False when behind the camera or outside D3D depth [0,1].
    bool ProjectToScreen(const D3DMATRIX& vp, const float* w,
                         const ImVec2& mn, const ImVec2& mx, ImVec2& out)
    {
        const float cx = w[0]*vp._11 + w[1]*vp._21 + w[2]*vp._31 + vp._41;
        const float cy = w[0]*vp._12 + w[1]*vp._22 + w[2]*vp._32 + vp._42;
        const float cz = w[0]*vp._13 + w[1]*vp._23 + w[2]*vp._33 + vp._43;
        const float cw = w[0]*vp._14 + w[1]*vp._24 + w[2]*vp._34 + vp._44;
        if (cw <= 0.0001f)
            return false;
        const float nz = cz / cw;
        if (nz < 0.0f || nz > 1.0f)
            return false;
        out.x = mn.x + ((cx / cw) * 0.5f + 0.5f) * (mx.x - mn.x);
        out.y = mn.y + (1.0f - ((cy / cw) * 0.5f + 0.5f)) * (mx.y - mn.y);
        return true;
    }
}

// ---------------------------------------------------------------------------

void AnimatorPreviewRenderer::Initialize(IDirect3DDevice9* device)
{
    m_device = device;
    m_meshes.Init(device);

    // Vertex declarations matching MeshVertex (stream 0) and the skin influence
    // stream (stream 1) — identical to SceneRenderer's layout.
    const D3DVERTEXELEMENT9 elems[] = {
        {0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
        {0, 24, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT,  0},
        {0, 36, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    device->CreateVertexDeclaration(elems, &m_mesh_decl);
    const D3DVERTEXELEMENT9 skin_elems[] = {
        {0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
        {0, 24, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT,  0},
        {0, 36, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {1,  0, D3DDECLTYPE_UBYTE4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0},
        {1,  4, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT,  0},
        D3DDECL_END()
    };
    device->CreateVertexDeclaration(skin_elems, &m_skin_decl);

    m_def_white  = SolidTexture(D3DCOLOR_ARGB(255, 255, 255, 255));
    m_def_normal = SolidTexture(D3DCOLOR_ARGB(255, 128, 128, 255)); // flat normal
    m_def_black  = SolidTexture(D3DCOLOR_ARGB(255, 0, 0, 0));

    if (SUCCEEDED(device->CreateCubeTexture(1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                            &m_def_envcube, nullptr)))
    {
        for (int f = 0; f < 6; ++f)
        {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(m_def_envcube->LockRect((D3DCUBEMAP_FACES)f, 0, &lr, nullptr, 0)))
            {
                *static_cast<DWORD*>(lr.pBits) = D3DCOLOR_ARGB(255, 0, 0, 0);
                m_def_envcube->UnlockRect((D3DCUBEMAP_FACES)f, 0);
            }
        }
    }
}

IDirect3DTexture9* AnimatorPreviewRenderer::SolidTexture(D3DCOLOR argb)
{
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(m_device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)))
        return nullptr;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0)))
    {
        *static_cast<DWORD*>(lr.pBits) = argb;
        tex->UnlockRect(0);
    }
    return tex;
}

void AnimatorPreviewRenderer::LoadShaders(const std::filesystem::path& project_root)
{
    if (project_root.empty())
        return;
    const std::filesystem::path out = project_root / "shaders";
    IDirect3DVertexShader9* vs      = shader::LoadVS(m_device, out / "standard_vs.cso");
    IDirect3DVertexShader9* skin_vs = shader::LoadVS(m_device, out / "standard_skin_vs.cso");
    IDirect3DPixelShader9*  ps      = shader::LoadPS(m_device, out / "standard_ps.cso");
    if (vs && ps)
    {
        if (m_vs) m_vs->Release();
        if (m_ps) m_ps->Release();
        m_vs = vs;
        m_ps = ps;
        if (skin_vs)
        {
            if (m_skin_vs) m_skin_vs->Release();
            m_skin_vs = skin_vs;
            skin_vs = nullptr;
        }
    }
    else
    {
        if (vs) vs->Release();
        if (ps) ps->Release();
        applog::Warn("Animator preview: shaders not compiled — press 'Compile shaders' in Settings");
    }
    if (skin_vs) skin_vs->Release();
}

void AnimatorPreviewRenderer::OnDeviceLost()
{
    if (m_depth)     { m_depth->Release();     m_depth = nullptr; }
    if (m_rtSurface) { m_rtSurface->Release(); m_rtSurface = nullptr; }
    if (m_rt)        { m_rt->Release();        m_rt = nullptr; }
    m_width = m_height = 0;
}

void AnimatorPreviewRenderer::SetFaceWeights(const float* weights)
{
    if (weights == nullptr)
    {
        if (!m_face_weights.empty()) m_face_dirty = true;
        m_face_weights.clear();
        return;
    }
    // The Face tab pushes weights every frame; only rebuild on a change.
    if (m_face_weights.size() != (std::size_t)face::kShapeCount ||
        !std::equal(m_face_weights.begin(), m_face_weights.end(), weights))
    {
        m_face_weights.assign(weights, weights + face::kShapeCount);
        m_face_dirty = true;
    }
}

IDirect3DVertexBuffer9* AnimatorPreviewRenderer::FaceVertexBuffer(GpuMesh& mesh)
{
    if (!mesh.morph.HasMorph() || mesh.morphBase.empty() ||
        m_face_weights.size() != (std::size_t)face::kShapeCount)
        return nullptr;

    std::vector<face::MorphTarget> targets(mesh.morph.targets.size());
    for (std::size_t t = 0; t < mesh.morph.targets.size(); ++t)
    {
        const MeshMorphTarget& source = mesh.morph.targets[t];
        targets[t].deltas        = source.deltas.data();
        targets[t].deltaCount    = (unsigned)source.deltas.size();
        targets[t].positionScale = source.positionScale;
        targets[t].shape         = source.shape;
    }
    face::MorphView view;
    view.targets     = targets.data();
    view.targetCount = (unsigned)targets.size();
    view.firstVertex = mesh.morph.firstVertex;
    view.vertexCount = mesh.morph.vertexCount;

    if (!face::AnyActive(view, m_face_weights.data(), face::kShapeCount))
        return nullptr;

    if (m_face_vb_model != m_pending_model || m_face_vb_vertices != mesh.vertexCount)
    {
        if (m_face_vb) { m_face_vb->Release(); m_face_vb = nullptr; }
        const UINT bytes = (UINT)(mesh.vertexCount * sizeof(MeshVertex));
        if (FAILED(m_device->CreateVertexBuffer(bytes, 0, 0, D3DPOOL_MANAGED, &m_face_vb, nullptr)))
            return nullptr;
        void* dst = nullptr;
        m_face_vb->Lock(0, 0, &dst, 0);
        std::memcpy(dst, mesh.morphBase.data(), bytes);   // seeded whole, once
        m_face_vb->Unlock();
        m_face_vb_model = m_pending_model;
        m_face_vb_vertices = mesh.vertexCount;
        m_face_dirty = true;
    }

    if (m_face_dirty)
    {
        const std::size_t offset = (std::size_t)view.firstVertex * sizeof(MeshVertex);
        void* dst = nullptr;
        if (SUCCEEDED(m_face_vb->Lock(0, 0, &dst, 0)))
        {
            face::Deform(mesh.morphBase.data() + offset, (unsigned)sizeof(MeshVertex), view,
                         m_face_weights.data(), face::kShapeCount, (unsigned char*)dst + offset);
            m_face_vb->Unlock();
        }
        m_face_dirty = false;
    }
    return m_face_vb;
}

void AnimatorPreviewRenderer::Shutdown()
{
    OnDeviceLost();
    if (m_face_vb) { m_face_vb->Release(); m_face_vb = nullptr; }
    m_face_vb_model.clear();
    m_face_vb_vertices = 0;
    if (m_def_envcube) { m_def_envcube->Release(); m_def_envcube = nullptr; }
    if (m_def_black)   { m_def_black->Release();   m_def_black = nullptr; }
    if (m_def_normal)  { m_def_normal->Release();  m_def_normal = nullptr; }
    if (m_def_white)   { m_def_white->Release();   m_def_white = nullptr; }
    if (m_skin_decl)   { m_skin_decl->Release();   m_skin_decl = nullptr; }
    if (m_mesh_decl)   { m_mesh_decl->Release();   m_mesh_decl = nullptr; }
    if (m_ps)          { m_ps->Release();          m_ps = nullptr; }
    if (m_skin_vs)     { m_skin_vs->Release();     m_skin_vs = nullptr; }
    if (m_vs)          { m_vs->Release();          m_vs = nullptr; }
    m_meshes.Shutdown();
    m_device = nullptr;
}

bool AnimatorPreviewRenderer::EnsureTarget(int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;
    if (m_rt && width == m_width && height == m_height)
        return true;
    OnDeviceLost();
    if (FAILED(m_device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                       D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_rt, nullptr)))
        return false;
    if (FAILED(m_rt->GetSurfaceLevel(0, &m_rtSurface)) ||
        FAILED(m_device->CreateDepthStencilSurface(width, height, D3DFMT_D16,
                                                   D3DMULTISAMPLE_NONE, 0, TRUE, &m_depth, nullptr)))
    {
        OnDeviceLost();
        return false;
    }
    m_width = width;
    m_height = height;
    return true;
}

void AnimatorPreviewRenderer::SetTimeNormalized(float t)
{
    time_normalized_ = std::clamp(t, 0.0f, 1.0f);
    current_time_seconds_ = time_normalized_ * active_duration_;
}

const AnimationClip* AnimatorPreviewRenderer::GetClip(const std::string& source_model,
                                                      const std::string& clip_name)
{
    if (source_model.empty() || clip_name.empty())
        return nullptr;
    const std::string key = source_model + "#" + clip_name;
    ClipCacheEntry& entry = m_clips[key];
    if (!entry.attempted)
    {
        entry.attempted = true;
        std::string error;
        entry.valid = animation::BakeClip(m_project_root / source_model, clip_name, 30.0f,
                                          entry.clip, error);
        if (!entry.valid)
            applog::Error("Animator preview clip '" + key + "': " + error);
    }
    return entry.valid ? &entry.clip : nullptr;
}

void AnimatorPreviewRenderer::EvaluatePose(const GpuMesh& mesh, const AnimationClip* clip,
                                           float sample_time, std::vector<float>& out_palette,
                                           std::vector<float>& out_joint_world)
{
    const std::size_t joint_count = mesh.skeleton.joints.size();
    out_palette.resize(joint_count * 12);
    out_joint_world.resize(joint_count * 3);
    std::vector<aiMatrix4x4> globals(joint_count);
    pending_joint_globals_.resize(joint_count * 16);

    uint32_t frame0 = 0, frame1 = 0;
    float blend = 0.0f;
    if (clip && clip->frame_count > 0)
    {
        const float frame_position = sample_time * clip->sample_rate;
        frame0 = (std::min)((uint32_t)frame_position, clip->frame_count - 1);
        frame1 = (std::min)(frame0 + 1, clip->frame_count - 1);
        blend = frame_position - (float)frame0;
    }

    for (std::size_t joint_index = 0; joint_index < joint_count; ++joint_index)
    {
        const MeshJoint& joint = mesh.skeleton.joints[joint_index];
        aiMatrix4x4 local;
        std::memcpy(&local, joint.bindLocal, sizeof(local));
        if (clip && clip->frame_count > 0)
        {
            const uint32_t hash = animation::NameHash(joint.name.c_str());
            for (const AnimationTrack& track : clip->tracks)
            {
                if (track.name_hash != hash) continue;
                const AnimationTransform& a = track.samples[frame0];
                const AnimationTransform& b = track.samples[frame1];
                const aiVector3D translation(
                    a.translation[0] + (b.translation[0] - a.translation[0]) * blend,
                    a.translation[1] + (b.translation[1] - a.translation[1]) * blend,
                    a.translation[2] + (b.translation[2] - a.translation[2]) * blend);
                const aiVector3D scale(
                    a.scale[0] + (b.scale[0] - a.scale[0]) * blend,
                    a.scale[1] + (b.scale[1] - a.scale[1]) * blend,
                    a.scale[2] + (b.scale[2] - a.scale[2]) * blend);
                aiQuaternion qa(a.rotation[3], a.rotation[0], a.rotation[1], a.rotation[2]);
                aiQuaternion qb(b.rotation[3], b.rotation[0], b.rotation[1], b.rotation[2]);
                aiQuaternion rotation;
                aiQuaternion::Interpolate(rotation, qa, qb, blend);
                rotation.Normalize();
                local = aiMatrix4x4(scale, rotation, translation);
                break;
            }
        }
        aiMatrix4x4 global = joint.parent >= 0
            ? globals[(std::size_t)joint.parent] * local : local;
        aiMatrix4x4 descendant_global = global;
        for (const AnimatorBoneModifier& modifier : bone_modifiers_)
        {
            if (modifier.type != AnimatorBoneModifierType::Physics ||
                modifier.bone_name != joint.name || joint.parent < 0) continue;
            BoneModifierPhysicsState& simulation = physics_states_[joint.name];
            const aiMatrix4x4& parent_global = globals[(std::size_t)joint.parent];
            bone_modifiers::ApplyPreviewPhysics(modifier, pose_dt_, parent_global,
                                                simulation, global);
            if (modifier.affects_children) descendant_global = global;
            break;
        }
        globals[joint_index] = descendant_global;
        std::memcpy(pending_joint_globals_.data() + joint_index * 16,
                    &descendant_global, sizeof(descendant_global));

        // Joint origin (model space): the transform's translation component.
        const aiMatrix4x4& g = global;
        float* origin = out_joint_world.data() + joint_index * 3;
        origin[0] = g.a4; origin[1] = g.b4; origin[2] = g.c4;

        aiMatrix4x4 inverse_bind;
        std::memcpy(&inverse_bind, joint.inverseBind, sizeof(inverse_bind));
        const aiMatrix4x4 skin = g * inverse_bind;
        float* palette = out_palette.data() + joint_index * 12;
        palette[0] = skin.a1; palette[1] = skin.a2; palette[2] = skin.a3; palette[3] = skin.a4;
        palette[4] = skin.b1; palette[5] = skin.b2; palette[6] = skin.b3; palette[7] = skin.b4;
        palette[8] = skin.c1; palette[9] = skin.c2; palette[10] = skin.c3; palette[11] = skin.c4;
    }
}

void AnimatorPreviewRenderer::BeginFrame()
{
    m_renderRequested = false;
}

bool AnimatorPreviewRenderer::ComputeBoneFitBox(const std::string& bone_name,
                                                std::array<float, 3>& half_extents,
                                                std::array<float, 3>& center)
{
    if (m_pending_model.empty() || bone_name.empty()) return false;
    GpuMesh* mesh = m_meshes.Get(m_pending_model);
    if (!mesh || !mesh->skeleton.IsSkinned()) return false;

    int bone_index = -1;
    for (std::size_t index = 0; index < mesh->skeleton.joints.size(); ++index)
        if (mesh->skeleton.joints[index].name == bone_name)
        { bone_index = (int)index; break; }
    if (bone_index < 0) return false;

    std::vector<aiMatrix4x4> globals(mesh->skeleton.joints.size());
    for (std::size_t index = 0; index < mesh->skeleton.joints.size(); ++index)
    {
        aiMatrix4x4 local;
        std::memcpy(&local, mesh->skeleton.joints[index].bindLocal, sizeof(local));
        const int parent = mesh->skeleton.joints[index].parent;
        globals[index] = parent >= 0 ? globals[(std::size_t)parent] * local : local;
    }
    aiMatrix4x4 inverse_bone = globals[(std::size_t)bone_index];
    inverse_bone.Inverse();
    aiVector3D minimum(0.0f), maximum(0.0f);
    bool found_child = false;
    for (std::size_t index = 0; index < mesh->skeleton.joints.size(); ++index)
    {
        if (mesh->skeleton.joints[index].parent != bone_index) continue;
        aiVector3D point(globals[index].a4, globals[index].b4, globals[index].c4);
        point *= inverse_bone;
        minimum.x = (std::min)(minimum.x, point.x); maximum.x = (std::max)(maximum.x, point.x);
        minimum.y = (std::min)(minimum.y, point.y); maximum.y = (std::max)(maximum.y, point.y);
        minimum.z = (std::min)(minimum.z, point.z); maximum.z = (std::max)(maximum.z, point.z);
        found_child = true;
    }
    if (!found_child) return false;
    center = {(minimum.x + maximum.x) * 0.5f,
              (minimum.y + maximum.y) * 0.5f,
              (minimum.z + maximum.z) * 0.5f};
    half_extents = {(std::max)(0.01f, (maximum.x - minimum.x) * 0.5f),
                    (std::max)(0.01f, (maximum.y - minimum.y) * 0.5f),
                    (std::max)(0.01f, (maximum.z - minimum.z) * 0.5f)};
    return true;
}

void AnimatorPreviewRenderer::RenderUi(EngineState& state, const std::string& model_path,
                                       const std::string& clip_source_model,
                                       const std::string& clip_name,
                                       const ImVec2& canvas_min, const ImVec2& canvas_max)
{
    if (!m_device)
        return;

    // Pick up the active project (shaders + mesh/clip resolution). Reloading
    // shaders + clearing caches on project change mirrors SceneRenderer.
    if (m_project_root != state.project_root)
    {
        m_project_root = state.project_root;
        m_clips.clear();
        m_framed_model.clear();
        LoadShaders(m_project_root);
    }
    m_meshes.SetProjectRoot(state.project_root);

    const int w = (int)(canvas_max.x - canvas_min.x);
    const int h = (int)(canvas_max.y - canvas_min.y);
    const bool ready = EnsureTarget(w, h);

    GpuMesh* mesh = model_path.empty() ? nullptr : m_meshes.Get(model_path);
    const AnimationClip* clip = GetClip(clip_source_model, clip_name);
    active_duration_ = clip ? clip->duration_seconds : 0.0f;

    // Advance / clamp playback time over the active clip.
    const float dt = ImGui::GetIO().DeltaTime;
    if (playing_ && active_duration_ > 0.0f)
        current_time_seconds_ = std::fmod(current_time_seconds_ + dt, active_duration_);
    if (active_duration_ > 0.0f)
    {
        current_time_seconds_ = std::clamp(current_time_seconds_, 0.0f, active_duration_);
        time_normalized_ = current_time_seconds_ / active_duration_;
    }
    else
    {
        current_time_seconds_ = 0.0f;
        time_normalized_ = 0.0f;
    }

    // Auto-frame the orbit camera to the model bounds the first time we see it.
    if (mesh && m_framed_model != model_path)
    {
        m_framed_model = model_path;
        physics_states_.clear();
        const float cx = (mesh->boundsMin[0] + mesh->boundsMax[0]) * 0.5f;
        const float cy = (mesh->boundsMin[1] + mesh->boundsMax[1]) * 0.5f;
        const float cz = (mesh->boundsMin[2] + mesh->boundsMax[2]) * 0.5f;
        const float ex = mesh->boundsMax[0] - mesh->boundsMin[0];
        const float ey = mesh->boundsMax[1] - mesh->boundsMin[1];
        const float ez = mesh->boundsMax[2] - mesh->boundsMin[2];
        const float radius = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
        m_target[0] = cx; m_target[1] = cy; m_target[2] = cz;
        m_distance = std::clamp(radius * 2.5f, 0.1f, 500.0f);
    }

    // Emit the offscreen image and record it for the GPU pass.
    ImGui::SetCursorScreenPos(canvas_min);
    if (ready && m_rt)
    {
        ImGui::Image((ImTextureID)(intptr_t)m_rt, ImVec2((float)w, (float)h));
        m_renderRequested = true;

        // Orbit-camera input (must follow the Image item — uses IsItemHovered).
        ImGuiIO& io = ImGui::GetIO();
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))  m_rotating = true;
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) m_panning  = true;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))  m_rotating = false;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) m_panning  = false;
        if (m_rotating)
        {
            m_yaw   -= io.MouseDelta.x * 0.01f;
            m_pitch -= io.MouseDelta.y * 0.01f;
            m_pitch = std::clamp(m_pitch, -1.55f, 1.55f);
        }
        if (m_panning && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
        {
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
            m_distance = std::clamp(m_distance * std::pow(0.9f, io.MouseWheel), 0.05f, 1000.0f);
    }

    // Camera matrices (shared with RenderGpu, so they stay in sync).
    if (m_width > 0 && m_height > 0)
    {
        Vec3 cdir = { std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch),
                      std::cos(m_pitch) * std::cos(m_yaw) };
        Vec3 cat  = { m_target[0], m_target[1], m_target[2] };
        Vec3 ceye = { cat.x + cdir.x * m_distance, cat.y + cdir.y * m_distance,
                      cat.z + cdir.z * m_distance };
        m_eye[0] = ceye.x; m_eye[1] = ceye.y; m_eye[2] = ceye.z;
        m_view = LookAtLH(ceye, cat, {0.0f, 1.0f, 0.0f});
        m_proj = PerspectiveFovLH(3.14159265f / 4.0f,
                                  (float)m_width / (float)m_height,
                                  (std::max)(0.01f, m_distance * 0.01f), m_distance * 10.0f + 100.0f);
    }

    // Evaluate the pose: palette (mesh skinning) + joint origins (rig overlay).
    m_pending_model = model_path;
    m_pending_palette.clear();
    std::vector<float> joint_world;
    pose_dt_ = (std::clamp)(dt, 0.0f, 4.0f / 60.0f);
    if (mesh && mesh->skeleton.IsSkinned())
        EvaluatePose(*mesh, clip, current_time_seconds_, m_pending_palette, joint_world);

    // Bone overlay (2D screen space) + click-to-select the nearest joint.
    if (show_skeleton_ && mesh && mesh->skeleton.IsSkinned() && ready && m_rt)
    {
        const D3DMATRIX vp = Multiply(m_view, m_proj);
        const std::size_t joint_count = mesh->skeleton.joints.size();
        std::vector<ImVec2> screen(joint_count);
        std::vector<bool> visible(joint_count, false);
        for (std::size_t j = 0; j < joint_count; ++j)
            visible[j] = ProjectToScreen(vp, joint_world.data() + j * 3, canvas_min, canvas_max, screen[j]);

        // Selection: nearest visible joint to a left click inside the canvas.
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool inside = mouse.x >= canvas_min.x && mouse.x <= canvas_max.x &&
                            mouse.y >= canvas_min.y && mouse.y <= canvas_max.y;
        if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            int best = -1;
            float best_dist = 8.0f; // pixels
            for (std::size_t j = 0; j < joint_count; ++j)
            {
                if (!visible[j]) continue;
                const float dx = screen[j].x - mouse.x, dy = screen[j].y - mouse.y;
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d < best_dist) { best_dist = d; best = (int)j; }
            }
            selected_bone_name_ = best >= 0 ? mesh->skeleton.joints[best].name : std::string();
        }

        int selected_index = -1;
        for (std::size_t j = 0; j < joint_count && !selected_bone_name_.empty(); ++j)
            if (mesh->skeleton.joints[j].name == selected_bone_name_) { selected_index = (int)j; break; }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 bone_col = IM_COL32(150, 190, 255, 220);
        const ImU32 sel_col  = IM_COL32(255, 150, 40, 255);
        for (std::size_t j = 0; j < joint_count; ++j)
        {
            const int parent = mesh->skeleton.joints[j].parent;
            if (parent < 0 || !visible[j] || !visible[(std::size_t)parent]) continue;
            const bool hot = (int)j == selected_index || parent == selected_index;
            dl->AddLine(screen[(std::size_t)parent], screen[j], hot ? sel_col : bone_col,
                        hot ? 2.5f : 1.5f);
        }
        for (std::size_t j = 0; j < joint_count; ++j)
        {
            if (!visible[j]) continue;
            const bool hot = (int)j == selected_index;
            dl->AddCircleFilled(screen[j], hot ? 4.5f : 2.5f, hot ? sel_col : bone_col);
        }
    }

    if (mesh && mesh->skeleton.IsSkinned() && ready && m_rt &&
        pending_joint_globals_.size() == mesh->skeleton.joints.size() * 16)
    {
        const D3DMATRIX view_projection = Multiply(m_view, m_proj);
        const int edges[12][2] = {{0,1},{0,2},{0,4},{1,3},{1,5},{2,3},
                                  {2,6},{3,7},{4,5},{4,6},{5,7},{6,7}};
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        for (const AnimatorBoneModifier& modifier : bone_modifiers_)
        {
            if (modifier.type != AnimatorBoneModifierType::Collision) continue;
            std::size_t joint_index = mesh->skeleton.joints.size();
            for (std::size_t candidate = 0; candidate < mesh->skeleton.joints.size(); ++candidate)
                if (mesh->skeleton.joints[candidate].name == modifier.bone_name)
                { joint_index = candidate; break; }
            if (joint_index == mesh->skeleton.joints.size()) continue;
            aiMatrix4x4 bone_global;
            std::memcpy(&bone_global, pending_joint_globals_.data() + joint_index * 16,
                        sizeof(bone_global));
            ImVec2 projected[8];
            bool visible[8] = {};
            for (int corner = 0; corner < 8; ++corner)
            {
                aiVector3D point(
                    modifier.box_center[0] + ((corner & 1) ? modifier.box_half_extents[0] : -modifier.box_half_extents[0]),
                    modifier.box_center[1] + ((corner & 2) ? modifier.box_half_extents[1] : -modifier.box_half_extents[1]),
                    modifier.box_center[2] + ((corner & 4) ? modifier.box_half_extents[2] : -modifier.box_half_extents[2]));
                point *= bone_global;
                const float model_point[3] = {point.x, point.y, point.z};
                visible[corner] = ProjectToScreen(view_projection, model_point,
                                                   canvas_min, canvas_max, projected[corner]);
            }
            const ImU32 color = IM_COL32(60, 220, 150, 235);
            for (int edge = 0; edge < 12; ++edge)
                if (visible[edges[edge][0]] && visible[edges[edge][1]])
                    draw_list->AddLine(projected[edges[edge][0]], projected[edges[edge][1]],
                                       color, 2.0f);
        }
    }
}

void AnimatorPreviewRenderer::RenderGpu(float /*dt*/)
{
    if (!m_device || !m_renderRequested || !m_rtSurface)
        return;

    IDirect3DSurface9* prevRt    = nullptr;
    IDirect3DSurface9* prevDepth = nullptr;
    m_device->GetRenderTarget(0, &prevRt);
    m_device->GetDepthStencilSurface(&prevDepth);

    m_device->SetRenderTarget(0, m_rtSurface);
    m_device->SetDepthStencilSurface(m_depth);
    D3DVIEWPORT9 vp = { 0, 0, (DWORD)m_width, (DWORD)m_height, 0.0f, 1.0f };
    m_device->SetViewport(&vp);
    m_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                    D3DCOLOR_ARGB(255, 30, 30, 33), 1.0f, 0);

    if (SUCCEEDED(m_device->BeginScene()))
    {
        D3DMATRIX world = Identity();
        m_device->SetTransform(D3DTS_WORLD, &world);
        m_device->SetTransform(D3DTS_VIEW, &m_view);
        m_device->SetTransform(D3DTS_PROJECTION, &m_proj);

        m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
        m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

        // Skinned/static mesh via the standard material shader.
        GpuMesh* mesh = m_pending_model.empty() ? nullptr : m_meshes.Get(m_pending_model);
        if (show_mesh_ && mesh && m_vs && m_ps && m_mesh_decl)
        {
            const bool use_skin = mesh->skinVb && mesh->skeleton.IsSkinned() &&
                                  m_skin_decl && m_skin_vs;
            m_device->SetPixelShader(m_ps);
            m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
            for (DWORD s = 0; s < 5; ++s)
            {
                m_device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                m_device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            }
            m_device->SetSamplerState(5, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(5, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
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
            m_device->SetTexture(5, (IDirect3DBaseTexture9*)m_def_envcube);

            // Neutral single-directional lighting; no point/spot lights, no glow.
            const float light_dir[4] = { -0.4f, -1.0f, -0.5f, 0.0f };
            const float cam_pos[4]   = { m_eye[0], m_eye[1], m_eye[2], 0.0f };
            const float ambient[4]   = { 0.40f, 0.40f, 0.45f, 0.0f };
            const float light_col[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
            const float zero[4]      = { 0.0f, 0.0f, 0.0f, 0.0f };
            const float bump[4]      = { 0.0f, 0.0f, 0.0f, 0.0f };
            const float mask[4]      = { 0.0f, 0.0f, 0.0f, 0.0f };
            m_device->SetPixelShaderConstantF(0, light_dir, 1);
            m_device->SetPixelShaderConstantF(1, cam_pos, 1);
            m_device->SetPixelShaderConstantF(2, ambient, 1);
            m_device->SetPixelShaderConstantF(5, bump, 1);
            m_device->SetPixelShaderConstantF(6, light_col, 1);
            // c22 (roughness max env mip) is zeroed with the rest: the preview
            // binds the 1x1 black cube, which has no levels to blur into.
            for (int c = 7; c <= 22; ++c)
                m_device->SetPixelShaderConstantF(c, zero, 1);
            m_device->SetPixelShaderConstantF(15, mask, 1);

            const D3DMATRIX wvp = Multiply(world, Multiply(m_view, m_proj));
            m_device->SetVertexShaderConstantF(0, &wvp.m[0][0], 4);
            m_device->SetVertexShaderConstantF(4, &world.m[0][0], 4);

            m_device->SetVertexDeclaration(use_skin ? m_skin_decl : m_mesh_decl);
            m_device->SetVertexShader(use_skin ? m_skin_vs : m_vs);
            IDirect3DVertexBuffer9* face_vb = FaceVertexBuffer(*mesh);
            m_device->SetStreamSource(0, face_vb ? face_vb : mesh->vb, 0, sizeof(MeshVertex));
            m_device->SetStreamSource(1, use_skin ? mesh->skinVb : nullptr, 0,
                                      use_skin ? sizeof(MeshSkinInfluence) : 0);
            m_device->SetIndices(mesh->ib);
            if (use_skin)
            {
                const std::size_t need = mesh->skeleton.joints.size() * 12;
                std::vector<float> bind;
                const float* palette = nullptr;
                if (m_pending_palette.size() == need)
                    palette = m_pending_palette.data();
                else
                {
                    bind.assign(need, 0.0f);
                    for (std::size_t joint = 0; joint < mesh->skeleton.joints.size(); ++joint)
                    {
                        float* col = bind.data() + joint * 12;
                        col[0] = 1.0f; col[5] = 1.0f; col[10] = 1.0f;
                    }
                    palette = bind.data();
                }
                m_device->SetVertexShaderConstantF(8, palette,
                    (UINT)mesh->skeleton.joints.size() * 3);
            }

            for (const GpuSubset& s : mesh->subsets)
            {
                const bool cutout = s.alpha == AlphaKind::Cutout;
                m_device->SetRenderState(D3DRS_ALPHATESTENABLE, cutout ? TRUE : FALSE);
                m_device->SetRenderState(D3DRS_ALPHAREF, 170);
                m_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
                m_device->SetTexture(0, (show_texture_ && s.diffuse) ? s.diffuse : m_def_white);
                m_device->SetTexture(1, s.normal   ? s.normal   : m_def_normal);
                m_device->SetTexture(2, s.specular ? s.specular : m_def_black);
                m_device->SetTexture(3, (show_texture_ && s.emissive) ? s.emissive : m_def_black);
                m_device->SetTexture(4, s.metallic ? s.metallic : m_def_black);
                m_device->SetTexture(6, s.clearcoat ? s.clearcoat : m_def_black);
                m_device->SetTexture(7, s.roughness ? s.roughness : m_def_black);
                m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                               mesh->vertexCount, s.indexStart, s.indexCount / 3);
            }
            m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        }

        m_device->EndScene();
    }

    m_device->SetRenderTarget(0, prevRt);
    m_device->SetDepthStencilSurface(prevDepth);
    if (prevRt)    prevRt->Release();
    if (prevDepth) prevDepth->Release();
}
