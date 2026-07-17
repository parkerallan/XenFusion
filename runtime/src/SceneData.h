#pragma once

#include <string>
#include <vector>

// The runtime's in-memory scene, loaded from the same .scene / .proj JSON the
// editor writes. Mirrors EngineState's SceneObject / ObjectAttribute, trimmed to
// what the runtime draws (no editor-only fields).

struct RtAttribute
{
    std::string type;        // "3D Model" | "Shader" | "Camera"
    std::string model_path;  // for "3D Model"
    std::string shader_path; // for "Shader"

    // For "Camera": the object's transform places/aims it (rotation 0 looks
    // down +Z). Defaults match the old fixed-camera projection.
    float cam_fov;    // vertical FOV, degrees
    float cam_near;
    float cam_far;
    bool  cam_active; // at most one per scene: drives the runtime view

    // For "Directional Light" / "Point Light": color * intensity. Directional
    // direction = object rotation (+Z forward); point position = object
    // position, fading to zero at range.
    float light_color[3];
    float light_intensity;
    float light_range; // "Point Light" only

    RtAttribute()
    {
        cam_fov    = 45.0f;
        cam_near   = 0.5f;
        cam_far    = 100.0f;
        cam_active = false;
        light_color[0] = light_color[1] = light_color[2] = 1.0f;
        light_intensity = 1.0f;
        light_range     = 15.0f;
    }
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
