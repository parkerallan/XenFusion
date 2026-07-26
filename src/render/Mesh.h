#pragma once

#include <d3d9.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Current baked-blob version. Bumped whenever MeshVertex or the material
// section changes so stale blobs are re-baked from their source.
constexpr uint32_t MESH_VERSION = 8;
constexpr uint32_t MESH_FLAG_SKINNED = 0x1u;

// The first Xbox skinning path uploads one 3x4 matrix per joint to vertex
// constants. Keep the limit explicit in the shared asset contract so models
// fail during baking instead of being truncated differently by each renderer.
constexpr uint32_t MAX_SKIN_JOINTS = 72;
constexpr uint32_t MAX_SKIN_INFLUENCES = 4;

// Runtime vertex layout: position + normal + tangent + one UV. Tangent is
// needed for normal mapping. Not FVF-expressible, so meshes are drawn with a
// vertex declaration (see SceneRenderer).
struct MeshVertex
{
    float px, py, pz;
    float nx, ny, nz;
    float tx, ty, tz;
    float u, v;
}; // 44 bytes

// Stored in a separate stream so static meshes retain their existing 44-byte
// vertex layout. Weights are normalized bytes whose sum is exactly 255.
struct MeshSkinInfluence
{
    uint8_t joint[MAX_SKIN_INFLUENCES];
    uint8_t weight[MAX_SKIN_INFLUENCES];
}; // 8 bytes

// Skeleton data is stored in skeleton-root space. inverseBind is row-major to
// match D3D9/D3DX matrix uploads; parent == -1 identifies the root joint.
struct MeshJoint
{
    std::string name;
    int32_t     parent = -1;
    float       inverseBind[16] = {};
    float       bindLocal[16] = {};
};

struct MeshSkeleton
{
    uint32_t fingerprint = 0;
    std::vector<MeshJoint> joints;

    bool IsSkinned() const { return !joints.empty(); }
    void Clear()
    {
        fingerprint = 0;
        joints.clear();
    }
};

struct MeshHeader
{
    char     magic[4];   // "M360"
    uint32_t version;    // MESH_VERSION
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t flags;
    uint32_t jointCount;
    uint32_t skeletonFingerprint;
};
// After the header: vertexCount vertices, indexCount uint32 indices, then a
// uint32 subset count followed by one subset per source material: uint32
// indexStart, uint32 indexCount, then six length-prefixed strings (diffuse,
// normal, specular, emissive, metallic, clearcoat texture paths relative to
// the mesh file; empty = absent). Skinned meshes then store vertexCount
// MeshSkinInfluence records followed by jointCount records: length-prefixed
// name, int32 parent, 16-float inverse bind, and 16-float local bind matrix.

// Diffuse / normal / specular / emissive / metallic / clearcoat texture
// references for one material.
struct MeshTextures
{
    std::string diffuse;
    std::string normal;
    std::string specular;
    std::string emissive;
    std::string metallic;
    std::string clearcoat;
};

// One material's range within a baked mesh: draw indices [indexStart,
// indexStart + indexCount) with its texture set.
struct MeshSubset
{
    uint32_t     indexStart = 0;
    uint32_t     indexCount = 0;
    MeshTextures textures;
};

// How the diffuse's alpha channel is used. Derived from the texture itself (no
// inspector toggle): a binary/hard-edged mask reads as Cutout (alpha-test, keeps
// depth — hair cards, foliage), smooth intermediate alpha reads as Blend (glass).
enum class AlphaKind { Opaque, Cutout, Blend };

// One material subset on the GPU: its index range, loaded textures, and how its
// diffuse alpha is used (classified from the texture — see AlphaKind).
struct GpuSubset
{
    uint32_t           indexStart = 0;
    uint32_t           indexCount = 0;
    IDirect3DTexture9* diffuse  = nullptr; // null = use default
    IDirect3DTexture9* normal   = nullptr;
    IDirect3DTexture9* specular = nullptr;
    IDirect3DTexture9* emissive = nullptr; // null = no glow (black default)
    IDirect3DTexture9* metallic = nullptr; // null = dielectric (black default)
    IDirect3DTexture9* clearcoat = nullptr; // null = no lacquer (black default)
    AlphaKind          alpha    = AlphaKind::Opaque;
    bool               normalHasHeight = false; // normal map's alpha carries a
                                                // height field (0.5 = neutral)
};

// A mesh in GPU memory (D3DPOOL_MANAGED — survives device resets). One shared
// vertex/index buffer; each material draws its own subset range.
struct GpuMesh
{
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DVertexBuffer9* skinVb = nullptr;
    IDirect3DIndexBuffer9*  ib = nullptr;
    std::vector<GpuSubset>  subsets;
    MeshSkeleton            skeleton;
    uint32_t vertexCount = 0;
    uint32_t indexCount  = 0;
    float boundsMin[3] = {};
    float boundsMax[3] = {};

    void Release()
    {
        for (GpuSubset& s : subsets)
        {
            if (s.clearcoat) { s.clearcoat->Release(); s.clearcoat = nullptr; }
            if (s.metallic) { s.metallic->Release(); s.metallic = nullptr; }
            if (s.emissive) { s.emissive->Release(); s.emissive = nullptr; }
            if (s.specular) { s.specular->Release(); s.specular = nullptr; }
            if (s.normal)   { s.normal->Release();   s.normal = nullptr; }
            if (s.diffuse)  { s.diffuse->Release();  s.diffuse = nullptr; }
        }
        subsets.clear();
        skeleton.Clear();
        if (ib) { ib->Release(); ib = nullptr; }
        if (skinVb) { skinVb->Release(); skinVb = nullptr; }
        if (vb) { vb->Release(); vb = nullptr; }
        vertexCount = indexCount = 0;
        for (int axis = 0; axis < 3; ++axis)
            boundsMin[axis] = boundsMax[axis] = 0.0f;
    }
};

namespace mesh
{
    // Import a source model with Assimp and bake it to a .mesh blob (editor).
    bool BakeModel(const std::filesystem::path& source,
                   const std::filesystem::path& out_mesh,
                   std::string& error);

    // Load a .mesh blob into GPU buffers; out_subsets receives each material's
    // index range + texture paths.
    bool LoadMeshBlob(IDirect3DDevice9* device,
                      const std::filesystem::path& mesh_path,
                      GpuMesh& out,
                      std::vector<MeshSubset>& out_subsets);

    // Load an image file into a D3D9 texture (stb_image). If out_alpha is given,
    // it reports how the alpha channel should be used (Opaque / Cutout / Blend),
    // classified from the pixel data — see AlphaKind. If out_height is given
    // (normal maps), it reports whether the alpha channel carries a height field
    // for bump offset: true only when the alpha actually varies — a plain normal
    // map (no alpha, or a blank one) reads false and gets zero UV offset.
    IDirect3DTexture9* LoadTexture(IDirect3DDevice9* device,
                                   const std::filesystem::path& path,
                                   AlphaKind* out_alpha = nullptr,
                                   bool* out_height = nullptr);

    // Load the project's environment cube map: six square, same-sized face
    // images env_px/nx/py/ny/pz/nz.png inside `dir`. Metal surfaces reflect it
    // (standard.hlsl s5). Returns null when any face is missing/mismatched —
    // the renderer then binds its black default cube (no reflections).
    IDirect3DCubeTexture9* LoadEnvCube(IDirect3DDevice9* device,
                                       const std::filesystem::path& dir);
}
