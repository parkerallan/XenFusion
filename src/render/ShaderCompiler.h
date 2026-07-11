#pragma once

#include <filesystem>

// The "Compile shaders" action: compile the engine's built-in standard shader
// plus every .hlsl in the project into one folder — <project>/shaders/ — so the
// project owns a complete, shippable set of .cso. Pure content operation (no
// renderer/GPU state); the renderer reloads the results afterward. Logs a summary.
namespace shadercompiler
{
    void CompileAll(const std::filesystem::path& standard_hlsl,
                    const std::filesystem::path& project_root);
}
