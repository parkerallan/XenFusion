#pragma once

#include <string>
#include <vector>

// The runtime's in-memory scene, loaded from the same .scene / .proj JSON the
// editor writes. Mirrors EngineState's SceneObject / ObjectAttribute, trimmed to
// what the runtime draws (no editor-only fields).

struct RtAttribute
{
    std::string type;        // "3D Model" | "Shader"
    std::string model_path;  // for "3D Model"
    std::string shader_path; // for "Shader"
};

struct RtObject
{
    std::string name;
    float position[3];
    float rotation[3];
    float scale[3];
    bool  visible;
    std::vector<RtAttribute> attributes;

    RtObject()
    {
        position[0] = position[1] = position[2] = 0.0f;
        rotation[0] = rotation[1] = rotation[2] = 0.0f;
        scale[0] = scale[1] = scale[2] = 1.0f;
        visible = true;
    }
};

struct RtScene
{
    std::string name;
    std::vector<RtObject> objects;
};

namespace scenedata
{
    // Read a <name>.proj and return its "startupScene" (e.g. "scenes/Main.scene"),
    // or an empty string if the file is missing / has no startupScene.
    std::string ReadStartupScene(const std::string& proj_path);

    // Parse a .scene JSON file into `out`. Returns false if the file can't be
    // read or parsed.
    bool LoadScene(const std::string& scene_path, RtScene& out);
}
