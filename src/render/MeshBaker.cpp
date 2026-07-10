#include "render/Mesh.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace mesh
{
    bool BakeModel(const fs::path& source, const fs::path& out_mesh, std::string& error)
    {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(
            source.string(),
            aiProcess_Triangulate |
            aiProcess_GenSmoothNormals |
            aiProcess_JoinIdenticalVertices |
            aiProcess_PreTransformVertices |   // flatten the node hierarchy
            aiProcess_ConvertToLeftHanded);    // match our LH D3D9 pipeline

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || scene->mNumMeshes == 0)
        {
            error = importer.GetErrorString();
            if (error.empty()) error = "no meshes in file";
            return false;
        }

        // Merge every mesh into one vertex/index array.
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t>   indices;
        for (unsigned m = 0; m < scene->mNumMeshes; ++m)
        {
            const aiMesh* am = scene->mMeshes[m];
            const uint32_t base = (uint32_t)vertices.size();

            for (unsigned v = 0; v < am->mNumVertices; ++v)
            {
                MeshVertex mv{};
                mv.px = am->mVertices[v].x;
                mv.py = am->mVertices[v].y;
                mv.pz = am->mVertices[v].z;
                if (am->mNormals)
                {
                    mv.nx = am->mNormals[v].x;
                    mv.ny = am->mNormals[v].y;
                    mv.nz = am->mNormals[v].z;
                }
                if (am->mTextureCoords[0])
                {
                    mv.u = am->mTextureCoords[0][v].x;
                    mv.v = am->mTextureCoords[0][v].y;
                }
                vertices.push_back(mv);
            }

            for (unsigned f = 0; f < am->mNumFaces; ++f)
            {
                const aiFace& face = am->mFaces[f];
                for (unsigned i = 0; i < face.mNumIndices; ++i)
                    indices.push_back(base + face.mIndices[i]);
            }
        }

        if (vertices.empty() || indices.empty())
        {
            error = "model has no triangle geometry";
            return false;
        }

        // Diffuse texture reference (first material that has one). Stored as the
        // path the model gave, relative to the model file.
        std::string diffuse;
        for (unsigned m = 0; m < scene->mNumMeshes && diffuse.empty(); ++m)
        {
            const aiMaterial* mat = scene->mMaterials[scene->mMeshes[m]->mMaterialIndex];
            aiString tex;
            if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &tex) == AI_SUCCESS)
                diffuse = tex.C_Str();
        }
        for (char& c : diffuse) if (c == '\\') c = '/';

        MeshHeader h{};
        h.magic[0] = 'M'; h.magic[1] = '3'; h.magic[2] = '6'; h.magic[3] = '0';
        h.version     = 2;
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
        out.write(reinterpret_cast<const char*>(indices.data()), indices.size() * sizeof(uint32_t));
        const uint32_t diffuse_len = (uint32_t)diffuse.size();
        out.write(reinterpret_cast<const char*>(&diffuse_len), sizeof(diffuse_len));
        out.write(diffuse.data(), diffuse_len);
        return true;
    }
}
