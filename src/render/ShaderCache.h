#pragma once

#include <d3d9.h>

#include <filesystem>
#include <string>
#include <unordered_map>

// A shader's render setup, declared by its own //@ header directives and parsed
// once at load into raw device values, so applying it is a flat list of
// SetRenderState calls (no per-draw switching).
//   //@geometry quad | volume | model
//   //@blend    opaque | alpha | additive
//   //@depth    on | test | off
//   //@cull     none | back
// "model" makes the shader the material for the object's 3D Model: it draws the
// mesh (with the mesh's textures bound to s0/s1/s2) instead of built-in geometry.
struct ShaderState
{
    enum Geometry { Quad, Volume, Model } geometry = Quad;
    DWORD alphaBlend = FALSE;
    DWORD srcBlend   = D3DBLEND_SRCALPHA;
    DWORD destBlend  = D3DBLEND_INVSRCALPHA;
    DWORD zEnable    = TRUE;
    DWORD zWrite     = TRUE;
    DWORD cull       = D3DCULL_NONE;
};

// A runtime-compiled custom shader (VSMain + PSMain) loaded from a project
// .hlsl asset. Editor-only: compiled with d3dcompiler. (A shipping/console
// build would bake these to .cso through the content pipeline, like meshes.)
struct CustomShader
{
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9*  ps = nullptr;
    ShaderState             state;
    bool Valid() const { return vs && ps; }
    void Release();
};

// Caches custom shaders by their project-relative path, recompiling when the
// source file changes (live reload) and holding nothing when a compile fails so
// the renderer can fall back to the built-in material. Mirrors MeshCache.
class ShaderCache
{
public:
    void Init(IDirect3DDevice9* device);
    void Shutdown();
    void SetProjectRoot(const std::filesystem::path& root); // clears on change
    void Clear();                                           // drop loaded shaders (after a recompile)

    // Loads the compiled bytecode for rel_path (the scene's .hlsl path) from
    // <project>/shaders. Does NOT compile — that's done by the Reload action.
    // Returns nullptr if not compiled yet.
    CustomShader* Get(const std::string& rel_path);

private:
    struct Entry
    {
        CustomShader                    shader;
        unsigned long long              next_check_ms = 0;
        std::filesystem::file_time_type mtime{};
        bool                            logged_error = false;
    };

    IDirect3DDevice9*                        m_device = nullptr;
    std::filesystem::path                    m_root;
    std::unordered_map<std::string, Entry>   m_cache;
};
