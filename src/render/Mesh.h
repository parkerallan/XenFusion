#pragma once

#include <d3d9.h>

#include <cstdint>
#include <filesystem>
#include <string>

// Runtime vertex layout. Kept FVF-expressible (position + normal + one UV) so
// the fixed-function pipeline can light/texture it without a vertex shader.
#define MESH_FVF (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)

struct MeshVertex
{
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
}; // 32 bytes

// Baked ".mesh" blob layout:
//   MeshHeader
//   vertexCount * MeshVertex
//   indexCount  * uint32   (indices)
//   [version >= 2] uint32 diffuseLen, diffuseLen bytes (diffuse texture path,
//                  relative to the mesh file; empty = untextured)
struct MeshHeader
{
    char     magic[4];   // "M360"
    uint32_t version;    // 2
    uint32_t vertexCount;
    uint32_t indexCount;
};

// A mesh living in GPU memory (D3DPOOL_MANAGED, so it survives device resets).
struct GpuMesh
{
    IDirect3DVertexBuffer9* vb      = nullptr;
    IDirect3DIndexBuffer9*  ib      = nullptr;
    IDirect3DTexture9*      diffuse = nullptr; // null = untextured
    uint32_t vertexCount = 0;
    uint32_t indexCount  = 0;

    void Release()
    {
        if (diffuse) { diffuse->Release(); diffuse = nullptr; }
        if (ib) { ib->Release(); ib = nullptr; }
        if (vb) { vb->Release(); vb = nullptr; }
        vertexCount = indexCount = 0;
    }
};

namespace mesh
{
    // Import a source model (obj/fbx/gltf/...) with Assimp and bake it into the
    // .mesh blob at out_mesh. Editor-side, offline. Returns false + error text.
    bool BakeModel(const std::filesystem::path& source,
                   const std::filesystem::path& out_mesh,
                   std::string& error);

    // Load a .mesh blob into GPU vertex/index buffers. out_diffuse receives the
    // diffuse texture path stored in the blob (relative to the mesh file).
    bool LoadMeshBlob(IDirect3DDevice9* device,
                      const std::filesystem::path& mesh_path,
                      GpuMesh& out,
                      std::string& out_diffuse);

    // Load an image file (png/jpg/tga/bmp) into a D3D9 texture via stb_image.
    IDirect3DTexture9* LoadTexture(IDirect3DDevice9* device,
                                   const std::filesystem::path& path);
}
