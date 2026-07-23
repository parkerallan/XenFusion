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

    // Camera behavior (camr::CamType): 0=Fixed, 1=Follow, 2=Track. Resolved
    // per-frame by the shared camera/CameraResolve.h.
    int         cam_type;
    std::string cam_follow_target;      // object name, same scene
    float       cam_follow_offset[3];
    float       cam_follow_orbit[3];    // yaw/pitch (z unused)
    float       cam_follow_rot_offset[3];
    bool        cam_follow_lock;
    float       cam_follow_smoothing;   // seconds (0 = snap)
    std::vector<float> cam_track_points; // world-space xyz triplets
    float       cam_track_speed;
    float       cam_track_accel;
    float       cam_track_rot_offset[3];

    // For "Directional Light" / "Point Light" / "Spot Light" / "Environment
    // Light": color * intensity. Directional direction = object rotation (+Z
    // forward); point/spot position = object position, fading to zero at range;
    // spot cones between the half-angles; environment = flat ambient sum.
    float light_color[3];
    float light_intensity;
    float light_range;     // "Point Light" / "Spot Light"
    float light_inner_deg; // "Spot Light": inner cone half-angle, degrees
    float light_outer_deg; // "Spot Light": outer cone half-angle, degrees
    bool  light_volumetric;           // "Spot Light": draw the beam cone
    float light_volumetric_intensity; // beam brightness scale

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

    // For "Video" (screen-space overlay quad; decoded by the shared
    // VideoPlayer from the pak's raw VIDE entry). Position/size in the
    // 1280x720 reference space; playMode 0=Off, 1=Play Once, 2=Loop.
    std::string video_path;
    float video_x, video_y, video_w, video_h;
    bool  video_stretch;
    bool  video_lock_aspect;
    float video_tint[3];
    float video_alpha;
    int   video_priority;   // higher = drawn behind
    int   video_play_mode;
    float video_volume;     // audio (Phase 3)
    bool  video_muted;

    // For "Audio" (a sound source at the object; .mp2 decoded by the shared
    // AudioPlayer from the pak's raw AUDI entry). playMode 0=Off, 1=On.
    std::string audio_path;
    int   audio_play;
    float audio_volume;     // linear, 0..20
    float audio_pitch;      // 0.1..4
    bool  audio_loop;
    bool  audio_spatial;    // 3D emitter (spatial phase)
    float audio_min_dist;
    float audio_max_dist;
    float audio_doppler;

    RtAttribute()
    {
        cam_fov    = 45.0f;
        cam_near   = 0.5f;
        cam_far    = 100.0f;
        cam_active = false;
        cam_type   = 0;
        cam_follow_offset[0] = 0.0f; cam_follow_offset[1] = 2.0f; cam_follow_offset[2] = -5.0f;
        cam_follow_orbit[0] = cam_follow_orbit[1] = cam_follow_orbit[2] = 0.0f;
        cam_follow_rot_offset[0] = cam_follow_rot_offset[1] = cam_follow_rot_offset[2] = 0.0f;
        cam_follow_lock      = false;
        cam_follow_smoothing = 0.0f;
        cam_track_speed      = 5.0f;
        cam_track_accel      = 0.0f;
        cam_track_rot_offset[0] = cam_track_rot_offset[1] = cam_track_rot_offset[2] = 0.0f;
        light_color[0] = light_color[1] = light_color[2] = 1.0f;
        light_intensity = 1.0f;
        light_range     = 15.0f;
        light_inner_deg = 20.0f;
        light_outer_deg = 30.0f;
        light_volumetric = false;
        light_volumetric_intensity = 1.0f;
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
        video_x = 320.0f; video_y = 180.0f;
        video_w = 640.0f; video_h = 360.0f;
        video_stretch = false;
        video_lock_aspect = true;
        video_tint[0] = video_tint[1] = video_tint[2] = 1.0f;
        video_alpha = 1.0f;
        video_priority = 1;
        video_play_mode = 2;
        video_volume = 1.0f;
        video_muted = false;
        audio_play = 0;
        audio_volume = 1.0f;
        audio_pitch = 1.0f;
        audio_loop = false;
        audio_spatial = true;
        audio_min_dist = 1.0f;
        audio_max_dist = 50.0f;
        audio_doppler = 1.0f;
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
