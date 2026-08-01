#include "render/Mesh.h"

#include "render/TextureSetResolve.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <array>
#include <cctype>
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

    // The seven material slots in blob order. Kept in one table so the importer
    // query, the extracted-file naming and the sibling search cannot disagree
    // about what a slot is: an embedded normal map named "_BaseColor" would be
    // both misleading and would send the sibling search after the wrong files.
    struct SlotDef
    {
        const char*   channel;      // name used for an extracted embedded file
        aiTextureType types[3];     // importer types that can fill it, NONE-terminated
        const char*   aliases[8];   // spellings accepted from sibling files, NULL-terminated
    };

    const SlotDef kSlots[7] = {
        { "BaseColor", { aiTextureType_DIFFUSE, aiTextureType_BASE_COLOR, aiTextureType_NONE },
          { "BaseColor", "Albedo", "Diffuse", NULL } },
        { "Normal",    { aiTextureType_NORMALS, aiTextureType_HEIGHT, aiTextureType_NONE },
          { "Normal", "Normal_OpenGL", "NormalOpenGL", "Normal_DirectX", "NormalDX", "NormalMap", "Nrm", NULL } },
        { "Specular",  { aiTextureType_SPECULAR, aiTextureType_NONE, aiTextureType_NONE },
          { "Specular", "Spec", NULL } },
        { "Emissive",  { aiTextureType_EMISSIVE, aiTextureType_EMISSION_COLOR, aiTextureType_NONE },
          { "Emissive", "Emission", "Emis", NULL } },
        { "Metallic",  { aiTextureType_METALNESS, aiTextureType_NONE, aiTextureType_NONE },
          { "Metallic", "Metalness", "Metal", NULL } },
        { "Clearcoat", { aiTextureType_CLEARCOAT, aiTextureType_NONE, aiTextureType_NONE },
          { "Clearcoat", "ClearCoat", "Coat", NULL } },
        { "Roughness", { aiTextureType_DIFFUSE_ROUGHNESS, aiTextureType_NONE, aiTextureType_NONE },
          { "Roughness", "Rough", NULL } },
    };

    // What the model file itself declares for one slot, or empty.
    std::string DeclaredTex(const aiMaterial* mat, const SlotDef& slot)
    {
        for (int t = 0; t < 3 && slot.types[t] != aiTextureType_NONE; ++t)
        {
            aiString tex;
            if (mat->GetTexture(slot.types[t], 0, &tex) == AI_SUCCESS)
            {
                std::string s = tex.C_Str();
                for (char& c : s) if (c == '\\') c = '/';
                return s;
            }
        }
        return {};
    }

    std::string SanitizeName(const std::string& s)
    {
        std::string out;
        for (char c : s)
            out += (c == ' ' || c == '.' || c == ':' || c == '/' || c == '\\' || c == '"') ? '_' : c;
        return out;
    }

    // --- Embedded textures --------------------------------------------------
    //
    // GLB, and FBX with "embed media", carry their images inside the model file
    // instead of as sibling files. Assimp hands those over as "*0", "*1", ...
    // which is an index into aiScene::mTextures, not a path. Writing them out
    // at bake time is what makes those models work at all: everything
    // downstream (the .mesh blob, the editor's loader, the 360 cooker) deals in
    // file paths.

    // 32-bit uncompressed TGA, top-down. aiTexel is already laid out b,g,r,a,
    // which is TGA's channel order, so the pixels copy straight through.
    bool WriteTga(const fs::path& path, const aiTexel* texels, unsigned w, unsigned h)
    {
        if (!texels || w == 0 || h == 0 || w > 0xFFFF || h > 0xFFFF)
            return false;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        unsigned char header[18] = {};
        header[2]  = 2;                              // uncompressed true-colour
        header[12] = (unsigned char)(w & 0xFF);
        header[13] = (unsigned char)(w >> 8);
        header[14] = (unsigned char)(h & 0xFF);
        header[15] = (unsigned char)(h >> 8);
        header[16] = 32;                             // bits per pixel
        header[17] = 0x28;                           // 8 alpha bits | top-down
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        out.write(reinterpret_cast<const char*>(texels), (std::streamsize)w * h * 4);
        return (bool)out;
    }

    // achFormatHint is allowed to be empty, so fall back to the magic bytes.
    const char* SniffImageExt(const unsigned char* d, std::size_t n)
    {
        if (n >= 8 && std::memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return "png";
        if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)  return "jpg";
        if (n >= 2 && d[0] == 'B' && d[1] == 'M')                    return "bmp";
        if (n >= 4 && std::memcmp(d, "DDS ", 4) == 0)                return "dds";
        return nullptr;
    }

    // Names for the extracted files, index-aligned with scene->mTextures.
    //
    // Named after the material and slot each texture is actually used in, so
    // the folder is readable AND the names follow the same convention the
    // sibling search understands. That second part matters: an extracted
    // diffuse called "<mesh>_<material>_BaseColor.png" lets the search derive
    // "<mesh>_<material>_Roughness.png" from it, which is its most reliable
    // rule. An opaque "_embedded0" name would leave only weaker guesses.
    //
    // An embedded texture no material references keeps an index-based name;
    // there is nothing better to call it.
    std::vector<std::string> EmbeddedBaseNames(const aiScene* scene, const std::string& mesh_stem)
    {
        std::vector<std::string> names(scene->mNumTextures);
        for (unsigned m = 0; m < scene->mNumMaterials; ++m)
        {
            const aiMaterial* mat = scene->mMaterials[m];
            aiString mat_name;
            mat->Get(AI_MATKEY_NAME, mat_name);
            const std::string material = SanitizeName(mat_name.C_Str());

            for (int s = 0; s < 7; ++s)
            {
                const std::string ref = DeclaredTex(mat, kSlots[s]);
                if (ref.empty()) continue;
                const int index = scene->GetEmbeddedTextureAndIndex(ref.c_str()).second;
                if (index < 0 || (unsigned)index >= names.size()) continue;
                if (!names[index].empty()) continue;   // first use names it
                names[index] = mesh_stem + (material.empty() ? "" : "_" + material)
                             + "_" + kSlots[s].channel;
            }
        }
        for (unsigned i = 0; i < names.size(); ++i)
            if (names[i].empty())
                names[i] = mesh_stem + "_embedded" + std::to_string(i);
        return names;
    }

    // Write every embedded texture next to the .mesh. The result is index
    // aligned with scene->mTextures; an entry is empty where extraction failed.
    std::vector<std::string> ExtractEmbedded(const aiScene* scene, const fs::path& out_mesh)
    {
        std::vector<std::string> rel(scene->mNumTextures);
        const fs::path dir  = out_mesh.parent_path();
        const std::vector<std::string> names = EmbeddedBaseNames(scene, out_mesh.stem().string());

        for (unsigned i = 0; i < scene->mNumTextures; ++i)
        {
            const aiTexture* tex = scene->mTextures[i];
            if (!tex || !tex->pcData) continue;
            const std::string base = names[i];

            if (tex->mHeight == 0)
            {
                // Compressed: pcData is mWidth bytes of an image file.
                const unsigned char* bytes = reinterpret_cast<const unsigned char*>(tex->pcData);
                const std::size_t    size  = tex->mWidth;
                std::string ext = tex->achFormatHint;
                if (ext.empty() || ext == "\0")
                {
                    const char* sniffed = SniffImageExt(bytes, size);
                    if (!sniffed) continue;          // unknown container, leave empty
                    ext = sniffed;
                }
                const std::string name = base + "." + ext;
                std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
                if (!out) continue;
                out.write(reinterpret_cast<const char*>(bytes), (std::streamsize)size);
                if (out) rel[i] = name;
            }
            else
            {
                const std::string name = base + ".tga";
                if (WriteTga(dir / name, tex->pcData, tex->mWidth, tex->mHeight))
                    rel[i] = name;
            }
        }
        return rel;
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

        // Embedded textures become real files before anything reads a path.
        const std::vector<std::string> embedded = ExtractEmbedded(scene, out_mesh);

        // One directory listing for the whole model, reused by every subset and
        // every slot, instead of a stat per candidate filename.
        const std::string model_stem = source.stem().string();
        texset::MapIndex  map_index;
        map_index.AddDir(source.parent_path(), "");
        map_index.AddDir(source.parent_path(), "textures/");

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
            aiString mat_name;
            mat->Get(AI_MATKEY_NAME, mat_name);
            // The material name is Substance's texture-set name. Keying the
            // sibling search off the model's filename alone would hand `body`
            // and `head` the same maps.
            const std::string material(mat_name.C_Str());

            std::string* slot[7] = {
                &subset.textures.diffuse,  &subset.textures.normal,
                &subset.textures.specular, &subset.textures.emissive,
                &subset.textures.metallic, &subset.textures.clearcoat,
                &subset.textures.roughness,
            };

            // Pass 1: what the model file declares, with any "*N" swapped for
            // the file ExtractEmbedded wrote. GetEmbeddedTextureAndIndex also
            // matches by filename, which is how some FBX exporters reference
            // their embedded media. A slot whose extraction failed goes empty
            // rather than keeping a dead reference, so pass 2 can rescue it.
            for (int s = 0; s < 7; ++s)
            {
                *slot[s] = DeclaredTex(mat, kSlots[s]);
                if (slot[s]->empty()) continue;
                const int index = scene->GetEmbeddedTextureAndIndex(slot[s]->c_str()).second;
                if (index < 0) continue;                    // an ordinary path
                *slot[s] = ((std::size_t)index < embedded.size()) ? embedded[index] : std::string();
            }

            // Substance exports often sit beside the textures rather than
            // beside the model, so index the diffuse's own directory too.
            const std::string& d = subset.textures.diffuse;
            if (!d.empty())
            {
                std::string ddir, dstem;
                texset::SplitRel(d, ddir, dstem);
                map_index.AddDir(source.parent_path(), ddir);
            }

            // Pass 2: sibling files, for whatever the format had no statement
            // for. Declarations always win, so no existing import changes.
            for (int s = 1; s < 7; ++s)   // the diffuse is never inferred
                if (slot[s]->empty())
                    *slot[s] = texset::Resolve(map_index, d, model_stem, material, kSlots[s].aliases);

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
            WriteStr(out, s.textures.roughness);
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
