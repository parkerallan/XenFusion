#include "Content.h"
#include "Endian.h"
#include "SpakFormat.h"

#include "stb_image.h"

#include <stdio.h>
#include <string.h>
#include <vector>

// ---------------------------------------------------------------------------
// Path + file helpers
// ---------------------------------------------------------------------------
namespace
{
    // Append `rel` to `base`, inserting a '\' if needed and normalising any '/'
    // in `rel` to '\' (the 360 filesystem uses backslashes; "game:\...").
    std::string Join(const std::string& base, const std::string& rel)
    {
        std::string out = base;
        if (!out.empty())
        {
            char last = out[out.size() - 1];
            if (last != '\\' && last != ':' && last != '/')
                out.push_back('\\');
        }
        for (size_t i = 0; i < rel.size(); ++i)
            out.push_back(rel[i] == '/' ? '\\' : rel[i]);
        return out;
    }

    // Directory portion of a resolved path (everything up to the last separator).
    std::string DirOf(const std::string& path)
    {
        size_t slash = path.find_last_of("\\/");
        if (slash == std::string::npos)
            return std::string();
        return path.substr(0, slash);
    }

    bool ReadWholeFileBin(const std::string& path, std::vector<unsigned char>& out)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n <= 0) { fclose(f); return false; }
        out.resize((size_t)n);
        size_t got = fread(&out[0], 1, (size_t)n, f);
        fclose(f);
        out.resize(got);
        return got > 0;
    }

    void Log(const char* msg, const std::string& detail)
    {
        OutputDebugStringA(msg);
        OutputDebugStringA(detail.c_str());
        OutputDebugStringA("\n");
    }
}

// ---------------------------------------------------------------------------
// RtMesh / RtShader
// ---------------------------------------------------------------------------
void RtMesh::Release()
{
    for (size_t i = 0; i < subsets.size(); ++i)
    {
        RtSubset& s = subsets[i];
        if (s.roughness) { s.roughness->Release(); s.roughness = NULL; }
        if (s.clearcoat) { s.clearcoat->Release(); s.clearcoat = NULL; }
        if (s.metallic) { s.metallic->Release(); s.metallic = NULL; }
        if (s.emissive) { s.emissive->Release(); s.emissive = NULL; }
        if (s.specular) { s.specular->Release(); s.specular = NULL; }
        if (s.normal)   { s.normal->Release();   s.normal = NULL; }
        if (s.diffuse)  { s.diffuse->Release();  s.diffuse = NULL; }
    }
    subsets.clear();
    joints.clear();
    morph.Clear();
    morphBase.clear();
    skeletonFingerprint = 0;
    if (ib) { ib->Release(); ib = NULL; }
    if (skinVb) { skinVb->Release(); skinVb = NULL; }
    if (vb) { vb->Release(); vb = NULL; }
    vertexCount = indexCount = 0;
}

void RtShader::Release()
{
    if (skinVsConstants) { skinVsConstants->Release(); skinVsConstants = NULL; }
    if (vsConstants) { vsConstants->Release(); vsConstants = NULL; }
    if (ps) { ps->Release(); ps = NULL; }
    if (shadowPs) { shadowPs->Release(); shadowPs = NULL; }
    if (lightmapVs) { lightmapVs->Release(); lightmapVs = NULL; }
    if (skinVs) { skinVs->Release(); skinVs = NULL; }
    if (vs) { vs->Release(); vs = NULL; }
}

// ---------------------------------------------------------------------------
// Mesh blob loading (the editor's "M360" format, read little-endian)
//
// Layout: MeshHeader { char magic[4]; u32 version; u32 vertexCount; u32 indexCount;
// flags; jointCount; skeletonFingerprint }
// then vertexCount * 44-byte vertices, indexCount * u32 indices, then a u32
// subset count followed by one record per material: u32 indexStart, u32
// indexCount, then seven length-prefixed strings (diffuse / normal / specular /
// emissive / metallic / clearcoat / roughness, relative to the .mesh).
// ---------------------------------------------------------------------------
namespace
{
    const unsigned int kMeshVersion = 10;  // must match the editor's MESH_VERSION
    const unsigned int kVertexBytes = 44;  // MeshVertex

    // Advance a cursor over a length-prefixed (u32 LE) string.
    std::string ReadStr(const unsigned char* base, size_t size, size_t& off)
    {
        std::string s;
        if (off + 4 > size) return s;
        unsigned int len = endian::LoadU32LE(base + off);
        off += 4;
        if (len > 0 && len < 4096 && off + len <= size)
        {
            s.assign((const char*)(base + off), len);
            off += len;
        }
        return s;
    }

    IDirect3DTexture9* LoadTexture(IDirect3DDevice9* dev, const std::string& path)
    {
        if (path.empty()) return NULL;
        IDirect3DTexture9* t = NULL;
        // D3DX handles the 360's texture tiling/format; the editor uses stb, but
        // relying on D3DX here keeps the console path simple and correct.
        if (FAILED(D3DXCreateTextureFromFileA(dev, path.c_str(), &t)))
            return NULL;
        return t;
    }

    // How the diffuse's alpha channel is used, classified from its pixels like the
    // editor: no alpha -> Opaque; real see-through holes (a mask: hair/foliage) ->
    // Cutout; uniformly translucent, no holes -> Blend (glass).
    RtAlphaKind ClassifyAlpha(const std::string& path)
    {
        int w = 0, h = 0, ch = 0;
        unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4); // force RGBA
        if (!px)
            return RtOpaque;
        const int total = w * h;
        int translucent = 0, transparent = 0;
        for (int i = 0; i < total; ++i)
        {
            unsigned char a = px[i * 4 + 3];
            if (a < 255) ++translucent;
            if (a < 24)  ++transparent;
        }
        RtAlphaKind kind = RtOpaque;
        if (translucent == 0)              kind = RtOpaque;
        else if (transparent > total / 32) kind = RtCutout; // >~3% see-through
        else                               kind = RtBlend;
        stbi_image_free(px);
        return kind;
    }

    // Does a normal map's alpha carry a height field (bump offset, 0.5 =
    // neutral)? Mirrors the editor/cooker check: only if the alpha actually
    // varies — a normal map without an alpha channel (stb reports the file's
    // channel count) or with a blank one carries no relief.
    bool NormalAlphaHasHeight(const std::string& path)
    {
        int w = 0, h = 0, ch = 0;
        unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4); // force RGBA
        if (!px)
            return false;
        bool varies = false;
        if (ch == 4)
        {
            unsigned char amin = 255, amax = 0;
            const int total = w * h;
            for (int i = 0; i < total; ++i)
            {
                unsigned char a = px[i * 4 + 3];
                if (a < amin) amin = a;
                if (a > amax) amax = a;
            }
            varies = (amax - amin) >= 8;
        }
        stbi_image_free(px);
        return varies;
    }
}

RtMesh* Content::GetMesh(const std::string& relPath)
{
    if (relPath.empty() || !m_device)
        return NULL;

    std::map<std::string, RtMesh>::iterator it = m_meshes.find(relPath);
    if (it != m_meshes.end())
        return it->second.vb ? &it->second : NULL;

    RtMesh mesh; // empty entry cached even on failure, so we don't retry each frame

    const std::string abs = Resolve(relPath);
    std::vector<unsigned char> blob;
    if (!ReadWholeFileBin(abs, blob) || blob.size() < 28)
    {
        Log("mesh: cannot read ", abs);
        m_meshes[relPath] = mesh;
        return NULL;
    }

    const unsigned char* p = &blob[0];
    const size_t size = blob.size();

    if (memcmp(p, "M360", 4) != 0)
    {
        Log("mesh: bad magic ", relPath);
        m_meshes[relPath] = mesh;
        return NULL;
    }
    unsigned int version = endian::LoadU32LE(p + 4);
    unsigned int vcount  = endian::LoadU32LE(p + 8);
    unsigned int icount  = endian::LoadU32LE(p + 12);
    unsigned int flags   = endian::LoadU32LE(p + 16);
    unsigned int jointCount = endian::LoadU32LE(p + 20);
    unsigned int skeletonFingerprint = endian::LoadU32LE(p + 24);
    if (version != kMeshVersion || vcount == 0 || icount == 0)
    {
        Log("mesh: version/empty ", relPath);
        m_meshes[relPath] = mesh;
        return NULL;
    }
    const bool skinned = (flags & 1u) != 0;
    if ((skinned && (jointCount == 0 || jointCount > spak::kMaxSkinJoints)) ||
        (!skinned && jointCount != 0))
    {
        Log("mesh: invalid skin metadata ", relPath);
        m_meshes[relPath] = mesh;
        return NULL;
    }

    size_t off = 28;
    const size_t vbytes = (size_t)vcount * kVertexBytes;
    const size_t ibytes = (size_t)icount * 4;
    if (off + vbytes + ibytes > size)
    {
        Log("mesh: truncated ", relPath);
        m_meshes[relPath] = mesh;
        return NULL;
    }

    for (unsigned int vertex = 0; vertex < vcount; ++vertex)
    {
        const unsigned char* source = p + off + (size_t)vertex * kVertexBytes;
        for (int axis = 0; axis < 3; ++axis)
        {
            const float value = endian::LoadF32LE(source + axis * sizeof(float));
            if (vertex == 0 || value < mesh.boundsMin[axis]) mesh.boundsMin[axis] = value;
            if (vertex == 0 || value > mesh.boundsMax[axis]) mesh.boundsMax[axis] = value;
        }
    }

    // Vertex + index buffers. Xenia fetches buffer data in native (big-endian)
    // order, so the little-endian file data is converted, not copied raw.
    if (FAILED(m_device->CreateVertexBuffer((UINT)vbytes, 0, 0, D3DPOOL_MANAGED, &mesh.vb, NULL)))
    {
        m_meshes[relPath] = mesh;
        return NULL;
    }
    {
        void* dst = NULL;
        mesh.vb->Lock(0, 0, &dst, 0);
        endian::StoreNativeFromLE32(dst, p + off, vbytes);
        mesh.vb->Unlock();
    }
    // Kept in system memory: deltas accumulate onto the base pose each frame.
    if ((flags & 2u) != 0)
    {
        mesh.morphBase.resize(vbytes);
        endian::StoreNativeFromLE32(&mesh.morphBase[0], p + off, vbytes);
    }
    off += vbytes;

    if (FAILED(m_device->CreateIndexBuffer((UINT)ibytes, 0, D3DFMT_INDEX32, D3DPOOL_MANAGED, &mesh.ib, NULL)))
    {
        mesh.Release();
        m_meshes[relPath] = mesh;
        return NULL;
    }
    {
        void* dst = NULL;
        mesh.ib->Lock(0, 0, &dst, 0);
        endian::StoreNativeFromLE32(dst, p + off, ibytes);
        mesh.ib->Unlock();
    }
    off += ibytes;

    mesh.vertexCount = vcount;
    mesh.indexCount  = icount;

    // Material subsets: index range + texture references (relative to the mesh
    // file's directory); each subset's alpha mode comes from its own diffuse.
    const std::string meshDir = DirOf(abs);
    unsigned int subsetCount = 0;
    if (off + 4 <= size) { subsetCount = endian::LoadU32LE(p + off); off += 4; }
    if (subsetCount == 0 || subsetCount > 1024)
    {
        Log("mesh: bad subset table ", relPath);
        mesh.Release();
        m_meshes[relPath] = mesh;
        return NULL;
    }
    for (unsigned int i = 0; i < subsetCount; ++i)
    {
        RtSubset sub;
        if (off + 8 > size)
        {
            Log("mesh: subset truncated ", relPath);
            mesh.Release();
            m_meshes[relPath] = mesh;
            return NULL;
        }
        sub.indexStart = endian::LoadU32LE(p + off); off += 4;
        sub.indexCount = endian::LoadU32LE(p + off); off += 4;
        std::string texDiffuse = ReadStr(p, size, off);
        std::string texNormal  = ReadStr(p, size, off);
        std::string texSpec    = ReadStr(p, size, off);
        std::string texEmis    = ReadStr(p, size, off);
        std::string texMetal   = ReadStr(p, size, off);
        std::string texCoat    = ReadStr(p, size, off);
        std::string texRough   = ReadStr(p, size, off);
        if (!texDiffuse.empty())
        {
            const std::string dabs = Join(meshDir, texDiffuse);
            sub.diffuse = LoadTexture(m_device, dabs);
            sub.alpha   = ClassifyAlpha(dabs); // transparency mode from the diffuse's alpha
        }
        if (!texNormal.empty())
        {
            const std::string nabs = Join(meshDir, texNormal);
            sub.normal          = LoadTexture(m_device, nabs);
            sub.normalHasHeight = NormalAlphaHasHeight(nabs); // bump offset from the normal's alpha
        }
        if (!texSpec.empty())   sub.specular = LoadTexture(m_device, Join(meshDir, texSpec));
        if (!texEmis.empty())   sub.emissive = LoadTexture(m_device, Join(meshDir, texEmis));
        if (!texMetal.empty())  sub.metallic = LoadTexture(m_device, Join(meshDir, texMetal));
        if (!texCoat.empty())   sub.clearcoat = LoadTexture(m_device, Join(meshDir, texCoat));
        if (!texRough.empty())  sub.roughness = LoadTexture(m_device, Join(meshDir, texRough));
        mesh.subsets.push_back(sub);
    }

    if (skinned)
    {
        const size_t skinBytes = (size_t)vcount * spak::kSkinInfluenceBytes;
        if (off + skinBytes > size ||
            FAILED(m_device->CreateVertexBuffer((UINT)skinBytes, 0, 0, D3DPOOL_MANAGED, &mesh.skinVb, NULL)))
        {
            Log("mesh: skin stream truncated ", relPath);
            mesh.Release();
            m_meshes[relPath] = mesh;
            return NULL;
        }
        void* dst = NULL;
        mesh.skinVb->Lock(0, 0, &dst, 0);
        memcpy(dst, p + off, skinBytes);
        mesh.skinVb->Unlock();
        off += skinBytes;

        for (unsigned int i = 0; i < jointCount; ++i)
        {
            RtJoint joint;
            std::string name = ReadStr(p, size, off);
            if (name.empty() || off + 4 + 128 > size)
            {
                Log("mesh: skeleton truncated ", relPath);
                mesh.Release();
                m_meshes[relPath] = mesh;
                return NULL;
            }
            joint.nameHash = spak::NameHash(name.c_str());
            joint.parent = (int)endian::LoadU32LE(p + off); off += 4;
            if (joint.parent >= (int)i)
            {
                Log("mesh: invalid joint parent ", relPath);
                mesh.Release();
                m_meshes[relPath] = mesh;
                return NULL;
            }
            endian::StoreNativeFromLE32(joint.inverseBind, p + off, 64); off += 64;
            endian::StoreNativeFromLE32(joint.bindLocal, p + off, 64); off += 64;
            mesh.joints.push_back(joint);
        }
        mesh.skeletonFingerprint = skeletonFingerprint;
    }

    if ((flags & 2u) != 0)
    {
        unsigned int targetCount = 0;
        if (off + 12 <= size)
        {
            targetCount            = endian::LoadU32LE(p + off); off += 4;
            mesh.morph.firstVertex = endian::LoadU32LE(p + off); off += 4;
            mesh.morph.vertexCount = endian::LoadU32LE(p + off); off += 4;
        }
        if (targetCount == 0 || targetCount > spak::kMaxMorphTargets ||
            mesh.morph.vertexCount == 0 ||
            mesh.morph.firstVertex + mesh.morph.vertexCount > vcount)
        {
            Log("mesh: bad morph block ", relPath);
            mesh.Release();
            m_meshes[relPath] = mesh;
            return NULL;
        }
        for (unsigned int t = 0; t < targetCount; ++t)
        {
            RtMorphTarget target;
            const std::string name = ReadStr(p, size, off); // skipped
            (void)name;
            if (off + 12 > size)
            {
                Log("mesh: morph target truncated ", relPath);
                mesh.Release();
                m_meshes[relPath] = mesh;
                return NULL;
            }
            target.shape         = (int)endian::LoadU32LE(p + off); off += 4;
            target.positionScale = endian::LoadF32LE(p + off);      off += 4;
            const unsigned int deltaCount = endian::LoadU32LE(p + off); off += 4;
            if (deltaCount == 0 || deltaCount > mesh.morph.vertexCount ||
                off + (size_t)deltaCount * sizeof(face::MorphDelta) > size)
            {
                Log("mesh: morph deltas truncated ", relPath);
                mesh.Release();
                m_meshes[relPath] = mesh;
                return NULL;
            }
            target.deltas.resize(deltaCount);
            for (unsigned int d = 0; d < deltaCount; ++d)
            {
                const unsigned char* record = p + off + (size_t)d * sizeof(face::MorphDelta);
                face::MorphDelta& delta = target.deltas[d];
                delta.vertex = endian::LoadU16LE(record);
                delta.px = (short)endian::LoadU16LE(record + 2);
                delta.py = (short)endian::LoadU16LE(record + 4);
                delta.pz = (short)endian::LoadU16LE(record + 6);
                delta.nx = (signed char)record[8];
                delta.ny = (signed char)record[9];
                delta.nz = (signed char)record[10];
                delta.pad = 0;
            }
            off += (size_t)deltaCount * sizeof(face::MorphDelta);
            mesh.morph.targets.push_back(target);
        }
    }

    m_meshes[relPath] = mesh;
    return &m_meshes[relPath];
}

// ---------------------------------------------------------------------------
// Shader loading: compile the .hlsl at load into Xenos microcode + parse //@.
// ---------------------------------------------------------------------------
namespace
{
    void LowerInPlace(std::string& s)
    {
        for (size_t i = 0; i < s.size(); ++i)
            if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] - 'A' + 'a');
    }

    // Parse //@ directives into raw device states. Reads from a .dir sidecar
    // baked at deploy (the shipped game has no .hlsl) or the .hlsl itself (dev).
    // Returns false if the file couldn't be opened.
    bool ParseState(const std::string& absPath, RtShaderState& s)
    {
        FILE* f = fopen(absPath.c_str(), "rb");
        if (!f) return false;
        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            const char* at = strstr(line, "//@");
            if (!at) continue;
            char keyb[64], valb[64];
            keyb[0] = valb[0] = '\0';
            sscanf(at + 3, "%63s %63s", keyb, valb);
            std::string key = keyb, val = valb;
            LowerInPlace(key); LowerInPlace(val);

            if (key == "geometry")
            {
                if (val == "volume")     s.geometry = RtShaderState::Volume;
                else if (val == "model") s.geometry = RtShaderState::Model;
                else if (val == "quad")  s.geometry = RtShaderState::Quad;
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
        fclose(f);
        return true;
    }

    // Compile one entry point of an .hlsl file to a shader. Returns the bytecode
    // blob (caller creates the shader + releases), or NULL on failure. If
    // outTable is non-NULL, the constant table is handed to the caller (for
    // packing-correct matrix upload) instead of released here.
    LPD3DXBUFFER CompileEntry(const std::string& absPath, const char* entry, const char* profile,
                              LPD3DXCONSTANTTABLE* outTable)
    {
        if (outTable) *outTable = NULL;
        LPD3DXBUFFER code = NULL;
        LPD3DXBUFFER errors = NULL;
        LPD3DXCONSTANTTABLE ct = NULL;
        // Column-major (the 360 compiler's default; it ignores ROWMAJOR). The
        // runtime transposes its matrices before upload to match — see RtMath's
        // Transpose() and SceneRuntime.
        HRESULT hr = D3DXCompileShaderFromFileA(absPath.c_str(), NULL, NULL, entry, profile,
                                                0, &code, &errors, &ct);
        if (FAILED(hr))
        {
            OutputDebugStringA("shader compile failed: ");
            OutputDebugStringA(absPath.c_str());
            OutputDebugStringA("\n");
            if (errors)
            {
                OutputDebugStringA((const char*)errors->GetBufferPointer());
                OutputDebugStringA("\n");
                errors->Release();
            }
            if (code) code->Release();
            if (ct) ct->Release();
            return NULL;
        }
        if (errors) errors->Release();
        // The VS constant table is handed to the caller (for packing-correct matrix
        // upload via SetMatrix); the PS table isn't needed.
        if (outTable) *outTable = ct;
        else if (ct)  ct->Release();
        return code;
    }

    bool CompileShaderFile(IDirect3DDevice9* dev, const std::string& absPath, RtShader& out)
    {
        LPD3DXBUFFER vsc = CompileEntry(absPath, "VSMain", "vs_3_0", &out.vsConstants);
        LPD3DXBUFFER psc = CompileEntry(absPath, "PSMain", "ps_3_0", NULL);
        bool ok = false;
        if (vsc && psc)
        {
            HRESULT hv = dev->CreateVertexShader((const DWORD*)vsc->GetBufferPointer(), &out.vs);
            HRESULT hp = dev->CreatePixelShader((const DWORD*)psc->GetBufferPointer(), &out.ps);
            ok = SUCCEEDED(hv) && SUCCEEDED(hp) && out.vs && out.ps;
            if (!ok) out.Release();
        }
        if (ok && out.vsConstants)
        {
            out.hWVP   = out.vsConstants->GetConstantByName(NULL, "gWVP");
            out.hWorld = out.vsConstants->GetConstantByName(NULL, "gWorld");
        }
        if (vsc) vsc->Release();
        if (psc) psc->Release();
        return ok;
    }

    // Filename stem (no directory, no extension) of a scene-relative path,
    // e.g. "assets/Fire.hlsl" -> "Fire".
    std::string StemOf(const std::string& rel)
    {
        const size_t slash = rel.find_last_of("\\/");
        const size_t start = (slash == std::string::npos) ? 0 : slash + 1;
        const size_t dot   = rel.find_last_of('.');
        const size_t end   = (dot == std::string::npos || dot < start) ? rel.size() : dot;
        return rel.substr(start, end - start);
    }

    // Load a precompiled Xenos shader pair (.cso, produced offline by the XDK's
    // fxc), plus the VS constant table for packing-correct matrix upload. This is
    // the shipping path — no HLSL compilation on the console. Returns false when
    // either .cso is missing/bad so the caller can fall back to compiling the HLSL.
    bool LoadShaderCso(IDirect3DDevice9* dev, const std::string& vsPath,
                       const std::string& psPath, RtShader& out)
    {
        std::vector<unsigned char> vs, ps;
        if (!ReadWholeFileBin(vsPath, vs) || !ReadWholeFileBin(psPath, ps))
            return false;
        if (FAILED(dev->CreateVertexShader((const DWORD*)&vs[0], &out.vs)))
            return false;
        if (FAILED(dev->CreatePixelShader((const DWORD*)&ps[0], &out.ps)))
        { out.Release(); return false; }
        if (SUCCEEDED(D3DXGetShaderConstantTable((const DWORD*)&vs[0], &out.vsConstants)) && out.vsConstants)
        {
            out.hWVP   = out.vsConstants->GetConstantByName(NULL, "gWVP");
            out.hWorld = out.vsConstants->GetConstantByName(NULL, "gWorld");
        }
        return out.vs && out.ps;
    }

    bool LoadSkinVertexCso(IDirect3DDevice9* dev, const std::string& path, RtShader& out)
    {
        std::vector<unsigned char> code;
        if (!ReadWholeFileBin(path, code) ||
            FAILED(dev->CreateVertexShader((const DWORD*)&code[0], &out.skinVs)))
            return false;
        if (SUCCEEDED(D3DXGetShaderConstantTable((const DWORD*)&code[0], &out.skinVsConstants)) &&
            out.skinVsConstants)
        {
            out.hSkinWVP = out.skinVsConstants->GetConstantByName(NULL, "gWVP");
            out.hSkinWorld = out.skinVsConstants->GetConstantByName(NULL, "gWorld");
        }
        return true;
    }

    bool LoadLightmapVertexCso(IDirect3DDevice9* dev, const std::string& path, RtShader& out)
    {
        std::vector<unsigned char> code;
        return ReadWholeFileBin(path, code) &&
               SUCCEEDED(dev->CreateVertexShader((const DWORD*)&code[0], &out.lightmapVs));
    }

    bool CompileSkinVertexFile(IDirect3DDevice9* dev, const std::string& path, RtShader& out)
    {
        LPD3DXBUFFER code = CompileEntry(path, "SkinVSMain", "vs_3_0", &out.skinVsConstants);
        if (!code)
            return false;
        const HRESULT hr = dev->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &out.skinVs);
        code->Release();
        if (FAILED(hr) || !out.skinVs)
            return false;
        if (out.skinVsConstants)
        {
            out.hSkinWVP = out.skinVsConstants->GetConstantByName(NULL, "gWVP");
            out.hSkinWorld = out.skinVsConstants->GetConstantByName(NULL, "gWorld");
        }
        return true;
    }

    bool CompileLightmapVertexFile(IDirect3DDevice9* dev, const std::string& path, RtShader& out)
    {
        LPD3DXCONSTANTTABLE constants = NULL;
        LPD3DXBUFFER code = CompileEntry(path, "LightmapVSMain", "vs_3_0", &constants);
        if (constants) constants->Release();
        if (!code) return false;
        const HRESULT hr = dev->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &out.lightmapVs);
        code->Release();
        return SUCCEEDED(hr) && out.lightmapVs;
    }

    bool LoadShadowPixelCso(IDirect3DDevice9* dev, const std::string& path, RtShader& out)
    {
        std::vector<unsigned char> code;
        return ReadWholeFileBin(path, code) && !code.empty() &&
               SUCCEEDED(dev->CreatePixelShader((const DWORD*)&code[0], &out.shadowPs));
    }

    bool CompileShadowPixelFile(IDirect3DDevice9* dev, const std::string& path, RtShader& out)
    {
        LPD3DXBUFFER code = CompileEntry(path, "ShadowPSMain", "ps_3_0", NULL);
        if (!code) return false;
        const HRESULT hr = dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &out.shadowPs);
        code->Release();
        return SUCCEEDED(hr) && out.shadowPs;
    }
}

RtShader* Content::GetShader(const std::string& relPath)
{
    if (relPath.empty() || !m_device)
        return NULL;

    std::map<std::string, RtShader>::iterator it = m_shaders.find(relPath);
    if (it != m_shaders.end())
        return it->second.Valid() ? &it->second : NULL;

    RtShader sh;
    const std::string abs  = Resolve(relPath);
    const std::string stem = StemOf(relPath);
    // Precompiled Xenos .cso first (shipping path); fall back to compiling the HLSL.
    bool ok = LoadShaderCso(m_device, Resolve("shaders/" + stem + "_vs.cso"),
                                      Resolve("shaders/" + stem + "_ps.cso"), sh);
    if (!ok) ok = CompileShaderFile(m_device, abs, sh); // dev fallback (needs .hlsl)
    if (ok)
    {
        // //@ render setup: the baked sidecar (shipped) or the .hlsl (dev).
        if (!ParseState(Resolve("shaders/" + stem + ".dir"), sh.state))
            ParseState(abs, sh.state);
    }
    else Log("shader: failed ", relPath);

    m_shaders[relPath] = sh;
    return m_shaders[relPath].Valid() ? &m_shaders[relPath] : NULL;
}

bool Content::LoadStandard()
{
    m_standard.Release();
    // Precompiled Xenos .cso first (shipping path); fall back to compiling the HLSL.
    if (!LoadShaderCso(m_device, Resolve("shaders/standard_vs.cso"),
                                 Resolve("shaders/standard_ps.cso"), m_standard) &&
        !CompileShaderFile(m_device, Resolve("shaders/standard.hlsl"), m_standard))
    {
        Log("shader: standard failed", "");
        return false;
    }
    if (!LoadSkinVertexCso(m_device, Resolve("shaders/standard_skin_vs.cso"), m_standard) &&
        !CompileSkinVertexFile(m_device, Resolve("shaders/standard.hlsl"), m_standard))
        Log("shader: standard skin vertex shader failed", "");
    if (!LoadLightmapVertexCso(m_device, Resolve("shaders/standard_lightmap_vs.cso"), m_standard) &&
        !CompileLightmapVertexFile(m_device, Resolve("shaders/standard.hlsl"), m_standard))
        Log("shader: standard lightmap vertex shader failed", "");
    if (!LoadShadowPixelCso(m_device, Resolve("shaders/standard_shadow_ps.cso"), m_standard) &&
        !CompileShadowPixelFile(m_device, Resolve("shaders/standard.hlsl"), m_standard))
        Log("shader: standard shadow pixel shader failed", "");
    return true;
}

IDirect3DCubeTexture9* Content::LoadEnvCube()
{
    if (!m_device)
        return NULL;

    // Face order matches D3DCUBEMAP_FACE_POSITIVE_X .. NEGATIVE_Z. Faces load
    // as plain 2D textures (D3DX handles decode + tiling), then D3DX copies
    // each into the cube face, converting as needed.
    static const char* kFaces[6] =
        { "env_px.png", "env_nx.png", "env_py.png", "env_ny.png", "env_pz.png", "env_nz.png" };

    IDirect3DTexture9* faces[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
    unsigned int size = 0;
    for (int f = 0; f < 6; ++f)
    {
        faces[f] = LoadTexture(m_device, Resolve("assets\\env\\" + std::string(kFaces[f])));
        if (!faces[f])
        {
            for (int k = 0; k < f; ++k) faces[k]->Release();
            return NULL; // no/partial env set -> no reflections
        }
        D3DSURFACE_DESC d;
        faces[f]->GetLevelDesc(0, &d);
        if (d.Width != d.Height || (size != 0 && d.Width != size))
        {
            for (int k = 0; k <= f; ++k) faces[k]->Release();
            Log("env: face size mismatch ", kFaces[f]);
            return NULL;
        }
        size = d.Width;
    }

    IDirect3DCubeTexture9* cube = NULL;
    if (SUCCEEDED(m_device->CreateCubeTexture(size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, NULL)))
    {
        for (int f = 0; f < 6; ++f)
        {
            IDirect3DSurface9* dst = NULL;
            IDirect3DSurface9* src = NULL;
            if (SUCCEEDED(cube->GetCubeMapSurface((D3DCUBEMAP_FACES)f, 0, &dst)) &&
                SUCCEEDED(faces[f]->GetSurfaceLevel(0, &src)))
            {
                D3DXLoadSurfaceFromSurface(dst, NULL, NULL, src, NULL, NULL, D3DX_FILTER_NONE, 0);
            }
            if (src) src->Release();
            if (dst) dst->Release();
        }
    }
    for (int f = 0; f < 6; ++f) faces[f]->Release();
    return cube;
}

bool Content::LoadBuiltin(const char* stem, RtShader& out)
{
    out.Release();
    const std::string s(stem);
    if (LoadShaderCso(m_device, Resolve("shaders/" + s + "_vs.cso"),
                                Resolve("shaders/" + s + "_ps.cso"), out))
        return true;
    if (CompileShaderFile(m_device, Resolve("shaders/" + s + ".hlsl"), out))
        return true;
    Log("shader: builtin failed ", s);
    return false;
}

// ---------------------------------------------------------------------------
std::string Content::Resolve(const std::string& rel) const
{
    return Join(m_root, rel);
}

void Content::Init(IDirect3DDevice9* device, const std::string& contentRoot)
{
    m_device = device;
    m_root   = contentRoot;
}

void Content::Shutdown()
{
    for (std::map<std::string, RtMesh>::iterator it = m_meshes.begin(); it != m_meshes.end(); ++it)
        it->second.Release();
    m_meshes.clear();
    for (std::map<std::string, RtShader>::iterator it = m_shaders.begin(); it != m_shaders.end(); ++it)
        it->second.Release();
    m_shaders.clear();
    m_standard.Release();
    m_device = NULL;
}
