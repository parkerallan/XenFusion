#include "render/Shader.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "SkinShaderCompileTest: expected standard.hlsl path\n";
        return 1;
    }

    const fs::path output = fs::temp_directory_path() / "xenfusion_skin_shader_test";
    std::error_code error_code;
    fs::remove_all(output, error_code);

    std::string error;
    if (!shader::BakeToCso(argv[1], output, error))
    {
        std::cerr << "SkinShaderCompileTest: " << error << '\n';
        return 1;
    }

    const fs::path expected[] = {
        output / "standard_vs.cso",
        output / "standard_skin_vs.cso",
        output / "standard_ps.cso",
    };
    for (const fs::path& path : expected)
        if (!fs::exists(path, error_code) || fs::file_size(path, error_code) == 0)
        {
            std::cerr << "SkinShaderCompileTest: missing " << path.filename().string() << '\n';
            return 1;
        }

    fs::remove_all(output, error_code);
    return 0;
}