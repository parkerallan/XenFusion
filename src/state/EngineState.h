#pragma once

#include "core/Log.h"

#include "imgui.h"

#include <filesystem>
#include <string>
#include <vector>

// A component-style attribute attached to an object. Types: "3D Model" (holds a
// model asset path) and "Shader" (holds a custom .hlsl path, replacing the
// built-in material for that object). Serialized into the scene JSON.
struct ObjectAttribute
{
    std::string type = "3D Model";
    std::string model_path;  // for "3D Model": path to the model asset
    std::string shader_path; // for "Shader": path to a custom .hlsl asset
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
    Light, // ImGui light theme
    Gray,  // light-based, shifted toward gray
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
    bool show_imgui_demo      = false;
    bool show_style_editor    = false;
    bool compile_shaders_requested = false; // compile all project shaders next frame
    bool build_run_requested       = false; // build the .xex + launch it in Xenia
    bool build_iso_requested       = false; // build the .xex + pack an XDVDFS .iso
    bool clean_build_requested     = false; // delete the Xbox build artifacts

    // Text/code file currently open in the Editor panel (empty = none).
    std::filesystem::path open_file_path;

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

    // --- Assets browser ---
    std::filesystem::path assets_cwd;                // current dir, relative to assets/
    ImFont*               large_icon_font = nullptr; // big glyphs for asset tiles

    // --- Asset import (drag-drop from Explorer) ---
    bool                               show_import_modal = false;
    std::vector<std::filesystem::path> import_files; // staged for import
    int                                import_dest = 0; // index into destination list

    // --- Viewport ---
    float clear_color[4] = {0.094f, 0.094f, 0.106f, 1.0f};

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
