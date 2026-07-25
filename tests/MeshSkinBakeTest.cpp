#include "render/Mesh.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    bool SkipString(std::ifstream& input)
    {
        uint32_t length = 0;
        input.read(reinterpret_cast<char*>(&length), sizeof(length));
        if (!input || length >= 4096)
            return false;
        input.seekg(length, std::ios::cur);
        return !!input;
    }

    bool ReadString(std::ifstream& input, std::string& value)
    {
        uint32_t length = 0;
        input.read(reinterpret_cast<char*>(&length), sizeof(length));
        if (!input || length == 0 || length >= 4096)
            return false;
        value.resize(length);
        input.read(value.data(), length);
        return !!input;
    }

    void Multiply(const float left[16], const float right[16], float output[16])
    {
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                output[row * 4 + column] =
                    left[row * 4 + 0] * right[0 * 4 + column] +
                    left[row * 4 + 1] * right[1 * 4 + column] +
                    left[row * 4 + 2] * right[2 * 4 + column] +
                    left[row * 4 + 3] * right[3 * 4 + column];
    }

    int Fail(const std::string& message)
    {
        std::cerr << "MeshSkinBakeTest: " << message << '\n';
        return 1;
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return Fail("expected the path to Fox.gltf");

    const fs::path source = argv[1];
    const fs::path output = fs::temp_directory_path() / "xenfusion_fox_skin_test.mesh";
    std::error_code remove_error;
    fs::remove(output, remove_error);

    std::string error;
    if (!mesh::BakeModel(source, output, error))
        return Fail("bake failed: " + error);

    std::ifstream input(output, std::ios::binary);
    MeshHeader header{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!input || std::memcmp(header.magic, "M360", 4) != 0)
        return Fail("invalid baked header");
    if (header.version != MESH_VERSION)
        return Fail("unexpected mesh version");
    if ((header.flags & MESH_FLAG_SKINNED) == 0)
        return Fail("Fox was not detected as skinned");
    if (header.jointCount == 0 || header.jointCount > MAX_SKIN_JOINTS)
        return Fail("joint count is outside the supported range");
    if (header.skeletonFingerprint == 0)
        return Fail("skeleton fingerprint was not generated");

    const std::streamoff geometry_bytes =
        (std::streamoff)header.vertexCount * sizeof(MeshVertex) +
        (std::streamoff)header.indexCount * sizeof(uint32_t);
    input.seekg(geometry_bytes, std::ios::cur);

    uint32_t subset_count = 0;
    input.read(reinterpret_cast<char*>(&subset_count), sizeof(subset_count));
    if (!input || subset_count == 0 || subset_count > 1024)
        return Fail("invalid subset table");
    for (uint32_t subset = 0; subset < subset_count; ++subset)
    {
        uint32_t range[2]{};
        input.read(reinterpret_cast<char*>(range), sizeof(range));
        for (int texture = 0; texture < 6; ++texture)
            if (!SkipString(input))
                return Fail("invalid subset texture string");
    }

    std::vector<MeshSkinInfluence> influences(header.vertexCount);
    input.read(reinterpret_cast<char*>(influences.data()),
               (std::streamsize)(influences.size() * sizeof(MeshSkinInfluence)));
    if (!input)
        return Fail("truncated influence stream");
    for (const MeshSkinInfluence& influence : influences)
    {
        unsigned total = 0;
        for (uint32_t slot = 0; slot < MAX_SKIN_INFLUENCES; ++slot)
        {
            if (influence.joint[slot] >= header.jointCount && influence.weight[slot] != 0)
                return Fail("weighted joint index is out of range");
            total += influence.weight[slot];
        }
        if (total != 255)
            return Fail("packed weights do not sum to 255");
    }

    std::vector<MeshJoint> joints(header.jointCount);
    std::vector<std::array<float, 16>> globals(header.jointCount);
    for (uint32_t joint_index = 0; joint_index < header.jointCount; ++joint_index)
    {
        MeshJoint& joint = joints[joint_index];
        if (!ReadString(input, joint.name))
            return Fail("invalid joint name");
        input.read(reinterpret_cast<char*>(&joint.parent), sizeof(joint.parent));
        input.read(reinterpret_cast<char*>(joint.inverseBind), sizeof(joint.inverseBind));
        input.read(reinterpret_cast<char*>(joint.bindLocal), sizeof(joint.bindLocal));
        if (!input || joint.parent >= (int32_t)joint_index)
            return Fail("invalid joint record");
        if (joint.parent >= 0)
            Multiply(globals[(size_t)joint.parent].data(), joint.bindLocal,
                     globals[joint_index].data());
        else
            std::memcpy(globals[joint_index].data(), joint.bindLocal, sizeof(joint.bindLocal));

        float skin[16];
        Multiply(globals[joint_index].data(), joint.inverseBind, skin);
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
            {
                const float expected = row == column ? 1.0f : 0.0f;
                if (std::fabs(skin[row * 4 + column] - expected) > 1e-3f)
                    return Fail("bind hierarchy does not cancel inverse bind for " + joint.name);
            }
    }

    fs::remove(output, remove_error);
    std::cout << "Fox skin bake: " << header.vertexCount << " vertices, "
              << header.jointCount << " joints, fingerprint 0x" << std::hex
              << header.skeletonFingerprint << '\n';
    return 0;
}