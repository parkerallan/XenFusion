#include "render/Mesh.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    void ReadStr(std::ifstream& in, std::string& s)
    {
        s.clear();
        uint32_t len = 0;
        in.read(reinterpret_cast<char*>(&len), sizeof(len));
        if (in && len > 0 && len < 4096)
        {
            s.resize(len);
            in.read(s.data(), len);
        }
    }
}

namespace mesh
{
    bool LoadMeshBlob(IDirect3DDevice9* device, const fs::path& mesh_path, GpuMesh& out, MeshTextures& out_tex)
    {
        out_tex = {};
        if (!device)
            return false;

        std::ifstream in(mesh_path, std::ios::binary);
        if (!in)
            return false;

        MeshHeader h{};
        in.read(reinterpret_cast<char*>(&h), sizeof(h));
        if (!in || std::memcmp(h.magic, "M360", 4) != 0 || h.version != MESH_VERSION ||
            h.vertexCount == 0 || h.indexCount == 0)
            return false; // wrong/old version -> caller re-bakes

        std::vector<MeshVertex> vertices(h.vertexCount);
        std::vector<uint32_t>   indices(h.indexCount);
        in.read(reinterpret_cast<char*>(vertices.data()), (std::streamsize)(vertices.size() * sizeof(MeshVertex)));
        in.read(reinterpret_cast<char*>(indices.data()),  (std::streamsize)(indices.size() * sizeof(uint32_t)));
        if (!in)
            return false;

        ReadStr(in, out_tex.diffuse);
        ReadStr(in, out_tex.normal);
        ReadStr(in, out_tex.specular);

        const UINT vb_bytes = (UINT)(vertices.size() * sizeof(MeshVertex));
        if (FAILED(device->CreateVertexBuffer(vb_bytes, 0, 0 /*non-FVF, uses a decl*/, D3DPOOL_MANAGED, &out.vb, nullptr)))
            return false;
        void* dst = nullptr;
        out.vb->Lock(0, 0, &dst, 0);
        std::memcpy(dst, vertices.data(), vb_bytes);
        out.vb->Unlock();

        const UINT ib_bytes = (UINT)(indices.size() * sizeof(uint32_t));
        if (FAILED(device->CreateIndexBuffer(ib_bytes, 0, D3DFMT_INDEX32, D3DPOOL_MANAGED, &out.ib, nullptr)))
        {
            out.Release();
            return false;
        }
        out.ib->Lock(0, 0, &dst, 0);
        std::memcpy(dst, indices.data(), ib_bytes);
        out.ib->Unlock();

        out.vertexCount = h.vertexCount;
        out.indexCount  = h.indexCount;
        return true;
    }
}
