#pragma once

#include <d3d9.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Current baked-blob version. Bumped whenever MeshVertex or the material
// section changes so stale blobs are re-baked from their source.
constexpr uint32_t MESH_VERSION = 5;

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

struct MeshHeader
{
    char     magic[4];   // "M360"
    uint32_t version;    // MESH_VERSION
    uint32_t vertexCount;
    uint32_t indexCount;
};
// After the header: vertexCount vertices, indexCount uint32 indices, then a
// uint32 subset count followed by one subset per source material: uint32
// indexStart, uint32 indexCount, then four length-prefixed strings (diffuse,
// normal, specular, emissive texture paths relative to the mesh file; empty =
// absent).

// Diffuse / normal / specular / emissive texture references for one material.
struct MeshTextures
{
    std::string diffuse;
    std::string normal;
    std::string specular;
    std::string emissive;
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
    AlphaKind          alpha    = AlphaKind::Opaque;
    bool               normalHasHeight = false; // normal map's alpha carries a
                                                // height field (0.5 = neutral)
};

// A mesh in GPU memory (D3DPOOL_MANAGED — survives device resets). One shared
// vertex/index buffer; each material draws its own subset range.
struct GpuMesh
{
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9*  ib = nullptr;
    std::vector<GpuSubset>  subsets;
    uint32_t vertexCount = 0;
    uint32_t indexCount  = 0;

    void Release()
    {
        for (GpuSubset& s : subsets)
        {
            if (s.emissive) { s.emissive->Release(); s.emissive = nullptr; }
            if (s.specular) { s.specular->Release(); s.specular = nullptr; }
            if (s.normal)   { s.normal->Release();   s.normal = nullptr; }
            if (s.diffuse)  { s.diffuse->Release();  s.diffuse = nullptr; }
        }
        subsets.clear();
        if (ib) { ib->Release(); ib = nullptr; }
        if (vb) { vb->Release(); vb = nullptr; }
        vertexCount = indexCount = 0;
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
}
