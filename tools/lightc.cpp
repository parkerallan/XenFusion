#include "light/LightBaker.h"
#include "project/SceneJson.h"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: lightc <project-root> <scene-file>\n";
        return 2;
    }

    SceneFile scene;
    scene.path = std::filesystem::absolute(argv[2]);
    if (!project::ReadSceneJson(scene))
    {
        std::cerr << "lightc: cannot read scene " << scene.path << "\n";
        return 1;
    }

    lightmap::BakeOptions options;
    lightmap::BakeResult result;
    std::string error;
    const bool baked = lightmap::BakeScene(std::filesystem::absolute(argv[1]), scene, options, result, error,
        [](const std::string& message) { std::cout << message << "\n"; });
    if (!baked)
    {
        std::cerr << "lightc: " << error << "\n";
        return 1;
    }

    std::cout << "static instances: " << result.staticInstanceCount << "\n"
              << "covered texels: " << result.coveredTexels << "\n"
              << "probes: " << result.probeCount << "\n"
              << result.colorPath << "\n"
              << result.directionPath << "\n"
              << result.metadataPath << "\n";
    return 0;
}