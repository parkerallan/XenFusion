#pragma once

#include <filesystem>
#include <string>

struct SceneFile;

// JSON authoring for project documents (scenes + manifest). Kept separate from
// the file/project operations in ProjectIO so serialization lives in one place.
namespace project
{
    void WriteSceneJson(const SceneFile& scene); // serialize to scene.path
    bool ReadSceneJson(SceneFile& scene);        // parse from scene.path

    bool        WriteProjectManifest(const std::filesystem::path& path, const std::string& name);
    std::string ReadProjectManifestName(const std::filesystem::path& path, const std::string& fallback);
}
