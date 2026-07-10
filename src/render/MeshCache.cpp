#include "render/MeshCache.h"

#include "core/Log.h"

#include <windows.h> // GetTickCount64

#include <cctype>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
    std::string LowerExt(const fs::path& p)
    {
        std::string e = p.extension().string();
        for (char& c : e) c = (char)std::tolower((unsigned char)c);
        return e;
    }

    // Given a baked ".mesh" path, find a same-stem source model next to it
    // (e.g. Fox.mesh -> Fox.gltf). Returns empty if there's no source, which is
    // the normal runtime case (only .mesh files are shipped).
    fs::path FindSiblingSource(const fs::path& mesh_path)
    {
        static const char* kSourceExts[] =
            {".gltf", ".glb", ".fbx", ".obj", ".dae", ".3ds", ".ply", ".stl"};
        const fs::path    dir  = mesh_path.parent_path();
        const std::string stem = mesh_path.stem().string();
        std::error_code ec;
        for (const char* e : kSourceExts)
        {
            fs::path candidate = dir / (stem + e);
            if (fs::exists(candidate, ec))
                return candidate;
        }
        return {};
    }
}

void MeshCache::Init(IDirect3DDevice9* device)
{
    m_device = device;
}

void MeshCache::Shutdown()
{
    for (auto& kv : m_cache) kv.second.mesh.Release();
    m_cache.clear();
    m_device = nullptr;
}

void MeshCache::SetProjectRoot(const fs::path& root)
{
    if (root == m_project_root)
        return;
    for (auto& kv : m_cache) kv.second.mesh.Release();
    m_cache.clear();
    m_project_root = root;
}

GpuMesh* MeshCache::Get(const std::string& model_path)
{
    const unsigned long long now = GetTickCount64();

    auto it = m_cache.find(model_path);
    const bool first_attempt = (it == m_cache.end());

    // Within the re-check window, return what we have (fast path).
    if (!first_attempt && now < it->second.next_check_ms)
        return it->second.mesh.vb ? &it->second.mesh : nullptr;

    // Resolve the baked .mesh + its source. The scene normally stores the .mesh
    // path; a legacy scene may store the source path (we derive the .mesh then).
    const fs::path input = m_project_root / model_path;
    fs::path baked, source;
    if (LowerExt(input) == ".mesh")
    {
        baked  = input;
        source = FindSiblingSource(baked); // may be empty (runtime: no source)
    }
    else
    {
        source = input;                    // legacy: source path in the scene
        baked  = input;
        baked.replace_extension(".mesh");
    }

    std::error_code ec;
    const bool baked_exists = fs::exists(baked, ec);
    const bool have_source  = !source.empty() && fs::exists(source, ec);
    const bool source_newer = have_source && baked_exists &&
                              fs::last_write_time(source, ec) > fs::last_write_time(baked, ec);

    const bool loaded_ok = (!first_attempt && it->second.mesh.vb);

    // Rebuild when: first seen, previously failed, or the source changed.
    const bool rebuild = first_attempt || !loaded_ok || source_newer;
    if (!rebuild)
    {
        it->second.next_check_ms = now + 1000;
        return it->second.mesh.vb ? &it->second.mesh : nullptr;
    }

    // Bake from the source if we have one and the blob is missing/stale. On the
    // console there's no source, so this is skipped and we load the shipped blob.
    bool bake_failed = false;
    if (have_source && (!baked_exists || source_newer))
    {
        std::string err;
        if (!mesh::BakeModel(source, baked, err))
        {
            bake_failed = true;
            if (first_attempt)
                applog::Error("Import failed: " + source.filename().string() + (err.empty() ? "" : " - " + err));
        }
        else
        {
            applog::Build("Baked " + baked.filename().string());
        }
    }

    // Load the blob + its texture.
    GpuMesh gm;
    std::string diffuse_rel;
    mesh::LoadMeshBlob(m_device, baked, gm, diffuse_rel);
    if (gm.vb && !diffuse_rel.empty())
    {
        gm.diffuse = mesh::LoadTexture(m_device, baked.parent_path() / diffuse_rel);
        if (!gm.diffuse)
            applog::Warn("Texture not found for " + model_path + ": " + diffuse_rel);
    }
    if (!gm.vb && !bake_failed && first_attempt)
        applog::Error("Failed to load model: " + model_path);

    // Store / replace the entry.
    Entry& slot = m_cache[model_path];
    slot.mesh.Release();
    slot.mesh = gm;
    slot.next_check_ms = now + 1000;
    return slot.mesh.vb ? &slot.mesh : nullptr;
}
