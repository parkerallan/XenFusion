#include "render/ShaderCache.h"

#include "core/Log.h"
#include "render/Shader.h"

#include <windows.h> // GetTickCount64

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
    // Parse //@ directives into raw device render-state values (done once at
    // load; unknown keys/values leave the defaults).
    ShaderState ParseState(const fs::path& hlsl)
    {
        ShaderState s;
        std::ifstream in(hlsl);
        std::string line;
        while (std::getline(in, line))
        {
            const std::size_t at = line.find("//@");
            if (at == std::string::npos)
                continue;
            std::istringstream ss(line.substr(at + 3));
            std::string key, val;
            ss >> key >> val;
            for (char& c : key) c = (char)std::tolower((unsigned char)c);
            for (char& c : val) c = (char)std::tolower((unsigned char)c);

            if (key == "geometry")
            {
                if (val == "volume")     s.geometry = ShaderState::Volume;
                else if (val == "model") s.geometry = ShaderState::Model;
                else if (val == "quad")  s.geometry = ShaderState::Quad;
            }
            else if (key == "blend")
            {
                if (val == "opaque")        { s.alphaBlend = FALSE; }
                else if (val == "alpha")    { s.alphaBlend = TRUE; s.srcBlend = D3DBLEND_SRCALPHA; s.destBlend = D3DBLEND_INVSRCALPHA; }
                else if (val == "additive") { s.alphaBlend = TRUE; s.srcBlend = D3DBLEND_SRCALPHA; s.destBlend = D3DBLEND_ONE; }
            }
            else if (key == "depth")
            {
                if (val == "on")        { s.zEnable = TRUE;  s.zWrite = TRUE; }
                else if (val == "test") { s.zEnable = TRUE;  s.zWrite = FALSE; }
                else if (val == "off")  { s.zEnable = FALSE; s.zWrite = FALSE; }
            }
            else if (key == "cull")
            {
                if (val == "none")      s.cull = D3DCULL_NONE;
                else if (val == "back") s.cull = D3DCULL_CCW;
            }
        }
        return s;
    }
}

void CustomShader::Release()
{
    if (ps) { ps->Release(); ps = nullptr; }
    if (vs) { vs->Release(); vs = nullptr; }
}

void ShaderCache::Init(IDirect3DDevice9* device)
{
    m_device = device;
}

void ShaderCache::Shutdown()
{
    for (auto& kv : m_cache) kv.second.shader.Release();
    m_cache.clear();
    m_device = nullptr;
}

void ShaderCache::SetProjectRoot(const fs::path& root)
{
    if (root == m_root)
        return;
    Clear();
    m_root = root;
}

void ShaderCache::Clear()
{
    for (auto& kv : m_cache) kv.second.shader.Release();
    m_cache.clear();
}

CustomShader* ShaderCache::Get(const std::string& rel_path)
{
    if (rel_path.empty() || !m_device)
        return nullptr;

    // Cached? (cleared on recompile / project change.)
    auto it = m_cache.find(rel_path);
    if (it != m_cache.end())
        return it->second.shader.Valid() ? &it->second.shader : nullptr;

    // Load the compiled bytecode from the project's shaders/ folder. Compiling is
    // the Reload-shaders action's job, not this. Render state comes from the .hlsl.
    const fs::path    out    = m_root / "shaders";
    const fs::path    abs    = m_root / rel_path; // .hlsl source (for the //@ state)
    const std::string stem   = fs::path(rel_path).stem().string();

    Entry& e = m_cache[rel_path];
    IDirect3DVertexShader9* vs = shader::LoadVS(m_device, out / (stem + "_vs.cso"));
    IDirect3DPixelShader9*  ps = shader::LoadPS(m_device, out / (stem + "_ps.cso"));
    if (vs && ps)
    {
        e.shader.vs = vs;
        e.shader.ps = ps;
        std::error_code ec;
        e.shader.state = fs::exists(abs, ec) ? ParseState(abs) : ShaderState{};
        return &e.shader;
    }

    if (vs) vs->Release();
    if (ps) ps->Release();
    applog::Warn("Shader not compiled (press 'Compile shaders'): " + rel_path);
    return nullptr;
}
