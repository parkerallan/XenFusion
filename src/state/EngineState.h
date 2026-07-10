#pragma once

#include "core/Log.h"

#include "imgui.h"

#include <filesystem>
#include <string>
#include <vector>

// A component-style attribute attached to an object. For now the only type is
// "3D Model" (holds a model asset path). Serialized into the scene JSON.
struct ObjectAttribute
{
    std::string type = "3D Model";
    std::string model_path; // for "3D Model": path to the model asset
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
