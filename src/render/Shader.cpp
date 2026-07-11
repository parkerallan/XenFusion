#include "render/Shader.h"

#include "core/Log.h"

#include <d3dcompiler.h>

#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace shader
{
    namespace
    {
        ID3DBlob* Compile(const char* src, size_t len, const char* name, const char* entry, const char* target)
        {
            ID3DBlob* code = nullptr;
            ID3DBlob* errors = nullptr;
            // Row-major so our D3DMATRIX uploads straight into the constants.
            const UINT flags = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_OPTIMIZATION_LEVEL3;
            const HRESULT hr = D3DCompile(src, len, name, nullptr, nullptr,
                                          entry, target, flags, 0, &code, &errors);
            if (FAILED(hr))
            {
                applog::Error(std::string("Shader compile failed (") + target + "): " +
                              (errors ? (const char*)errors->GetBufferPointer() : "unknown"));
                if (errors) errors->Release();
                if (code) code->Release();
                return nullptr;
            }
            if (errors) errors->Release();
            return code;
        }

        // Read a whole file into bytes. Returns false if it can't be opened.
        bool ReadFile(const std::filesystem::path& path, std::vector<char>& out)
        {
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f) return false;
            const std::streamsize n = f.tellg();
            f.seekg(0);
            out.resize((size_t)(n > 0 ? n : 0));
            if (n > 0) f.read(out.data(), n);
            return true;
        }

        bool WriteBlob(const std::filesystem::path& path, ID3DBlob* blob)
        {
            std::ofstream o(path, std::ios::binary | std::ios::trunc);
            if (!o) return false;
            o.write((const char*)blob->GetBufferPointer(), (std::streamsize)blob->GetBufferSize());
            return (bool)o;
        }
    }

    IDirect3DVertexShader9* CompileVS(IDirect3DDevice9* device, const char* src, const char* entry)
    {
        ID3DBlob* code = Compile(src, std::strlen(src), nullptr, entry, "vs_3_0");
        if (!code) return nullptr;
        IDirect3DVertexShader9* vs = nullptr;
        device->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &vs);
        code->Release();
        return vs;
    }

    IDirect3DPixelShader9* CompilePS(IDirect3DDevice9* device, const char* src, const char* entry)
    {
        ID3DBlob* code = Compile(src, std::strlen(src), nullptr, entry, "ps_3_0");
        if (!code) return nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps);
        code->Release();
        return ps;
    }

    IDirect3DVertexShader9* CompileVSFile(IDirect3DDevice9* device, const std::filesystem::path& hlsl, const char* entry)
    {
        std::vector<char> src;
        if (!ReadFile(hlsl, src))
        {
            applog::Error("Shader source not found: " + hlsl.string());
            return nullptr;
        }
        ID3DBlob* code = Compile(src.data(), src.size(), hlsl.string().c_str(), entry, "vs_3_0");
        if (!code) return nullptr;
        IDirect3DVertexShader9* vs = nullptr;
        device->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &vs);
        code->Release();
        return vs;
    }

    IDirect3DPixelShader9* CompilePSFile(IDirect3DDevice9* device, const std::filesystem::path& hlsl, const char* entry)
    {
        std::vector<char> src;
        if (!ReadFile(hlsl, src))
        {
            applog::Error("Shader source not found: " + hlsl.string());
            return nullptr;
        }
        ID3DBlob* code = Compile(src.data(), src.size(), hlsl.string().c_str(), entry, "ps_3_0");
        if (!code) return nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        device->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps);
        code->Release();
        return ps;
    }

    IDirect3DVertexShader9* LoadVS(IDirect3DDevice9* device, const std::filesystem::path& cso)
    {
        std::vector<char> bytes;
        if (!ReadFile(cso, bytes) || bytes.empty())
            return nullptr; // absent -> caller falls back to source
        IDirect3DVertexShader9* vs = nullptr;
        if (FAILED(device->CreateVertexShader((const DWORD*)bytes.data(), &vs)))
        {
            applog::Error("Bad vertex shader bytecode: " + cso.string());
            return nullptr;
        }
        return vs;
    }

    IDirect3DPixelShader9* LoadPS(IDirect3DDevice9* device, const std::filesystem::path& cso)
    {
        std::vector<char> bytes;
        if (!ReadFile(cso, bytes) || bytes.empty())
            return nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        if (FAILED(device->CreatePixelShader((const DWORD*)bytes.data(), &ps)))
        {
            applog::Error("Bad pixel shader bytecode: " + cso.string());
            return nullptr;
        }
        return ps;
    }

    bool BakeToCso(const std::filesystem::path& hlsl, const std::filesystem::path& out_dir, std::string& err)
    {
        std::vector<char> src;
        if (!ReadFile(hlsl, src))
        {
            err = "cannot read " + hlsl.string();
            return false;
        }
        const std::string name = hlsl.string();
        ID3DBlob* vs = Compile(src.data(), src.size(), name.c_str(), "VSMain", "vs_3_0");
        ID3DBlob* ps = Compile(src.data(), src.size(), name.c_str(), "PSMain", "ps_3_0");

        bool ok = (vs && ps); // Compile() already logged any compile error
        if (ok)
        {
            std::error_code ec;
            std::filesystem::create_directories(out_dir, ec);
            const std::string stem = hlsl.stem().string();
            ok = WriteBlob(out_dir / (stem + "_vs.cso"), vs) &&
                 WriteBlob(out_dir / (stem + "_ps.cso"), ps);
            if (!ok) err = "cannot write .cso to " + out_dir.string();
        }
        else
        {
            err = "compile error";
        }
        if (vs) vs->Release();
        if (ps) ps->Release();
        return ok;
    }
}
