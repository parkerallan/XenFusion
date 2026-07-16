#include "render/Mesh.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <fstream>
#include <map>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    void WriteStr(std::ofstream& out, const std::string& s)
    {
        const uint32_t len = (uint32_t)s.size();
        out.write(reinterpret_cast<const char*>(&len), sizeof(len));
        out.write(s.data(), len);
    }
}

namespace mesh
{
    bool BakeModel(const fs::path& source, const fs::path& out_mesh, std::string& error)
    {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(
            source.string(),
            aiProcess_Triangulate |
            aiProcess_GenSmoothNormals |
            aiProcess_CalcTangentSpace |        // tangents for normal mapping
            aiProcess_JoinIdenticalVertices |
            aiProcess_PreTransformVertices |
            aiProcess_ConvertToLeftHanded);

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || scene->mNumMeshes == 0)
        {
            error = importer.GetErrorString();
            if (error.empty()) error = "no meshes in file";
            return false;
        }

        // One subset per source material: group the scene's meshes by material
        // index so each material's triangles are a contiguous index range with
        // its own texture set (e.g. hair = wispy strands + solid cap).
        std::map<unsigned, std::vector<unsigned>> by_material; // material -> meshes
        for (unsigned m = 0; m < scene->mNumMeshes; ++m)
            by_material[scene->mMeshes[m]->mMaterialIndex].push_back(m);

        auto find_tex = [](const aiMaterial* mat, std::initializer_list<aiTextureType> types) -> std::string
        {
            for (aiTextureType t : types)
            {
                aiString tex;
                if (mat->GetTexture(t, 0, &tex) == AI_SUCCESS)
                {
                    std::string s = tex.C_Str();
                    for (char& c : s) if (c == '\\') c = '/';
                    return s;
                }
            }
            return {};
        };

        std::vector<MeshVertex> vertices;
        std::vector<uint32_t>   indices;
        std::vector<MeshSubset> subsets;
        for (const auto& [mat_index, mesh_list] : by_material)
        {
            MeshSubset subset;
            subset.indexStart = (uint32_t)indices.size();

            for (unsigned m : mesh_list)
            {
                const aiMesh* am = scene->mMeshes[m];
                const uint32_t base = (uint32_t)vertices.size();

                for (unsigned v = 0; v < am->mNumVertices; ++v)
                {
                    MeshVertex mv{};
                    mv.px = am->mVertices[v].x; mv.py = am->mVertices[v].y; mv.pz = am->mVertices[v].z;
                    if (am->mNormals)  { mv.nx = am->mNormals[v].x;  mv.ny = am->mNormals[v].y;  mv.nz = am->mNormals[v].z; }
                    if (am->mTangents) { mv.tx = am->mTangents[v].x; mv.ty = am->mTangents[v].y; mv.tz = am->mTangents[v].z; }
                    if (am->mTextureCoords[0]) { mv.u = am->mTextureCoords[0][v].x; mv.v = am->mTextureCoords[0][v].y; }
                    vertices.push_back(mv);
                }
                for (unsigned f = 0; f < am->mNumFaces; ++f)
                {
                    const aiFace& face = am->mFaces[f];
                    for (unsigned i = 0; i < face.mNumIndices; ++i)
                        indices.push_back(base + face.mIndices[i]);
                }
            }

            subset.indexCount = (uint32_t)indices.size() - subset.indexStart;
            if (subset.indexCount == 0)
                continue;

            const aiMaterial* mat = scene->mMaterials[mat_index];
            subset.textures.diffuse  = find_tex(mat, {aiTextureType_DIFFUSE, aiTextureType_BASE_COLOR});
            subset.textures.normal   = find_tex(mat, {aiTextureType_NORMALS, aiTextureType_HEIGHT});
            subset.textures.specular = find_tex(mat, {aiTextureType_SPECULAR});
            subsets.push_back(std::move(subset));
        }

        if (vertices.empty() || indices.empty() || subsets.empty())
        {
            error = "model has no triangle geometry";
            return false;
        }

        MeshHeader h{};
        h.magic[0] = 'M'; h.magic[1] = '3'; h.magic[2] = '6'; h.magic[3] = '0';
        h.version     = MESH_VERSION;
        h.vertexCount = (uint32_t)vertices.size();
        h.indexCount  = (uint32_t)indices.size();

        std::ofstream out(out_mesh, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "cannot write " + out_mesh.string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(&h), sizeof(h));
        out.write(reinterpret_cast<const char*>(vertices.data()), vertices.size() * sizeof(MeshVertex));
        out.write(reinterpret_cast<const char*>(indices.data()),  indices.size() * sizeof(uint32_t));

        const uint32_t subset_count = (uint32_t)subsets.size();
        out.write(reinterpret_cast<const char*>(&subset_count), sizeof(subset_count));
        for (const MeshSubset& s : subsets)
        {
            out.write(reinterpret_cast<const char*>(&s.indexStart), sizeof(s.indexStart));
            out.write(reinterpret_cast<const char*>(&s.indexCount), sizeof(s.indexCount));
            WriteStr(out, s.textures.diffuse);
            WriteStr(out, s.textures.normal);
            WriteStr(out, s.textures.specular);
        }
        return true;
    }
}
