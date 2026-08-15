#include "render/SceneRenderer.h"

#include <assimp/matrix4x4.h>
#include <assimp/quaternion.h>
#include <assimp/vector3.h>

#include "anim/FaceClipAsset.h"
#include "anim/FaceShapes.h"
#include "camera/EnvCubeViews.h"
#include "camera/SkyView.h"
#include "core/Log.h"
#include "light/LightmapUnwrap.h"
#include "project/ProjectIO.h"
#include "render/Shader.h"
#include "render/ShaderCompiler.h"
#include "scene/Hierarchy.h"
#include "state/EngineState.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

#include "stb/stb_image.h" // GIF decode (implementation lives in TextureLoad.cpp)

#include "imgui.h"
#include "ImGuizmo.h"

#include <windows.h> // GetModuleFileNameW
#include <Xinput.h>
#include "input/XInputPoll.h"
#include "input/ControllerMapping.h"

#include <algorithm>
#include <cfloat>
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

    void BuildAnimationPalette(const GpuMesh& mesh, const AnimationClip& clip,
                               float sample_time, std::vector<float>& output,
                               const std::vector<AnimatorBoneModifier>* modifiers = nullptr,
                               std::map<std::string, BoneModifierPhysicsState>* physics_states = nullptr,
                               float delta_time = 0.0f, const float* object_world = nullptr,
                               std::vector<SceneBoneColliderPose>* collider_poses = nullptr)
    {
        const float frame_position = sample_time * clip.sample_rate;
        const uint32_t frame0 = (std::min)((uint32_t)frame_position, clip.frame_count - 1);
        const uint32_t frame1 = (std::min)(frame0 + 1, clip.frame_count - 1);
        const float blend = frame_position - (float)frame0;
        std::vector<aiMatrix4x4> globals(mesh.skeleton.joints.size());
        output.resize(mesh.skeleton.joints.size() * 12);
        if (collider_poses) collider_poses->clear();

        for (size_t joint_index = 0; joint_index < mesh.skeleton.joints.size(); ++joint_index)
        {
            const MeshJoint& joint = mesh.skeleton.joints[joint_index];
            aiMatrix4x4 local;
            std::memcpy(&local, joint.bindLocal, sizeof(local));
            const uint32_t hash = animation::NameHash(joint.name.c_str());
            for (const AnimationTrack& track : clip.tracks)
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
            aiMatrix4x4 global = joint.parent >= 0
                ? globals[(size_t)joint.parent] * local : local;
            aiMatrix4x4 descendant_global = global;
            if (modifiers && physics_states && object_world && joint.parent >= 0)
            {
                for (const AnimatorBoneModifier& modifier : *modifiers)
                {
                    if (modifier.type != AnimatorBoneModifierType::Physics ||
                        modifier.bone_name != joint.name) continue;
                    BoneModifierPhysicsState& simulation = (*physics_states)[joint.name];
                    bone_modifiers::ApplyScenePhysics(modifier, delta_time,
                        globals[(size_t)joint.parent], object_world, simulation, global);
                    if (modifier.affects_children) descendant_global = global;
                    break;
                }
            }
            globals[joint_index] = descendant_global;
            if (modifiers && collider_poses)
                for (const AnimatorBoneModifier& modifier : *modifiers)
                {
                    if (modifier.type != AnimatorBoneModifierType::Collision ||
                        modifier.bone_name != joint.name) continue;
                    SceneBoneColliderPose pose;
                    pose.bone_name = joint.name;
                    for (int axis = 0; axis < 3; ++axis)
                        pose.half_extents[axis] = modifier.box_half_extents[axis];
                    const aiMatrix4x4& matrix = global;
                    pose.model_matrix[0] = matrix.a1; pose.model_matrix[1] = matrix.b1;
                    pose.model_matrix[2] = matrix.c1; pose.model_matrix[3] = matrix.d1;
                    pose.model_matrix[4] = matrix.a2; pose.model_matrix[5] = matrix.b2;
                    pose.model_matrix[6] = matrix.c2; pose.model_matrix[7] = matrix.d2;
                    pose.model_matrix[8] = matrix.a3; pose.model_matrix[9] = matrix.b3;
                    pose.model_matrix[10] = matrix.c3; pose.model_matrix[11] = matrix.d3;
                    pose.model_matrix[12] = matrix.a4; pose.model_matrix[13] = matrix.b4;
                    pose.model_matrix[14] = matrix.c4; pose.model_matrix[15] = matrix.d4;
                    pose.model_matrix[12] += modifier.box_center[0] * pose.model_matrix[0] +
                        modifier.box_center[1] * pose.model_matrix[4] +
                        modifier.box_center[2] * pose.model_matrix[8];
                    pose.model_matrix[13] += modifier.box_center[0] * pose.model_matrix[1] +
                        modifier.box_center[1] * pose.model_matrix[5] +
                        modifier.box_center[2] * pose.model_matrix[9];
                    pose.model_matrix[14] += modifier.box_center[0] * pose.model_matrix[2] +
                        modifier.box_center[1] * pose.model_matrix[6] +
                        modifier.box_center[2] * pose.model_matrix[10];
                    collider_poses->push_back(pose);
                    break;
                }
            aiMatrix4x4 inverse_bind;
            std::memcpy(&inverse_bind, joint.inverseBind, sizeof(inverse_bind));
            const aiMatrix4x4 skin = global * inverse_bind;
            float* palette = output.data() + joint_index * 12;
            palette[0] = skin.a1; palette[1] = skin.a2; palette[2] = skin.a3; palette[3] = skin.a4;
            palette[4] = skin.b1; palette[5] = skin.b2; palette[6] = skin.b3; palette[7] = skin.b4;
            palette[8] = skin.c1; palette[9] = skin.c2; palette[10] = skin.c3; palette[11] = skin.c4;
        }
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

    bool IntersectBounds(const Vec3& origin, const Vec3& direction,
                         const float* bounds_min, const float* bounds_max, float& distance)
    {
        float near_distance = 0.0f;
        float far_distance = 1.0e30f;
        const float ray_origin[3] = {origin.x, origin.y, origin.z};
        const float ray_direction[3] = {direction.x, direction.y, direction.z};
        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::abs(ray_direction[axis]) < 1.0e-7f)
            {
                if (ray_origin[axis] < bounds_min[axis] || ray_origin[axis] > bounds_max[axis])
                    return false;
                continue;
            }
            float first = (bounds_min[axis] - ray_origin[axis]) / ray_direction[axis];
            float second = (bounds_max[axis] - ray_origin[axis]) / ray_direction[axis];
            if (first > second)
                std::swap(first, second);
            near_distance = (std::max)(near_distance, first);
            far_distance = (std::min)(far_distance, second);
            if (near_distance > far_distance)
                return false;
        }
        distance = near_distance;
        return far_distance >= 0.0f;
    }

    bool InvertAffine(const D3DMATRIX& matrix, D3DMATRIX& inverse)
    {
        const float determinant =
            matrix._11 * (matrix._22 * matrix._33 - matrix._23 * matrix._32) -
            matrix._12 * (matrix._21 * matrix._33 - matrix._23 * matrix._31) +
            matrix._13 * (matrix._21 * matrix._32 - matrix._22 * matrix._31);
        if (std::abs(determinant) < 1.0e-8f)
            return false;
        const float inv_det = 1.0f / determinant;
        inverse = {};
        inverse._11 =  (matrix._22 * matrix._33 - matrix._23 * matrix._32) * inv_det;
        inverse._12 = -(matrix._12 * matrix._33 - matrix._13 * matrix._32) * inv_det;
        inverse._13 =  (matrix._12 * matrix._23 - matrix._13 * matrix._22) * inv_det;
        inverse._21 = -(matrix._21 * matrix._33 - matrix._23 * matrix._31) * inv_det;
        inverse._22 =  (matrix._11 * matrix._33 - matrix._13 * matrix._31) * inv_det;
        inverse._23 = -(matrix._11 * matrix._23 - matrix._13 * matrix._21) * inv_det;
        inverse._31 =  (matrix._21 * matrix._32 - matrix._22 * matrix._31) * inv_det;
        inverse._32 = -(matrix._11 * matrix._32 - matrix._12 * matrix._31) * inv_det;
        inverse._33 =  (matrix._11 * matrix._22 - matrix._12 * matrix._21) * inv_det;
        inverse._41 = -(matrix._41 * inverse._11 + matrix._42 * inverse._21 + matrix._43 * inverse._31);
        inverse._42 = -(matrix._41 * inverse._12 + matrix._42 * inverse._22 + matrix._43 * inverse._32);
        inverse._43 = -(matrix._41 * inverse._13 + matrix._42 * inverse._23 + matrix._43 * inverse._33);
        inverse._44 = 1.0f;
        return true;
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

    D3DMATRIX OrthoOffCenterLH(float left, float right, float bottom, float top, float zn, float zf)
    {
        D3DMATRIX m = {};
        m._11 = 2.0f / (right - left);
        m._22 = 2.0f / (top - bottom);
        m._33 = 1.0f / (zf - zn);
        m._41 = (left + right) / (left - right);
        m._42 = (top + bottom) / (bottom - top);
        m._43 = zn / (zn - zf);
        m._44 = 1.0f;
        return m;
    }

    void UploadShadowMatrix(IDirect3DDevice9* device, const D3DMATRIX& matrix)
    {
        const float columns[4][4] = {
            {matrix._11, matrix._21, matrix._31, matrix._41},
            {matrix._12, matrix._22, matrix._32, matrix._42},
            {matrix._13, matrix._23, matrix._33, matrix._43},
            {matrix._14, matrix._24, matrix._34, matrix._44}
        };
        device->SetVertexShaderConstantF(225, &columns[0][0], 4);
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

    // World point -> viewport screen position through a view*proj matrix
    // (row-vector). False when behind the camera or outside D3D depth [0,1].
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
    const D3DVERTEXELEMENT9 skin_elems[] = {
        {0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
        {0, 24, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT,  0},
        {0, 36, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {1,  0, D3DDECLTYPE_UBYTE4,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0},
        {1,  4, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT,  0},
        D3DDECL_END()
    };
    device->CreateVertexDeclaration(skin_elems, &m_skin_mesh_decl);
    const D3DVERTEXELEMENT9 lightmap_elems[] = {
        {0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
        {0, 24, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0},
        {0, 36, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 44, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END()
    };
    device->CreateVertexDeclaration(lightmap_elems, &m_lightmap_mesh_decl);

    // Fallback textures for meshes missing a map.
    m_def_white  = CreateSolidTexture(device, D3DCOLOR_ARGB(255, 255, 255, 255)); // diffuse
    m_def_normal = CreateSolidTexture(device, D3DCOLOR_ARGB(255, 128, 128, 255)); // flat normal (0,0,1)
    m_def_black  = CreateSolidTexture(device, D3DCOLOR_ARGB(255, 0, 0, 0));       // no specular / metal

    // Black 1x1 cube: the env-map fallback (metal reflects nothing).
    if (SUCCEEDED(device->CreateCubeTexture(1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m_def_envcube, nullptr)))
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

// Load shaders from the project's shaders/ folder. Missing optional built-ins
// fall back to the engine's shipped HLSL in memory so older projects gain new
// renderer features without requiring an immediate manual shader rebuild.
void SceneRenderer::LoadStandardShader()
{
    if (m_project_root.empty())
        return;
    const std::filesystem::path out = m_project_root / "shaders";
    IDirect3DVertexShader9* vs = shader::LoadVS(m_device, out / "standard_vs.cso");
    IDirect3DVertexShader9* skin_vs = shader::LoadVS(m_device, out / "standard_skin_vs.cso");
    IDirect3DVertexShader9* lightmap_vs = shader::LoadVS(m_device, out / "standard_lightmap_vs.cso");
        IDirect3DPixelShader9* shadow_ps = shader::LoadPS(m_device, out / "standard_shadow_ps.cso");
    IDirect3DPixelShader9*  ps = shader::LoadPS(m_device, out / "standard_ps.cso");
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
        if (lightmap_vs)
        {
            if (m_lightmap_vs) m_lightmap_vs->Release();
            m_lightmap_vs = lightmap_vs;
            lightmap_vs = nullptr;
        }
        if (shadow_ps)
        {
            if (m_shadow_ps) m_shadow_ps->Release();
            m_shadow_ps = shadow_ps;
            shadow_ps = nullptr;
        }
    }
    else
    {
        if (vs) vs->Release();
        if (ps) ps->Release();
        applog::Warn("Shaders not compiled yet — press 'Compile shaders' in Settings");
    }
    if (skin_vs) skin_vs->Release();
    if (lightmap_vs) lightmap_vs->Release();
        if (shadow_ps) shadow_ps->Release();
    if (m_vs && !m_skin_vs)
        applog::Warn("Skin shader not compiled — rigged models use bind-pose fallback until 'Compile shaders'");

    // The bloom post chain (emissive glow). Optional: missing .cso just turns
    // the glow off (older projects until their shaders are recompiled).
    auto reload = [&](const char* stem, IDirect3DVertexShader9*& ovs, IDirect3DPixelShader9*& ops)
    {
        IDirect3DVertexShader9* nvs = shader::LoadVS(m_device, out / (std::string(stem) + "_vs.cso"));
        IDirect3DPixelShader9*  nps = shader::LoadPS(m_device, out / (std::string(stem) + "_ps.cso"));
        if (!nvs || !nps)
        {
            if (nvs) { nvs->Release(); nvs = nullptr; }
            if (nps) { nps->Release(); nps = nullptr; }
            const std::filesystem::path source = m_standard_src.parent_path() /
                (std::string(stem) + ".hlsl");
            nvs = shader::CompileVSFile(m_device, source, "VSMain");
            nps = shader::CompilePSFile(m_device, source, "PSMain");
        }
        if (nvs && nps)
        {
            if (ovs) ovs->Release();
            if (ops) ops->Release();
            ovs = nvs;
            ops = nps;
        }
        else
        {
            if (nvs) nvs->Release();
            if (nps) nps->Release();
        }
    };
    reload("bloom_bright",  m_bloom_bright_vs,  m_bloom_bright_ps);
    reload("bloom_blur",    m_bloom_blur_vs,    m_bloom_blur_ps);
    reload("bloom_combine", m_bloom_combine_vs, m_bloom_combine_ps);
    reload("env_blur", m_env_blur_vs, m_env_blur_ps); // roughness reflection chain
    reload("skybox", m_sky_vs, m_sky_ps); // Skybox attribute background (optional)
    reload("beam", m_beam_vs, m_beam_ps); // spot volumetric shaft (optional)
    reload("image", m_image_vs, m_image_ps); // Image attribute overlay (optional)
    reload("color", m_color_vs, m_color_ps); // Color attribute block (optional)
    reload("text", m_text_vs, m_text_ps); // Text attribute overlay (optional)
    reload("video", m_video_vs, m_video_ps); // Video attribute overlay (optional)
    reload("gui", m_gui_vs, m_gui_ps); // Lua-scriptable GUI (optional)
    if (m_vs && (!m_bloom_bright_ps || !m_bloom_blur_ps || !m_bloom_combine_ps))
        applog::Warn("Bloom shaders not compiled — emissive glow off until 'Compile shaders'");
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
    m_audio.Shutdown();
    m_video.Shutdown();
    for (VideoTex& t : m_video_tex)
    {
        if (t.y)  t.y->Release();
        if (t.cb) t.cb->Release();
        if (t.cr) t.cr->Release();
    }
    m_video_tex.clear();
    for (auto& image : m_image_textures)
        if (image.second) image.second->Release();
    m_image_textures.clear();
    for (auto& gif : m_gif_frames)
        for (IDirect3DTexture9* frame : gif.second.frames)
            if (frame) frame->Release();
    m_gif_frames.clear();
    m_gif_play.clear();
    for (auto& font : m_text_fonts)
        if (font.second.atlas) font.second.atlas->Release();
    m_text_fonts.clear();
    if (m_image_ps)   { m_image_ps->Release();   m_image_ps = nullptr; }
    if (m_image_vs)   { m_image_vs->Release();   m_image_vs = nullptr; }
    if (m_color_ps)   { m_color_ps->Release();   m_color_ps = nullptr; }
    if (m_color_vs)   { m_color_vs->Release();   m_color_vs = nullptr; }
    if (m_text_ps)    { m_text_ps->Release();    m_text_ps = nullptr; }
    if (m_text_vs)    { m_text_vs->Release();    m_text_vs = nullptr; }
    if (m_video_ps)   { m_video_ps->Release();   m_video_ps = nullptr; }
    if (m_video_vs)   { m_video_vs->Release();   m_video_vs = nullptr; }
    // GUI: the textures themselves are owned by m_image_textures / m_gif_frames
    // / m_text_fonts above, so only the id tables and the solid-fill white are
    // ours to drop.
    m_gui_textures.clear();
    m_gui_texture_ids.clear();
    m_gui_font_atlas_ids.clear();
    if (m_gui_white)  { m_gui_white->Release();  m_gui_white = nullptr; }
    if (m_gui_ps)     { m_gui_ps->Release();     m_gui_ps = nullptr; }
    if (m_gui_vs)     { m_gui_vs->Release();     m_gui_vs = nullptr; }
    if (m_cube_ib)    { m_cube_ib->Release();    m_cube_ib = nullptr; }
    if (m_cube_vb)    { m_cube_vb->Release();    m_cube_vb = nullptr; }
    if (m_quad_ib)    { m_quad_ib->Release();    m_quad_ib = nullptr; }
    if (m_quad_vb)    { m_quad_vb->Release();    m_quad_vb = nullptr; }
    if (m_env)         { m_env->Release();         m_env = nullptr; }
    if (m_def_envcube) { m_def_envcube->Release(); m_def_envcube = nullptr; }
    if (m_def_black)  { m_def_black->Release();  m_def_black = nullptr; }
    if (m_def_normal) { m_def_normal->Release(); m_def_normal = nullptr; }
    if (m_def_white)  { m_def_white->Release();  m_def_white = nullptr; }
    if (m_skin_mesh_decl) { m_skin_mesh_decl->Release(); m_skin_mesh_decl = nullptr; }
    if (m_lightmap_mesh_decl) { m_lightmap_mesh_decl->Release(); m_lightmap_mesh_decl = nullptr; }
    if (m_mesh_decl)  { m_mesh_decl->Release();  m_mesh_decl = nullptr; }
    if (m_bloom_combine_ps) { m_bloom_combine_ps->Release(); m_bloom_combine_ps = nullptr; }
    if (m_bloom_combine_vs) { m_bloom_combine_vs->Release(); m_bloom_combine_vs = nullptr; }
    if (m_bloom_blur_ps)    { m_bloom_blur_ps->Release();    m_bloom_blur_ps = nullptr; }
    if (m_bloom_blur_vs)    { m_bloom_blur_vs->Release();    m_bloom_blur_vs = nullptr; }
    if (m_bloom_bright_ps)  { m_bloom_bright_ps->Release();  m_bloom_bright_ps = nullptr; }
    if (m_bloom_bright_vs)  { m_bloom_bright_vs->Release();  m_bloom_bright_vs = nullptr; }
    if (m_env_blur_ps)      { m_env_blur_ps->Release();      m_env_blur_ps = nullptr; }
    if (m_env_blur_vs)      { m_env_blur_vs->Release();      m_env_blur_vs = nullptr; }
    if (m_sky_ps)     { m_sky_ps->Release();     m_sky_ps = nullptr; }
    if (m_sky_vs)     { m_sky_vs->Release();     m_sky_vs = nullptr; }
    if (m_sky_tex)    { m_sky_tex->Release();    m_sky_tex = nullptr; m_sky_loaded.clear(); }
    if (m_beam_ps)    { m_beam_ps->Release();    m_beam_ps = nullptr; }
    if (m_beam_vs)    { m_beam_vs->Release();    m_beam_vs = nullptr; }
    if (m_ps)         { m_ps->Release();         m_ps = nullptr; }
    if (m_skin_vs)    { m_skin_vs->Release();    m_skin_vs = nullptr; }
    if (m_lightmap_vs) { m_lightmap_vs->Release(); m_lightmap_vs = nullptr; }
        if (m_shadow_ps) { m_shadow_ps->Release(); m_shadow_ps = nullptr; }
    if (m_vs)         { m_vs->Release();         m_vs = nullptr; }
    m_shaders.Shutdown();
    ClearLightmaps();
    ReleaseMorphInstances();
    m_meshes.Shutdown();
    OnDeviceLost();
    m_device = nullptr;
}

void SceneRenderer::BindMeshForDraw(GpuMesh* mesh, const std::vector<float>* live_palette,
                                    IDirect3DVertexBuffer9* morph_vb)
{
    const bool use_skin = mesh && mesh->skinVb && mesh->skeleton.IsSkinned() &&
                          m_skin_mesh_decl && m_skin_vs;
    m_device->SetVertexDeclaration(use_skin ? m_skin_mesh_decl : m_mesh_decl);
    m_device->SetVertexShader(use_skin ? m_skin_vs : m_vs);
    m_device->SetStreamSource(0, morph_vb ? morph_vb : mesh->vb, 0, sizeof(MeshVertex));
    m_device->SetStreamSource(1, use_skin ? mesh->skinVb : nullptr, 0,
                              use_skin ? sizeof(MeshSkinInfluence) : 0);
    m_device->SetIndices(mesh->ib);

    if (use_skin)
    {
        // Three float4 columns per row-vector affine matrix. Identity keeps the
        // baked vertices exactly in bind pose until the controller sampler
        // supplies a live palette on Play.
        float bind_palette[MAX_SKIN_JOINTS * 12] = {};
        const float* palette = nullptr;
        if (live_palette && live_palette->size() == mesh->skeleton.joints.size() * 12)
            palette = live_palette->data();
        else
        {
            for (size_t joint = 0; joint < mesh->skeleton.joints.size(); ++joint)
            {
                float* columns = bind_palette + joint * 12;
                columns[0] = 1.0f;
                columns[5] = 1.0f;
                columns[10] = 1.0f;
            }
            palette = bind_palette;
        }
        m_device->SetVertexShaderConstantF(8, palette,
            (UINT)mesh->skeleton.joints.size() * 3);
    }
}

IDirect3DVertexBuffer9* SceneRenderer::UpdateMorph(int object_index, const std::string& model_path,
                                                   GpuMesh& mesh, const std::vector<float>& weights)
{
    face::MorphView view;
    std::vector<face::MorphTarget> targets;
    if (!mesh.morph.HasMorph() || mesh.morphBase.empty() || weights.size() != face::kShapeCount)
        return nullptr;

    targets.resize(mesh.morph.targets.size());
    for (size_t t = 0; t < mesh.morph.targets.size(); ++t)
    {
        const MeshMorphTarget& source = mesh.morph.targets[t];
        targets[t].deltas        = source.deltas.data();
        targets[t].deltaCount    = (unsigned)source.deltas.size();
        targets[t].positionScale = source.positionScale;
        targets[t].shape         = source.shape;
    }
    view.targets     = targets.data();
    view.targetCount = (unsigned)targets.size();
    view.firstVertex = mesh.morph.firstVertex;
    view.vertexCount = mesh.morph.vertexCount;

    if (!face::AnyActive(view, weights.data(), face::kShapeCount))
        return nullptr;

    MorphInstance& instance = m_morph_instances[object_index];
    if (instance.model_path != model_path || instance.vertexCount != mesh.vertexCount)
    {
        instance.Release();
        const UINT bytes = (UINT)(mesh.vertexCount * sizeof(MeshVertex));
        for (int i = 0; i < 2; ++i)
        {
            if (FAILED(m_device->CreateVertexBuffer(bytes, 0, 0, D3DPOOL_MANAGED,
                                                    &instance.vb[i], nullptr)))
            {
                instance.Release();
                return nullptr;
            }
            // Seeded whole; only the morph region is refreshed per frame.
            void* dst = nullptr;
            instance.vb[i]->Lock(0, 0, &dst, 0);
            std::memcpy(dst, mesh.morphBase.data(), bytes);
            instance.vb[i]->Unlock();
        }
        instance.model_path  = model_path;
        instance.vertexCount = mesh.vertexCount;
    }

    instance.write ^= 1;
    IDirect3DVertexBuffer9* target = instance.vb[instance.write];
    const std::size_t offset = (std::size_t)view.firstVertex * sizeof(MeshVertex);
    // Lock the whole buffer and index in here: whether a partial lock returns a
    // region-relative or base pointer is platform behaviour (see the console).
    void* dst = nullptr;
    if (FAILED(target->Lock(0, 0, &dst, 0)))
        return nullptr;
    face::Deform(mesh.morphBase.data() + offset, (unsigned)sizeof(MeshVertex), view,
                 weights.data(), face::kShapeCount, (unsigned char*)dst + offset);
    target->Unlock();
    return target;
}

void SceneRenderer::ReleaseMorphInstances()
{
    for (auto& entry : m_morph_instances)
        entry.second.Release();
    m_morph_instances.clear();
}

void SceneRenderer::ClearLightmaps()
{
    for (auto& entry : m_lightmap_meshes)
    {
        if (entry.second.ib) entry.second.ib->Release();
        if (entry.second.vb) entry.second.vb->Release();
    }
    m_lightmap_meshes.clear();
    m_probe_grid.Clear();
    if (m_lightmap0) { m_lightmap0->Release(); m_lightmap0 = nullptr; }
    if (m_lightmap1) { m_lightmap1->Release(); m_lightmap1 = nullptr; }
}

void SceneRenderer::EnsureLightmaps(const SceneFile& scene)
{
    std::filesystem::path metadata = scene.path;
    metadata.replace_extension(".lmap");
    std::error_code ec;
    const bool exists = std::filesystem::exists(metadata, ec);
    const std::filesystem::file_time_type timestamp = exists
        ? std::filesystem::last_write_time(metadata, ec) : std::filesystem::file_time_type{};
    if (m_lightmap_scene == scene.path && m_lightmap_timestamp == timestamp)
        return;

    ClearLightmaps();
    m_lightmap_scene = scene.path;
    m_lightmap_timestamp = timestamp;
    if (!exists || ec || !m_lightmap_mesh_decl || !m_lightmap_vs)
        return;

    std::ifstream input(metadata, std::ios::binary);
    char magic[4] = {};
    uint32_t version = 0, atlasSize = 0, directionSize = 0, instanceCount = 0, probeCount = 0;
    float probeSpacing = 0.0f, probeOrigin[3] = {};
    input.read(magic, sizeof(magic));
    input.read(reinterpret_cast<char*>(&version), sizeof(version));
    input.read(reinterpret_cast<char*>(&atlasSize), sizeof(atlasSize));
    input.read(reinterpret_cast<char*>(&directionSize), sizeof(directionSize));
    input.read(reinterpret_cast<char*>(&instanceCount), sizeof(instanceCount));
    input.read(reinterpret_cast<char*>(&probeCount), sizeof(probeCount));
    input.read(reinterpret_cast<char*>(&probeSpacing), sizeof(probeSpacing));
    input.read(reinterpret_cast<char*>(probeOrigin), sizeof(probeOrigin));
    if (!input || std::memcmp(magic, "LMP0", 4) != 0 || version != 3 ||
        atlasSize == 0 || directionSize == 0 || instanceCount > scene.objects.size())
    {
        applog::Warn("Ignoring invalid lightmap metadata: " + metadata.string());
        return;
    }

    struct Binding { uint32_t objectIndex; float scaleOffset[4]; };
    std::vector<Binding> bindings(instanceCount);
    for (Binding& binding : bindings)
    {
        input.read(reinterpret_cast<char*>(&binding.objectIndex), sizeof(binding.objectIndex));
        input.read(reinterpret_cast<char*>(binding.scaleOffset), sizeof(binding.scaleOffset));
        if (!input || binding.objectIndex >= scene.objects.size())
        {
            applog::Warn("Ignoring stale lightmap object bindings: " + metadata.string());
            return;
        }
    }

    const size_t probeOffset = 40 + static_cast<size_t>(instanceCount) * 20;
    struct DiskProbe { float position[3]; float sh[12]; };
    std::vector<DiskProbe> probes(probeCount);
    if (probeCount)
    {
        input.read(reinterpret_cast<char*>(probes.data()),
                   static_cast<std::streamsize>(probes.size() * sizeof(DiskProbe)));
        if (!input || probeSpacing <= 0.0f)
        {
            applog::Warn("Ignoring invalid light probe grid: " + metadata.string());
            return;
        }
        (void)probeOffset;
        m_probe_grid.spacing = probeSpacing;
        for (int axis = 0; axis < 3; ++axis) m_probe_grid.origin[axis] = probeOrigin[axis];
        for (const DiskProbe& probe : probes)
            for (int axis = 0; axis < 3; ++axis)
            {
                const float coordinate = (probe.position[axis] - probeOrigin[axis]) / probeSpacing;
                const unsigned int dimension = static_cast<unsigned int>(std::floor(coordinate + 0.5f)) + 1;
                m_probe_grid.dimensions[axis] = (std::max)(m_probe_grid.dimensions[axis], dimension);
            }
        if (static_cast<size_t>(m_probe_grid.dimensions[0]) * m_probe_grid.dimensions[1] *
            m_probe_grid.dimensions[2] != probes.size())
        {
            m_probe_grid.Clear();
            applog::Warn("Ignoring irregular light probe grid: " + metadata.string());
            return;
        }
        m_probe_grid.probes.resize(probes.size());
        for (size_t index = 0; index < probes.size(); ++index)
            std::memcpy(m_probe_grid.probes[index].sh, probes[index].sh, sizeof(probes[index].sh));
    }

    std::filesystem::path colorPath = scene.path;
    colorPath.replace_extension();
    std::filesystem::path directionPath = colorPath;
    colorPath += "_lm0.png";
    directionPath += "_lm1.png";
    IDirect3DTexture9* color = mesh::LoadTexture(m_device, colorPath, nullptr, nullptr);
    IDirect3DTexture9* direction = mesh::LoadTexture(m_device, directionPath, nullptr, nullptr);
    if (!color || !direction)
    {
        if (color) color->Release();
        if (direction) direction->Release();
        applog::Warn("Ignoring lightmap with missing textures: " + metadata.string());
        return;
    }

    std::map<int, LightmapGpuMesh> loaded;
    bool valid = true;
    for (const Binding& binding : bindings)
    {
        const SceneObject& object = scene.objects[binding.objectIndex];
        std::string modelPath;
        for (const ObjectAttribute& attribute : object.attributes)
            if (attribute.type == "3D Model" && !attribute.model_path.empty())
            { modelPath = attribute.model_path; break; }
        if (modelPath.empty()) { valid = false; break; }

        std::filesystem::path meshPath = m_project_root / modelPath;
        if (meshPath.extension() != ".mesh") meshPath.replace_extension(".mesh");
        std::ifstream meshInput(meshPath, std::ios::binary);
        MeshHeader header = {};
        meshInput.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!meshInput || std::memcmp(header.magic, "M360", 4) != 0 ||
            header.version != MESH_VERSION || (header.flags & MESH_FLAG_SKINNED) != 0)
        { valid = false; break; }
        std::vector<MeshVertex> sourceVertices(header.vertexCount);
        meshInput.read(reinterpret_cast<char*>(sourceVertices.data()),
                       static_cast<std::streamsize>(sourceVertices.size() * sizeof(MeshVertex)));
        if (!meshInput) { valid = false; break; }

        std::filesystem::path uvPath = meshPath;
        uvPath.replace_extension(".lmuv");
        lightmap::UvMesh uv;
        std::string uvError;
        if (!lightmap::ReadUvSidecar(uvPath, uv, uvError) || uv.sourceVertexCount != header.vertexCount)
        { valid = false; break; }

        std::vector<LightmapVertex> vertices(uv.vertices.size());
        for (size_t index = 0; index < uv.vertices.size(); ++index)
        {
            const lightmap::UvVertex& source = uv.vertices[index];
            vertices[index].base = sourceVertices[source.sourceVertex];
            vertices[index].u1 = source.u * binding.scaleOffset[0] + binding.scaleOffset[2];
            vertices[index].v1 = source.v * binding.scaleOffset[1] + binding.scaleOffset[3];
        }

        LightmapGpuMesh gpu;
        const UINT vertexBytes = static_cast<UINT>(vertices.size() * sizeof(LightmapVertex));
        const UINT indexBytes = static_cast<UINT>(uv.indices.size() * sizeof(uint32_t));
        if (FAILED(m_device->CreateVertexBuffer(vertexBytes, 0, 0, D3DPOOL_MANAGED, &gpu.vb, nullptr)) ||
            FAILED(m_device->CreateIndexBuffer(indexBytes, 0, D3DFMT_INDEX32, D3DPOOL_MANAGED, &gpu.ib, nullptr)))
        { if (gpu.vb) gpu.vb->Release(); if (gpu.ib) gpu.ib->Release(); valid = false; break; }
        void* destination = nullptr;
        if (FAILED(gpu.vb->Lock(0, vertexBytes, &destination, 0)))
        { gpu.vb->Release(); gpu.ib->Release(); valid = false; break; }
        std::memcpy(destination, vertices.data(), vertexBytes);
        gpu.vb->Unlock();
        if (FAILED(gpu.ib->Lock(0, indexBytes, &destination, 0)))
        { gpu.vb->Release(); gpu.ib->Release(); valid = false; break; }
        std::memcpy(destination, uv.indices.data(), indexBytes);
        gpu.ib->Unlock();
        gpu.vertexCount = static_cast<UINT>(vertices.size());
        loaded[static_cast<int>(binding.objectIndex)] = gpu;
    }

    if (!valid)
    {
        for (auto& entry : loaded)
        { if (entry.second.ib) entry.second.ib->Release(); if (entry.second.vb) entry.second.vb->Release(); }
        color->Release();
        direction->Release();
        applog::Warn("Ignoring stale lightmap geometry: " + metadata.string());
        return;
    }
    m_lightmap_meshes.swap(loaded);
    m_lightmap0 = color;
    m_lightmap1 = direction;
    applog::Info("Loaded directional lightmap: " + metadata.filename().string());
}

void SceneRenderer::SetFacePreviewWeights(int object_index, const float* weights)
{
    if (weights == nullptr)
    {
        m_face_preview.erase(object_index);
        return;
    }
    m_face_preview[object_index].assign(weights, weights + face::kShapeCount);
}


const AnimatorController* SceneRenderer::GetController(const std::string& controller_path)
{
    if (controller_path.empty())
        return nullptr;
    ControllerCacheEntry& entry = m_animator_controllers[controller_path];
    if (!entry.attempted)
    {
        entry.attempted = true;
        std::string error;
        entry.valid = animator::LoadController(m_project_root / controller_path,
                                               entry.controller, error);
        if (!entry.valid)
            applog::Error("Animator '" + controller_path + "': " + error);
    }
    return entry.valid ? &entry.controller : nullptr;
}

const face::Pose* SceneRenderer::FindFacePose(int object_index, const std::string& pose_name)
{
    std::string controller_path;
    for (const DrawItem& item : m_draw_items)
        if (item.object_index == object_index) { controller_path = item.animator_controller_path; break; }
    if (controller_path.empty() || pose_name.empty())
        return nullptr;

    const std::string key = controller_path + "#" + pose_name;
    const auto cached = m_face_poses.find(key);
    if (cached != m_face_poses.end())
        return &cached->second;

    const AnimatorController* controller = GetController(controller_path);
    if (!controller)
        return nullptr;

    // A pose that drives nothing is still a real pose: an authored "Neutral"
    // means everything at zero. Only an unknown name is a miss.
    const FaceExpressionPose* authored = nullptr;
    for (const FaceExpressionPose& candidate : controller->face.poses)
        if (candidate.name == pose_name) { authored = &candidate; break; }
    if (!authored)
        return nullptr;

    face::Pose& pose = m_face_poses[key];
    pose.nameHash = animation::NameHash(pose_name.c_str());
    for (const auto& [target, weight] : authored->weights)
    {
        const int shape = face::ShapeIndex(target.c_str());
        if (shape == face::kShapeNone) continue;
        const float clamped = std::clamp(weight, 0.0f, 1.0f);
        pose.targets.push_back({(unsigned char)shape,
                                (unsigned char)std::lround(clamped * 255.0f)});
    }
    return &pose;
}


bool SceneRenderer::FaceSetPose(int objectIndex, const char* pose, float weight, float speed)
{
    if (objectIndex < 0) return false;
    FaceInstance& instance = m_face_instances[objectIndex];

    std::string name = pose ? pose : "";
    if (name.empty())
    {
        // Ease back to the controller's own default, which is what clearing a
        // pose means.
        for (const DrawItem& item : m_draw_items)
            if (item.object_index == objectIndex)
            {
                if (const AnimatorController* c = GetController(item.animator_controller_path))
                    name = c->face.default_pose;
                break;
            }
    }
    const face::Pose* resolved = name.empty() ? nullptr : FindFacePose(objectIndex, name);
    if (pose && pose[0] && !resolved)
        return false;
    instance.layer.SetPose(resolved, weight, speed);
    instance.started = true;
    return true;
}


const std::vector<unsigned char>* SceneRenderer::GetFaceClip(const std::string& relative_path)
{
    // Re-cook when the file changes on disk: a clip is edited far more often
    // than the engine is restarted, and a stale cache looks like the edit
    // silently did nothing.
    std::error_code time_error;
    const auto written = std::filesystem::last_write_time(m_project_root / relative_path, time_error);
    const auto cached = m_face_clips.find(relative_path);
    if (cached != m_face_clips.end())
    {
        const auto stamp = m_face_clip_times.find(relative_path);
        if (time_error || (stamp != m_face_clip_times.end() && stamp->second == written))
            return cached->second.empty() ? nullptr : &cached->second;
    }
    if (!time_error)
        m_face_clip_times[relative_path] = written;

    std::vector<unsigned char>& payload = m_face_clips[relative_path];
    payload.clear();
    FaceClipAsset asset;
    std::string error;
    if (!faceclip::Load(m_project_root / relative_path, asset, error) ||
        !faceclip::CookBE(asset, payload, error))
    {
        applog::Error("Face clip '" + relative_path + "': " + error);
        payload.clear();
        return nullptr;
    }
    return &payload;
}

std::string SceneRenderer::FaceAudioKey(int object_index, unsigned int generation) const
{
    return "face#" + std::to_string(object_index) + "#" + std::to_string(generation);
}

bool SceneRenderer::FacePlayClip(int objectIndex, const char* clip, bool loop)
{
    if (objectIndex < 0 || !clip || !clip[0]) return false;

    // A script names a clip by its stem; the controller holds the paths.
    std::string path;
    for (const DrawItem& item : m_draw_items)
    {
        if (item.object_index != objectIndex) continue;
        if (const AnimatorController* c = GetController(item.animator_controller_path))
            for (const std::string& candidate : c->face.clips)
                if (std::filesystem::path(candidate).stem().string() == clip ||
                    candidate == clip)
                { path = candidate; break; }
        break;
    }
    if (path.empty()) path = clip;

    const std::vector<unsigned char>* payload = GetFaceClip(path);
    if (!payload) return false;

    FaceInstance& instance = m_face_instances[objectIndex];
    instance.clip_bytes = *payload;   // the layer views these, so own them here
    face::ClipView view;
    if (!face::ParseClip(instance.clip_bytes.data(), (unsigned)instance.clip_bytes.size(), view))
        return false;
    instance.layer.PlayClip(view, loop);
    instance.started = true;
    instance.audio_path.assign(view.audioPath ? view.audioPath : "", view.audioPathBytes);
    ++instance.audio_gen;
    return true;
}

void SceneRenderer::FaceStopClip(int objectIndex)
{
    const auto instance = m_face_instances.find(objectIndex);
    if (instance == m_face_instances.end()) return;
    instance->second.layer.StopClip();
    instance->second.audio_path.clear();
}

bool SceneRenderer::FaceClipPlaying(int objectIndex)
{
    const auto instance = m_face_instances.find(objectIndex);
    return instance != m_face_instances.end() && instance->second.layer.ClipPlaying();
}

bool SceneRenderer::FaceLookAt(int objectIndex, float x, float y, float z)
{
    const DrawItem* item = nullptr;
    for (const DrawItem& candidate : m_draw_items)
        if (candidate.object_index == objectIndex) { item = &candidate; break; }
    if (!item) return false;

    // Into the character's own space: a rigid world matrix inverts by transpose.
    const D3DMATRIX& world = item->world;
    const float local[3] = {x - world._41, y - world._42, z - world._43};
    float dir[3] = {
        local[0] * world._11 + local[1] * world._12 + local[2] * world._13,
        local[0] * world._21 + local[1] * world._22 + local[2] * world._23,
        local[0] * world._31 + local[1] * world._32 + local[2] * world._33};
    const float length = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (length <= 0.0001f)
        return false;
    for (float& axis : dir) axis /= length;

    // Eyes rotate about 35 degrees before a head would follow; treat that as a
    // full look. +X is the character's right, and the layer takes yaw positive
    // toward its LEFT.
    const auto axisWeight = [](float component) {
        return std::clamp(component / 0.6f, -1.0f, 1.0f);
    };
    m_face_instances[objectIndex].layer.SetGaze(axisWeight(-dir[0]), axisWeight(dir[1]));
    m_face_instances[objectIndex].started = true;
    return true;
}

void SceneRenderer::FaceClearGaze(int objectIndex)
{
    const auto instance = m_face_instances.find(objectIndex);
    if (instance != m_face_instances.end()) instance->second.layer.ClearGaze();
}

void SceneRenderer::FaceSetBlink(int objectIndex, bool enabled)
{
    if (objectIndex < 0) return;
    m_face_instances[objectIndex].layer.SetBlinkEnabled(enabled);
    m_face_instances[objectIndex].started = true;
}

void SceneRenderer::UpdateFaceLayers(float dt)
{
    // Faces run on Play only; in edit mode the Face tab's override drives them.
    if (!m_phys_on)
    {
        m_face_instances.clear();
        return;
    }

    for (DrawItem& item : m_draw_items)
    {
        if (item.object_index < 0)
            continue;
        GpuMesh* mesh = m_meshes.Get(item.model_path);
        if (!mesh || !mesh->morph.HasMorph())
        {
            item.face_weights.clear();
            continue;
        }
        FaceInstance& instance = m_face_instances[item.object_index];
        if (!instance.started)
        {
            instance.layer.SetSeed(animation::NameHash(item.model_path.c_str()) ^
                                   (unsigned)(item.object_index * 2654435761u));
            if (const AnimatorController* c = GetController(item.animator_controller_path))
                if (!c->face.default_pose.empty())
                    instance.layer.SetPose(FindFacePose(item.object_index, c->face.default_pose),
                                           1.0f, 0.0f);
            instance.started = true;
        }

        float audio_seconds = -1.0f;
        if (instance.layer.ClipPlaying() && !instance.audio_path.empty())
            audio_seconds = m_audio.PlaybackSeconds(
                FaceAudioKey(item.object_index, instance.audio_gen));

        instance.layer.Update(dt, audio_seconds);
        if (!instance.layer.ClipPlaying())
            instance.audio_path.clear();
        item.face_weights.assign(instance.layer.Weights(),
                                 instance.layer.Weights() + face::kShapeCount);
    }
}

void SceneRenderer::UpdateFaces()
{
    std::set<int> live;   // objects that stopped morphing give their clones back

    for (DrawItem& item : m_draw_items)
    {
        item.morph_vb = nullptr;
        if (item.object_index < 0)
            continue;

        // The panel's preview override wins over the running face layer.
        const std::vector<float>* weights = &item.face_weights;
        const auto preview = m_face_preview.find(item.object_index);
        if (preview != m_face_preview.end())
            weights = &preview->second;
        if (weights->size() != face::kShapeCount)
            continue;

        GpuMesh* mesh = m_meshes.Get(item.model_path);
        if (!mesh || !mesh->morph.HasMorph())
            continue;

        item.morph_vb = UpdateMorph(item.object_index, item.model_path, *mesh, *weights);
        if (item.morph_vb)
            live.insert(item.object_index);
    }

    for (auto entry = m_morph_instances.begin(); entry != m_morph_instances.end();)
    {
        if (live.count(entry->first))
        {
            ++entry;
            continue;
        }
        entry->second.Release();
        entry = m_morph_instances.erase(entry);
    }
}

void SceneRenderer::UpdateAnimations(float dt)
{
    if (!m_phys_on)
    {
        m_animator_instances.clear();
        return;
    }

    for (DrawItem& item : m_draw_items)
    {
        item.skin_palette.clear();
        if (item.animator_controller_path.empty() || !item.animator_auto_play)
            continue;
        GpuMesh* mesh = m_meshes.Get(item.model_path);
        if (!mesh || !mesh->skeleton.IsSkinned())
            continue;

        ControllerCacheEntry& controller_entry = m_animator_controllers[item.animator_controller_path];
        if (!controller_entry.attempted)
        {
            controller_entry.attempted = true;
            std::string error;
            controller_entry.valid = animator::LoadController(
                m_project_root / item.animator_controller_path, controller_entry.controller, error);
            if (!controller_entry.valid)
                applog::Error("Animator '" + item.animator_controller_path + "': " + error);
        }
        if (!controller_entry.valid)
            continue;
        const AnimatorController& controller = controller_entry.controller;

        AnimatorInstance& instance = m_animator_instances[item.object_index];
        if (instance.controller_path != item.animator_controller_path)
        {
            instance.controller_path = item.animator_controller_path;
            instance.physics_states.clear();
            animator::InitializeRuntime(controller, item.animator_initial_state, instance.runtime);
        }
        animator::AdvanceRuntime(controller, dt, item.animator_playback_speed,
                                 item.animator_auto_play, instance.runtime);

        const AnimatorStateDefinition* state = nullptr;
        for (const AnimatorStateDefinition& candidate : controller.states)
            if (candidate.name == instance.runtime.active_state) { state = &candidate; break; }
        if (!state)
            continue;
        const AnimatorClipReference* clip_reference = nullptr;
        for (const AnimatorClipReference& candidate : controller.clips)
            if (candidate.id == state->clip_id) { clip_reference = &candidate; break; }
        if (!clip_reference)
            continue;

        const std::string clip_key = clip_reference->source_model_path + "#" + clip_reference->clip_name;
        ClipCacheEntry& clip_entry = m_animation_clips[clip_key];
        if (!clip_entry.attempted)
        {
            clip_entry.attempted = true;
            std::string error;
            clip_entry.valid = animation::BakeClip(m_project_root / clip_reference->source_model_path,
                clip_reference->clip_name, 30.0f, clip_entry.clip, error);
            if (!clip_entry.valid)
                applog::Error("Animation '" + clip_key + "': " + error);
        }
        if (!clip_entry.valid)
            continue;
        const AnimationClip& clip = clip_entry.clip;

        float sample_time = instance.runtime.state_time;
        if (state->loop)
            sample_time = std::fmod(sample_time, clip.duration_seconds);
        else
            sample_time = (std::min)(sample_time, clip.duration_seconds);

        BuildAnimationPalette(*mesh, clip, sample_time, item.skin_palette,
            &controller.bone_modifiers, &instance.physics_states,
            (std::clamp)(dt, 0.0f, 4.0f / 60.0f), &item.world.m[0][0],
            &instance.bone_collider_poses);

        if (instance.bone_collider_handles.size() != instance.bone_collider_poses.size())
        {
            instance.bone_collider_handles.clear();
            for (const SceneBoneColliderPose& pose : instance.bone_collider_poses)
            {
                float scaled_extents[3] = {
                    pose.half_extents[0] * std::fabs(item.scale[0]),
                    pose.half_extents[1] * std::fabs(item.scale[1]),
                    pose.half_extents[2] * std::fabs(item.scale[2])};
                instance.bone_collider_handles.push_back(m_phys.AddBoneCollider(
                    item.object_index, animation::NameHash(pose.bone_name.c_str()),
                    scaled_extents));
            }
        }
        for (size_t collider_index = 0;
             collider_index < instance.bone_collider_poses.size(); ++collider_index)
        {
            D3DMATRIX bone_model;
            std::memcpy(&bone_model.m[0][0],
                instance.bone_collider_poses[collider_index].model_matrix, sizeof(bone_model.m));
            const D3DMATRIX collider_world = Multiply(bone_model, item.world);
            m_phys.UpdateBoneCollider(instance.bone_collider_handles[collider_index],
                                      &collider_world.m[0][0]);
        }

        if (!instance.runtime.previous_state.empty() && instance.runtime.blend_duration > 0.0f)
        {
            const AnimatorStateDefinition* previous_state = nullptr;
            for (const AnimatorStateDefinition& candidate : controller.states)
                if (candidate.name == instance.runtime.previous_state)
                { previous_state = &candidate; break; }
            const AnimatorClipReference* previous_reference = nullptr;
            if (previous_state)
                for (const AnimatorClipReference& candidate : controller.clips)
                    if (candidate.id == previous_state->clip_id)
                    { previous_reference = &candidate; break; }
            if (previous_state && previous_reference)
            {
                const std::string previous_key = previous_reference->source_model_path + "#" +
                                                 previous_reference->clip_name;
                ClipCacheEntry& previous_entry = m_animation_clips[previous_key];
                if (!previous_entry.attempted)
                {
                    previous_entry.attempted = true;
                    std::string error;
                    previous_entry.valid = animation::BakeClip(
                        m_project_root / previous_reference->source_model_path,
                        previous_reference->clip_name, 30.0f, previous_entry.clip, error);
                    if (!previous_entry.valid)
                        applog::Error("Animation '" + previous_key + "': " + error);
                }
                if (previous_entry.valid)
                {
                    float previous_time = instance.runtime.previous_state_time;
                    if (previous_state->loop)
                        previous_time = std::fmod(previous_time, previous_entry.clip.duration_seconds);
                    else
                        previous_time = (std::min)(previous_time, previous_entry.clip.duration_seconds);
                    std::vector<float> previous_palette;
                    BuildAnimationPalette(*mesh, previous_entry.clip, previous_time, previous_palette);
                    const float alpha = 1.0f - (std::clamp)(
                        instance.runtime.blend_remaining / instance.runtime.blend_duration,
                        0.0f, 1.0f);
                    for (size_t value = 0; value < item.skin_palette.size(); ++value)
                        item.skin_palette[value] = previous_palette[value] +
                            (item.skin_palette[value] - previous_palette[value]) * alpha;
                }
            }
        }
    }
}

void SceneRenderer::InvalidateAnimator(const std::string& controller_path)
{
    std::set<std::string> clip_keys;
    std::map<std::string, ControllerCacheEntry>::iterator cached =
        m_animator_controllers.find(controller_path);
    if (cached != m_animator_controllers.end() && cached->second.valid)
        for (const AnimatorClipReference& clip : cached->second.controller.clips)
            clip_keys.insert(clip.source_model_path + "#" + clip.clip_name);

    AnimatorController saved;
    std::string error;
    if (animator::LoadController(m_project_root / controller_path, saved, error))
        for (const AnimatorClipReference& clip : saved.clips)
            clip_keys.insert(clip.source_model_path + "#" + clip.clip_name);

    m_animator_controllers.erase(controller_path);
    for (const std::string& key : clip_keys) m_animation_clips.erase(key);
    for (std::map<int, AnimatorInstance>::iterator instance = m_animator_instances.begin();
         instance != m_animator_instances.end();)
    {
        if (instance->second.controller_path == controller_path)
            instance = m_animator_instances.erase(instance);
        else
            ++instance;
    }
}

void SceneRenderer::AnimatorSetFloat(int objectIndex, const char* name, float value)
{
    auto instance = m_animator_instances.find(objectIndex);
    if (instance != m_animator_instances.end()) animator::SetFloat(instance->second.runtime, name, value);
}

void SceneRenderer::AnimatorSetBool(int objectIndex, const char* name, bool value)
{
    auto instance = m_animator_instances.find(objectIndex);
    if (instance != m_animator_instances.end()) animator::SetBool(instance->second.runtime, name, value);
}

void SceneRenderer::AnimatorSetTrigger(int objectIndex, const char* name)
{
    auto instance = m_animator_instances.find(objectIndex);
    if (instance != m_animator_instances.end()) animator::SetTrigger(instance->second.runtime, name);
}

void SceneRenderer::AnimatorSetState(int objectIndex, const char* name)
{
    auto instance = m_animator_instances.find(objectIndex);
    if (instance == m_animator_instances.end()) return;
    auto controller = m_animator_controllers.find(instance->second.controller_path);
    if (controller == m_animator_controllers.end() || !controller->second.valid) return;
    for (const AnimatorStateDefinition& state : controller->second.controller.states)
        if (state.name == name)
        {
            instance->second.runtime.active_state = name;
            instance->second.runtime.previous_state.clear();
            instance->second.runtime.state_time = 0.0f;
            instance->second.runtime.previous_state_time = 0.0f;
            instance->second.runtime.blend_duration = 0.0f;
            instance->second.runtime.blend_remaining = 0.0f;
            return;
        }
}

void SceneRenderer::OnDeviceLost()
{
    if (m_shadow_depth)   { m_shadow_depth->Release();   m_shadow_depth = nullptr; }
    if (m_shadow_surface) { m_shadow_surface->Release(); m_shadow_surface = nullptr; }
    if (m_shadow_texture) { m_shadow_texture->Release(); m_shadow_texture = nullptr; }
    if (m_env_dyn_depth) { m_env_dyn_depth->Release(); m_env_dyn_depth = nullptr; }
    if (m_env_blur_tmp)  { m_env_blur_tmp->Release();  m_env_blur_tmp = nullptr; }
    if (m_env_blur)      { m_env_blur->Release();      m_env_blur = nullptr; }
    if (m_env_dyn)       { m_env_dyn->Release();       m_env_dyn = nullptr; }
    if (m_bloomBSurf) { m_bloomBSurf->Release(); m_bloomBSurf = nullptr; }
    if (m_bloomB)     { m_bloomB->Release();     m_bloomB = nullptr; }
    if (m_bloomASurf) { m_bloomASurf->Release(); m_bloomASurf = nullptr; }
    if (m_bloomA)     { m_bloomA->Release();     m_bloomA = nullptr; }
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
        // Project environment cube map (metal reflections); optional.
        if (m_env) { m_env->Release(); m_env = nullptr; }
        m_env = mesh::LoadEnvCube(m_device, m_project_root / "assets" / "env");
        // The sky is cached against a project-relative path, which means a
        // different project's identically-named sky would keep the old image.
        if (m_sky_tex) { m_sky_tex->Release(); m_sky_tex = nullptr; }
        m_sky_loaded.clear();
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
    // and the scene lights (first directional plus per-object point/spot selection).
    bool  have_cam = false;
    float cam_pos[3] = {}, cam_rot[3] = {};
    float cam_fov = 45.0f, cam_near = 0.5f, cam_far = 100.0f;
    const ObjectAttribute* cam_attr = nullptr; // the active camera (Follow/Track params)
    int   cam_object = -1;

    // Changing the selected object drops any track-point selection/preview.
    if (state.selected_object != m_last_selected_object)
    {
        m_last_selected_object = state.selected_object;
        state.selected_track_point_index = -1;
        m_track_preview = false;
    }
    bool  have_dir = false;
    bool  have_authored_direct = false;
    bool  have_env = false;
    m_shadow_enabled = false;
    m_point_lights.clear();
    m_spot_lights.clear();
    std::memset(m_light_col, 0, sizeof(m_light_col));
    std::memset(m_point_pos, 0, sizeof(m_point_pos));
    std::memset(m_point_col, 0, sizeof(m_point_col));
    std::memset(m_spot_pos, 0, sizeof(m_spot_pos));
    std::memset(m_spot_dir, 0, sizeof(m_spot_dir));
    std::memset(m_spot_col, 0, sizeof(m_spot_col));
    // Unused spot slots keep a valid cone pair (inner cos 1, outer cos 0) so the
    // shader's smoothstep edges never collapse; zero color mutes them anyway.
    m_spot_dir[0][2] = m_spot_dir[1][2] = 1.0f;
    m_spot_dir[0][3] = m_spot_dir[1][3] = 1.0f;
    m_ambient[0] = m_ambient[1] = m_ambient[2] = 0.0f;
    m_baked_ambient[0] = m_baked_ambient[1] = m_baked_ambient[2] = 0.0f;
    m_draw_items.clear();
    m_pick_items.clear();
    m_shader_items.clear();
    m_spot_gizmos.clear();
    m_spot_beams.clear();
    m_image_items.clear();
    m_color_items.clear();
    m_text_items.clear();
    m_video_items.clear();
    m_audio_items.clear();
    m_sky_path.clear();
    m_sky_rotation = 0.0f;
    m_phys_debug.clear(); // rebuilt from the scene each frame (authoring wireframes)
    int overlay_sequence = 0;
    std::vector<hier::Resolved> resolved_objects;
    std::vector<int> object_parents;
    if (SceneFile* scene = state.SelectedScene())
    {
        EnsureLightmaps(*scene);
        std::vector<SceneObject> frame_objects(scene->objects.size());
        std::vector<std::string> object_names(scene->objects.size());
        std::vector<std::string> parent_names(scene->objects.size());
        std::vector<hier::Node> hierarchy_nodes(scene->objects.size());
        for (int i = 0; i < (int)scene->objects.size(); ++i)
        {
            SceneObject overlaid;
            frame_objects[i] = ApplyLive(i, scene->objects[i], overlaid) ? overlaid : scene->objects[i];
            object_names[i] = frame_objects[i].name;
            parent_names[i] = frame_objects[i].parent;
        }
        hier::MapParents(object_names, parent_names, object_parents);
        for (int i = 0; i < (int)frame_objects.size(); ++i)
        {
            const SceneObject& object = frame_objects[i];
            hier::Node& node = hierarchy_nodes[i];
            for (int k = 0; k < 3; ++k)
            {
                node.pos[k] = object.position[k];
                node.rot[k] = object.rotation[k];
                node.scale[k] = object.scale[k];
            }
            node.visible = object.visible;
            node.parent = object_parents[i];
        }
        hier::Resolve(hierarchy_nodes, resolved_objects);
        std::vector<std::array<float, 2>> image_origins(scene->objects.size(), {0.0f, 0.0f});
        std::vector<bool> image_origin_done(scene->objects.size(), false);
        for (int pass = 0; pass < (int)frame_objects.size(); ++pass)
            for (int i = 0; i < (int)frame_objects.size(); ++i)
            {
                if (image_origin_done[i] || (object_parents[i] >= 0 && !image_origin_done[object_parents[i]])) continue;
                if (object_parents[i] >= 0) image_origins[i] = image_origins[object_parents[i]];
                for (const ObjectAttribute& attribute : frame_objects[i].attributes)
                    if (attribute.type == "Image")
                    {
                        image_origins[i][0] += attribute.image_x;
                        image_origins[i][1] += attribute.image_y;
                        break;
                    }
                image_origin_done[i] = true;
            }
        if (m_phys_on && !m_phys_poses.empty())
        {
            std::vector<bool> resolved_visibility(resolved_objects.size());
            for (size_t i = 0; i < resolved_objects.size(); ++i)
                resolved_visibility[i] = resolved_objects[i].visible;
            for (size_t poseIndex = 0; poseIndex < m_phys_poses.size(); ++poseIndex)
            {
                const phys::Pose& pose = m_phys_poses[poseIndex];
                if (pose.objectIndex < 0 || pose.objectIndex >= (int)hierarchy_nodes.size()) continue;
                hier::Node& node = hierarchy_nodes[pose.objectIndex];
                float ignoredScale[3];
                hier::Decompose(pose.matrix, node.pos, node.rot, ignoredScale);
                for (int axis = 0; axis < 3; ++axis)
                    node.scale[axis] = resolved_objects[pose.objectIndex].scale[axis];
                node.parent = -1;
            }
            hier::Resolve(hierarchy_nodes, resolved_objects);
            for (size_t i = 0; i < resolved_objects.size(); ++i)
                resolved_objects[i].visible = resolved_visibility[i];
        }

        for (int i = 0; i < (int)scene->objects.size(); ++i)
        {
            // Draw the object as a script left it, without touching the project:
            // the authored SceneObject is used directly unless a script wrote to
            // it this Play session, in which case we render an overlaid copy.
            SceneObject resolved_object = frame_objects[i];
            for (int k = 0; k < 3; ++k)
            {
                resolved_object.position[k] = resolved_objects[i].pos[k];
                resolved_object.rotation[k] = resolved_objects[i].rot[k];
                resolved_object.scale[k] = resolved_objects[i].scale[k];
            }
            const SceneObject& o = resolved_object;
            if (!resolved_objects[i].visible)
                continue;
            const bool selected = (i == state.selected_object);
            D3DMATRIX resolved_world;
            std::memcpy(&resolved_world.m[0][0], resolved_objects[i].world, sizeof(resolved_objects[i].world));

            std::string model_path, shader_path, animator_path, animator_state;
            float animator_speed = 1.0f;
            bool animator_auto_play = true;
            bool dynamic_lighting = false;
            bool cast_shadow = false;
            for (const ObjectAttribute& a : o.attributes)
            {
                if (a.type == "3D Model" && !a.model_path.empty() && model_path.empty())
                {
                    model_path = a.model_path;
                    cast_shadow = a.cast_shadow;
                }
                else if (a.type == "Shader" && !a.shader_path.empty() && shader_path.empty())
                    shader_path = a.shader_path;
                else if (a.type == "Animator")
                {
                    dynamic_lighting = true;
                    if (!a.animator_controller_path.empty() && animator_path.empty())
                    {
                        animator_path = a.animator_controller_path;
                        animator_state = a.animator_initial_state;
                        animator_speed = a.animator_playback_speed;
                        animator_auto_play = a.animator_auto_play;
                    }
                }
                else if (a.type == "Rigid Body" && a.phys_kind != 0)
                {
                    dynamic_lighting = true;
                }
                else if (a.type == "Camera" && a.cam_active && !have_cam)
                {
                    for (int k = 0; k < 3; ++k) { cam_pos[k] = o.position[k]; cam_rot[k] = o.rotation[k]; }
                    cam_fov = a.cam_fov; cam_near = a.cam_near; cam_far = a.cam_far;
                    cam_attr = &a;
                    cam_object = i;
                    have_cam = true;
                }
                else if (a.type == "Directional Light" && !have_dir)
                {
                    have_authored_direct = true;
                    have_dir = true;
                    m_shadow_enabled = true;
                    // Direction = the object's forward (+Z through its rotation).
                    const float d2r = 3.14159265f / 180.0f;
                    const D3DMATRIX r = Multiply(Multiply(RotationX(o.rotation[0] * d2r),
                                                          RotationY(o.rotation[1] * d2r)),
                                                 RotationZ(o.rotation[2] * d2r));
                    m_light_dir[0] = r._31; m_light_dir[1] = r._32; m_light_dir[2] = r._33;
                    if (lsel::IsRealtime(a.light_mode))
                        for (int k = 0; k < 3; ++k) m_light_col[k] = a.light_color[k] * a.light_intensity;
                }
                else if (a.type == "Point Light")
                {
                    have_authored_direct = true;
                    if (!lsel::IsRealtime(a.light_mode))
                        continue;
                    const float range = (std::max)(a.light_range, 0.1f);
                    lsel::PointLight light = {};
                    for (int k = 0; k < 3; ++k)
                    {
                        light.position[k] = o.position[k];
                        light.color[k] = a.light_color[k] * a.light_intensity;
                    }
                    light.invRangeSq = 1.0f / (range * range);
                    m_point_lights.push_back(light);
                }
                else if (a.type == "Spot Light")
                {
                    have_authored_direct = true;
                    // Position + beam direction (+Z forward) from the transform.
                    const float d2r = 3.14159265f / 180.0f;
                    const D3DMATRIX r = Multiply(Multiply(RotationX(o.rotation[0] * d2r),
                                                          RotationY(o.rotation[1] * d2r)),
                                                 RotationZ(o.rotation[2] * d2r));
                    const float range = (std::max)(a.light_range, 0.1f);
                    // Keep the smoothstep edges apart: outer strictly wider.
                    const float inner = (std::min)((std::max)(a.light_inner_deg, 0.1f), 89.0f);
                    const float outer = (std::max)(a.light_outer_deg, inner + 0.1f);
                    if (lsel::IsRealtime(a.light_mode))
                    {
                        lsel::SpotLight light = {};
                        light.position[0] = o.position[0];
                        light.position[1] = o.position[1];
                        light.position[2] = o.position[2];
                        light.direction[0] = r._31;
                        light.direction[1] = r._32;
                        light.direction[2] = r._33;
                        light.invRange = 1.0f / range;
                        light.innerCos = std::cos(inner * d2r);
                        light.outerCos = std::cos(outer * d2r);
                        for (int k = 0; k < 3; ++k)
                            light.color[k] = a.light_color[k] * a.light_intensity;
                        m_spot_lights.push_back(light);
                    }
                    if (selected)
                    {
                        SpotGizmo sg;
                        sg.base  = Multiply(r, Translation(o.position[0], o.position[1], o.position[2]));
                        sg.range = range;
                        sg.inner_deg = inner;
                        sg.outer_deg = outer;
                        m_spot_gizmos.push_back(sg);
                    }
                    if (a.light_volumetric && a.light_volumetric_intensity > 0.0f)
                    {
                        // Beam brightness: additive and double-sided, so a small
                        // gain reads as haze rather than a solid (tune by eye;
                        // keep in sync with SceneRuntime.cpp).
                        const float kBeamGain = 0.10f;
                        const float tano = std::tan(outer * d2r);
                        const float rr = tano * range;
                        SpotBeam b;
                        b.world = Multiply(Multiply(Scaling(rr, rr, range), r),
                                           Translation(o.position[0], o.position[1], o.position[2]));
                        for (int k = 0; k < 3; ++k)
                            b.color[k] = a.light_color[k] * a.light_intensity *
                                         a.light_volumetric_intensity * kBeamGain;
                        b.color[3] = 0.0f;
                        b.apex[0] = o.position[0]; b.apex[1] = o.position[1];
                        b.apex[2] = o.position[2]; b.apex[3] = 0.0f;
                        b.axis[0] = r._31; b.axis[1] = r._32; b.axis[2] = r._33;
                        b.axis[3] = tano;
                        m_spot_beams.push_back(b);
                    }
                }
                else if (a.type == "Environment Light")
                {
                    have_env = true;
                    if (lsel::IsRealtime(a.light_mode))
                        for (int k = 0; k < 3; ++k)
                            m_ambient[k] += a.light_color[k] * a.light_intensity;
                    if (a.light_mode != lsel::Realtime)
                        for (int k = 0; k < 3; ++k)
                            m_baked_ambient[k] += a.light_color[k] * a.light_intensity;
                }
                else if (a.type == "Skybox" && !a.sky_path.empty() && m_sky_path.empty())
                {
                    // First one wins, like the directional light — two skies
                    // have no meaningful blend.
                    m_sky_path     = a.sky_path;
                    m_sky_rotation = a.sky_rotation;
                }
                else if (a.type == "Image" && !a.image_path.empty())
                {
                    ImageItem image;
                    image.path_abs = (state.project_root / a.image_path).string();
                    image.x = a.image_x; image.y = a.image_y;
                    if (object_parents[i] >= 0)
                    {
                        image.x += image_origins[object_parents[i]][0];
                        image.y += image_origins[object_parents[i]][1];
                    }
                    image.w = a.image_w; image.h = a.image_h;
                    image.stretch = a.image_stretch;
                    image.lock_aspect = a.image_lock_aspect;
                    for (int k = 0; k < 3; ++k) image.tint[k] = a.image_tint[k];
                    image.alpha = a.image_alpha;
                    image.priority = a.image_priority;
                    image.play_mode = a.image_play_mode;
                    image.key = o.name + "#" + std::to_string((int)(&a - &o.attributes[0]));
                    image.sequence = overlay_sequence++;
                    m_image_items.push_back(image);
                }
                // A fully transparent block is dropped here rather than blended
                // away — it costs a full-screen fill otherwise, and a script
                // raising alpha again puts it back next frame.
                else if (a.type == "Color" && a.color_alpha > 0.0f)
                {
                    ColorItem block;
                    block.x = a.color_x; block.y = a.color_y;
                    block.w = a.color_w; block.h = a.color_h;
                    block.stretch = a.color_stretch;
                    for (int k = 0; k < 3; ++k) block.rgb[k] = a.color_rgb[k];
                    block.alpha = a.color_alpha;
                    block.priority = a.color_priority;
                    block.sequence = overlay_sequence++;
                    m_color_items.push_back(block);
                }
                else if (a.type == "Text" && !a.text_font_path.empty() && !a.text_value.empty())
                {
                    TextItem text_item;
                    text_item.object = o.name;
                    text_item.font_path_abs = (state.project_root / a.text_font_path).string();
                    text_item.value = a.text_value;
                    text_item.x = a.text_x; text_item.y = a.text_y;
                    text_item.w = a.text_w; text_item.h = a.text_h;
                    text_item.font_size = a.text_font_size;
                    for (int k = 0; k < 3; ++k) text_item.color[k] = a.text_color[k];
                    text_item.alpha = a.text_alpha;
                    text_item.lock_aspect = a.text_lock_aspect;
                    text_item.priority = a.text_priority;
                    text_item.sequence = overlay_sequence++;
                    m_text_items.push_back(text_item);
                }
                else if (a.type == "Video" && !a.video_path.empty())
                {
                    VideoItem vi;
                    const int attr_index = (int)(&a - &o.attributes[0]);
                    vi.key      = o.name + "#" + std::to_string(attr_index);
                    vi.object   = o.name;
                    vi.path_abs = (state.project_root / a.video_path).string();
                    vi.x = a.video_x; vi.y = a.video_y;
                    vi.w = a.video_w; vi.h = a.video_h;
                    vi.stretch     = a.video_stretch;
                    vi.lock_aspect = a.video_lock_aspect;
                    for (int k = 0; k < 3; ++k) vi.tint[k] = a.video_tint[k];
                    vi.alpha     = a.video_alpha;
                    vi.priority  = a.video_priority;
                    vi.sequence  = overlay_sequence++;
                    vi.play_mode = a.video_play_mode;
                    vi.volume    = a.video_volume;
                    vi.muted     = a.video_muted;
                    m_video_items.push_back(vi);
                }
                else if (a.type == "Audio" && !a.audio_path.empty())
                {
                    AudioItem ai;
                    const int attr_index = (int)(&a - &o.attributes[0]);
                    ai.key      = o.name + "#" + std::to_string(attr_index);
                    ai.object   = o.name;
                    ai.path_abs = (state.project_root / a.audio_path).string();
                    ai.play     = a.audio_play != 0;
                    ai.loop     = a.audio_loop;
                    ai.volume   = a.audio_volume;
                    ai.pitch    = a.audio_pitch;
                    ai.audio_class = a.audio_class;
                    ai.priority = a.audio_priority;
                    ai.load_mode = a.audio_load_mode;
                    ai.spatial  = a.audio_spatial;
                    ai.min_dist = a.audio_min_dist;
                    ai.max_dist = a.audio_max_dist;
                    ai.doppler  = a.audio_doppler;
                    for (int k = 0; k < 3; ++k) ai.pos[k] = o.position[k];
                    ai.object_index = i;
                    m_audio_items.push_back(ai);
                }
                else if (a.type == "Rigid Body" || a.type == "Trigger Volume")
                {
                    if (a.type == "Rigid Body" && a.phys_kind != 0) dynamic_lighting = true;
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
                si.world = resolved_world;
                si.object_index = i;
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
                di.world        = resolved_world;
                di.selected     = selected;
                di.object_index = i;
                di.scale[0] = o.scale[0]; di.scale[1] = o.scale[1]; di.scale[2] = o.scale[2];
                di.animator_controller_path = animator_path;
                di.animator_initial_state = animator_state;
                di.animator_playback_speed = animator_speed;
                di.animator_auto_play = animator_auto_play;
                di.dynamic_lighting = dynamic_lighting;
                di.cast_shadow = cast_shadow;
                m_draw_items.push_back(di);
            }
            if (!model_path.empty())
            {
                PickItem pick;
                pick.model_path = model_path;
                pick.world = resolved_world;
                pick.object_index = i;
                m_pick_items.push_back(pick);
            }
        }
    }
    else if (!m_lightmap_scene.empty())
    {
        ClearLightmaps();
        m_lightmap_scene.clear();
        m_lightmap_timestamp = {};
    }

    // Physics preview: build the Bullet world on the Play rising edge, tear it
    // down on Stop (or when the scene selection goes away). The button that
    // toggles state.physics_playing is drawn in the viewport overlay below.
    {
        // scene.load() from a script: switch the editor to that scene and start
        // a fresh Play session on it — the editor equivalent of the console's
        // unload/reload, expressed in the editor's own Play/Stop terms.
        if (m_phys_on && !m_pending_scene.empty() && m_load_frames >= 2 &&
            m_time >= m_load_ready_time)
        {
            const std::string want = m_pending_scene;
            StopPhysics(); // clears m_pending_scene and undoes the live layer
            for (std::size_t s = 0; s < state.scenes.size(); ++s)
            {
                const std::string& have = state.scenes[s].name;
                if (have != want && have != want + ".scene") continue;
                state.selected_scene = (int)s;
                break;
            }
        }

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
        if (m_phys_on)
            for (int i = 0; i < (int)resolved_objects.size(); ++i)
                m_phys.SetHierarchyTransform(i, resolved_objects[i].pos, resolved_objects[i].rot);
    }

    // While playing, poll the controller, then overlay keyboard/mouse per the
    // editor's Mapping panel (state.controller_mapping). Done here in RenderUi
    // where the ImGui key/mouse state is live; RenderGpu's script Update reads it.
    if (m_phys_on)
    {
        input::PollXInput(m_input);
        input::ApplyMapping(state.controller_mapping, m_input);
        // Primed after the mapping, not inside the poll: in the editor a button
        // can come from the keyboard overlay, so the state is only complete here.
        if (!m_input.primed) m_input.Prime();
    }

    // No directional light authored: keep the legacy fixed sun — white when the
    // scene has no direct lights at all (old scenes render unchanged), black
    // when point/spot lights exist (the author owns the lighting; only c0's
    // direction, which custom shaders read, stays meaningful).
    if (!have_dir)
    {
        const Vec3 ld = Normalize({-0.4f, -1.0f, -0.5f});
        m_light_dir[0] = ld.x; m_light_dir[1] = ld.y; m_light_dir[2] = ld.z;
        const float w = have_authored_direct ? 0.0f : 1.0f;
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

    // Focus-key ("F") target: the selected object.
    m_has_focus = false;
    if (SceneObject* sel = state.SelectedObject())
    {
        const int selected = state.selected_object;
        const bool resolved = selected >= 0 && selected < (int)resolved_objects.size();
        m_focus_pos[0] = resolved ? resolved_objects[selected].pos[0] : sel->position[0];
        m_focus_pos[1] = resolved ? resolved_objects[selected].pos[1] : sel->position[1];
        m_focus_pos[2] = resolved ? resolved_objects[selected].pos[2] : sel->position[2];
        m_focus_scale = resolved
            ? (std::max)({resolved_objects[selected].scale[0], resolved_objects[selected].scale[1], resolved_objects[selected].scale[2]})
            : (std::max)({sel->scale[0], sel->scale[1], sel->scale[2]});
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
        const bool viewport_clicked = ImGui::IsItemHovered() &&
                                      ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool selection_click_consumed = false;
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
            // Through the active scene camera via the shared resolver (same math
            // as the 360 runtime). Track advances while the toggle is on and
            // restarts with it; follow smoothing eases across frames. Live
            // physics poses (last step) stand in for authored transforms.
            if (m_cam_key != cam_object)
            {
                m_cam_key = cam_object;
                m_track_dist = m_track_speed = 0.0f;
                m_follow_smooth.Reset();
            }
            auto find_pose = [&](int idx) -> const phys::Pose*
            {
                if (!m_phys_on) return nullptr;
                for (const phys::Pose& p : m_phys_poses)
                    if (p.objectIndex == idx) return &p;
                return nullptr;
            };

            camr::View v;
            if (const phys::Pose* cp = find_pose(cam_object))
            {
                const float* m = cp->matrix;
                v.pos[0] = m[12]; v.pos[1] = m[13]; v.pos[2] = m[14];
                v.fwd[0] = m[8];  v.fwd[1] = m[9];  v.fwd[2] = m[10];
                v.up[0]  = m[4];  v.up[1]  = m[5];  v.up[2]  = m[6];
            }
            else
                camr::ResolveFixed(cam_pos, cam_rot, v);

            SceneFile* scene = state.SelectedScene();
            if (cam_attr->cam_type == camr::CamFollow && scene &&
                !cam_attr->cam_follow_target.empty())
            {
                int target = -1;
                for (int i = 0; i < (int)scene->objects.size(); ++i)
                    if (scene->objects[i].name == cam_attr->cam_follow_target) { target = i; break; }
                if (target >= 0)
                {
                    float tpos[3];
                    if (const phys::Pose* tp = find_pose(target))
                    { tpos[0] = tp->matrix[12]; tpos[1] = tp->matrix[13]; tpos[2] = tp->matrix[14]; }
                    else if (target < (int)resolved_objects.size())
                        for (int k = 0; k < 3; ++k) tpos[k] = resolved_objects[target].pos[k];
                    else
                        for (int k = 0; k < 3; ++k) tpos[k] = scene->objects[target].position[k];
                    camr::ResolveFollow(cam_pos, cam_rot, tpos,
                                        cam_attr->cam_follow_offset, cam_attr->cam_follow_orbit,
                                        cam_attr->cam_follow_rot_offset, cam_attr->cam_follow_lock, v);
                    camr::SmoothFollow(v, m_follow_smooth, cam_attr->cam_follow_smoothing,
                                       ImGui::GetIO().DeltaTime);
                }
            }
            else if (cam_attr->cam_type == camr::CamTrack && cam_attr->cam_track_points.size() >= 6)
            {
                const float* pts = cam_attr->cam_track_points.data();
                const int    n   = (int)(cam_attr->cam_track_points.size() / 3);
                const float total = camr::TrackTotalLength(pts, n);
                camr::AdvanceTrack(m_track_dist, m_track_speed, cam_attr->cam_track_speed,
                                   cam_attr->cam_track_accel, ImGui::GetIO().DeltaTime, total);
                camr::ResolveTrack(pts, n, m_track_dist, cam_attr->cam_track_rot_offset, v);
            }

            const float d2r = 3.14159265f / 180.0f;
            m_eye[0] = v.pos[0]; m_eye[1] = v.pos[1]; m_eye[2] = v.pos[2];
            m_view = LookAtLH({v.pos[0], v.pos[1], v.pos[2]},
                              {v.pos[0] + v.fwd[0], v.pos[1] + v.fwd[1], v.pos[2] + v.fwd[2]},
                              {v.up[0], v.up[1], v.up[2]});
            m_proj = PerspectiveFovLH(cam_fov * d2r, (float)m_width / (float)m_height,
                                      cam_near, cam_far);
        }
        else
        {
            m_cam_key = -1; // restart track/smoothing next time look-through engages
            Vec3 cdir = { std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch),
                          std::cos(m_pitch) * std::cos(m_yaw) };
            Vec3 cat  = { m_target[0], m_target[1], m_target[2] };
            Vec3 ceye = { cat.x + cdir.x * m_distance, cat.y + cdir.y * m_distance,
                          cat.z + cdir.z * m_distance };
            m_eye[0] = ceye.x; m_eye[1] = ceye.y; m_eye[2] = ceye.z;
            m_view = LookAtLH(ceye, cat, {0.0f, 1.0f, 0.0f});
            m_proj = PerspectiveFovLH(3.14159265f / 4.0f, (float)m_width / (float)m_height, 0.1f, 200.0f);
        }

        // Track camera path editing: when the selected object has a Track
        // camera, draw its spline + point handles and let a selected point be
        // gizmo-dragged instead of the object (the Vulkan editor's flow).
        SceneObject*     sel        = state.SelectedObject();
        ObjectAttribute* track_attr = nullptr;
        int              track_attr_index = -1;
        if (sel)
            for (int ai = 0; ai < (int)sel->attributes.size(); ++ai)
                if (sel->attributes[ai].type == "Camera" && sel->attributes[ai].cam_type == camr::CamTrack)
                { track_attr = &sel->attributes[ai]; track_attr_index = ai; break; }

        int tp_count = track_attr ? (int)(track_attr->cam_track_points.size() / 3) : 0;
        if (state.selected_track_point_index >= tp_count)
            state.selected_track_point_index = -1; // point was removed in the Inspector
        const bool track_point_editing = track_attr && state.selected_track_point_index >= 0;

        if (track_attr && tp_count >= 2)
        {
            // Substitute the mid-drag preview point so the spline follows the gizmo.
            std::vector<float> pts(track_attr->cam_track_points);
            if (m_track_preview && m_track_preview_object == state.selected_object &&
                m_track_preview_attr == track_attr_index && m_track_preview_index < tp_count)
                for (int k = 0; k < 3; ++k)
                    pts[m_track_preview_index * 3 + k] = m_track_preview_point[k];

            const D3DMATRIX vpm = Multiply(m_view, m_proj);
            const ImVec2 mn = p0, mx = ImVec2(p0.x + avail.x, p0.y + avail.y);
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Path spline: 96 segments through the same arc-length sampler the
            // runtime rides (blue, like the reference editor).
            const float total = camr::TrackTotalLength(pts.data(), tp_count);
            const int   kSegs = 96;
            float prevPos[3], tanUnused[3];
            camr::SampleTrackAtDistance(pts.data(), tp_count, 0.0f, prevPos, tanUnused);
            ImVec2 prevScr;
            bool   prevOk = ProjectToScreen(vpm, prevPos, mn, mx, prevScr);
            for (int seg = 1; seg <= kSegs; ++seg)
            {
                float curPos[3];
                camr::SampleTrackAtDistance(pts.data(), tp_count, total * (float)seg / (float)kSegs,
                                            curPos, tanUnused);
                ImVec2 curScr;
                const bool curOk = ProjectToScreen(vpm, curPos, mn, mx, curScr);
                if (prevOk && curOk)
                    dl->AddLine(prevScr, curScr, IM_COL32(120, 200, 255, 230), 2.0f);
                prevScr = curScr;
                prevOk  = curOk;
            }

            // Point handles: hover within 12px, click to select (unless the
            // gizmo owns the mouse), 6px filled circle + dark outline.
            ImGuiIO& io = ImGui::GetIO();
            const bool over_viewport = io.MousePos.x >= mn.x && io.MousePos.x <= mx.x &&
                                       io.MousePos.y >= mn.y && io.MousePos.y <= mx.y;
            const bool gizmo_busy = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
            int hovered_point = -1;
            std::vector<ImVec2> scr(tp_count);
            std::vector<bool>   ok(tp_count);
            for (int p = 0; p < tp_count; ++p)
            {
                ok[p] = ProjectToScreen(vpm, &pts[p * 3], mn, mx, scr[p]);
                if (ok[p] && hovered_point < 0)
                {
                    const float dx = io.MousePos.x - scr[p].x, dy = io.MousePos.y - scr[p].y;
                    if (dx * dx + dy * dy <= 12.0f * 12.0f)
                        hovered_point = p;
                }
            }
            if (over_viewport && !gizmo_busy && hovered_point >= 0 &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                state.selected_track_point_index = hovered_point;
                selection_click_consumed = true;
            }
            for (int p = 0; p < tp_count; ++p)
            {
                if (!ok[p]) continue;
                const ImU32 col = (p == state.selected_track_point_index) ? IM_COL32(255, 220, 120, 255)
                                : (p == hovered_point)                    ? IM_COL32(200, 235, 255, 255)
                                                                          : IM_COL32(120, 200, 255, 255);
                dl->AddCircleFilled(scr[p], 6.0f, col, 16);
                dl->AddCircle(scr[p], 7.5f, IM_COL32(20, 30, 40, 200), 16, 1.5f);
            }
        }

        // Translation gizmo: the selected track point when one is picked,
        // otherwise the selected object.
        if (sel)
        {
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist();
            ImGuizmo::SetRect(p0.x, p0.y, avail.x, avail.y);
        }
        if (track_point_editing)
        {
            const int idx = state.selected_track_point_index;
            float cur[3];
            for (int k = 0; k < 3; ++k) cur[k] = track_attr->cam_track_points[idx * 3 + k];
            if (m_track_preview && m_track_preview_object == state.selected_object &&
                m_track_preview_attr == track_attr_index && m_track_preview_index == idx)
                for (int k = 0; k < 3; ++k) cur[k] = m_track_preview_point[k];

            D3DMATRIX pm = Identity();
            pm._41 = cur[0]; pm._42 = cur[1]; pm._43 = cur[2];
            ImGuizmo::Manipulate(&m_view.m[0][0], &m_proj.m[0][0],
                                 ImGuizmo::TRANSLATE, ImGuizmo::WORLD, &pm.m[0][0]);
            if (ImGuizmo::IsUsing())
            {
                // Accumulate into the preview; the scene stays untouched mid-drag.
                m_track_preview        = true;
                m_track_preview_object = state.selected_object;
                m_track_preview_attr   = track_attr_index;
                m_track_preview_index  = idx;
                m_track_preview_point[0] = pm._41;
                m_track_preview_point[1] = pm._42;
                m_track_preview_point[2] = pm._43;
            }
            else if (m_track_preview)
            {
                // Drag ended: commit the point once and save the scene.
                for (int k = 0; k < 3; ++k)
                    track_attr->cam_track_points[m_track_preview_index * 3 + k] = m_track_preview_point[k];
                if (SceneFile* s = state.SelectedScene())
                {
                    s->dirty = true;
                    project::SaveScene(*s);
                }
                m_track_preview = false;
            }
        }
        else if (sel)
        {
            D3DMATRIX model = ComposeWorld(*sel);
            const int selected = state.selected_object;
            if (selected >= 0 && selected < (int)resolved_objects.size())
                std::memcpy(&model.m[0][0], resolved_objects[selected].world, sizeof(resolved_objects[selected].world));
            ImGuizmo::Manipulate(&m_view.m[0][0], &m_proj.m[0][0],
                                 ImGuizmo::TRANSLATE, ImGuizmo::WORLD, &model.m[0][0]);
            if (ImGuizmo::IsUsing())
            {
                float local[16];
                std::memcpy(local, &model.m[0][0], sizeof(local));
                if (selected >= 0 && selected < (int)object_parents.size() && object_parents[selected] >= 0)
                {
                    float inverse_parent[16];
                    hier::AffineInverse(resolved_objects[object_parents[selected]].world, inverse_parent);
                    hier::Multiply(local, inverse_parent, local);
                }
                sel->position[0] = local[12];
                sel->position[1] = local[13];
                sel->position[2] = local[14];
                if (SceneFile* s = state.SelectedScene()) s->dirty = true;
                m_gizmo_editing = true;
            }
            else if (m_gizmo_editing)
            {
                if (SceneFile* s = state.SelectedScene()) project::SaveScene(*s);
                m_gizmo_editing = false;
            }
        }

        if (viewport_clicked && !selection_click_consumed &&
            !ImGuizmo::IsOver() && !ImGuizmo::IsUsing())
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const float ndc_x = ((mouse.x - p0.x) / avail.x) * 2.0f - 1.0f;
            const float ndc_y = 1.0f - ((mouse.y - p0.y) / avail.y) * 2.0f;
            const Vec3 view_direction = Normalize({ndc_x / m_proj._11, ndc_y / m_proj._22, 1.0f});
            const Vec3 world_direction = Normalize({
                view_direction.x * m_view._11 + view_direction.y * m_view._12 + view_direction.z * m_view._13,
                view_direction.x * m_view._21 + view_direction.y * m_view._22 + view_direction.z * m_view._23,
                view_direction.x * m_view._31 + view_direction.y * m_view._32 + view_direction.z * m_view._33});

            int picked_object = -1;
            float nearest_distance = 1.0e30f;
            for (const PickItem& pick : m_pick_items)
            {
                GpuMesh* mesh = m_meshes.Get(pick.model_path);
                D3DMATRIX inverse_world;
                if (!mesh || !InvertAffine(pick.world, inverse_world))
                    continue;
                const Vec3 local_origin = TransformPoint({m_eye[0], m_eye[1], m_eye[2]}, inverse_world);
                const Vec3 local_direction = {
                    world_direction.x * inverse_world._11 + world_direction.y * inverse_world._21 + world_direction.z * inverse_world._31,
                    world_direction.x * inverse_world._12 + world_direction.y * inverse_world._22 + world_direction.z * inverse_world._32,
                    world_direction.x * inverse_world._13 + world_direction.y * inverse_world._23 + world_direction.z * inverse_world._33};
                float local_distance = 0.0f;
                if (!IntersectBounds(local_origin, local_direction, mesh->boundsMin, mesh->boundsMax, local_distance))
                    continue;
                const Vec3 local_hit = {local_origin.x + local_direction.x * local_distance,
                                        local_origin.y + local_direction.y * local_distance,
                                        local_origin.z + local_direction.z * local_distance};
                const Vec3 world_hit = TransformPoint(local_hit, pick.world);
                const float world_distance = Dot(Sub(world_hit, {m_eye[0], m_eye[1], m_eye[2]}), world_direction);
                if (world_distance >= 0.0f && world_distance < nearest_distance)
                {
                    nearest_distance = world_distance;
                    picked_object = pick.object_index;
                }
            }
            state.selected_object = picked_object;
            state.selected_track_point_index = -1;
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
void SceneRenderer::RenderShadowMap()
{
    if (!m_shadow_enabled || !m_shadow_ps || !m_vs || m_lightmap_meshes.empty())
        return;

    lsel::Aabb bounds;
    lsel::Aabb casterBounds;
    for (int axis = 0; axis < 3; ++axis)
    {
        bounds.min[axis] = casterBounds.min[axis] = FLT_MAX;
        bounds.max[axis] = casterBounds.max[axis] = -FLT_MAX;
    }
    bool haveBounds = false;
    bool haveCasterBounds = false;
    for (const DrawItem& item : m_draw_items)
    {
        GpuMesh* mesh = m_meshes.Get(item.model_path);
        if (!mesh) continue;
        lsel::Aabb transformed;
        lsel::TransformAabb(mesh->boundsMin, mesh->boundsMax, &item.world.m[0][0], transformed);
        if (m_lightmap_meshes.find(item.object_index) != m_lightmap_meshes.end())
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                bounds.min[axis] = (std::min)(bounds.min[axis], transformed.min[axis]);
                bounds.max[axis] = (std::max)(bounds.max[axis], transformed.max[axis]);
            }
            haveBounds = true;
        }
        if (item.cast_shadow)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                casterBounds.min[axis] = (std::min)(casterBounds.min[axis], transformed.min[axis]);
                casterBounds.max[axis] = (std::max)(casterBounds.max[axis], transformed.max[axis]);
            }
            haveCasterBounds = true;
        }
    }
    if (!haveBounds || !haveCasterBounds) return;

    const Vec3 center = {(bounds.min[0] + bounds.max[0]) * 0.5f,
                         (bounds.min[1] + bounds.max[1]) * 0.5f,
                         (bounds.min[2] + bounds.max[2]) * 0.5f};
    const Vec3 lightDirection = Normalize({m_light_dir[0], m_light_dir[1], m_light_dir[2]});
    if (Dot(lightDirection, lightDirection) < 1.0e-6f) return;
    const float dx = bounds.max[0] - bounds.min[0];
    const float dy = bounds.max[1] - bounds.min[1];
    const float dz = bounds.max[2] - bounds.min[2];
    const float radius = (std::max)(1.0f, std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f);
    const Vec3 eye = {center.x - lightDirection.x * (radius * 2.0f + 10.0f),
                      center.y - lightDirection.y * (radius * 2.0f + 10.0f),
                      center.z - lightDirection.z * (radius * 2.0f + 10.0f)};
    const Vec3 up = std::abs(lightDirection.y) > 0.95f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    const D3DMATRIX view = LookAtLH(eye, center, up);

    Vec3 lightMin = {FLT_MAX, FLT_MAX, FLT_MAX};
    Vec3 lightMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (int corner = 0; corner < 8; ++corner)
    {
        const Vec3 world = {(corner & 1) ? bounds.max[0] : bounds.min[0],
                            (corner & 2) ? bounds.max[1] : bounds.min[1],
                            (corner & 4) ? bounds.max[2] : bounds.min[2]};
        const Vec3 light = TransformPoint(world, view);
        lightMin.x = (std::min)(lightMin.x, light.x); lightMax.x = (std::max)(lightMax.x, light.x);
        lightMin.y = (std::min)(lightMin.y, light.y); lightMax.y = (std::max)(lightMax.y, light.y);
        lightMin.z = (std::min)(lightMin.z, light.z); lightMax.z = (std::max)(lightMax.z, light.z);
    }
    Vec3 casterLightMin = {FLT_MAX, FLT_MAX, FLT_MAX};
    Vec3 casterLightMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (int corner = 0; corner < 8; ++corner)
    {
        const Vec3 world = {(corner & 1) ? casterBounds.max[0] : casterBounds.min[0],
                            (corner & 2) ? casterBounds.max[1] : casterBounds.min[1],
                            (corner & 4) ? casterBounds.max[2] : casterBounds.min[2]};
        const Vec3 light = TransformPoint(world, view);
        casterLightMin.x = (std::min)(casterLightMin.x, light.x);
        casterLightMin.y = (std::min)(casterLightMin.y, light.y);
        casterLightMin.z = (std::min)(casterLightMin.z, light.z);
        casterLightMax.x = (std::max)(casterLightMax.x, light.x);
        casterLightMax.y = (std::max)(casterLightMax.y, light.y);
        casterLightMax.z = (std::max)(casterLightMax.z, light.z);
    }
    lightMin.z = (std::min)(lightMin.z, casterLightMin.z);
    lightMax.z = (std::max)(lightMax.z, casterLightMax.z);
    const float casterExtent = (std::max)(casterLightMax.x - casterLightMin.x,
                                          casterLightMax.y - casterLightMin.y);
    const float padding = (std::max)(0.25f, casterExtent * 0.05f);
    const D3DMATRIX projection = OrthoOffCenterLH(casterLightMin.x - padding, casterLightMax.x + padding,
                                                   casterLightMin.y - padding, casterLightMax.y + padding,
                                                   (std::max)(0.1f, lightMin.z - 5.0f), lightMax.z + 5.0f);
    m_shadow_matrix = Multiply(view, projection);

    const UINT shadowSize = 1024;
    if (!m_shadow_texture && SUCCEEDED(m_device->CreateTexture(shadowSize, shadowSize, 1,
            D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_shadow_texture, nullptr)))
        m_shadow_texture->GetSurfaceLevel(0, &m_shadow_surface);
    if (!m_shadow_depth)
        m_device->CreateDepthStencilSurface(shadowSize, shadowSize, D3DFMT_D16,
                                            D3DMULTISAMPLE_NONE, 0, TRUE, &m_shadow_depth, nullptr);
    if (!m_shadow_surface || !m_shadow_depth) return;

    IDirect3DSurface9* previousTarget = nullptr;
    IDirect3DSurface9* previousDepth = nullptr;
    D3DVIEWPORT9 previousViewport;
    m_device->GetRenderTarget(0, &previousTarget);
    m_device->GetDepthStencilSurface(&previousDepth);
    m_device->GetViewport(&previousViewport);
    m_device->SetTexture(10, nullptr);
    if (SUCCEEDED(m_device->BeginScene()))
    {
        m_device->SetRenderTarget(0, m_shadow_surface);
        m_device->SetDepthStencilSurface(m_shadow_depth);
        D3DVIEWPORT9 viewport = {0, 0, shadowSize, shadowSize, 0.0f, 1.0f};
        m_device->SetViewport(&viewport);
        m_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0f, 0);
        m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_device->SetPixelShader(m_shadow_ps);
        UploadShadowMatrix(m_device, m_shadow_matrix);

        for (const DrawItem& item : m_draw_items)
        {
            GpuMesh* mesh = m_meshes.Get(item.model_path);
            if (!mesh || !item.cast_shadow) continue;
            const D3DMATRIX wvp = Multiply(item.world, m_shadow_matrix);
            m_device->SetVertexShaderConstantF(0, &wvp.m[0][0], 4);
            m_device->SetVertexShaderConstantF(4, &item.world.m[0][0], 4);
            BindMeshForDraw(mesh, &item.skin_palette, item.morph_vb);
            m_device->SetPixelShader(m_shadow_ps);
            for (const GpuSubset& subset : mesh->subsets)
            {
                if (subset.alpha == AlphaKind::Blend) continue;
                m_device->SetTexture(0, subset.diffuse ? subset.diffuse : m_def_white);
                m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, mesh->vertexCount,
                                               subset.indexStart, subset.indexCount / 3);
            }
        }
        m_device->EndScene();
    }
    m_device->SetRenderTarget(0, previousTarget);
    m_device->SetDepthStencilSurface(previousDepth);
    m_device->SetViewport(&previousViewport);
    if (previousTarget) previousTarget->Release();
    if (previousDepth) previousDepth->Release();
}

void SceneRenderer::RenderGpu(float dt)
{
    if (!m_device || !m_renderRequested || !m_rtSurface)
        return;

    m_time += dt; // drives animated custom shaders (gTime)
    // A scene.load in flight ticks here so the switch in RenderUi can require a
    // frame to have been DRAWN first — otherwise the loading screen a script just
    // built is torn down with the old scene before it ever reaches the display.
    if (!m_pending_scene.empty()) ++m_load_frames;

    // Physics preview: step Bullet, then override this frame's draw items with the
    // simulated poses (world = Scale * pose, keeping authored scale). Trigger
    // enter/exit events go to the Log. Same code the 360 runtime runs.
    if (m_phys_on)
    {
        // Input was polled in RenderUi (controller + Mapping-panel keyboard/mouse).
        // GUI first: it turns this frame's input into focus/confirm events, and
        // ScriptVM::Update drains them before running any on_update.
        m_gui.Update(dt, m_input);
        m_script.Update(dt); // scripts set impulses/velocity/transforms for this step
        // Objects a script destroyed this frame leave the world now, after every
        // on_update has run — removing them mid-loop would edit the lists the
        // update is walking.
        ApplyPendingDestroys();
        RefreshPlayHierarchy();
        // gui.set_paused(true) freezes the simulation but NOT scripts or
        // rendering, so the menu that raised it stays live. Poses are still
        // read back: the draw items are rebuilt from the scene every frame, so
        // skipping the readback would snap simulated objects to their authored
        // transforms instead of holding them where they stopped.
        if (!m_gui.Paused())
            m_phys.Step(dt);
        m_phys.ReadPoses(m_phys_poses);
        RefreshPlayHierarchy();

        // Dispatch trigger enter/stay/exit events to scripts (no automatic engine
        // logging — a script's own on_trigger + log() is the only trigger output).
        std::vector<phys::TriggerEvent> events;
        m_phys.DrainTriggerEvents(events);
        for (size_t ei = 0; ei < events.size(); ++ei)
            m_script.FireTrigger(events[ei].triggerObjectIndex, events[ei].otherObjectIndex,
                                 nullptr, events[ei].phase);

        // ...and contacts between ordinary bodies (on_collision).
        std::vector<phys::CollisionEvent> contacts;
        m_phys.DrainCollisionEvents(contacts);
        for (size_t ci = 0; ci < contacts.size(); ++ci)
            m_script.FireCollision(contacts[ci].aObjectIndex, contacts[ci].bObjectIndex,
                                   contacts[ci].phase);
    }

    UpdateAnimations(m_gui.Paused() ? 0.0f : dt);
    // The deform pass runs in edit mode too, for the Face tab's preview.
    UpdateFaceLayers(m_gui.Paused() ? 0.0f : dt);
    UpdateFaces();

    // Redirect rendering to the offscreen target, remembering the back buffer.
    IDirect3DSurface9* prevRt    = nullptr;
    IDirect3DSurface9* prevDepth = nullptr;
    m_device->GetRenderTarget(0, &prevRt);
    m_device->GetDepthStencilSurface(&prevDepth);

    RenderShadowMap();

    // --- Dynamic environment capture (reflections) ---
    // Reflective surfaces reflect the ACTUAL scene: render it into a small cube
    // map from the first reflective object's position; the main pass samples it
    // on s5. The captured object itself is skipped (a mirror can't mirror
    // itself). Roughness counts as reflective just as much as metal does — a
    // matte object alone in a scene must still reflect the room, not fall
    // through to the painted env cube.
    m_env_captured = false;
    {
        const UINT kEnvSize = (UINT)envcube::kSize;
        int   env_obj = -1;
        float env_pos[3] = { 0.0f, 0.0f, 0.0f };
        // Two passes: metal first, so scenes that already had a capture keep
        // capturing from exactly the object they did before; rough/clearcoat
        // only supply a capture point when there is no metal at all.
        for (int pass = 0; pass < 2 && env_obj == -1; ++pass)
            for (const DrawItem& item : m_draw_items)
            {
                GpuMesh* gm = m_meshes.Get(item.model_path);
                if (!gm) continue;
                bool reflective = false;
                for (const GpuSubset& s : gm->subsets)
                    if (pass == 0 ? (s.metallic != nullptr) : (s.roughness || s.clearcoat))
                    { reflective = true; break; }
                if (!reflective) continue;
                env_obj = item.object_index;
                env_pos[0] = item.world._41; env_pos[1] = item.world._42; env_pos[2] = item.world._43;
                break;
            }
        if (env_obj != -1 && m_vs && m_ps && m_mesh_decl)
        {
            if (!m_env_dyn)
                m_device->CreateCubeTexture(kEnvSize, 1, D3DUSAGE_RENDERTARGET,
                                            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_env_dyn, nullptr);
            if (!m_env_blur)
                m_device->CreateCubeTexture(kEnvSize, envcube::kMips, D3DUSAGE_RENDERTARGET,
                                            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_env_blur, nullptr);
            if (!m_env_blur_tmp)
                m_device->CreateCubeTexture(kEnvSize, envcube::kMips, D3DUSAGE_RENDERTARGET,
                                            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_env_blur_tmp, nullptr);
            if (!m_env_dyn_depth)
                m_device->CreateDepthStencilSurface(kEnvSize, kEnvSize, D3DFMT_D16,
                                                    D3DMULTISAMPLE_NONE, 0, TRUE, &m_env_dyn_depth, nullptr);
            if (m_env_dyn && m_env_dyn_depth && SUCCEEDED(m_device->BeginScene()))
            {
                float pm[16];
                envcube::FaceProj(0.2f, 100.0f, pm);
                D3DMATRIX proj;
                memcpy(&proj, pm, sizeof(pm));
                for (int f = 0; f < 6; ++f)
                {
                    IDirect3DSurface9* face = nullptr;
                    if (FAILED(m_env_dyn->GetCubeMapSurface((D3DCUBEMAP_FACES)f, 0, &face)))
                        continue;
                    m_device->SetRenderTarget(0, face);
                    m_device->SetDepthStencilSurface(m_env_dyn_depth);
                    D3DVIEWPORT9 evp = { 0, 0, kEnvSize, kEnvSize, 0.0f, 1.0f };
                    m_device->SetViewport(&evp);
                    m_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, m_background, 1.0f, 0);
                    float vm[16];
                    envcube::FaceView(f, env_pos, vm);
                    D3DMATRIX view;
                    memcpy(&view, vm, sizeof(vm));
                    // Sky first, with no depth test — the face is empty, and
                    // without this a chrome sphere under a sunset mirrors the
                    // flat clear colour the face was just filled with.
                    RenderSkybox(view, proj, false);
                    DrawSceneForEnv(view, proj, env_pos, env_obj);
                    face->Release();
                }
                BuildEnvBlurChain();
                m_device->EndScene();
                m_env_captured = true;
            }
        }
    }

    // Render into the multisampled target when available (for alpha-to-coverage);
    // it is resolved into m_rt after EndScene.
    IDirect3DSurface9* sceneRt    = (m_msaa != D3DMULTISAMPLE_NONE) ? m_rtMS    : m_rtSurface;
    IDirect3DSurface9* sceneDepth = (m_msaa != D3DMULTISAMPLE_NONE) ? m_depthMS : m_depth;
    m_device->SetRenderTarget(0, sceneRt);
    m_device->SetDepthStencilSurface(sceneDepth);

    // With the bloom chain present, the target's alpha channel carries the
    // emissive glow mask: cleared to zero, written ONLY by opaque material
    // subsets (color writes default to RGB), read back by RenderBloom, and
    // saturated to opaque by its combine pass (ImGui samples the alpha).
    // Without the chain, alpha stays at the opaque clear — the old behavior.
    const bool bloom_ok = m_bloomASurf && m_bloomBSurf &&
        m_bloom_bright_vs && m_bloom_bright_ps &&
        m_bloom_blur_vs   && m_bloom_blur_ps &&
        m_bloom_combine_vs && m_bloom_combine_ps;
    m_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                    bloom_ok ? (m_background & 0x00FFFFFF) : m_background, 1.0f, 0);
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);

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

        // --- Scene models (shader: diffuse / normal / specular / emissive /
        //     metallic + environment cube) ---
        const bool mesh_pass = !m_draw_items.empty() && m_vs && m_ps && m_mesh_decl;
        if (mesh_pass)
        {
            m_device->SetVertexDeclaration(m_mesh_decl);
            m_device->SetVertexShader(m_vs);
            m_device->SetPixelShader(m_ps);
            m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
            for (DWORD s = 0; s < 5; ++s)
            {
                m_device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                m_device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            }
            // s5 = environment cube (bound once per frame). MIPFILTER is the
            // whole roughness blur: D3D9 defaults it to D3DTEXF_NONE, so without
            // this line texCUBElod silently reads level 0 and nothing blurs.
            m_device->SetSamplerState(5, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(5, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(5, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(5, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            m_device->SetSamplerState(5, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            m_device->SetSamplerState(5, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
            // s6 = clearcoat mask, s7 = roughness (per subset, 2D).
            for (DWORD s = 6; s <= 7; ++s)
            {
                m_device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                m_device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            }
            for (DWORD s = 8; s <= 9; ++s)
            {
                m_device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                m_device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            }
            m_device->SetSamplerState(10, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            m_device->SetSamplerState(10, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            m_device->SetSamplerState(10, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            m_device->SetSamplerState(10, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            m_device->SetSamplerState(10, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            m_device->SetTexture(8, m_lightmap0 ? m_lightmap0 : m_def_black);
            m_device->SetTexture(9, m_lightmap1 ? m_lightmap1 : m_def_normal);
            m_device->SetTexture(10, m_shadow_texture ? m_shadow_texture : m_def_white);
            UploadShadowMatrix(m_device, m_shadow_matrix);
            // Only the blurred chain has levels to sample; the static painted
            // cube and the black default are single-level, so roughness gets a
            // max mip of 0 there rather than a level that was never written.
            const bool env_blurred = m_env_captured && m_env_blur && m_env_blur_vs && m_env_blur_ps;
            m_device->SetTexture(5, env_blurred   ? (IDirect3DBaseTexture9*)m_env_blur
                                  : m_env_captured ? (IDirect3DBaseTexture9*)m_env_dyn
                                  : m_env          ? (IDirect3DBaseTexture9*)m_env
                                                   : (IDirect3DBaseTexture9*)m_def_envcube);
            const float rough_cfg[4] = { env_blurred ? envcube::kMaxMip : 0.0f,
                                         m_baked_ambient[0], m_baked_ambient[1], m_baked_ambient[2] };
            m_device->SetPixelShaderConstantF(22, rough_cfg, 1);

            // Pixel-shader constants: light direction, camera position, ambient.
            const float cam_pos[4]   = { m_eye[0], m_eye[1], m_eye[2], 0.0f };
            m_device->SetPixelShaderConstantF(0, m_light_dir, 1);
            m_device->SetPixelShaderConstantF(1, cam_pos, 1);
            m_device->SetPixelShaderConstantF(2, m_ambient, 1);
            m_device->SetPixelShaderConstantF(6, m_light_col, 1);
            const D3DMATRIX vp = Multiply(m_view, m_proj);
            // Per-item transform + buffers, then each material subset binds its
            // own textures and draws its index range (multi-material meshes).
            auto set_item = [&](GpuMesh* gm, const DrawItem& item) -> UINT
            {
                lsel::Aabb bounds;
                lsel::TransformAabb(gm->boundsMin, gm->boundsMax, &item.world.m[0][0], bounds);
                lsel::PackPoints(m_point_lights.empty() ? nullptr : m_point_lights.data(),
                                 m_point_lights.size(), bounds, m_point_pos, m_point_col);
                lsel::PackSpots(m_spot_lights.empty() ? nullptr : m_spot_lights.data(),
                                m_spot_lights.size(), bounds, m_spot_pos, m_spot_dir, m_spot_col);
                m_device->SetPixelShaderConstantF(7, &m_point_pos[0][0], 4);
                m_device->SetPixelShaderConstantF(11, &m_point_col[0][0], 4);
                m_device->SetPixelShaderConstantF(16, &m_spot_pos[0][0], 2);
                m_device->SetPixelShaderConstantF(18, &m_spot_dir[0][0], 2);
                m_device->SetPixelShaderConstantF(20, &m_spot_col[0][0], 2);
                const D3DMATRIX wvp = Multiply(item.world, vp);
                m_device->SetVertexShaderConstantF(0, &wvp.m[0][0], 4);
                m_device->SetVertexShaderConstantF(4, &item.world.m[0][0], 4);
                const auto lightmap = m_lightmap_meshes.find(item.object_index);
                const bool useLightmap = lightmap != m_lightmap_meshes.end() &&
                                         m_lightmap0 && m_lightmap1 && m_lightmap_vs;
                float probeColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                float probeDirection[4] = {0.5f, 0.5f, 1.0f, 0.0f};
                if (item.dynamic_lighting || gm->skinVb)
                {
                    const float position[3] = {item.world._41, item.world._42, item.world._43};
                    lprobe::Sample(m_probe_grid, position, probeColor, probeDirection);
                }
                m_device->SetPixelShaderConstantF(24, probeColor, 1);
                m_device->SetPixelShaderConstantF(25, probeDirection, 1);
                const float shadowParams[4] = {1.0f / 1024.0f, 1.0f / 1024.0f,
                                               0.0015f,
                                               useLightmap && !item.dynamic_lighting && m_shadow_enabled ? 1.0f : 0.0f};
                m_device->SetPixelShaderConstantF(23, shadowParams, 1);
                if (useLightmap)
                {
                    m_device->SetVertexDeclaration(m_lightmap_mesh_decl);
                    m_device->SetVertexShader(m_lightmap_vs);
                    m_device->SetStreamSource(0, lightmap->second.vb, 0, sizeof(LightmapVertex));
                    m_device->SetStreamSource(1, nullptr, 0, 0);
                    m_device->SetIndices(lightmap->second.ib);
                    return lightmap->second.vertexCount;
                }
                BindMeshForDraw(gm, &item.skin_palette, item.morph_vb);
                return gm->vertexCount;
            };
            // Bump-offset strength: max UV shift across the height field packed
            // in a normal map's alpha (tune by eye; keep in sync with
            // SceneRuntime.cpp). Subsets without a height field upload exactly
            // 0, so their UVs — and the cutout alpha they feed — are untouched.
            const float kBumpScale = 0.08f;
            auto draw_subset = [&](GpuMesh* gm, const GpuSubset& s, UINT vertexCount)
            {
                const float bump[4] = { s.normalHasHeight ? kBumpScale : 0.0f, 0.0f, 0.0f, 0.0f };
                m_device->SetPixelShaderConstantF(5, bump, 1);
                // Opaque subsets export their emissive strength in the target's
                // alpha (the bloom glow source). Cutout/blend keep the diffuse's
                // alpha for masking/blending and must not touch the mask channel.
                const bool glow = bloom_ok && s.alpha == AlphaKind::Opaque;
                const float mask[4] = { glow ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
                m_device->SetPixelShaderConstantF(15, mask, 1);
                m_device->SetRenderState(D3DRS_COLORWRITEENABLE, glow
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
                m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                               vertexCount, s.indexStart, s.indexCount / 3);
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
                UINT vertexCount = gm->vertexCount;
                for (const GpuSubset& s : gm->subsets)
                {
                    if (s.alpha == AlphaKind::Blend)
                        continue; // smooth translucency -> pass 2
                    if (!item_bound) { vertexCount = set_item(gm, item); item_bound = true; }
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
                    draw_subset(gm, s, vertexCount);
                    if (cutout && m_hasA2C)
                        m_device->SetRenderState(m_a2cState, m_a2cOff);
                }
            }
            m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);

            // --- Skybox ---
            // Between the two passes on purpose: the opaque depth buffer is
            // complete, so early-Z throws the sky away under geometry, and the
            // blend pass still composites glass and water over it.
            RenderSkybox(m_view, m_proj, true);
            // BindMeshForDraw re-binds the declaration and vertex shader per
            // mesh but never the pixel shader, so pass 2 would otherwise shade
            // glass with the sky.
            m_device->SetVertexDeclaration(m_mesh_decl);
            m_device->SetVertexShader(m_vs);
            m_device->SetPixelShader(m_ps);

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
                UINT vertexCount = gm->vertexCount;
                for (const GpuSubset& s : gm->subsets)
                {
                    if (s.alpha != AlphaKind::Blend)
                        continue;
                    if (!item_bound) { vertexCount = set_item(gm, item); item_bound = true; }
                    draw_subset(gm, s, vertexCount);
                }
            }
            m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
            m_device->SetRenderState(D3DRS_COLORWRITEENABLE,   // back to RGB-only:
                D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);

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
        else
        {
            // A scene with no meshes never entered the pass above, so the sky
            // would silently go missing on an empty or shader-only scene.
            RenderSkybox(m_view, m_proj, true);
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

        // --- Spot volumetric beams (additive cone mesh, beam.hlsl) ---
        // Last of the translucents: additive is order-independent, depth test
        // keeps geometry occluding the shaft, no depth write. RGB only so the
        // ONE+ONE blend can't disturb the bloom glow mask in the alpha channel.
        if (!m_spot_beams.empty() && m_beam_vs && m_beam_ps && m_mesh_decl)
        {
            if (m_beam_cone.empty())
                BuildBeamCone();
            m_device->SetVertexDeclaration(m_mesh_decl);
            m_device->SetVertexShader(m_beam_vs);
            m_device->SetPixelShader(m_beam_ps);
            m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_ONE);
            m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
            m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
                D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
            const float cam[4] = { m_eye[0], m_eye[1], m_eye[2], 0.0f };
            m_device->SetPixelShaderConstantF(1, cam, 1);
            const D3DMATRIX vp = Multiply(m_view, m_proj);
            for (const SpotBeam& b : m_spot_beams)
            {
                const D3DMATRIX wvp = Multiply(b.world, vp);
                m_device->SetVertexShaderConstantF(0, &wvp.m[0][0], 4);
                m_device->SetVertexShaderConstantF(4, &b.world.m[0][0], 4);
                m_device->SetPixelShaderConstantF(0, b.color, 1);
                m_device->SetPixelShaderConstantF(2, b.apex, 1);
                m_device->SetPixelShaderConstantF(3, b.axis, 1);
                m_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(m_beam_cone.size() / 3),
                                          m_beam_cone.data(), sizeof(MeshVertex));
            }
            m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        }

        // --- Physics collider wireframes (green rigid, cyan trigger) and the
        //     selected object's spot-light cone gizmo ---
        // Colliders are drawn whenever the scene has them — during authoring
        // (from the authored transform, so you can size them against the mesh)
        // and while playing (RenderGpu's pose override makes them track the
        // simulation). The spot cone rides the same line list.
        if (!m_phys_debug.empty() || !m_spot_gizmos.empty())
        {
            std::vector<Vertex> lines;
            lines.reserve(m_phys_debug.size() * 48 + m_spot_gizmos.size() * 104);
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
            for (size_t i = 0; i < m_spot_gizmos.size(); ++i)
                AppendSpotCone(m_spot_gizmos[i], lines);
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

    // Emissive glow: blur the exported glow mask and lay it over the scene.
    RenderBloom();

    // Image/video overlays, composited on top of the finished frame. The video
    // clock only advances in the Play preview; edit mode holds the first frame.
    RenderOverlay(m_phys_on ? dt : 0.0f);
    RenderGui();

    // Audio attribute sources — audible only while the Play preview runs.
    UpdateAudio(dt);

    // ImGui composites the viewport texture with alpha and manages most of its
    // own states, but not the color-write mask — leave it fully enabled.
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);

    // Restore the back buffer as the active target.
    m_device->SetRenderTarget(0, prevRt);
    m_device->SetDepthStencilSurface(prevDepth);
    if (prevRt)    prevRt->Release();
    if (prevDepth) prevDepth->Release();
}

// One cube face of the dynamic environment capture: a reduced scene pass —
// models only, from the metal object's point of view. No grid, gizmos,
// outlines or post; metal inside a reflection samples the black cube (no
// recursion) and no glow mask is exported (c15 = 0).
void SceneRenderer::DrawSceneForEnv(const D3DMATRIX& view, const D3DMATRIX& proj,
                                    const float eye[3], int skip_object)
{
    const D3DMATRIX vp = Multiply(view, proj);

    m_device->SetVertexDeclaration(m_mesh_decl);
    m_device->SetVertexShader(m_vs);
    m_device->SetPixelShader(m_ps);
    m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
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
    m_device->SetTexture(5, m_def_envcube);

    const float cam[4]     = { eye[0], eye[1], eye[2], 0.0f };
    const float zero[4]    = { 0.0f, 0.0f, 0.0f, 0.0f };
    // s5 here is the single-level black cube, so roughness must not reach for a
    // mip that does not exist (c22.x = 0 -> texCUBElod stays at level 0).
    m_device->SetPixelShaderConstantF(22, zero, 1);
    m_device->SetPixelShaderConstantF(0, m_light_dir, 1);
    m_device->SetPixelShaderConstantF(1, cam, 1);
    m_device->SetPixelShaderConstantF(2, m_ambient, 1);
    m_device->SetPixelShaderConstantF(6, m_light_col, 1);
    m_device->SetPixelShaderConstantF(15, zero, 1);
    const float baked[4] = { 0.0f, m_baked_ambient[0], m_baked_ambient[1], m_baked_ambient[2] };
    m_device->SetPixelShaderConstantF(23, baked, 1);

    m_device->SetRenderState(D3DRS_ALPHAREF, 128);
    m_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool blendPass = (pass == 1);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, blendPass ? TRUE : FALSE);
        if (blendPass)
        {
            m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
            m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        }
        for (const DrawItem& item : m_draw_items)
        {
            if (item.object_index == skip_object)
                continue;
            GpuMesh* gm = m_meshes.Get(item.model_path);
            if (!gm)
                continue;
            bool bound = false;
            for (const GpuSubset& s : gm->subsets)
            {
                if ((s.alpha == AlphaKind::Blend) != blendPass)
                    continue;
                if (!bound)
                {
                    lsel::Aabb bounds;
                    lsel::TransformAabb(gm->boundsMin, gm->boundsMax, &item.world.m[0][0], bounds);
                    lsel::PackPoints(m_point_lights.empty() ? nullptr : m_point_lights.data(),
                                     m_point_lights.size(), bounds, m_point_pos, m_point_col);
                    lsel::PackSpots(m_spot_lights.empty() ? nullptr : m_spot_lights.data(),
                                    m_spot_lights.size(), bounds, m_spot_pos, m_spot_dir, m_spot_col);
                    m_device->SetPixelShaderConstantF(7, &m_point_pos[0][0], 4);
                    m_device->SetPixelShaderConstantF(11, &m_point_col[0][0], 4);
                    m_device->SetPixelShaderConstantF(16, &m_spot_pos[0][0], 2);
                    m_device->SetPixelShaderConstantF(18, &m_spot_dir[0][0], 2);
                    m_device->SetPixelShaderConstantF(20, &m_spot_col[0][0], 2);
                    const D3DMATRIX wvp = Multiply(item.world, vp);
                    m_device->SetVertexShaderConstantF(0, &wvp.m[0][0], 4);
                    m_device->SetVertexShaderConstantF(4, &item.world.m[0][0], 4);
                    BindMeshForDraw(gm, &item.skin_palette, item.morph_vb);
                    bound = true;
                }
                if (!blendPass)
                    m_device->SetRenderState(D3DRS_ALPHATESTENABLE,
                                             s.alpha == AlphaKind::Cutout ? TRUE : FALSE);
                const float bump[4] = { s.normalHasHeight ? 0.08f : 0.0f, 0.0f, 0.0f, 0.0f };
                m_device->SetPixelShaderConstantF(5, bump, 1);
                m_device->SetTexture(0, s.diffuse  ? s.diffuse  : m_def_white);
                m_device->SetTexture(1, s.normal   ? s.normal   : m_def_normal);
                m_device->SetTexture(2, s.specular ? s.specular : m_def_black);
                m_device->SetTexture(3, s.emissive ? s.emissive : m_def_black);
                m_device->SetTexture(4, s.metallic ? s.metallic : m_def_black);
                m_device->SetTexture(6, s.clearcoat ? s.clearcoat : m_def_black);
                m_device->SetTexture(7, s.roughness ? s.roughness : m_def_black);
                m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                               gm->vertexCount, s.indexStart, s.indexCount / 3);
            }
        }
    }
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
}

// Draw the "Skybox" attribute's image as the background: a screen quad whose
// pixels each work out the direction they look down, then read that direction
// out of an equirectangular image (skybox.hlsl).
//
// depth_test is on for the main pass — the quad sits at the far plane, so the
// depth buffer left behind by the opaque pass rejects it wherever geometry
// already drew and the sky costs nothing there. The reflection capture passes
// false: it draws the sky first, into a face that has nothing in it yet.
//
// Called inside the caller's BeginScene. Only the states it changes are put
// back, matching how every other pass here behaves.
void SceneRenderer::RenderSkybox(const D3DMATRIX& view, const D3DMATRIX& proj, bool depth_test)
{
    if (m_sky_path.empty() || !m_sky_vs || !m_sky_ps)
        return;

    // Load on first use and whenever the attribute points somewhere else.
    if (m_sky_loaded != m_sky_path)
    {
        if (m_sky_tex) { m_sky_tex->Release(); m_sky_tex = nullptr; }
        const std::filesystem::path abs = m_project_root / m_sky_path;
        m_sky_tex = mesh::LoadTexture(m_device, abs.string(), nullptr, nullptr);
        m_sky_loaded = m_sky_path;
        if (!m_sky_tex)
            applog::Warn("Skybox image failed to load: " + m_sky_path);
    }
    if (!m_sky_tex)
        return;

    float view16[16], proj16[16];
    std::memcpy(view16, &view, sizeof(view16));
    std::memcpy(proj16, &proj, sizeof(proj16));
    float right[3], up[3], fwd[3];
    sky::Basis(view16, proj16, right, up, fwd);

    struct QuadVtx { float x, y, z, u, v; };
    static const QuadVtx quad[4] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
    };

    const float vsR[4] = { right[0], right[1], right[2], 0.0f };
    const float vsU[4] = { up[0],    up[1],    up[2],    0.0f };
    const float vsF[4] = { fwd[0],   fwd[1],   fwd[2],   0.0f };
    const float psRot[4] = { m_sky_rotation * sky::kRotToTurns, 0.0f, 0.0f, 0.0f };

    m_device->SetVertexShader(m_sky_vs);
    m_device->SetPixelShader(m_sky_ps);
    m_device->SetVertexShaderConstantF(0, vsR, 1);
    m_device->SetVertexShaderConstantF(1, vsU, 1);
    m_device->SetVertexShaderConstantF(2, vsF, 1);
    m_device->SetPixelShaderConstantF(0, psRot, 1);
    m_device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    m_device->SetTexture(0, m_sky_tex);
    // WRAP on u so the seam behind the camera closes; CLAMP on v so the top and
    // bottom rows do not bleed into each other at the poles. MIPFILTER off is
    // belt-and-braces — the shader takes level 0 explicitly.
    m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    m_device->SetRenderState(D3DRS_ZENABLE, depth_test ? TRUE : FALSE);
    m_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);

    m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVtx));

    m_device->SetTexture(0, nullptr);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
}

// Fill m_env_blur from the sharp capture by running env_blur.hlsl over every
// face of every level. Each level filters the one below it, repeatedly, so the
// blur compounds out of narrow gap-free steps (see envcube::BlurPasses).
//
// Two rules this depends on:
//   - m_env_blur and m_env_blur_tmp take turns as source and destination, since
//     D3D9 cannot sample a texture while rendering into another of its levels.
//   - Levels run outermost: taps cross face boundaries, so all six faces of
//     level L-1 must exist before any face of level L.
//
// Level 0 uses zero tap spacing, making it a plain copy — and the cheapest check
// that the shader's direction math is right, since it must come out identical to
// the capture.
//
// Runs inside the capture's BeginScene, after the six faces are drawn.
void SceneRenderer::BuildEnvBlurChain()
{
    if (!m_env_dyn || !m_env_blur || !m_env_blur_tmp || !m_env_blur_vs || !m_env_blur_ps)
        return;

    struct QuadVtx { float x, y, z, u, v; };
    static const QuadVtx quad[4] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
    };

    m_device->SetDepthStencilSurface(nullptr);
    m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    m_device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    m_device->SetVertexShader(m_env_blur_vs);
    m_device->SetPixelShader(m_env_blur_ps);
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
        m_device->SetVertexShaderConstantF(0, half, 1);

        // Pass 0 halves resolution, the rest re-filter in place. The cubes
        // alternate and the count is even, so the last write always lands in
        // m_env_blur — what s5 binds and what the next level reads.
        for (int pass = 0; pass < passes; ++pass)
        {
            const bool to_blur = (level == 0) || (pass & 1);
            IDirect3DCubeTexture9* dst = to_blur ? m_env_blur : m_env_blur_tmp;
            IDirect3DCubeTexture9* src = (level == 0) ? m_env_dyn
                                       : (to_blur ? m_env_blur_tmp : m_env_blur);
            const float src_lod = (pass == 0) ? (float)(level - 1) : (float)level;
            const float step[4] = { (pass == 0) ? envcube::TapStep(level)
                                                : envcube::TapStepAt(level),
                                    (level == 0) ? 0.0f : src_lod,
                                    envcube::EdgeFixup(level), 0.0f };
            m_device->SetTexture(0, src);
            m_device->SetPixelShaderConstantF(3, step, 1);
            for (int f = 0; f < 6; ++f)
            {
                IDirect3DSurface9* surf = nullptr;
                if (FAILED(dst->GetCubeMapSurface((D3DCUBEMAP_FACES)f, level, &surf)))
                    continue;
                float r3[3], u3[3], f3[3];
                envcube::FaceBasis(f, r3, u3, f3);
                const float face_r[4] = { r3[0], r3[1], r3[2], 0.0f };
                const float face_u[4] = { u3[0], u3[1], u3[2], 0.0f };
                const float face_f[4] = { f3[0], f3[1], f3[2], 0.0f };
                m_device->SetPixelShaderConstantF(0, face_r, 1);
                m_device->SetPixelShaderConstantF(1, face_u, 1);
                m_device->SetPixelShaderConstantF(2, face_f, 1);
                m_device->SetRenderTarget(0, surf);
                D3DVIEWPORT9 vp = { 0, 0, (DWORD)size, (DWORD)size, 0.0f, 1.0f };
                m_device->SetViewport(&vp);
                m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVtx));
                surf->Release();
            }
        }
    }
    m_device->SetTexture(0, nullptr);
    m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
}

// The bloom chain: extract the glow (scene rgb * exported alpha mask) into a
// quarter-res target, blur it wide (separable H+V), and add it back over the
// scene. The wide soft halo is what makes emissive surfaces read as glowing
// (streetlamp corona) instead of merely rendering at full brightness.
void SceneRenderer::RenderBloom()
{
    if (!m_bloomASurf || !m_bloomBSurf || !m_rt || !m_rtSurface ||
        !m_bloom_bright_vs || !m_bloom_bright_ps ||
        !m_bloom_blur_vs   || !m_bloom_blur_ps ||
        !m_bloom_combine_vs || !m_bloom_combine_ps)
        return;
    if (FAILED(m_device->BeginScene()))
        return;

    struct QuadVtx { float x, y, z, u, v; };
    static const QuadVtx quad[4] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
    };

    m_device->SetDepthStencilSurface(nullptr);
    m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    m_device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    const int bw = (m_width  / 4 > 0) ? m_width  / 4 : 1;
    const int bh = (m_height / 4 > 0) ? m_height / 4 : 1;

    auto pass = [&](IDirect3DSurface9* dst, int dw, int dh,
                    IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps,
                    IDirect3DTexture9* src)
    {
        m_device->SetRenderTarget(0, dst);
        D3DVIEWPORT9 vp = { 0, 0, (DWORD)dw, (DWORD)dh, 0.0f, 1.0f };
        m_device->SetViewport(&vp);
        const float half[4] = { 1.0f / (float)dw, 1.0f / (float)dh, 0.0f, 0.0f };
        m_device->SetVertexShaderConstantF(0, half, 1);
        m_device->SetVertexShader(vs);
        m_device->SetPixelShader(ps);
        m_device->SetTexture(0, src);
        m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVtx));
    };

    // Glow extract (rgb * mask) into quarter-res, then the separable blur. The
    // 1.5-texel tap spacing widens the 9-tap kernel; two passes at quarter res
    // give a halo tens of full-res pixels across.
    pass(m_bloomASurf, bw, bh, m_bloom_bright_vs, m_bloom_bright_ps, m_rt);
    const float stepH[4] = { 1.5f / (float)bw, 0.0f, 0.0f, 0.0f };
    m_device->SetPixelShaderConstantF(0, stepH, 1);
    pass(m_bloomBSurf, bw, bh, m_bloom_blur_vs, m_bloom_blur_ps, m_bloomA);
    const float stepV[4] = { 0.0f, 1.5f / (float)bh, 0.0f, 0.0f };
    m_device->SetPixelShaderConstantF(0, stepV, 1);
    pass(m_bloomASurf, bw, bh, m_bloom_blur_vs, m_bloom_blur_ps, m_bloomB);

    // Additive combine over the resolved scene (alpha saturates to opaque).
    const float gain[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    m_device->SetPixelShaderConstantF(0, gain, 1);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_ONE);
    m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    pass(m_rtSurface, m_width, m_height, m_bloom_combine_vs, m_bloom_combine_ps, m_bloomA);

    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    m_device->SetTexture(0, nullptr);
    m_device->EndScene();
}

// Create (or resize) the three L8 plane textures for a video stream and upload
// the frame when its id changed. MANAGED pool: uploads are a plain LockRect row
// copy and the textures survive device resets.
SceneRenderer::GifFrames* SceneRenderer::EnsureGifFrames(const std::string& path_abs)
{
    auto found = m_gif_frames.find(path_abs);
    if (found != m_gif_frames.end())
        return found->second.frames.empty() ? nullptr : &found->second;

    // Insert first, so a file that fails to decode is remembered as an empty
    // entry and is not re-read every frame.
    GifFrames& cached = m_gif_frames[path_abs];
    if (!m_device)
        return nullptr;

    std::vector<unsigned char> bytes;
    if (FILE* f = _wfopen(std::filesystem::path(path_abs).wstring().c_str(), L"rb"))
    {
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0)
        {
            bytes.resize((size_t)size);
            if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size())
                bytes.clear();
        }
        std::fclose(f);
    }
    if (bytes.empty())
        return nullptr;

    // stb composites GIF frame disposal, so each frame comes back as a full
    // canvas — the same decode the cooker does, so the frames match the
    // console's slices exactly.
    int* delays = nullptr;
    int w = 0, h = 0, frame_count = 0, comp = 0;
    unsigned char* frames = stbi_load_gif_from_memory(
        bytes.data(), (int)bytes.size(), &delays, &w, &h, &frame_count, &comp, 4);
    if (!frames || frame_count <= 0 || w <= 0 || h <= 0)
    {
        if (delays) stbi_image_free(delays);
        if (frames) stbi_image_free(frames);
        return nullptr;
    }

    cached.info.frameW     = w;
    cached.info.frameH     = h;
    cached.info.frameCount = frame_count;
    cached.info.delaysMs.resize((size_t)frame_count);
    const size_t frame_pixels = (size_t)w * (size_t)h;
    for (int i = 0; i < frame_count; ++i)
    {
        cached.info.delaysMs[(size_t)i] =
            (unsigned int)(delays && delays[i] > 0 ? delays[i] : 0);
        IDirect3DTexture9* tex = mesh::CreateTextureFromRgba(
            m_device, frames + frame_pixels * 4 * (size_t)i, w, h);
        if (!tex)
        {
            for (IDirect3DTexture9* t : cached.frames) t->Release();
            cached.frames.clear();
            stbi_image_free(delays);
            stbi_image_free(frames);
            return nullptr;
        }
        cached.frames.push_back(tex);
    }
    stbi_image_free(delays);
    stbi_image_free(frames);
    return &cached;
}

SceneRenderer::VideoTex* SceneRenderer::EnsureVideoTex(const std::string& key, const vid::Frame& frame)
{
    VideoTex* t = nullptr;
    for (VideoTex& vt : m_video_tex)
        if (vt.key == key) { t = &vt; break; }
    if (!t)
    {
        m_video_tex.push_back(VideoTex());
        t = &m_video_tex.back();
        t->key = key;
    }

    if (t->y && (t->yW != frame.yW || t->yH != frame.yH))
    {
        t->y->Release();  t->y = nullptr;
        t->cb->Release(); t->cb = nullptr;
        t->cr->Release(); t->cr = nullptr;
    }
    if (!t->y)
    {
        if (FAILED(m_device->CreateTexture(frame.yW, frame.yH, 1, 0, D3DFMT_L8,
                                           D3DPOOL_MANAGED, &t->y, nullptr)) ||
            FAILED(m_device->CreateTexture(frame.cW, frame.cH, 1, 0, D3DFMT_L8,
                                           D3DPOOL_MANAGED, &t->cb, nullptr)) ||
            FAILED(m_device->CreateTexture(frame.cW, frame.cH, 1, 0, D3DFMT_L8,
                                           D3DPOOL_MANAGED, &t->cr, nullptr)))
        {
            if (t->y)  { t->y->Release();  t->y = nullptr; }
            if (t->cb) { t->cb->Release(); t->cb = nullptr; }
            return nullptr;
        }
        t->yW = frame.yW; t->yH = frame.yH;
        t->cW = frame.cW; t->cH = frame.cH;
        t->lastFrame = frame.frameId - 1; // force the first upload
    }

    if (t->lastFrame != frame.frameId)
    {
        auto upload = [&](IDirect3DTexture9* tex, const unsigned char* src, int w, int h)
        {
            D3DLOCKED_RECT lr;
            if (FAILED(tex->LockRect(0, &lr, nullptr, 0)))
                return;
            for (int row = 0; row < h; ++row)
                std::memcpy((unsigned char*)lr.pBits + row * lr.Pitch, src + row * w, w);
            tex->UnlockRect(0);
        };
        upload(t->y,  frame.y,  frame.yW, frame.yH);
        upload(t->cb, frame.cb, frame.cW, frame.cH);
        upload(t->cr, frame.cr, frame.cW, frame.cH);
        t->lastFrame = frame.frameId;
    }
    return t;
}

SceneRenderer::PreviewFont* SceneRenderer::EnsurePreviewFont(const std::string& path)
{
    PreviewFont& output = m_text_fonts[path];
    if (output.attempted)
        return output.atlas ? &output : nullptr;
    output.attempted = true;

    std::ifstream file(path, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    if (bytes.empty())
        return nullptr;
    stbtt_fontinfo info;
    const int offset = stbtt_GetFontOffsetForIndex(&bytes[0], 0);
    if (offset < 0 || !stbtt_InitFont(&info, &bytes[0], offset) ||
        stbtt_FindGlyphIndex(&info, '?') == 0)
        return nullptr;

    const int atlas_width = 1024, atlas_height = 1024, padding = 5;
    const float source_size = 48.0f;
    const float scale = stbtt_ScaleForPixelHeight(&info, source_size);
    std::vector<unsigned char> pixels((size_t)atlas_width * atlas_height, 0);
    int next_x = 1, next_y = 1, row_height = 0;
    for (unsigned int codepoint = 0x20; codepoint <= 0xFF; ++codepoint)
    {
        if ((codepoint > 0x7E && codepoint < 0xA0) ||
            stbtt_FindGlyphIndex(&info, (int)codepoint) == 0)
            continue;
        int advance = 0, left_bearing = 0;
        stbtt_GetCodepointHMetrics(&info, (int)codepoint, &advance, &left_bearing);
        text::Glyph glyph;
        glyph.codepoint = codepoint;
        glyph.advance = advance * scale;
        int width = 0, height = 0, x_offset = 0, y_offset = 0;
        unsigned char* bitmap = stbtt_GetCodepointSDF(
            &info, scale, (int)codepoint, padding, 128, 128.0f / padding,
            &width, &height, &x_offset, &y_offset);
        if (bitmap && width > 0 && height > 0)
        {
            if (next_x + width + 1 > atlas_width)
            { next_x = 1; next_y += row_height + 1; row_height = 0; }
            if (next_y + height + 1 > atlas_height)
            { stbtt_FreeSDF(bitmap, info.userdata); return nullptr; }
            for (int row = 0; row < height; ++row)
                std::memcpy(&pixels[(size_t)(next_y + row) * atlas_width + next_x],
                            bitmap + (size_t)row * width, width);
            glyph.bearingX = (float)x_offset;
            glyph.bearingY = (float)-y_offset;
            glyph.width = (float)width; glyph.height = (float)height;
            glyph.u0 = (float)next_x / atlas_width; glyph.v0 = (float)next_y / atlas_height;
            glyph.u1 = (float)(next_x + width) / atlas_width;
            glyph.v1 = (float)(next_y + height) / atlas_height;
            next_x += width + 1;
            row_height = (std::max)(row_height, height);
        }
        if (bitmap) stbtt_FreeSDF(bitmap, info.userdata);
        output.metrics.glyphs.push_back(glyph);
    }
    for (const text::Glyph& left : output.metrics.glyphs)
        for (const text::Glyph& right : output.metrics.glyphs)
        {
            const int amount = stbtt_GetCodepointKernAdvance(
                &info, (int)left.codepoint, (int)right.codepoint);
            if (amount != 0)
            {
                text::KerningPair pair;
                pair.left = left.codepoint; pair.right = right.codepoint;
                pair.advance = amount * scale;
                output.metrics.kerning.push_back(pair);
            }
        }
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    output.metrics.sourcePixelSize = source_size;
    output.metrics.ascent = ascent * scale;
    output.metrics.descent = descent * scale;
    output.metrics.lineHeight = (ascent - descent + line_gap) * scale;
    if (FAILED(m_device->CreateTexture(atlas_width, atlas_height, 1, 0, D3DFMT_A8,
                                       D3DPOOL_MANAGED, &output.atlas, nullptr)))
        return nullptr;
    D3DLOCKED_RECT locked = {};
    if (FAILED(output.atlas->LockRect(0, &locked, nullptr, 0)))
    { output.atlas->Release(); output.atlas = nullptr; return nullptr; }
    for (int row = 0; row < atlas_height; ++row)
        std::memcpy((unsigned char*)locked.pBits + row * locked.Pitch,
                    &pixels[(size_t)row * atlas_width], atlas_width);
    output.atlas->UnlockRect(0);
    return &output;
}

// The Audio attribute reconciler (the Vulkan engine's shape): every playing
// attribute becomes a wanted stream; edit mode passes nothing, which tears
// every voice down — audio exists only inside the Play preview. The listener
// is the current camera; spatial emitters ride the object's live pose.
// The item's stream key with the script override's restart generation folded
// in (audio.play on a stopped clip decodes a fresh stream from the top).
std::string SceneRenderer::AudioKeyFor(const AudioItem& item) const
{
    const auto ov = m_audio_overrides.find(item.object);
    if (ov == m_audio_overrides.end() || ov->second.gen == 0)
        return item.key;
    return item.key + "#g" + std::to_string(ov->second.gen);
}

void SceneRenderer::UpdateAudio(float dt)
{
    std::vector<aud::Want> wants;
    if (m_phys_on)
    {
        for (const AudioItem& item : m_audio_items)
        {
            // Script overrides win over the authored attribute state.
            const auto ov = m_audio_overrides.find(item.object);
            const bool play = (ov != m_audio_overrides.end() && ov->second.play >= 0)
                                  ? ov->second.play != 0 : item.play;
            if (!play)
                continue;
            aud::Want w;
            w.key     = AudioKeyFor(item);
            w.path    = item.path_abs;
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
            for (int k = 0; k < 3; ++k) w.pos[k] = item.pos[k];
            for (const phys::Pose& p : m_phys_poses)
                if (p.objectIndex == item.object_index)
                {
                    w.pos[0] = p.matrix[12];
                    w.pos[1] = p.matrix[13];
                    w.pos[2] = p.matrix[14];
                    break;
                }
            wants.push_back(w);
        }
    }

    // A face clip carries its own dialogue, and the layer clocks itself off
    // that stream's position -- so the stream has to be wanted for as long as
    // the clip plays, independently of any authored Audio attribute.
    for (const DrawItem& item : m_draw_items)
    {
        const auto entry = m_face_instances.find(item.object_index);
        if (entry == m_face_instances.end())
            continue;
        const FaceInstance& instance = entry->second;
        if (!instance.layer.ClipPlaying() || instance.audio_path.empty())
            continue;
        aud::Want w;
        w.key      = FaceAudioKey(item.object_index, instance.audio_gen);
        w.path     = (m_project_root / instance.audio_path).string();
        w.audioClass = aud::AudioDialogue;
        w.priority = 1;
        w.spatial  = true;
        w.pos[0] = item.world._41; w.pos[1] = item.world._42; w.pos[2] = item.world._43;
        wants.push_back(w);
    }

    // Listener = the current view: LookAtLH's basis lives in the view matrix
    // columns (col 2 = forward, col 1 = up), position is the camera eye.
    aud::ListenerState listener;
    listener.pos[0] = m_eye[0]; listener.pos[1] = m_eye[1]; listener.pos[2] = m_eye[2];
    listener.fwd[0] = m_view._13; listener.fwd[1] = m_view._23; listener.fwd[2] = m_view._33;
    listener.up[0]  = m_view._12; listener.up[1]  = m_view._22; listener.up[2]  = m_view._32;

    m_audio.Update(wants.empty() ? nullptr : &wants[0], (int)wants.size(),
                   dt, &listener);
}

// The item's stream key with the script override's restart generation folded
// in — bumping the generation changes the key, so the player closes the old
// stream and decodes a fresh one from the top (that's how video.play replays
// a finished/stopped video).
std::string SceneRenderer::VideoKeyFor(const VideoItem& item) const
{
    const auto ov = m_video_overrides.find(item.object);
    if (ov == m_video_overrides.end() || ov->second.gen == 0)
        return item.key;
    return item.key + "#g" + std::to_string(ov->second.gen);
}
int SceneRenderer::VideoModeFor(const VideoItem& item) const
{
    const auto ov = m_video_overrides.find(item.object);
    return ov != m_video_overrides.end() ? ov->second.mode : item.play_mode;
}

// Reconcile video streams, then draw static images and video frames together
// over the finished scene. One priority list preserves ordering across types.
void SceneRenderer::RenderOverlay(float dt)
{
    // Reconcile + advance the decoder even when nothing draws (teardown path).
    // Keys/modes go through the script override here — after this frame's
    // scripts ran — so a video.play/stop lands on the same frame's draw.
    std::vector<std::string> keys;
    std::vector<vid::Want> wants;
    for (const VideoItem& item : m_video_items)
    {
        keys.push_back(VideoKeyFor(item));
        vid::Want w;
        w.key      = keys.back();
        w.path     = item.path_abs;
        w.playMode = VideoModeFor(item);
        // Audio only in the Play preview; the flip restarts the stream, which
        // is what puts the frozen first frame back after Stop.
        w.audible  = m_phys_on;
        w.volume   = item.volume;
        w.muted    = item.muted;
        wants.push_back(w);
    }
    m_video.Update(wants.empty() ? nullptr : &wants[0], (int)wants.size(), dt);

    // Drop plane textures for streams that vanished.
    for (VideoTex& t : m_video_tex)
        t.used = false;
    for (size_t i = 0; i < m_video_items.size(); ++i)
        for (VideoTex& t : m_video_tex)
            if (t.key == keys[i]) { t.used = true; break; }
    for (size_t i = 0; i < m_video_tex.size(); )
    {
        if (!m_video_tex[i].used)
        {
            VideoTex& t = m_video_tex[i];
            if (t.y)  t.y->Release();
            if (t.cb) t.cb->Release();
            if (t.cr) t.cr->Release();
            m_video_tex.erase(m_video_tex.begin() + i);
        }
        else
            ++i;
    }

    if ((m_image_items.empty() && m_color_items.empty() && m_text_items.empty()
         && m_video_items.empty()) || !m_rtSurface)
        return;

    struct OverlayRef { int kind; int index; int priority; int sequence; };
    std::vector<OverlayRef> order;
    for (int i = 0; i < (int)m_image_items.size(); ++i)
        order.push_back({0, i, m_image_items[i].priority, m_image_items[i].sequence});
    for (int i = 0; i < (int)m_color_items.size(); ++i)
        order.push_back({3, i, m_color_items[i].priority, m_color_items[i].sequence});
    for (int i = 0; i < (int)m_text_items.size(); ++i)
        order.push_back({1, i, m_text_items[i].priority, m_text_items[i].sequence});
    for (int i = 0; i < (int)m_video_items.size(); ++i)
        order.push_back({2, i, m_video_items[i].priority, m_video_items[i].sequence});
    std::stable_sort(order.begin(), order.end(),
                     [](const OverlayRef& a, const OverlayRef& b)
                     {
                         return a.priority != b.priority
                             ? a.priority > b.priority : a.sequence < b.sequence;
                     });

    if (FAILED(m_device->BeginScene()))
        return;

    m_device->SetRenderTarget(0, m_rtSurface);
    m_device->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = { 0, 0, (DWORD)m_width, (DWORD)m_height, 0.0f, 1.0f };
    m_device->SetViewport(&vp);

    m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    // Preserve the target's alpha — ImGui samples it (bloom left it opaque).
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    m_device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    for (DWORD st = 0; st < 3; ++st)
    {
        m_device->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        m_device->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    const float halfTexel[4] = { 1.0f / (float)m_width, 1.0f / (float)m_height, 0.0f, 0.0f };

    struct QuadVtx { float x, y, z, u, v; };
    for (const OverlayRef& ref : order)
    {
        if (ref.kind == 0)
        {
            if (!m_image_vs || !m_image_ps)
                continue;
            const ImageItem& item = m_image_items[ref.index];

            // A .gif animates: the clock picks a frame and the draw binds that
            // frame's own texture. Everything downstream — quad, UVs, shader,
            // blending — is identical to a still image.
            IDirect3DTexture9* texture = nullptr;
            float sourceW = 0.0f, sourceH = 0.0f;
            if (gifanim::IsGifPath(item.path_abs))
            {
                GifFrames* gif = EnsureGifFrames(item.path_abs);
                if (!gif)
                    continue;
                gifanim::Playback& play = m_gif_play[item.key];
                // dt is 0 unless Play is running, which is what freezes a GIF on
                // frame 1 while the scene is being edited — matching the Video
                // attribute rather than the Vulkan engine, which animates live.
                gifanim::Advance(play, gif->info, item.play_mode, dt);
                const int index = (play.frame >= 0 && play.frame < (int)gif->frames.size())
                    ? play.frame : 0;
                texture = gif->frames[(size_t)index];
                sourceW = (float)gif->info.frameW;
                sourceH = (float)gif->info.frameH;
            }
            else
            {
                auto found = m_image_textures.find(item.path_abs);
                if (found == m_image_textures.end())
                    found = m_image_textures.emplace(item.path_abs,
                        mesh::LoadTexture(m_device, item.path_abs, nullptr, nullptr)).first;
                texture = found->second;
                if (!texture)
                    continue;
                D3DSURFACE_DESC desc = {};
                if (FAILED(texture->GetLevelDesc(0, &desc)))
                    continue;
                sourceW = (float)desc.Width;
                sourceH = (float)desc.Height;
            }

            float x0 = -1.0f, y0 = 1.0f, x1 = 1.0f, y1 = -1.0f;
            if (!item.stretch)
            {
                const float width = item.w;
                const float height = item.lock_aspect && sourceW > 0.0f
                    ? width * sourceH / sourceW : item.h;
                x0 = item.x / 1280.0f * 2.0f - 1.0f;
                x1 = (item.x + width) / 1280.0f * 2.0f - 1.0f;
                y0 = 1.0f - item.y / 720.0f * 2.0f;
                y1 = 1.0f - (item.y + height) / 720.0f * 2.0f;
            }
            const QuadVtx quad[4] = {
                { x0, y0, 0.0f, 0.0f, 0.0f }, { x1, y0, 0.0f, 1.0f, 0.0f },
                { x0, y1, 0.0f, 0.0f, 1.0f }, { x1, y1, 0.0f, 1.0f, 1.0f },
            };
            const float tint[4] = { item.tint[0], item.tint[1], item.tint[2], item.alpha };
            m_device->SetVertexShader(m_image_vs);
            m_device->SetPixelShader(m_image_ps);
            m_device->SetVertexShaderConstantF(0, halfTexel, 1);
            m_device->SetPixelShaderConstantF(0, tint, 1);
            m_device->SetTexture(0, texture);
            m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVtx));
            continue;
        }

        if (ref.kind == 3)
        {
            if (!m_color_vs || !m_color_ps)
                continue;
            const ColorItem& item = m_color_items[ref.index];

            float x0 = -1.0f, y0 = 1.0f, x1 = 1.0f, y1 = -1.0f;
            if (!item.stretch)
            {
                x0 = item.x / 1280.0f * 2.0f - 1.0f;
                x1 = (item.x + item.w) / 1280.0f * 2.0f - 1.0f;
                y0 = 1.0f - item.y / 720.0f * 2.0f;
                y1 = 1.0f - (item.y + item.h) / 720.0f * 2.0f;
            }
            const QuadVtx quad[4] = {
                { x0, y0, 0.0f, 0.0f, 0.0f }, { x1, y0, 0.0f, 1.0f, 0.0f },
                { x0, y1, 0.0f, 0.0f, 1.0f }, { x1, y1, 0.0f, 1.0f, 1.0f },
            };
            const float rgba[4] = { item.rgb[0], item.rgb[1], item.rgb[2], item.alpha };
            m_device->SetVertexShader(m_color_vs);
            m_device->SetPixelShader(m_color_ps);
            m_device->SetVertexShaderConstantF(0, halfTexel, 1);
            m_device->SetPixelShaderConstantF(0, rgba, 1);
            m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVtx));
            continue;
        }

        if (ref.kind == 1)
        {
            if (!m_text_vs || !m_text_ps)
                continue;
            const TextItem& item = m_text_items[ref.index];
            PreviewFont* font = EnsurePreviewFont(item.font_path_abs);
            if (!font)
                continue;
            text::Layout layout;
            const auto override_value = m_text_overrides.find(item.object);
            const std::string& value = override_value != m_text_overrides.end()
                ? override_value->second : item.value;
            if (!text::BuildLayout(font->metrics, value, item.font_size,
                                   item.lock_aspect ? 0.0f : item.w, layout) || layout.glyphs.empty())
                continue;
            float fit_scale = 1.0f;
            if (item.lock_aspect && layout.width > 0.0f && layout.height > 0.0f)
                fit_scale = (std::min)(item.w / layout.width, item.h / layout.height);
            const float glyph_scale = item.font_size / font->metrics.sourcePixelSize * fit_scale;
            std::vector<QuadVtx> vertices;
            vertices.reserve(layout.glyphs.size() * 6);
            for (const text::PositionedGlyph& positioned : layout.glyphs)
            {
                const text::Glyph& glyph = font->metrics.glyphs[positioned.glyphIndex];
                const float px0 = item.x + positioned.x * fit_scale;
                const float py0 = item.y + positioned.y * fit_scale;
                const float px1 = px0 + glyph.width * glyph_scale;
                const float py1 = py0 + glyph.height * glyph_scale;
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
            const float tint[4] = { item.color[0], item.color[1], item.color[2], item.alpha };
            m_device->SetVertexShader(m_text_vs);
            m_device->SetPixelShader(m_text_ps);
            m_device->SetVertexShaderConstantF(0, halfTexel, 1);
            m_device->SetPixelShaderConstantF(0, tint, 1);
            m_device->SetTexture(0, font->atlas);
            m_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)layout.glyphs.size() * 2,
                                      &vertices[0], sizeof(QuadVtx));
            continue;
        }

        if (!m_video_vs || !m_video_ps)
            continue;
        const int idx = ref.index;
        const VideoItem* item = &m_video_items[idx];
        vid::Frame f;
        if (!m_video.GetFrame(keys[idx], f))
            continue;
        VideoTex* t = EnsureVideoTex(keys[idx], f);
        if (!t)
            continue;

        // Quad rect in clip space, from the 1280x720 reference mapping.
        float x0 = -1.0f, y0 = 1.0f, x1 = 1.0f, y1 = -1.0f;
        if (!item->stretch)
        {
            const float w = item->w;
            const float h = item->lock_aspect && f.width > 0
                                ? w * (float)f.height / (float)f.width
                                : item->h;
            x0 = item->x / 1280.0f * 2.0f - 1.0f;
            x1 = (item->x + w) / 1280.0f * 2.0f - 1.0f;
            y0 = 1.0f - item->y / 720.0f * 2.0f;
            y1 = 1.0f - (item->y + h) / 720.0f * 2.0f;
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

        const float tint[4] = { item->tint[0], item->tint[1], item->tint[2], item->alpha };
        m_device->SetVertexShader(m_video_vs);
        m_device->SetPixelShader(m_video_ps);
        m_device->SetVertexShaderConstantF(0, halfTexel, 1);
        m_device->SetPixelShaderConstantF(0, tint, 1);
        m_device->SetTexture(0, t->y);
        m_device->SetTexture(1, t->cb);
        m_device->SetTexture(2, t->cr);
        m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVtx));
    }

    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    m_device->SetTexture(0, nullptr);
    m_device->SetTexture(1, nullptr);
    m_device->SetTexture(2, nullptr);
    m_device->EndScene();
}

// --- gui::HostAssets ------------------------------------------------------
// The GUI core is entirely shared code; these four functions are the only part
// of it that knows about the editor. Paths arrive project-relative (that is
// what a menu script writes and what the console cook records), so they are
// resolved against the project root here and nowhere else.

int SceneRenderer::AcquireTexture(const char* relPath)
{
    if (!relPath || !*relPath || m_project_root.empty())
        return -1;
    const std::string key = relPath;
    const auto cached = m_gui_texture_ids.find(key);
    if (cached != m_gui_texture_ids.end())
        return cached->second;

    const std::string abs = (m_project_root / relPath).string();
    auto found = m_image_textures.find(abs);
    if (found == m_image_textures.end())
        found = m_image_textures.emplace(abs,
            mesh::LoadTexture(m_device, abs, nullptr, nullptr)).first;
    if (!found->second)
    {
        // Remember the failure as "no id" so a bad path costs one load attempt,
        // not one per frame.
        m_gui_texture_ids[key] = -1;
        return -1;
    }
    const int id = (int)m_gui_textures.size();
    m_gui_textures.push_back(found->second);
    m_gui_texture_ids[key] = id;
    return id;
}

const gifanim::Info* SceneRenderer::AcquireGifFrame(const char* relPath, int frame,
                                                    int& outTextureId, float& outGifSlice)
{
    outTextureId = -1;
    outGifSlice  = -1.0f; // PC D3D9 has no array textures: always a plain 2D bind
    if (!relPath || !*relPath || m_project_root.empty() || !gifanim::IsGifPath(relPath))
        return NULL;

    GifFrames* gif = EnsureGifFrames((m_project_root / relPath).string());
    if (!gif || gif->frames.empty())
        return NULL;

    const int index = (frame >= 0 && frame < (int)gif->frames.size()) ? frame : 0;
    // Each frame gets its own id, cached under "<path>#<frame>", so ids stay
    // stable for the context's lifetime as HostAssets requires.
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "#%d", index);
    const std::string key = std::string(relPath) + suffix;
    const auto cached = m_gui_texture_ids.find(key);
    if (cached != m_gui_texture_ids.end())
        outTextureId = cached->second;
    else
    {
        outTextureId = (int)m_gui_textures.size();
        m_gui_textures.push_back(gif->frames[(size_t)index]);
        m_gui_texture_ids[key] = outTextureId;
    }
    return &gif->info;
}

bool SceneRenderer::TextureSize(int textureId, int& outWidth, int& outHeight)
{
    if (textureId < 0 || textureId >= (int)m_gui_textures.size())
        return false;
    IDirect3DTexture9* texture = m_gui_textures[textureId];
    D3DSURFACE_DESC desc = {};
    if (!texture || FAILED(texture->GetLevelDesc(0, &desc)))
        return false;
    outWidth = (int)desc.Width;
    outHeight = (int)desc.Height;
    return true;
}

const text::FontMetrics* SceneRenderer::AcquireFont(const char* relPath)
{
    if (!relPath || !*relPath || m_project_root.empty())
        return nullptr;
    PreviewFont* font = EnsurePreviewFont((m_project_root / relPath).string());
    return font ? &font->metrics : nullptr;
}

int SceneRenderer::AcquireFontAtlas(const char* relPath)
{
    if (!relPath || !*relPath || m_project_root.empty())
        return -1;
    const std::string key = relPath;
    const auto cached = m_gui_font_atlas_ids.find(key);
    if (cached != m_gui_font_atlas_ids.end())
        return cached->second;

    PreviewFont* font = EnsurePreviewFont((m_project_root / relPath).string());
    if (!font || !font->atlas)
        return -1; // not cached: EnsurePreviewFont already guards its own retries
    const int id = (int)m_gui_textures.size();
    m_gui_textures.push_back(font->atlas);
    m_gui_font_atlas_ids[key] = id;
    return id;
}

// Draw the GUI tree. Called straight after RenderOverlay — that is, after the
// bloom combine — so menus are never glowed and never tonemapped. The device
// state block mirrors the overlay's proven one; the only difference is that a
// whole batch of quads goes out in one DrawPrimitiveUP.
void SceneRenderer::RenderGui()
{
    if (!m_device || !m_rtSurface || !m_gui_vs || !m_gui_ps)
        return;
    const gui::DrawList& list = m_gui.Emit();
    if (list.Empty())
        return;

    // 1x1 white for solid quads: the shader always samples, so give it
    // something defined. MANAGED, so it rides out a device reset like the
    // overlay's textures.
    if (!m_gui_white)
    {
        if (SUCCEEDED(m_device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8,
                                              D3DPOOL_MANAGED, &m_gui_white, nullptr)))
        {
            D3DLOCKED_RECT locked = {};
            if (SUCCEEDED(m_gui_white->LockRect(0, &locked, nullptr, 0)))
            {
                *(DWORD*)locked.pBits = 0xFFFFFFFF;
                m_gui_white->UnlockRect(0);
            }
        }
    }

    if (FAILED(m_device->BeginScene()))
        return;

    m_device->SetRenderTarget(0, m_rtSurface);
    m_device->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = { 0, 0, (DWORD)m_width, (DWORD)m_height, 0.0f, 1.0f };
    m_device->SetViewport(&vp);

    m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    // Preserve the target's alpha — ImGui samples it (bloom left it opaque).
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    m_device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    const float halfTexel[4] = { 1.0f / (float)m_width, 1.0f / (float)m_height, 0.0f, 0.0f };
    m_device->SetVertexShader(m_gui_vs);
    m_device->SetPixelShader(m_gui_ps);
    m_device->SetVertexShaderConstantF(0, halfTexel, 1);

    struct QuadVtx { float x, y, z, u, v; };
    std::vector<QuadVtx> vertices;

    for (const gui::Batch& batch : list.batches)
    {
        IDirect3DTexture9* texture = m_gui_white;
        if (batch.textureId >= 0 && batch.textureId < (int)m_gui_textures.size())
            texture = m_gui_textures[batch.textureId];
        // A solid quad's sample is multiplied out by the shader's kind select,
        // so it draws correctly with nothing bound. Only textured/glyph batches
        // actually need one — skipping solids here would make a whole menu
        // invisible if the 1x1 white ever failed to allocate.
        if (!texture && batch.kind != gui::QuadSolid)
            continue;

        // Kind select, matching gui.hlsl: Solid (0,0) Texture (0,1) Glyph (1,0).
        const float mode[4] = {
            batch.kind == gui::QuadGlyph   ? 1.0f : 0.0f,
            batch.kind == gui::QuadTexture ? 1.0f : 0.0f, 0.0f, 0.0f };

        vertices.clear();
        vertices.reserve((size_t)batch.count * 6);
        for (int i = 0; i < batch.count; ++i)
        {
            const gui::Quad& q = list.quads[batch.first + i];
            // 1280x720 reference space -> NDC, top-left origin. Same mapping as
            // the Image/Text overlays, so a GUI and an authored overlay line up.
            const float x0 = q.x0 / gui::kRefWidth * 2.0f - 1.0f;
            const float x1 = q.x1 / gui::kRefWidth * 2.0f - 1.0f;
            const float y0 = 1.0f - q.y0 / gui::kRefHeight * 2.0f;
            const float y1 = 1.0f - q.y1 / gui::kRefHeight * 2.0f;
            const QuadVtx tri[6] = {
                { x0, y0, 0.0f, q.u0, q.v0 }, { x1, y0, 0.0f, q.u1, q.v0 }, { x0, y1, 0.0f, q.u0, q.v1 },
                { x0, y1, 0.0f, q.u0, q.v1 }, { x1, y0, 0.0f, q.u1, q.v0 }, { x1, y1, 0.0f, q.u1, q.v1 },
            };
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

    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    m_device->SetTexture(0, nullptr);
    m_device->EndScene();
}

// Draw one standalone shader: pick geometry, build its transform + uniforms,
// set its parsed render states (raw values), and draw. The whole "how a shader
// draws" story is here in one place.
void SceneRenderer::DrawShaderItem(const ShaderItem& item, const CustomShader& shader, const D3DMATRIX& viewProj)
{
    const ShaderState& s = shader.state;
    const D3DMATRIX w = item.world;

    IDirect3DVertexBuffer9* vb = m_quad_vb;
    IDirect3DIndexBuffer9*  ib = m_quad_ib;
    UINT verts = 4, prims = 2;

    if (s.geometry == ShaderState::Volume && m_cube_vb && m_cube_ib)
    {
        vb = m_cube_vb; ib = m_cube_ib; verts = 8; prims = 12;
        // Camera in the cube's local space, so the pixel shader can raymarch it.
        float inverse_world[16];
        hier::AffineInverse(&w.m[0][0], inverse_world);
        D3DMATRIX invW;
        std::memcpy(&invW.m[0][0], inverse_world, sizeof(inverse_world));
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
    const float time4[4]    = { m_time, 0.0f, 0.0f, 0.0f };

    const D3DMATRIX wvp = Multiply(w, viewProj);
    m_device->SetVertexShader(shader.vs);
    m_device->SetPixelShader(shader.ps);
    m_device->SetVertexShaderConstantF(0, &wvp.m[0][0], 4);
    m_device->SetVertexShaderConstantF(4, &w.m[0][0], 4);
    m_device->SetPixelShaderConstantF(0, lightDir, 1);
    m_device->SetPixelShaderConstantF(1, camPos, 1);
    m_device->SetPixelShaderConstantF(2, m_ambient, 1);
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

// A project-relative .lua read whole. Shared by the Play-session load and by
// spawn(), so a spawned object's script comes up the same way an authored one does.
bool SceneRenderer::ReadScriptSource(const std::string& relPath, std::string& out)
{
    std::ifstream in(m_project_root / relPath, std::ios::binary);
    if (!in) { applog::Error("Script not found: " + relPath); return false; }
    std::ostringstream ss; ss << in.rdbuf();
    out = ss.str();
    return true;
}

void SceneRenderer::StartPhysics(SceneFile& scene)
{
    m_play_scene = &scene;
    m_authored_object_count = scene.objects.size();
    m_obj_gen.assign(scene.objects.size(), 1u);
    m_obj_alive.assign(scene.objects.size(), 1);
    m_pending_destroy.clear();
    m_pending_scene.clear();

    m_video_overrides.clear(); // each Play session starts from the authored play modes
    m_audio_overrides.clear();
    m_text_overrides.clear();
    // Drop the edge history so a key still held from the last session (or from
    // clicking Play itself) doesn't read as a press on the first frame.
    m_input.Reset();
    std::vector<phys::BodyDesc> descs;
    m_phys_names.assign(scene.objects.size(), std::string());

    // Seed the live layer from the authored scene. Every entry starts clean, so
    // the draw loop skips the overlay entirely until a script writes something.
    m_live.assign(scene.objects.size(), LiveObject());
    for (size_t i = 0; i < scene.objects.size(); ++i)
    {
        const SceneObject& so = scene.objects[i];
        LiveObject& lo = m_live[i];
        for (int k = 0; k < 3; ++k)
        { lo.pos[k] = so.position[k]; lo.rot[k] = so.rotation[k]; lo.scale[k] = so.scale[k]; }
        lo.visible = so.visible;
        lo.tags    = so.tags;
        // The attribute clone is what obj:get/obj:set read and write. It is
        // seeded here (the host methods have no other route to the authored
        // scene), but stays UNUSED by the draw path until a script writes —
        // has_attrs gates that — so editing a light in the Inspector mid-Play
        // still takes effect.
        lo.attrs = so.attributes;
    }

    // Heap-owned mesh geometry kept alive until Build() copies what it needs.
    std::vector<std::vector<float>*>        geomPos;
    std::vector<std::vector<unsigned int>*> geomIdx;

    for (int i = 0; i < (int)scene.objects.size(); ++i)
    {
        m_phys_names[i] = scene.objects[i].name;
        AppendBodyDescs(scene, i, descs, geomPos, geomIdx);
    }

    m_phys.Build(descs);

    for (std::size_t g = 0; g < geomPos.size(); ++g) delete geomPos[g];
    for (std::size_t g = 0; g < geomIdx.size(); ++g) delete geomIdx[g];
    geomPos.clear();
    geomIdx.clear();
    StartScripts(scene);
}

// One object's Rigid Body / Trigger Volume attributes as neutral descriptors.
// Split out of StartPhysics so spawn() can add a clone's collider to the live
// world through PhysicsWorld::AddBody with exactly the same setup.
void SceneRenderer::AppendBodyDescs(const SceneFile& scene, int objectIndex,
                                    std::vector<phys::BodyDesc>& descs,
                                    std::vector<std::vector<float>*>& geomPos,
                                    std::vector<std::vector<unsigned int>*>& geomIdx)
{
    if (objectIndex < 0 || objectIndex >= (int)scene.objects.size()) return;
    const int i = objectIndex;
    const SceneObject& o = scene.objects[i];

    std::vector<std::string> names(scene.objects.size()), parent_names(scene.objects.size());
    std::vector<int> parents;
    std::vector<hier::Node> nodes(scene.objects.size());
    for (size_t object = 0; object < scene.objects.size(); ++object)
    {
        names[object] = scene.objects[object].name;
        parent_names[object] = scene.objects[object].parent;
    }
    hier::MapParents(names, parent_names, parents);
    for (size_t object = 0; object < scene.objects.size(); ++object)
    {
        for (int k = 0; k < 3; ++k)
        {
            nodes[object].pos[k] = scene.objects[object].position[k];
            nodes[object].rot[k] = scene.objects[object].rotation[k];
            nodes[object].scale[k] = scene.objects[object].scale[k];
        }
        nodes[object].visible = scene.objects[object].visible;
        nodes[object].parent = parents[object];
    }
    std::vector<hier::Resolved> resolved;
    hier::Resolve(nodes, resolved);
    const hier::Resolved& world = resolved[i];

    // The object's 3D Model, used by mesh-shape colliders.
    std::string model_path;
    for (const ObjectAttribute& ma : o.attributes)
        if (ma.type == "3D Model" && !ma.model_path.empty()) { model_path = ma.model_path; break; }

    {
        for (const ObjectAttribute& a : o.attributes)
        {
            const bool rigid = (a.type == "Rigid Body");
            const bool trig  = (a.type == "Trigger Volume");
            if (!rigid && !trig)
                continue;

            phys::BodyDesc d;
            d.objectIndex = i;
            for (int k = 0; k < 3; ++k) { d.pos[k] = world.pos[k]; d.rotEulerDeg[k] = world.rot[k]; }
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
                for (int axis = 0; axis < 3; ++axis)
                    d.lockRotation[axis] = a.phys_lock_rotation[axis];
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
                        (*pv)[vi + 0] *= world.scale[0];
                        (*pv)[vi + 1] *= world.scale[1];
                        (*pv)[vi + 2] *= world.scale[2];
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
}

// Lua scripts: load each object's .lua and run its on_start. The GUI tree lives
// for exactly the Play session, so it starts fresh alongside the VM.
void SceneRenderer::StartScripts(const SceneFile& scene)
{
    m_gui.Begin(this);
    m_script.Begin(&m_phys, this, &m_gui);
    int scriptCount = 0;
    for (int i = 0; i < (int)scene.objects.size(); ++i)
    {
        const SceneObject& o = scene.objects[i];
        for (const ObjectAttribute& a : o.attributes)
        {
            if (a.type != "Script" || a.script_path.empty())
                continue;
            std::string source_text;
            if (!ReadScriptSource(a.script_path, source_text)) continue;
            m_script.LoadScript(i, source_text,
                                std::filesystem::path(a.script_path).filename().string());
            ++scriptCount;
        }
    }
    m_script.Start();

    m_phys_poses.clear();
    m_phys_on = true;
    applog::Info("Physics: play (" + std::to_string((int)scene.objects.size()) +
                 " objects, " + std::to_string(scriptCount) + " scripts)");
}

void SceneRenderer::StopPhysics()
{
    m_video_overrides.clear(); // edit mode shows the authored state again
    m_audio_overrides.clear();
    m_text_overrides.clear();
    m_script.Clear();
    m_gui.Clear();
    m_phys.Clear();
    m_phys_poses.clear();
    m_live.clear(); // every script-side scene change was live-only; Stop restores
    // Drop anything spawn() appended, so the project is exactly as authored.
    if (m_play_scene && m_play_scene->objects.size() > m_authored_object_count)
        m_play_scene->objects.resize(m_authored_object_count);
    m_play_scene = nullptr;
    m_authored_object_count = 0;
    m_obj_gen.clear();
    m_obj_alive.clear();
    m_pending_destroy.clear();
    m_pending_scene.clear();
    // Rewind every GIF so Stop returns the viewport to frame 1, the same still
    // the scene shows before Play. The decoded frames stay cached.
    m_gif_play.clear();
    m_phys_on = false;
    applog::Info("Physics: stop");
    // m_phys_debug is rebuilt from the scene each RenderUi (authoring wireframes).
}

// --- script::ScriptHost (editor) ---
bool  SceneRenderer::InputButton(const char* name) { return m_input.Button(name); }
float SceneRenderer::InputAxis(const char* name)   { return m_input.Axis(name); }
bool  SceneRenderer::InputButtonPressed(const char* name)  { return m_input.Pressed(name); }
bool  SceneRenderer::InputButtonReleased(const char* name) { return m_input.Released(name); }
void  SceneRenderer::Log(const char* msg)          { applog::Script(msg); }        // [LOG]
void  SceneRenderer::LogError(const char* msg)     { applog::Error(msg); }          // [ERROR]
void SceneRenderer::TextSetValue(int objectIndex, const char* value)
{
    const char* name = ObjectName(objectIndex);
    if (name && *name)
        m_text_overrides[name] = value ? value : "";
}

// --- Live scene state (editor) -----------------------------------------------
// Writes land in m_live, never in the authored SceneObject, so Play can move,
// hide and re-tag anything without marking the project dirty. StopPhysics drops
// the whole layer and the authored scene reappears.

bool SceneRenderer::ApplyLive(int objectIndex, const SceneObject& authored,
                              SceneObject& out) const
{
    if (objectIndex < 0 || objectIndex >= (int)m_live.size()) return false;
    const LiveObject& lo = m_live[objectIndex];
    if (!lo.dirty) return false;

    out = authored;
    for (int k = 0; k < 3; ++k)
    { out.position[k] = lo.pos[k]; out.rotation[k] = lo.rot[k]; out.scale[k] = lo.scale[k]; }
    out.visible = lo.visible;
    out.tags    = lo.tags;
    if (lo.has_attrs) out.attributes = lo.attrs; // only once a script wrote a field
    if (lo.cam_active >= 0)
        for (size_t a = 0; a < out.attributes.size(); ++a)
            if (out.attributes[a].type == "Camera")
                out.attributes[a].cam_active = (lo.cam_active != 0);
    return true;
}

void SceneRenderer::RefreshPlayHierarchy()
{
    if (!m_play_scene) return;
    const std::vector<SceneObject>& authored = m_play_scene->objects;
    std::vector<SceneObject> objects(authored.size());
    std::vector<std::string> names(authored.size()), parentNames(authored.size());
    std::vector<int> parents;
    std::vector<hier::Node> nodes(authored.size());
    for (int i = 0; i < (int)authored.size(); ++i)
    {
        SceneObject overlaid;
        objects[i] = ApplyLive(i, authored[i], overlaid) ? overlaid : authored[i];
        names[i] = objects[i].name;
        parentNames[i] = objects[i].parent;
    }
    hier::MapParents(names, parentNames, parents);
    for (int i = 0; i < (int)objects.size(); ++i)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            nodes[i].pos[axis] = objects[i].position[axis];
            nodes[i].rot[axis] = objects[i].rotation[axis];
            nodes[i].scale[axis] = objects[i].scale[axis];
        }
        nodes[i].visible = objects[i].visible;
        nodes[i].parent = parents[i];
    }

    std::vector<hier::Resolved> resolved;
    hier::Resolve(nodes, resolved);
    for (size_t poseIndex = 0; poseIndex < m_phys_poses.size(); ++poseIndex)
    {
        const phys::Pose& pose = m_phys_poses[poseIndex];
        if (pose.objectIndex < 0 || pose.objectIndex >= (int)nodes.size()) continue;
        hier::Node& node = nodes[pose.objectIndex];
        float ignoredScale[3];
        hier::Decompose(pose.matrix, node.pos, node.rot, ignoredScale);
        for (int axis = 0; axis < 3; ++axis)
            node.scale[axis] = resolved[pose.objectIndex].scale[axis];
        node.parent = -1;
    }
    hier::Resolve(nodes, resolved);

    for (int i = 0; i < (int)resolved.size(); ++i)
        m_phys.SetHierarchyTransform(i, resolved[i].pos, resolved[i].rot);
    for (size_t itemIndex = 0; itemIndex < m_draw_items.size(); ++itemIndex)
    {
        DrawItem& item = m_draw_items[itemIndex];
        if (item.object_index < 0 || item.object_index >= (int)resolved.size()) continue;
        std::memcpy(&item.world.m[0][0], resolved[item.object_index].world, sizeof(resolved[item.object_index].world));
        for (int axis = 0; axis < 3; ++axis)
            item.scale[axis] = resolved[item.object_index].scale[axis];
    }
    for (size_t itemIndex = 0; itemIndex < m_pick_items.size(); ++itemIndex)
    {
        PickItem& item = m_pick_items[itemIndex];
        if (item.object_index < 0 || item.object_index >= (int)resolved.size()) continue;
        std::memcpy(&item.world.m[0][0], resolved[item.object_index].world, sizeof(resolved[item.object_index].world));
    }
    for (size_t itemIndex = 0; itemIndex < m_shader_items.size(); ++itemIndex)
    {
        ShaderItem& item = m_shader_items[itemIndex];
        const int objectIndex = item.object_index;
        if (objectIndex < 0 || objectIndex >= (int)resolved.size()) continue;
        std::memcpy(&item.world.m[0][0], resolved[objectIndex].world, sizeof(resolved[objectIndex].world));
        for (int axis = 0; axis < 3; ++axis)
        {
            item.pos[axis] = resolved[objectIndex].pos[axis];
            item.rot[axis] = resolved[objectIndex].rot[axis];
            item.scale[axis] = resolved[objectIndex].scale[axis];
        }
    }
    for (size_t itemIndex = 0; itemIndex < m_audio_items.size(); ++itemIndex)
    {
        AudioItem& item = m_audio_items[itemIndex];
        if (item.object_index < 0 || item.object_index >= (int)resolved.size()) continue;
        for (int axis = 0; axis < 3; ++axis)
            item.pos[axis] = resolved[item.object_index].pos[axis];
    }
}

bool SceneRenderer::GetObjectTransform(int objectIndex, float pos[3], float rot[3], float scale[3])
{
    if (objectIndex < 0 || objectIndex >= (int)m_live.size()) return false;
    const LiveObject& lo = m_live[objectIndex];
    for (int k = 0; k < 3; ++k)
    { pos[k] = lo.pos[k]; rot[k] = lo.rot[k]; scale[k] = lo.scale[k]; }
    return true;
}

void SceneRenderer::SetObjectTransform(int objectIndex, const float pos[3], const float rot[3],
                                       const float scale[3], int mask)
{
    if (objectIndex < 0 || objectIndex >= (int)m_live.size()) return;
    LiveObject& lo = m_live[objectIndex];
    if (mask & script::ScriptHost::XformPos)
        for (int k = 0; k < 3; ++k) lo.pos[k] = pos[k];
    if (mask & script::ScriptHost::XformRot)
        for (int k = 0; k < 3; ++k) lo.rot[k] = rot[k];
    if (mask & script::ScriptHost::XformScale)
        for (int k = 0; k < 3; ++k) lo.scale[k] = scale[k];
    lo.dirty = true;
}

bool SceneRenderer::GetObjectVisible(int objectIndex)
{
    if (objectIndex < 0 || objectIndex >= (int)m_live.size()) return false;
    return m_live[objectIndex].visible;
}

void SceneRenderer::SetObjectVisible(int objectIndex, bool visible)
{
    if (objectIndex < 0 || objectIndex >= (int)m_live.size()) return;
    m_live[objectIndex].visible = visible;
    m_live[objectIndex].dirty   = true;
}

bool SceneRenderer::SetActiveCamera(int objectIndex)
{
    if (objectIndex < 0 || objectIndex >= (int)m_live.size()) return false;
    // RenderUi takes the first active Camera in scene order, so deactivating the
    // others is what actually makes the switch happen.
    for (size_t i = 0; i < m_live.size(); ++i)
    {
        const int want = ((int)i == objectIndex) ? 1 : 0;
        if (m_live[i].cam_active == want) continue;
        m_live[i].cam_active = want;
        m_live[i].dirty      = true;
    }
    return true;
}

int SceneRenderer::GetActiveCamera()
{
    for (size_t i = 0; i < m_live.size(); ++i)
        if (m_live[i].cam_active == 1) return (int)i;
    return -1; // no script override: the authored active camera is in the scene
}

int SceneRenderer::ObjectTagCount(int objectIndex)
{
    if (objectIndex < 0 || objectIndex >= (int)m_live.size()) return 0;
    return (int)m_live[objectIndex].tags.size();
}

const char* SceneRenderer::ObjectTag(int objectIndex, int tagIndex)
{
    if (objectIndex < 0 || objectIndex >= (int)m_live.size()) return "";
    const std::vector<std::string>& tags = m_live[objectIndex].tags;
    if (tagIndex < 0 || tagIndex >= (int)tags.size()) return "";
    return tags[tagIndex].c_str();
}

bool SceneRenderer::AddObjectTag(int objectIndex, const char* tag)
{
    if (objectIndex < 0 || objectIndex >= (int)m_live.size() || !tag || !tag[0]) return false;
    std::vector<std::string>& tags = m_live[objectIndex].tags;
    for (size_t i = 0; i < tags.size(); ++i)
        if (tags[i] == tag) return false;
    tags.push_back(tag);
    m_live[objectIndex].dirty = true;
    return true;
}

// --- Object lifetime (editor) ------------------------------------------------
// Same slot/generation scheme the console uses. The one difference is where a
// spawned object lives: RenderUi draws from the authored SceneFile, so a clone
// has to be appended to it. StopPhysics truncates those away and the project is
// never marked dirty, so nothing a script spawned can reach disk on its own.

unsigned SceneRenderer::ObjectGeneration(int objectIndex)
{
    if (objectIndex < 0 || objectIndex >= (int)m_obj_gen.size()) return 0;
    return m_obj_gen[objectIndex];
}

bool SceneRenderer::ObjectAlive(int objectIndex)
{
    if (objectIndex < 0 || objectIndex >= (int)m_obj_alive.size()) return false;
    return m_obj_alive[objectIndex] != 0;
}

int SceneRenderer::SpawnObject(int sourceObjectIndex, const char* name, const float pos[3])
{
    if (!m_play_scene) return -1;
    std::vector<SceneObject>& objects = m_play_scene->objects;
    if (sourceObjectIndex < 0 || sourceObjectIndex >= (int)objects.size()) return -1;

    int slot = -1;
    for (std::size_t i = 0; i < m_obj_alive.size(); ++i)
        if (!m_obj_alive[i]) { slot = (int)i; break; }
    if (slot < 0)
    {
        objects.push_back(SceneObject());
        m_obj_gen.push_back(0u);
        m_obj_alive.push_back(0);
        m_live.push_back(LiveObject());
        m_phys_names.push_back(std::string());
        slot = (int)objects.size() - 1;
    }

    // By value: push_back above may have reallocated, invalidating a reference.
    const SceneObject source = objects[sourceObjectIndex];
    SceneObject& clone = objects[slot];
    clone = source;
    if (name && *name) clone.name = name;
    clone.parent.clear();
    for (int k = 0; k < 3; ++k) clone.position[k] = pos[k];
    clone.visible = true; // templates are usually authored hidden

    ++m_obj_gen[slot];
    m_obj_alive[slot] = 1;
    m_phys_names[slot] = clone.name;

    LiveObject& lo = m_live[slot];
    lo = LiveObject();
    for (int k = 0; k < 3; ++k)
    { lo.pos[k] = clone.position[k]; lo.rot[k] = clone.rotation[k]; lo.scale[k] = clone.scale[k]; }
    lo.visible = clone.visible;
    lo.tags    = clone.tags;
    lo.attrs   = clone.attributes;

    // The editor rebuilds its draw lists from the scene every frame, so the
    // clone appears immediately; only its collider has to be created by hand.
    std::vector<std::vector<float>*>        geomPos;
    std::vector<std::vector<unsigned int>*> geomIdx;
    std::vector<phys::BodyDesc> descs;
    AppendBodyDescs(*m_play_scene, slot, descs, geomPos, geomIdx);
    for (std::size_t d = 0; d < descs.size(); ++d)
        m_phys.AddBody(descs[d]);
    for (std::size_t g = 0; g < geomPos.size(); ++g) delete geomPos[g];
    for (std::size_t g = 0; g < geomIdx.size(); ++g) delete geomIdx[g];

    // A cloned Script attribute gets its own environment and its own on_start.
    for (const ObjectAttribute& a : clone.attributes)
    {
        if (a.type != "Script" || a.script_path.empty()) continue;
        std::string source_text;
        if (!ReadScriptSource(a.script_path, source_text)) continue;
        m_script.LoadScript(slot, source_text, a.script_path);
        m_script.StartOne(slot);
    }
    return slot;
}

bool SceneRenderer::DestroyObject(int objectIndex)
{
    if (!ObjectAlive(objectIndex)) return false;
    // Dead at once so handles stop resolving immediately; the physics body and
    // the script environment come down at the end of the frame, because tearing
    // them down here would edit the lists the update loop is walking.
    m_obj_alive[objectIndex] = 0;
    ++m_obj_gen[objectIndex];
    m_pending_destroy.push_back(objectIndex);
    return true;
}

void SceneRenderer::ApplyPendingDestroys()
{
    for (std::size_t i = 0; i < m_pending_destroy.size(); ++i)
    {
        const int slot = m_pending_destroy[i];
        m_phys.RemoveBody(slot);
        m_script.UnloadScript(slot);
        if (m_play_scene && slot >= 0 && slot < (int)m_play_scene->objects.size())
        {
            // Emptying the object stops it drawing and contributing light. The
            // slot itself stays put — the indices around it must not shift.
            const std::string keep = m_play_scene->objects[slot].name;
            m_play_scene->objects[slot] = SceneObject();
            m_play_scene->objects[slot].name    = keep;
            m_play_scene->objects[slot].visible = false;
        }
        if (slot >= 0 && slot < (int)m_live.size())
        {
            m_live[slot] = LiveObject();
            m_live[slot].visible = false;
            m_live[slot].dirty   = true;
        }
    }
    m_pending_destroy.clear();
}

// Scene switching in the editor is a Play-session restart against a different
// scene: the request is remembered here and the viewport's Play/Stop logic in
// RenderUi acts on it, exactly as if the user had picked the scene and hit Play.
//
// The editor keeps every scene's assets in memory already, so there is nothing
// to stream and progress is 1 immediately. The minimum display time is still
// honoured, so a loading screen authored against the console behaves the same
// way here — it just never has to wait for anything.
bool SceneRenderer::LoadScene(const char* name, float minSeconds)
{
    if (!name || !*name) return false;
    if (!m_pending_scene.empty()) return false; // one load at a time
    m_pending_scene   = name;
    m_load_frames     = 0;
    m_load_min_time   = minSeconds > 0.0f ? minSeconds : 0.0f;
    m_load_ready_time = m_time + m_load_min_time;
    return true;
}

bool SceneRenderer::SceneIsLoading() { return !m_pending_scene.empty(); }

// Every scene is already in memory in the editor, so a load has no bytes to read
// and is genuinely complete the moment it starts. Reporting a timer here instead
// would be inventing work that isn't happening.
float SceneRenderer::SceneProgress() { return 1.0f; }

const char* SceneRenderer::SceneName()
{
    return m_play_scene ? m_play_scene->name.c_str() : "";
}

// --- Attribute fields (obj:get / obj:set) ------------------------------------
// Reads and writes the per-Play attribute clone, never the authored scene, so a
// script can dim a light or fade an overlay without the project going dirty.

int SceneRenderer::AttrCount(int objectIndex)
{
    if (objectIndex < 0 || objectIndex >= (int)m_live.size()) return 0;
    return (int)m_live[objectIndex].attrs.size();
}

const char* SceneRenderer::AttrType(int objectIndex, int attrIndex)
{
    if (objectIndex < 0 || objectIndex >= (int)m_live.size()) return "";
    const std::vector<ObjectAttribute>& attrs = m_live[objectIndex].attrs;
    if (attrIndex < 0 || attrIndex >= (int)attrs.size()) return "";
    return attrs[attrIndex].type.c_str();
}

bool SceneRenderer::AttrGet(int objectIndex, int attrIndex, const char* field,
                            attrfields::Value& out)
{
    if (objectIndex < 0 || objectIndex >= (int)m_live.size()) return false;
    const attrfields::FieldDesc* desc = attrfields::Find(field);
    if (!desc) return false;
    const std::vector<ObjectAttribute>& attrs = m_live[objectIndex].attrs;
    const int a = attrfields::ResolveIndex(attrs, attrIndex, *desc);
    if (a < 0) return false;
    return attrfields::Get(&attrs[a], *desc, out);
}

bool SceneRenderer::AttrSet(int objectIndex, int attrIndex, const char* field,
                            const attrfields::Value& in)
{
    if (objectIndex < 0 || objectIndex >= (int)m_live.size()) return false;
    const attrfields::FieldDesc* desc = attrfields::Find(field);
    if (!desc) return false;
    LiveObject& lo = m_live[objectIndex];
    const int a = attrfields::ResolveIndex(lo.attrs, attrIndex, *desc);
    if (a < 0) return false;
    if (!attrfields::Set(&lo.attrs[a], *desc, in)) return false;
    // From here on the overlay's attributes are what gets drawn (RenderUi rebuilds
    // from the scene every frame, so this shows up immediately).
    lo.has_attrs = true;
    lo.dirty     = true;
    return true;
}

bool SceneRenderer::RemoveObjectTag(int objectIndex, const char* tag)
{
    if (objectIndex < 0 || objectIndex >= (int)m_live.size() || !tag) return false;
    std::vector<std::string>& tags = m_live[objectIndex].tags;
    for (size_t i = 0; i < tags.size(); ++i)
        if (tags[i] == tag)
        {
            tags.erase(tags.begin() + i);
            m_live[objectIndex].dirty = true;
            return true;
        }
    return false;
}
// Lua video.play/stop: an override on the object's Video attribute play mode,
// applied at capture time and dropped when the Play session ends — the
// authored scene value is never touched (same contract as physics poses).
// play on a video that isn't currently running bumps the restart generation
// (new stream key -> decode from the top); play while running only updates
// the mode (Once <-> Loop applies live, no restart).
void SceneRenderer::VideoSetPlaying(int objectIndex, bool play, bool loop)
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
bool SceneRenderer::VideoIsPlaying(int objectIndex)
{
    const char* name = ObjectName(objectIndex);
    if (!name || !*name)
        return false;
    // Streams reconcile at draw time, so a play/stop issued this same script
    // tick is answered from the override; an existing stream (at the current
    // effective key) still decides Once-finished.
    const auto ov = m_video_overrides.find(name);
    for (const VideoItem& item : m_video_items)
        if (item.object == name)
        {
            if (ov != m_video_overrides.end() && ov->second.mode == (int)vid::PlayOff)
                return false;
            const std::string key = VideoKeyFor(item);
            if (m_video.HasStream(key))
                return m_video.IsPlaying(key);
            // play() issued; the stream opens at this frame's draw.
            return ov != m_video_overrides.end();
        }
    return false;
}

// Lua audio.*: transient overrides on the object's Audio attribute (same
// contract as the video overrides — the authored scene is never touched).
void SceneRenderer::AudioSetPlaying(int objectIndex, bool play)
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
bool SceneRenderer::AudioIsPlaying(int objectIndex)
{
    const char* name = ObjectName(objectIndex);
    if (!name || !*name)
        return false;
    const auto ov = m_audio_overrides.find(name);
    for (const AudioItem& item : m_audio_items)
        if (item.object == name)
        {
            if (ov != m_audio_overrides.end() && ov->second.play == 0)
                return false;
            const std::string key = AudioKeyFor(item);
            if (m_audio.HasStream(key))
                return m_audio.IsPlaying(key);
            // Not opened yet: report the effective wanted state.
            if (ov != m_audio_overrides.end() && ov->second.play == 1)
                return true;
            return item.play && (ov == m_audio_overrides.end() || ov->second.play != 0);
        }
    return false;
}
void SceneRenderer::AudioSetVolume(int objectIndex, float volume)
{
    const char* name = ObjectName(objectIndex);
    if (name && *name)
        // Clamped to the same 0..20 the attribute editor allows, so a stray
        // audio.set_volume(500) can't blow the mix out.
        m_audio_overrides[name].volume = volume < 0.0f ? 0.0f : (volume > 20.0f ? 20.0f : volume);
}
void SceneRenderer::AudioSetPitch(int objectIndex, float pitch)
{
    const char* name = ObjectName(objectIndex);
    if (name && *name)
        m_audio_overrides[name].pitch = pitch < 0.1f ? 0.1f : (pitch > 4.0f ? 4.0f : pitch);
}
void SceneRenderer::AudioSetLoop(int objectIndex, bool loop)
{
    const char* name = ObjectName(objectIndex);
    if (name && *name)
        m_audio_overrides[name].loop = loop ? 1 : 0;
}

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
int SceneRenderer::ObjectCount()
{
    // m_phys_names is filled by StartPhysics, so this is the scene's object count
    // for the whole Play session — which is the only time scripts run.
    return (int)m_phys_names.size();
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

// Unit cone for the spot volumetric beam: apex at the origin opening down +Z,
// ring radius 1 at z = 1 (radius = z), so the world matrix's (r, r, range)
// scale shapes it into any spot's outer cone. Side surface only — the far end
// stays open; the shader fades to zero there anyway. Smooth per-vertex ring
// normals ((cos, sin, -1) / sqrt2 for radius = z); the apex takes its face's
// mid-angle normal. uv.x carries t along the beam (0 = apex, 1 = end).
void SceneRenderer::BuildBeamCone()
{
    const int   seg = 24;
    const float TAU = 6.28318531f;
    const float inv = 0.70710678f; // 1/sqrt2
    m_beam_cone.clear();
    m_beam_cone.reserve(seg * 3);
    auto vert = [](float x, float y, float z, float nx, float ny, float nz, float t)
    {
        MeshVertex v = {};
        v.px = x;  v.py = y;  v.pz = z;
        v.nx = nx; v.ny = ny; v.nz = nz;
        v.u  = t;
        return v;
    };
    for (int s = 0; s < seg; ++s)
    {
        const float a0 = (float)s / seg * TAU;
        const float a1 = (float)(s + 1) / seg * TAU;
        const float am = (a0 + a1) * 0.5f;
        m_beam_cone.push_back(vert(0, 0, 0, std::cos(am) * inv, std::sin(am) * inv, -inv, 0.0f));
        m_beam_cone.push_back(vert(std::cos(a0), std::sin(a0), 1.0f,
                                   std::cos(a0) * inv, std::sin(a0) * inv, -inv, 1.0f));
        m_beam_cone.push_back(vert(std::cos(a1), std::sin(a1), 1.0f,
                                   std::cos(a1) * inv, std::sin(a1) * inv, -inv, 1.0f));
    }
}

// Spot-light cone (mirrors the Vulkan editor's gizmo): the beam axis down local
// +Z, the outer cone's end ring at range (radius = tan(outer) * range) with
// four spokes back to the apex, and a fainter inner-cone preview ring at 65% of
// the beam. Sized purely by the attribute, so dragging Range / Inner / Outer in
// the Inspector reshapes it live.
void SceneRenderer::AppendSpotCone(const SpotGizmo& sg, std::vector<Vertex>& out) const
{
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

    const int   seg = 24;
    const float TAU = 6.28318531f, d2r = 3.14159265f / 180.0f;
    const D3DCOLOR outer_col = D3DCOLOR_ARGB(255, 255, 221, 87); // warm gizmo yellow
    const D3DCOLOR inner_col = D3DCOLOR_ARGB(255, 168, 156, 96); // dimmed preview
    const float ro = std::tan(sg.outer_deg * d2r) * sg.range;
    const float zi = sg.range * 0.65f;
    const float ri = std::tan(sg.inner_deg * d2r) * zi;

    // Beam axis.
    out.push_back(L::P(0, 0, 0, sg.base, outer_col));
    out.push_back(L::P(0, 0, sg.range, sg.base, outer_col));
    for (int s = 0; s < seg; ++s)
    {
        const float a0 = (float)s / seg * TAU, a1 = (float)(s + 1) / seg * TAU;
        const float c0 = std::cos(a0), s0 = std::sin(a0);
        const float c1 = std::cos(a1), s1 = std::sin(a1);
        // Outer ring at the cone's end...
        out.push_back(L::P(c0 * ro, s0 * ro, sg.range, sg.base, outer_col));
        out.push_back(L::P(c1 * ro, s1 * ro, sg.range, sg.base, outer_col));
        // ...the inner-cone preview partway down the beam...
        out.push_back(L::P(c0 * ri, s0 * ri, zi, sg.base, inner_col));
        out.push_back(L::P(c1 * ri, s1 * ri, zi, sg.base, inner_col));
        // ...and four spokes from the apex to the outer ring.
        if ((s % (seg / 4)) == 0)
        {
            out.push_back(L::P(0, 0, 0, sg.base, outer_col));
            out.push_back(L::P(c0 * ro, s0 * ro, sg.range, sg.base, outer_col));
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

    // Quarter-res ping/pong targets for the bloom chain. Optional: if either
    // fails, RenderBloom just skips (no glow), the scene still renders.
    const int bw = (width  / 4 > 0) ? width  / 4 : 1;
    const int bh = (height / 4 > 0) ? height / 4 : 1;
    if (SUCCEEDED(m_device->CreateTexture(bw, bh, 1, D3DUSAGE_RENDERTARGET,
                                          D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_bloomA, nullptr)) &&
        SUCCEEDED(m_device->CreateTexture(bw, bh, 1, D3DUSAGE_RENDERTARGET,
                                          D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_bloomB, nullptr)))
    {
        if (FAILED(m_bloomA->GetSurfaceLevel(0, &m_bloomASurf)) ||
            FAILED(m_bloomB->GetSurfaceLevel(0, &m_bloomBSurf)))
        {
            if (m_bloomBSurf) { m_bloomBSurf->Release(); m_bloomBSurf = nullptr; }
            if (m_bloomASurf) { m_bloomASurf->Release(); m_bloomASurf = nullptr; }
        }
    }
    if (!m_bloomASurf || !m_bloomBSurf)
    {
        if (m_bloomB) { m_bloomB->Release(); m_bloomB = nullptr; }
        if (m_bloomA) { m_bloomA->Release(); m_bloomA = nullptr; }
    }

    m_width  = width;
    m_height = height;
    return true;
}
