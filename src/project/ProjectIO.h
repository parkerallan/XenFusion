#pragma once

#include <filesystem>
#include <string>

struct EngineState;
struct SceneFile;

// Project + scene persistence. A project is a folder containing a <name>.proj
// manifest, a scenes/ folder of .scene files (minimal JSON), and an assets/
// folder. Mirrors the reference engine's project scaffold, trimmed down.
namespace project
{
    // Scaffolds a new project inside `root` (folder name becomes the project
    // name) and opens it. Returns false on failure.
    bool CreateProject(const std::filesystem::path& root, EngineState& state);

    // Opens an existing project folder (must contain a *.proj manifest).
    bool OpenProject(const std::filesystem::path& root, EngineState& state);

    void CloseProject(EngineState& state);

    // (Re)loads every scenes/*.scene file into state.scenes.
    void LoadScenes(EngineState& state);

    bool SaveScene(SceneFile& scene);
    void SaveAllDirty(EngineState& state);

    // Creates a new empty scene file; returns its index in state.scenes (-1 on
    // failure) and selects it.
    int NewScene(EngineState& state, const std::string& name);

    // Adds a default object to `scene`, saves it, and returns the new index.
    int NewObject(EngineState& state, SceneFile& scene, const std::string& name);

    // Scene file operations (rename/copy/delete on disk + in state.scenes).
    bool RenameScene(EngineState& state, int index, const std::string& new_name);
    int  CopyScene(EngineState& state, int index);   // returns new index or -1
    void DeleteScene(EngineState& state, int index);

    // Recent projects list, persisted to recent_projects.txt next to the exe.
    void LoadRecents(EngineState& state);
    void AddRecent(EngineState& state, const std::filesystem::path& root);
    void RemoveRecent(EngineState& state, int index);
    void SaveRecents(const EngineState& state);
}
