#pragma once

#include <d3d9.h>

#include <cstdint>
#include <filesystem>
#include <string>

// Current baked-blob version. Bumped whenever MeshVertex or the material
// section changes so stale blobs are re-baked from their source.
constexpr uint32_t MESH_VERSION = 3;

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
// After the header: vertexCount vertices, indexCount uint32 indices, then three
// length-prefixed strings: diffuse, normal, specular texture paths (relative to
// the mesh file; empty = that map is absent).

// Diffuse / normal / specular texture references from a baked mesh.
struct MeshTextures
{
    std::string diffuse;
    std::string normal;
    std::string specular;
};

// How the diffuse's alpha channel is used. Derived from the texture itself (no
// inspector toggle): a binary/hard-edged mask reads as Cutout (alpha-test, keeps
// depth — hair cards, foliage), smooth intermediate alpha reads as Blend (glass).
enum class AlphaKind { Opaque, Cutout, Blend };

// A mesh in GPU memory (D3DPOOL_MANAGED — survives device resets).
struct GpuMesh
{
    IDirect3DVertexBuffer9* vb       = nullptr;
    IDirect3DIndexBuffer9*  ib       = nullptr;
    IDirect3DTexture9*      diffuse  = nullptr; // null = use default
    IDirect3DTexture9*      normal   = nullptr;
    IDirect3DTexture9*      specular = nullptr;
    uint32_t  vertexCount = 0;
    uint32_t  indexCount  = 0;
    AlphaKind alpha       = AlphaKind::Opaque; // how the diffuse's alpha is used

    void Release()
    {
        if (specular) { specular->Release(); specular = nullptr; }
        if (normal)   { normal->Release();   normal = nullptr; }
        if (diffuse)  { diffuse->Release();  diffuse = nullptr; }
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

    // Load a .mesh blob into GPU buffers; out_tex receives the texture paths.
    bool LoadMeshBlob(IDirect3DDevice9* device,
                      const std::filesystem::path& mesh_path,
                      GpuMesh& out,
                      MeshTextures& out_tex);

    // Load an image file into a D3D9 texture (stb_image). If out_alpha is given,
    // it reports how the alpha channel should be used (Opaque / Cutout / Blend),
    // classified from the pixel data — see AlphaKind.
    IDirect3DTexture9* LoadTexture(IDirect3DDevice9* device,
                                   const std::filesystem::path& path,
                                   AlphaKind* out_alpha = nullptr);
}
