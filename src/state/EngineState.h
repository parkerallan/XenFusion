#pragma once

#include "core/Log.h"
#include "input/ControllerMapping.h"

#include "imgui.h"

#include <filesystem>
#include <string>
#include <vector>

// A component-style attribute attached to an object. Types: "3D Model" (holds a
// model asset path), "Shader" (holds a custom .hlsl path, replacing the
// built-in material for that object), and "Camera" (projection params; the
// object's transform places and aims it). Serialized into the scene JSON.
struct ObjectAttribute
{
    std::string type = "3D Model";
    std::string model_path;  // for "3D Model": path to the model asset
    std::string shader_path; // for "Shader": path to a custom .hlsl asset
    std::string script_path; // for "Script": path to a .lua gameplay script
    bool        cast_shadow = false; // for "3D Model": explicit directional-shadow caster toggle

    // For "Animator": a project-relative .anim controller assigned to the
    // object's skinned 3D Model. Runtime instances remain transient.
    std::string animator_controller_path;
    std::string animator_initial_state;
    float       animator_playback_speed = 1.0f;
    bool        animator_auto_play = true;

    // For "Camera". Rotation (0,0,0) looks down +Z; at most one camera per
    // scene is active — it drives the runtime view (defaults match the old
    // fixed-camera projection).
    float cam_fov    = 45.0f; // vertical FOV, degrees
    float cam_near   = 0.5f;
    float cam_far    = 100.0f;
    bool  cam_active = false;

    // Camera behavior (camr::CamType): 0=Fixed, 1=Follow, 2=Track.
    // Follow chases a named object's live (physics/script) position; Track
    // rides a B-spline through world-space points. Resolved by the shared
    // camera/CameraResolve.h on both targets.
    int         cam_type = 0;
    std::string cam_follow_target;                       // object name, same scene
    float       cam_follow_offset[3] = {0.0f, 2.0f, -5.0f}; // behind (+Z-forward engine) and above
    float       cam_follow_orbit[3]  = {0.0f, 0.0f, 0.0f};  // yaw/pitch of the offset (z unused)
    float       cam_follow_rot_offset[3] = {0.0f, 0.0f, 0.0f};
    bool        cam_follow_lock      = false;            // stay put, only aim at target
    float       cam_follow_smoothing = 0.0f;             // ease time constant, seconds (0 = snap)
    std::vector<float> cam_track_points;                 // world-space xyz triplets
    float       cam_track_speed      = 5.0f;             // units/sec cruise
    float       cam_track_accel      = 0.0f;             // units/sec^2 ramp (0 = instant)
    float       cam_track_rot_offset[3] = {0.0f, 0.0f, 0.0f};

    // For "Directional Light" / "Point Light" / "Spot Light" / "Environment
    // Light" (color * intensity lights the standard material). Directional:
    // direction = object rotation (+Z forward, like the camera), first light in
    // scene order wins. Point: position = object position, fades to zero at
    // range, first four win. Spot: position + direction from the object
    // transform, smoothstep cone between the half-angles, first two win.
    // Environment: flat ambient boost, every instance sums.
    float light_color[3]  = {1.0f, 1.0f, 1.0f};
    float light_intensity = 1.0f;
    float light_range     = 15.0f; // "Point Light" / "Spot Light"
    float light_inner_deg = 20.0f; // "Spot Light": inner cone half-angle, degrees
    float light_outer_deg = 30.0f; // "Spot Light": outer cone half-angle, degrees
    int   light_mode      = 1;     // 0=Baked, 1=Realtime, 2=Mixed
    bool  light_volumetric = false;          // "Spot Light": draw the beam cone
    float light_volumetric_intensity = 1.0f; // beam brightness scale

    // For "Rigid Body" (simulated by Bullet in the editor preview and on the
    // console). The object's transform is the initial pose; scale stays authored
    // (the collider is sized by phys_size below, not the object scale).
    int   phys_kind        = 0;    // 0=Static, 1=Dynamic, 2=Kinematic
    int   phys_shape       = 0;    // 0=Box, 1=Sphere
    float phys_size[3]     = {0.5f, 0.5f, 0.5f}; // Box half-extents / Sphere radius (x)
    float phys_mass        = 1.0f; // Dynamic only
    float phys_lin_damping = 0.0f;
    float phys_ang_damping = 0.0f;
    float phys_restitution = 0.0f; // bounciness, 0 = dead .. 1 = fully elastic
    float phys_friction    = 0.5f; // contact friction
    bool  phys_gravity     = true; // false = this body ignores world gravity
    float phys_gravity_scale = 1.0f; // world-gravity multiplier (2 = falls faster)
    bool  phys_lock_rotation[3] = {false, false, false};

    // For "Trigger Volume" (overlap-report only, no physical response).
    int   trig_shape       = 0;    // 0=Box, 1=Sphere
    float trig_size[3]     = {0.5f, 0.5f, 0.5f}; // Box half-extents / Sphere radius (x)

    // For "Image" (static screen-space overlay). Source PNG/JPG files are
    // cooked to native Xbox textures for the console runtime.
    std::string image_path;
    float image_x      = 0.0f;
    float image_y      = 0.0f;
    float image_w      = 256.0f;
    float image_h      = 256.0f;
    bool  image_stretch = false;
    bool  image_lock_aspect = false;
    float image_tint[3] = {1.0f, 1.0f, 1.0f};
    float image_alpha   = 1.0f;
    int   image_priority = 1;

    // For "Color" (a flat screen-space block — fades, letterbox bars, a panel
    // behind some Text). Image's field set minus the asset: there is nothing to
    // load, so the same 1280x720 reference space drives a solid quad.
    float color_x      = 0.0f;
    float color_y      = 0.0f;
    float color_w      = 256.0f;
    float color_h      = 256.0f;
    bool  color_stretch = false;
    bool  color_lock_aspect = false;
    float color_rgb[3] = {1.0f, 1.0f, 1.0f};
    float color_alpha  = 1.0f;
    int   color_priority = 1;

    // For "Skybox" (the scene background, behind everything). An
    // equirectangular (lat-long) PNG/JPG, cooked like any other image; rotation
    // is a yaw about world Y, in degrees. The first Skybox in scene order wins
    // and the owning object's transform is ignored.
    std::string sky_path;
    float       sky_rotation = 0.0f;

    // For "Text" (screen-space glyph overlay). Fonts are previewed from the
    // source TTF/OTF in the editor and cooked to an Xbox-native atlas at build.
    // Position/size use the same 1280x720 reference space as Image and Video.
    std::string text_font_path;
    std::string text_value;
    float text_x         = 64.0f;
    float text_y         = 36.0f;
    float text_w         = 400.0f;
    float text_h         = 80.0f;
    float text_font_size = 32.0f;
    float text_color[3]  = {1.0f, 1.0f, 1.0f};
    float text_alpha     = 1.0f;
    bool  text_lock_aspect = false;
    int   text_priority  = 1;

    // For "Video" (screen-space overlay quad, mirrors the Vulkan engine's
    // Video2D): an .mpg (MPEG-1 + MP2) decoded by the shared VideoPlayer.
    // Position/size are in a 1280x720 reference space; edit mode shows the
    // first frame, Play mode (and the console) plays.
    std::string video_path;                      // project-relative .mpg
    int   video_import_profile = 0;              // 0=360p, 1=480p, 2=720p
    float video_x      = 320.0f;                 // top-left, reference px
    float video_y      = 180.0f;
    float video_w      = 640.0f;                 // size, reference px
    float video_h      = 360.0f;                 // ignored while aspect-locked
    bool  video_stretch = false;                 // fill the whole screen
    bool  video_lock_aspect = true;              // height from the video's aspect
    float video_tint[3] = {1.0f, 1.0f, 1.0f};
    float video_alpha   = 1.0f;
    int   video_priority = 1;                    // higher = drawn behind
    int   video_play_mode = 2;                   // 0=Off, 1=Play Once, 2=Loop
    float video_volume  = 1.0f;                  // audio (Phase 3)
    bool  video_muted   = false;

    // For "Audio" (a positional/2D sound source, mirroring the Vulkan
    // engine's Audio attribute): an .mp2 clip decoded by the shared
    // AudioPlayer. Plays only in Play mode (and on the console); playMode On
    // at scene start = autoplay.
    std::string audio_path;                      // project-relative .mp2
    int   audio_play      = 0;                   // 0=Off, 1=On
    float audio_volume    = 1.0f;                // linear, 0..20
    float audio_pitch     = 1.0f;                // 0.1..4
    bool  audio_loop      = false;
    int   audio_class     = 0;                   // Effect/Music/Ambience/Dialogue
    int   audio_priority  = 0;                   // admission rank within/above class
    int   audio_load_mode = 0;                   // Auto/Resident/Stream
    bool  audio_spatial   = true;                // 3D emitter at the object
    float audio_min_dist  = 1.0f;                // full volume inside this
    float audio_max_dist  = 50.0f;               // inaudible beyond this
    float audio_doppler   = 1.0f;                // doppler scale (0 = off)
};

// A single object within a scene. Objects are not files — they live inside a
// .scene file and are shown as children of that file in the Files tree.
struct SceneObject
{
    std::string name;
    float       position[3] = {0.0f, 0.0f, 0.0f};
    float       rotation[3] = {0.0f, 0.0f, 0.0f};
    float       scale[3]    = {1.0f, 1.0f, 1.0f};
    bool        visible     = true;
    // Free-form labels for gameplay queries (find_by_tag / obj:has_tag).
    // Matching is exact and case-sensitive. Omitted from the JSON when empty,
    // so scenes authored before tags existed load and re-save unchanged.
    std::vector<std::string> tags;
    std::vector<ObjectAttribute> attributes;
};

// One .scene file on disk, parsed into memory. Serialized as minimal JSON.
struct SceneFile
{
    std::filesystem::path    path;    // absolute path to the .scene file
    std::string              name;    // scene display name
    std::vector<SceneObject> objects;
    bool                     dirty = false; // unsaved edits
};

// Editor color theme.
enum class Theme
{
    Light,   // ImGui light theme
    Gray,    // light-based, shifted toward gray
    Classic, // ImGui classic theme
};

// Central editor state. Every panel takes a reference to this and reads/writes
// through it, so panels stay decoupled from one another (mirrors the reference
// engine's state-driven design).
struct EngineState
{
    Theme theme = Theme::Gray;

    // --- Dock / window visibility ---
    bool dock_layout_built    = false;
    bool show_viewport_panel  = true;
    bool show_inspector_panel = true;
    bool show_files_panel     = true;
    bool show_version_control_panel = true;
    bool show_assets_panel    = true;
    bool show_settings_panel  = true;
    bool show_log_panel       = true;
    bool show_performance_panel = true;
    bool show_editor_panel    = true;
    bool show_mapper_panel    = true;
    bool show_animator_panel  = true;
    bool show_style_editor    = false;
    bool compile_shaders_requested = false; // compile all project shaders next frame
    bool build_run_requested       = false; // build the .xex + launch it in Xenia
    bool build_iso_requested       = false; // build the .xex + pack an XDVDFS .iso
    bool clean_build_requested     = false; // delete the Xbox build artifacts

    // Text/code file currently open in the Editor panel (empty = none).
    std::filesystem::path open_file_path;
    std::filesystem::path open_animator_path;
    std::filesystem::path saved_animator_path;

    // --- Project ---
    std::filesystem::path project_root; // empty = no project open
    std::string           project_name;
    std::vector<SceneFile> scenes;      // all .scene files under scenes/

    // Recently opened project roots (most-recent first).
    std::vector<std::filesystem::path> recent_projects;
    bool show_recent_modal = false;

    // Selection: an object inside a scene (both -1 = nothing).
    int selected_scene  = -1; // index into scenes
    int selected_object = -1; // index into scenes[selected_scene].objects

    // Selected control point of the selected object's Track camera (-1 = none).
    // While a point is selected the viewport gizmo moves the point, not the
    // object. Reset whenever the object selection changes.
    int selected_track_point_index = -1;

    // --- Assets browser ---
    std::filesystem::path assets_cwd;                // current dir, relative to assets/
    ImFont*               large_icon_font = nullptr; // big glyphs for asset tiles

    // --- Asset import (drag-drop from Explorer) ---
    bool                               show_import_modal = false;
    std::vector<std::filesystem::path> import_files; // staged for import
    int                                import_dest = 0; // index into destination list

    // --- Viewport ---
    float clear_color[4] = {0.094f, 0.094f, 0.106f, 1.0f};

    // Physics preview: when true, the viewport steps the Bullet simulation and
    // overrides object transforms for rendering (the saved scene is untouched).
    // Transient — never serialized.
    bool physics_playing = false;

    // Editor-only PC keyboard/mouse -> Xbox control mapping, for driving scripts
    // in the Play preview. Loaded per-project by the Mapping panel.
    input::ControllerMapping controller_mapping;

    // --- Toolchain (console build) paths, chosen via the folder picker ---
    std::string toolchain_xdk;      // Xbox 360 SDK (XDK) root -> passed to the build as XEDK
    std::string toolchain_emulator; // Xenia emulator
    int         toolchain_pick = 0; // 0=none, 1=xdk, 2=emulator, 3=build output (EngineApplication picks)

    // --- Build configuration (console build outputs) ---
    std::string build_output_dir;          // base dir for all build artifacts (empty = runtime/)
    std::string build_config = "Release";  // "Release" or "Debug"
    std::string build_iso_name;            // disc image filename (empty = <project>.iso)

    // --- Log --- (entries live in the global applog buffer)
    bool auto_scroll_log = true;
    bool log_show_info    = true;
    bool log_show_warning = true;
    bool log_show_error   = true;
    bool log_show_build   = true;
    bool log_show_script  = true; // script log() output ([LOG])

    void AddLog(const std::string& message, LogLevel level = LogLevel::Info)
    {
        applog::Add(level, message);
    }

    bool HasProject() const { return !project_root.empty(); }

    std::filesystem::path AssetsDir() const { return project_root / "assets"; }
    std::filesystem::path ScenesDir() const { return project_root / "scenes"; }

    SceneFile* SelectedScene()
    {
        if (selected_scene < 0 || selected_scene >= (int)scenes.size())
            return nullptr;
        return &scenes[selected_scene];
    }

    SceneObject* SelectedObject()
    {
        SceneFile* scene = SelectedScene();
        if (!scene)
            return nullptr;
        if (selected_object < 0 || selected_object >= (int)scene->objects.size())
            return nullptr;
        return &scene->objects[selected_object];
    }
};
