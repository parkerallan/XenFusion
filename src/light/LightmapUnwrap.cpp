#include "light/LightmapUnwrap.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "xatlas.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace
{
    const uint32_t kUvVersion = 2;

    template <typename T>
    bool ReadValue(std::ifstream& input, T& value)
    {
        input.read(reinterpret_cast<char*>(&value), sizeof(value));
        return !!input;
    }

    template <typename T>
    void WriteValue(std::ofstream& output, const T& value)
    {
        output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    bool WriteUvSidecar(const fs::path& path, const lightmap::UvMesh& mesh,
                        std::string& error)
    {
        const fs::path temporary = path.string() + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            error = "cannot write " + temporary.string();
            return false;
        }

        output.write("LMU0", 4);
        WriteValue(output, kUvVersion);
        WriteValue(output, mesh.sourceVertexCount);
        WriteValue(output, mesh.width);
        WriteValue(output, mesh.height);
        const uint32_t vertexCount = (uint32_t)mesh.vertices.size();
        const uint32_t indexCount = (uint32_t)mesh.indices.size();
        WriteValue(output, vertexCount);
        WriteValue(output, indexCount);
        for (const lightmap::UvVertex& vertex : mesh.vertices)
        {
            WriteValue(output, vertex.sourceVertex);
            WriteValue(output, vertex.u);
            WriteValue(output, vertex.v);
        }
        output.write(reinterpret_cast<const char*>(mesh.indices.data()),
                     (std::streamsize)(mesh.indices.size() * sizeof(uint32_t)));
        output.close();
        if (!output)
        {
            error = "failed writing " + temporary.string();
            fs::remove(temporary);
            return false;
        }

        std::error_code ec;
        fs::remove(path, ec);
        ec.clear();
        fs::rename(temporary, path, ec);
        if (ec)
        {
            error = "cannot replace " + path.string() + ": " + ec.message();
            fs::remove(temporary);
            return false;
        }
        return true;
    }
}

namespace lightmap
{
    bool ReadUvSidecar(const fs::path& path, UvMesh& output, std::string& error)
    {
        output = UvMesh{};
        std::ifstream input(path, std::ios::binary);
        char magic[4] = {};
        uint32_t version = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        input.read(magic, sizeof(magic));
        if (!input || std::memcmp(magic, "LMU0", 4) != 0 ||
            !ReadValue(input, version) || version != kUvVersion ||
            !ReadValue(input, output.sourceVertexCount) ||
            !ReadValue(input, output.width) || !ReadValue(input, output.height) ||
            !ReadValue(input, vertexCount) || !ReadValue(input, indexCount) ||
            output.sourceVertexCount == 0 || output.width == 0 || output.height == 0 ||
            vertexCount == 0 || indexCount == 0 || indexCount % 3 != 0)
        {
            error = "invalid LMUV sidecar " + path.string();
            return false;
        }

        output.vertices.resize(vertexCount);
        output.indices.resize(indexCount);
        for (UvVertex& vertex : output.vertices)
            if (!ReadValue(input, vertex.sourceVertex) || !ReadValue(input, vertex.u) ||
                !ReadValue(input, vertex.v) || vertex.sourceVertex >= output.sourceVertexCount ||
                vertex.u < 0.0f || vertex.u > 1.0f || vertex.v < 0.0f || vertex.v > 1.0f)
            {
                error = "invalid LMUV vertex in " + path.string();
                return false;
            }
        input.read(reinterpret_cast<char*>(output.indices.data()),
                   (std::streamsize)(output.indices.size() * sizeof(uint32_t)));
        if (!input || std::any_of(output.indices.begin(), output.indices.end(),
                                  [vertexCount](uint32_t index) { return index >= vertexCount; }))
        {
            error = "invalid LMUV indices in " + path.string();
            return false;
        }
        return true;
    }

    bool EnsureUvSidecar(const fs::path& source, const fs::path& output,
                         UvMesh& mesh, std::string& error)
    {
        std::error_code ec;
        if (fs::exists(output, ec) && fs::last_write_time(output, ec) >= fs::last_write_time(source, ec) &&
            ReadUvSidecar(output, mesh, error))
            return true;
        error.clear();

        Assimp::Importer importer;
        const unsigned flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
            aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices | aiProcess_ConvertToLeftHanded |
            aiProcess_PreTransformVertices;
        const aiScene* scene = importer.ReadFile(source.string(), flags);
        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || scene->mNumMeshes == 0)
        {
            error = importer.GetErrorString();
            if (error.empty()) error = "no meshes in " + source.string();
            return false;
        }
        for (unsigned index = 0; index < scene->mNumMeshes; ++index)
            if (scene->mMeshes[index]->HasBones())
            {
                error = "skinned meshes use probes and cannot be lightmapped";
                return false;
            }

        std::map<unsigned, std::vector<unsigned>> byMaterial;
        for (unsigned index = 0; index < scene->mNumMeshes; ++index)
            byMaterial[scene->mMeshes[index]->mMaterialIndex].push_back(index);

        std::vector<float> positions;
        std::vector<float> normals;
        std::vector<uint32_t> indices;
        for (const auto& entry : byMaterial)
            for (unsigned meshIndex : entry.second)
            {
                const aiMesh* sourceMesh = scene->mMeshes[meshIndex];
                const uint32_t base = (uint32_t)(positions.size() / 3);
                for (unsigned vertex = 0; vertex < sourceMesh->mNumVertices; ++vertex)
                {
                    const aiVector3D& position = sourceMesh->mVertices[vertex];
                    const aiVector3D& normal = sourceMesh->mNormals[vertex];
                    positions.insert(positions.end(), {position.x, position.y, position.z});
                    normals.insert(normals.end(), {normal.x, normal.y, normal.z});
                }
                for (unsigned face = 0; face < sourceMesh->mNumFaces; ++face)
                    for (unsigned corner = 0; corner < 3; ++corner)
                        indices.push_back(base + sourceMesh->mFaces[face].mIndices[corner]);
            }
        if (positions.empty() || indices.empty())
        {
            error = "model has no triangle geometry";
            return false;
        }

        xatlas::Atlas* atlas = xatlas::Create();
        xatlas::MeshDecl declaration;
        declaration.vertexPositionData = positions.data();
        declaration.vertexPositionStride = sizeof(float) * 3;
        declaration.vertexNormalData = normals.data();
        declaration.vertexNormalStride = sizeof(float) * 3;
        declaration.vertexCount = (uint32_t)(positions.size() / 3);
        declaration.indexData = indices.data();
        declaration.indexCount = (uint32_t)indices.size();
        declaration.indexFormat = xatlas::IndexFormat::UInt32;
        const xatlas::AddMeshError addError = xatlas::AddMesh(atlas, declaration);
        if (addError != xatlas::AddMeshError::Success)
        {
            error = std::string("xatlas AddMesh failed: ") + xatlas::StringForEnum(addError);
            xatlas::Destroy(atlas);
            return false;
        }

        xatlas::PackOptions pack;
        pack.padding = 4;
        pack.texelsPerUnit = 16.0f;
        pack.blockAlign = true;
        xatlas::Generate(atlas, xatlas::ChartOptions(), pack);
        if (atlas->atlasCount != 1 || atlas->meshCount != 1 || atlas->width == 0 || atlas->height == 0)
        {
            error = "xatlas unwrap did not fit one mesh page";
            xatlas::Destroy(atlas);
            return false;
        }

        const xatlas::Mesh& result = atlas->meshes[0];
        mesh = UvMesh{};
        mesh.sourceVertexCount = declaration.vertexCount;
        mesh.width = atlas->width;
        mesh.height = atlas->height;
        mesh.vertices.resize(result.vertexCount);
        mesh.indices.assign(result.indexArray, result.indexArray + result.indexCount);
        for (uint32_t index = 0; index < result.vertexCount; ++index)
        {
            mesh.vertices[index].sourceVertex = result.vertexArray[index].xref;
            mesh.vertices[index].u = result.vertexArray[index].uv[0] / (float)atlas->width;
            mesh.vertices[index].v = result.vertexArray[index].uv[1] / (float)atlas->height;
        }
        xatlas::Destroy(atlas);
        return WriteUvSidecar(output, mesh, error);
    }
}