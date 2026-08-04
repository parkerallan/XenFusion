#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lightmap
{
    struct UvVertex
    {
        uint32_t sourceVertex = 0;
        float u = 0.0f;
        float v = 0.0f;
    };

    struct UvMesh
    {
        uint32_t sourceVertexCount = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<UvVertex> vertices;
        std::vector<uint32_t> indices;
    };

    bool ReadUvSidecar(const std::filesystem::path& path, UvMesh& output,
                       std::string& error);
    bool EnsureUvSidecar(const std::filesystem::path& source,
                         const std::filesystem::path& output,
                         UvMesh& mesh, std::string& error);
}