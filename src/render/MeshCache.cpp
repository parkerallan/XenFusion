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

    // Load the blob + each material subset's textures (diffuse / normal /
    // specular / emissive / metallic).
    auto try_load = [&](GpuMesh& g)
    {
        std::vector<MeshSubset> subsets;
        if (mesh::LoadMeshBlob(m_device, baked, g, subsets) && g.vb)
        {
            auto load = [&](const std::string& rel, AlphaKind* alpha = nullptr, bool* height = nullptr) -> IDirect3DTexture9*
            {
                if (rel.empty()) return nullptr;
                IDirect3DTexture9* t = mesh::LoadTexture(m_device, baked.parent_path() / rel, alpha, height);
                if (!t) applog::Warn("Texture not found for " + model_path + ": " + rel);
                return t;
            };
            for (const MeshSubset& s : subsets)
            {
                GpuSubset gs;
                gs.indexStart = s.indexStart;
                gs.indexCount = s.indexCount;
                gs.diffuse  = load(s.textures.diffuse, &gs.alpha); // transparency mode from the diffuse's alpha
                gs.normal   = load(s.textures.normal, nullptr, &gs.normalHasHeight); // bump offset from the normal's alpha
                gs.specular = load(s.textures.specular);
                gs.emissive = load(s.textures.emissive);
                gs.metallic = load(s.textures.metallic);
                gs.clearcoat = load(s.textures.clearcoat);
                g.subsets.push_back(gs);
            }
        }
    };

    GpuMesh gm;
    try_load(gm);

    // Blob missing or an old format? If we have a source, force a re-bake.
    if (!gm.vb && !bake_failed && have_source && baked_exists && !source_newer)
    {
        std::string err;
        if (mesh::BakeModel(source, baked, err))
        {
            applog::Build("Rebaked " + baked.filename().string());
            try_load(gm);
        }
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
