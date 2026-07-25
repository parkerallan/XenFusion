#include "render/Mesh.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
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

    bool HasSkin(const aiScene* scene)
    {
        for (unsigned m = 0; m < scene->mNumMeshes; ++m)
            if (scene->mMeshes[m]->HasBones())
                return true;
        return false;
    }

    void CopyMatrix(const aiMatrix4x4& source, float destination[16])
    {
        const float values[16] = {
            source.a1, source.a2, source.a3, source.a4,
            source.b1, source.b2, source.b3, source.b4,
            source.c1, source.c2, source.c3, source.c4,
            source.d1, source.d2, source.d3, source.d4,
        };
        std::memcpy(destination, values, sizeof(values));
    }

    void SetIdentity(float matrix[16])
    {
        std::memset(matrix, 0, sizeof(float) * 16);
        matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    }

    bool MarkRelevantNodes(const aiNode* node, const std::set<std::string>& weighted_names,
                           std::set<const aiNode*>& relevant)
    {
        bool keep = weighted_names.find(node->mName.C_Str()) != weighted_names.end();
        for (unsigned child = 0; child < node->mNumChildren; ++child)
            keep = MarkRelevantNodes(node->mChildren[child], weighted_names, relevant) || keep;
        if (keep)
            relevant.insert(node);
        return keep;
    }

    void BuildJointTable(const aiNode* node, int32_t parent,
                         const std::set<const aiNode*>& relevant,
                         MeshSkeleton& skeleton,
                         std::unordered_map<std::string, uint32_t>& joint_by_name)
    {
        if (relevant.find(node) == relevant.end())
            return;

        MeshJoint joint;
        joint.name = node->mName.C_Str();
        joint.parent = parent;
        SetIdentity(joint.inverseBind);
        CopyMatrix(node->mTransformation, joint.bindLocal);
        const uint32_t index = (uint32_t)skeleton.joints.size();
        skeleton.joints.push_back(joint);
        joint_by_name[joint.name] = index;

        for (unsigned child = 0; child < node->mNumChildren; ++child)
            BuildJointTable(node->mChildren[child], (int32_t)index, relevant,
                            skeleton, joint_by_name);
    }

    uint32_t HashSkeleton(const MeshSkeleton& skeleton)
    {
        uint32_t hash = 2166136261u;
        for (const MeshJoint& joint : skeleton.joints)
        {
            for (char c : joint.name)
            {
                hash ^= (uint8_t)c;
                hash *= 16777619u;
            }
            const uint32_t parent = (uint32_t)joint.parent;
            for (unsigned shift = 0; shift < 32; shift += 8)
            {
                hash ^= (uint8_t)(parent >> shift);
                hash *= 16777619u;
            }
        }
        return hash;
    }

    MeshSkinInfluence PackInfluence(std::vector<std::pair<uint32_t, float>> values)
    {
        MeshSkinInfluence packed{};
        std::sort(values.begin(), values.end(), [](const auto& left, const auto& right)
        {
            return left.second > right.second;
        });
        if (values.size() > MAX_SKIN_INFLUENCES)
            values.resize(MAX_SKIN_INFLUENCES);

        float total = 0.0f;
        for (const auto& value : values)
            total += (std::max)(0.0f, value.second);
        if (total <= 0.0f)
        {
            packed.weight[0] = 255;
            return packed;
        }

        unsigned assigned = 0;
        for (size_t i = 0; i < values.size(); ++i)
        {
            packed.joint[i] = (uint8_t)values[i].first;
            const unsigned weight = i + 1 == values.size()
                ? 255u - assigned
                : (unsigned)std::lround((std::max)(0.0f, values[i].second) * 255.0f / total);
            packed.weight[i] = (uint8_t)(std::min)(weight, 255u - assigned);
            assigned += packed.weight[i];
        }
        return packed;
    }
}

namespace mesh
{
    bool BakeModel(const fs::path& source, const fs::path& out_mesh, std::string& error)
    {
        Assimp::Importer importer;
        const unsigned common_flags = aiProcess_Triangulate |
            aiProcess_GenSmoothNormals |
            aiProcess_CalcTangentSpace |        // tangents for normal mapping
            aiProcess_JoinIdenticalVertices |
            aiProcess_ConvertToLeftHanded;
        const aiScene* scene = importer.ReadFile(source.string(), common_flags);

        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || scene->mNumMeshes == 0)
        {
            error = importer.GetErrorString();
            if (error.empty()) error = "no meshes in file";
            return false;
        }

        const bool skinned = HasSkin(scene);
        if (!skinned)
        {
            // Preserve the established static-mesh behavior exactly. Rigged
            // scenes cannot use this flag because it destroys their hierarchy.
            scene = importer.ReadFile(source.string(), common_flags | aiProcess_PreTransformVertices);
            if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || scene->mNumMeshes == 0)
            {
                error = importer.GetErrorString();
                return false;
            }
        }

        MeshSkeleton skeleton;
        std::unordered_map<std::string, uint32_t> joint_by_name;
        if (skinned)
        {
            std::set<std::string> weighted_names;
            for (unsigned mesh_index = 0; mesh_index < scene->mNumMeshes; ++mesh_index)
                for (unsigned bone = 0; bone < scene->mMeshes[mesh_index]->mNumBones; ++bone)
                    weighted_names.insert(scene->mMeshes[mesh_index]->mBones[bone]->mName.C_Str());

            std::set<const aiNode*> relevant;
            MarkRelevantNodes(scene->mRootNode, weighted_names, relevant);
            BuildJointTable(scene->mRootNode, -1, relevant, skeleton, joint_by_name);
            if (skeleton.joints.empty())
            {
                error = "model has skin weights but no matching skeleton nodes";
                return false;
            }
            if (skeleton.joints.size() > MAX_SKIN_JOINTS)
            {
                error = "skeleton has " + std::to_string(skeleton.joints.size()) +
                        " joints; Xbox skinning limit is " + std::to_string(MAX_SKIN_JOINTS);
                return false;
            }

            for (unsigned mesh_index = 0; mesh_index < scene->mNumMeshes; ++mesh_index)
                for (unsigned bone_index = 0; bone_index < scene->mMeshes[mesh_index]->mNumBones; ++bone_index)
                {
                    const aiBone* bone = scene->mMeshes[mesh_index]->mBones[bone_index];
                    const auto found = joint_by_name.find(bone->mName.C_Str());
                    if (found != joint_by_name.end())
                        CopyMatrix(bone->mOffsetMatrix, skeleton.joints[found->second].inverseBind);
                }
            skeleton.fingerprint = HashSkeleton(skeleton);
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
        std::vector<std::vector<std::pair<uint32_t, float>>> vertex_influences;
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
                if (skinned)
                {
                    vertex_influences.resize(vertices.size());
                    for (unsigned bone_index = 0; bone_index < am->mNumBones; ++bone_index)
                    {
                        const aiBone* bone = am->mBones[bone_index];
                        const auto joint = joint_by_name.find(bone->mName.C_Str());
                        if (joint == joint_by_name.end())
                            continue;
                        for (unsigned weight = 0; weight < bone->mNumWeights; ++weight)
                        {
                            const aiVertexWeight& source_weight = bone->mWeights[weight];
                            if (source_weight.mVertexId < am->mNumVertices && source_weight.mWeight > 0.0f)
                                vertex_influences[base + source_weight.mVertexId].push_back(
                                    {joint->second, source_weight.mWeight});
                        }
                    }
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
            subset.textures.emissive = find_tex(mat, {aiTextureType_EMISSIVE, aiTextureType_EMISSION_COLOR});
            subset.textures.metallic = find_tex(mat, {aiTextureType_METALNESS});
            subset.textures.clearcoat = find_tex(mat, {aiTextureType_CLEARCOAT});
            if (subset.textures.clearcoat.empty())
            {
                // Substance-style sibling export: <model>_Clearcoat.png next to
                // the source (OBJ/MTL has no clearcoat map statement to read).
                const std::string sibling = source.stem().string() + "_Clearcoat.png";
                std::error_code ec;
                if (fs::exists(source.parent_path() / sibling, ec))
                    subset.textures.clearcoat = sibling;
            }
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
        h.flags       = skinned ? MESH_FLAG_SKINNED : 0;
        h.jointCount  = (uint32_t)skeleton.joints.size();
        h.skeletonFingerprint = skeleton.fingerprint;

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
            WriteStr(out, s.textures.emissive);
            WriteStr(out, s.textures.metallic);
            WriteStr(out, s.textures.clearcoat);
        }
        if (skinned)
        {
            for (const auto& source_influences : vertex_influences)
            {
                const MeshSkinInfluence influence = PackInfluence(source_influences);
                out.write(reinterpret_cast<const char*>(&influence), sizeof(influence));
            }
            for (const MeshJoint& joint : skeleton.joints)
            {
                WriteStr(out, joint.name);
                out.write(reinterpret_cast<const char*>(&joint.parent), sizeof(joint.parent));
                out.write(reinterpret_cast<const char*>(joint.inverseBind), sizeof(joint.inverseBind));
                out.write(reinterpret_cast<const char*>(joint.bindLocal), sizeof(joint.bindLocal));
            }
        }
        return true;
    }
}
