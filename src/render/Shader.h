#pragma once

#include <d3d9.h>

#include <filesystem>

// D3D9 shader creation. Three sources, all producing the same shader objects:
//   * Compile*     - from an HLSL source string (in-code / fallback)
//   * Compile*File - from an HLSL source file (dev hot-reload)
//   * Load*        - from a precompiled .cso bytecode blob (shipping path;
//                    mirrors the 360, which has no runtime shader compiler)
// Compilation uses d3dcompiler (Windows SDK) — no legacy d3dx9.
namespace shader
{
    IDirect3DVertexShader9* CompileVS(IDirect3DDevice9* device, const char* src, const char* entry);
    IDirect3DPixelShader9*  CompilePS(IDirect3DDevice9* device, const char* src, const char* entry);

    IDirect3DVertexShader9* CompileVSFile(IDirect3DDevice9* device, const std::filesystem::path& hlsl, const char* entry);
    IDirect3DPixelShader9*  CompilePSFile(IDirect3DDevice9* device, const std::filesystem::path& hlsl, const char* entry);

    // Return nullptr quietly if the .cso is absent (expected when only source
    // shipped); log an error only if a present blob fails to create.
    IDirect3DVertexShader9* LoadVS(IDirect3DDevice9* device, const std::filesystem::path& cso);
    IDirect3DPixelShader9*  LoadPS(IDirect3DDevice9* device, const std::filesystem::path& cso);

    // Content-pipeline bake (editor): compile an .hlsl (VSMain + PSMain) to
    // <out_dir>/<stem>_vs.cso and <out_dir>/<stem>_ps.cso. out_dir is the
    // project's shaders/ folder, which holds ALL compiled shaders (standard +
    // custom) so a shipped game is self-contained. Returns false (sets err) on
    // failure. Creates out_dir if needed.
    bool BakeToCso(const std::filesystem::path& hlsl, const std::filesystem::path& out_dir, std::string& err);
}
