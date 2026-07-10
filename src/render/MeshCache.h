#pragma once

#include "render/Mesh.h"

#include <d3d9.h>

#include <filesystem>
#include <string>
#include <unordered_map>

// Owns GpuMeshes keyed by project-relative model path. Bakes source models
// (obj/fbx/gltf) to a .mesh blob on demand, loads them, and caches the GPU
// buffers. Keeps mesh/asset management out of the renderer — the renderer just
// calls Get() and draws the result.
class MeshCache
{
public:
    void Init(IDirect3DDevice9* device);
    void Shutdown();

    // Point the cache at a project root; clears loaded meshes if it changed.
    void SetProjectRoot(const std::filesystem::path& root);

    // GpuMesh for model_path (baking/loading/caching as needed), or null on
    // failure. Failures are cached so a broken path isn't re-baked each frame.
    GpuMesh* Get(const std::string& model_path);

private:
    struct Entry
    {
        GpuMesh            mesh;
        unsigned long long next_check_ms = 0; // re-validate (stat/reload) after this
    };

    IDirect3DDevice9*                       m_device = nullptr;
    std::filesystem::path                   m_project_root;
    std::unordered_map<std::string, Entry>  m_cache;
};
