#pragma once

#include <string>
#include <vector>

// The runtime's in-memory scene, loaded from the same .scene / .proj JSON the
// editor writes. Mirrors EngineState's SceneObject / ObjectAttribute, trimmed to
// what the runtime draws (no editor-only fields).

struct RtAttribute
{
    std::string type;        // "3D Model" | "Shader" | "Camera" | "Script" | ...
    std::string model_path;  // for "3D Model"
    std::string shader_path; // for "Shader"
    std::string script_path; // for "Script": .lua gameplay script

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

    // For "Rigid Body" (Bullet). The object transform is the initial pose; scale
    // stays authored (colliders are sized by phys_size, not the object scale).
    int   phys_kind;        // 0=Static, 1=Dynamic, 2=Kinematic
    int   phys_shape;       // 0=Box, 1=Sphere
    float phys_size[3];     // Box half-extents / Sphere radius (x)
    float phys_mass;
    float phys_lin_damping;
    float phys_ang_damping;
    float phys_restitution;
    float phys_friction;
    bool  phys_gravity;
    float phys_gravity_scale;

    // For "Trigger Volume" (overlap-report only).
    int   trig_shape;       // 0=Box, 1=Sphere
    float trig_size[3];

    RtAttribute()
    {
        cam_fov    = 45.0f;
        cam_near   = 0.5f;
        cam_far    = 100.0f;
        cam_active = false;
        light_color[0] = light_color[1] = light_color[2] = 1.0f;
        light_intensity = 1.0f;
        light_range     = 15.0f;
        phys_kind = 0;
        phys_shape = 0;
        phys_size[0] = phys_size[1] = phys_size[2] = 0.5f;
        phys_mass = 1.0f;
        phys_lin_damping = 0.0f;
        phys_ang_damping = 0.0f;
        phys_restitution = 0.0f;
        phys_friction = 0.5f;
        phys_gravity = true;
        phys_gravity_scale = 1.0f;
        trig_shape = 0;
        trig_size[0] = trig_size[1] = trig_size[2] = 0.5f;
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
