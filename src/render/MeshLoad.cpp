#include "render/Mesh.h"

#include <algorithm>
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
    bool BlobIsCurrent(const fs::path& mesh_path)
    {
        std::ifstream in(mesh_path, std::ios::binary);
        if (!in)
            return false;
        char     magic[4] = {};
        uint32_t version  = 0;
        in.read(magic, sizeof(magic));
        in.read(reinterpret_cast<char*>(&version), sizeof(version));
        return in && std::memcmp(magic, "M360", 4) == 0 && version == MESH_VERSION;
    }

    bool LoadMeshBlob(IDirect3DDevice9* device, const fs::path& mesh_path, GpuMesh& out, std::vector<MeshSubset>& out_subsets)
    {
        out_subsets.clear();
        if (!device)
            return false;

        std::ifstream in(mesh_path, std::ios::binary);
        if (!in)
            return false;

        MeshHeader h{};
        in.read(reinterpret_cast<char*>(&h), sizeof(h));
        if (!in || std::memcmp(h.magic, "M360", 4) != 0 || h.version != MESH_VERSION ||
            h.vertexCount == 0 || h.indexCount == 0 ||
            (h.flags & ~MESH_FLAG_SKINNED) != 0 ||
            h.jointCount > MAX_SKIN_JOINTS ||
            ((h.flags & MESH_FLAG_SKINNED) == 0 && h.jointCount != 0) ||
            ((h.flags & MESH_FLAG_SKINNED) != 0 && h.jointCount == 0))
            return false; // wrong/old version -> caller re-bakes

        std::vector<MeshVertex> vertices(h.vertexCount);
        std::vector<uint32_t>   indices(h.indexCount);
        in.read(reinterpret_cast<char*>(vertices.data()), (std::streamsize)(vertices.size() * sizeof(MeshVertex)));
        in.read(reinterpret_cast<char*>(indices.data()),  (std::streamsize)(indices.size() * sizeof(uint32_t)));
        if (!in)
            return false;

        out.boundsMin[0] = out.boundsMax[0] = vertices[0].px;
        out.boundsMin[1] = out.boundsMax[1] = vertices[0].py;
        out.boundsMin[2] = out.boundsMax[2] = vertices[0].pz;
        for (const MeshVertex& vertex : vertices)
        {
            const float position[3] = {vertex.px, vertex.py, vertex.pz};
            for (int axis = 0; axis < 3; ++axis)
            {
                out.boundsMin[axis] = (std::min)(out.boundsMin[axis], position[axis]);
                out.boundsMax[axis] = (std::max)(out.boundsMax[axis], position[axis]);
            }
        }

        // Per-material subsets: index range + texture paths.
        uint32_t subset_count = 0;
        in.read(reinterpret_cast<char*>(&subset_count), sizeof(subset_count));
        if (!in || subset_count == 0 || subset_count > 1024)
            return false;
        for (uint32_t i = 0; i < subset_count; ++i)
        {
            MeshSubset s;
            in.read(reinterpret_cast<char*>(&s.indexStart), sizeof(s.indexStart));
            in.read(reinterpret_cast<char*>(&s.indexCount), sizeof(s.indexCount));
            ReadStr(in, s.textures.diffuse);
            ReadStr(in, s.textures.normal);
            ReadStr(in, s.textures.specular);
            ReadStr(in, s.textures.emissive);
            ReadStr(in, s.textures.metallic);
            ReadStr(in, s.textures.clearcoat);
            ReadStr(in, s.textures.roughness);
            if (!in || s.indexStart + s.indexCount > h.indexCount)
                return false;
            out_subsets.push_back(std::move(s));
        }

        std::vector<MeshSkinInfluence> influences;
        if ((h.flags & MESH_FLAG_SKINNED) != 0)
        {
            influences.resize(h.vertexCount);
            in.read(reinterpret_cast<char*>(influences.data()),
                    (std::streamsize)(influences.size() * sizeof(MeshSkinInfluence)));
            if (!in)
                return false;

            out.skeleton.fingerprint = h.skeletonFingerprint;
            out.skeleton.joints.resize(h.jointCount);
            for (MeshJoint& joint : out.skeleton.joints)
            {
                ReadStr(in, joint.name);
                in.read(reinterpret_cast<char*>(&joint.parent), sizeof(joint.parent));
                in.read(reinterpret_cast<char*>(joint.inverseBind), sizeof(joint.inverseBind));
                in.read(reinterpret_cast<char*>(joint.bindLocal), sizeof(joint.bindLocal));
                if (!in || joint.name.empty() || joint.parent < -1 ||
                    joint.parent >= (int32_t)h.jointCount)
                {
                    out.Release();
                    return false;
                }
            }
        }

        const UINT vb_bytes = (UINT)(vertices.size() * sizeof(MeshVertex));
        if (FAILED(device->CreateVertexBuffer(vb_bytes, 0, 0 /*non-FVF, uses a decl*/, D3DPOOL_MANAGED, &out.vb, nullptr)))
            return false;
        void* dst = nullptr;
        out.vb->Lock(0, 0, &dst, 0);
        std::memcpy(dst, vertices.data(), vb_bytes);
        out.vb->Unlock();

        if (!influences.empty())
        {
            const UINT skin_bytes = (UINT)(influences.size() * sizeof(MeshSkinInfluence));
            if (FAILED(device->CreateVertexBuffer(skin_bytes, 0, 0, D3DPOOL_MANAGED,
                                                  &out.skinVb, nullptr)))
            {
                out.Release();
                return false;
            }
            out.skinVb->Lock(0, 0, &dst, 0);
            std::memcpy(dst, influences.data(), skin_bytes);
            out.skinVb->Unlock();
        }

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
