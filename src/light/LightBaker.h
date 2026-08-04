#pragma once

#include "state/EngineState.h"

#include <filesystem>
#include <functional>
#include <string>

namespace lightmap
{
    struct BakeOptions
    {
        unsigned atlasSize = 1024;
        unsigned directionSize = 512;
        float texelsPerUnit = 16.0f;
        float probeSpacing = 4.0f;
        unsigned skyRays = 32;
        unsigned bounceRays = 16;
    };

    struct BakeResult
    {
        std::filesystem::path colorPath;
        std::filesystem::path directionPath;
        std::filesystem::path metadataPath;
        unsigned staticInstanceCount = 0;
        unsigned probeCount = 0;
        unsigned coveredTexels = 0;
    };

    typedef std::function<void(const std::string&)> ProgressFn;

    bool BakeScene(const std::filesystem::path& projectRoot, const SceneFile& scene,
                   const BakeOptions& options, BakeResult& result,
                   std::string& error, const ProgressFn& progress = ProgressFn());
}