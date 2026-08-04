#include "light/LightmapUnwrap.h"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    const std::filesystem::path source = argv[1];
    const std::filesystem::path output = std::filesystem::temp_directory_path() /
        "xenfusion_lightmap_unwrap_test.lmuv";
    std::error_code ec;
    std::filesystem::remove(output, ec);

    lightmap::UvMesh mesh;
    std::string error;
    if (!lightmap::EnsureUvSidecar(source, output, mesh, error))
    {
        std::cerr << error << '\n';
        return 1;
    }
    if (mesh.sourceVertexCount == 0 || mesh.vertices.empty() || mesh.indices.empty() ||
        mesh.width == 0 || mesh.height == 0)
        return 1;

    lightmap::UvMesh cached;
    if (!lightmap::EnsureUvSidecar(source, output, cached, error) ||
        cached.vertices.size() != mesh.vertices.size() || cached.indices != mesh.indices)
        return 1;
    std::filesystem::remove(output, ec);
    return 0;
}