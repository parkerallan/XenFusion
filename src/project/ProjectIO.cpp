#include "project/ProjectIO.h"

#include "project/SceneJson.h"
#include "state/EngineState.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
    fs::path RecentsPath()
    {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return fs::path(std::wstring(buf, n)).parent_path() / "recent_projects.txt";
    }
}

namespace project
{
    std::string MakeUniqueObjectName(const SceneFile& scene, const std::string& name, int ignore_index)
    {
        std::string candidate = name;
        int suffix = 2;
        for (;;)
        {
            bool used = false;
            for (int i = 0; i < (int)scene.objects.size(); ++i)
                if (i != ignore_index && scene.objects[i].name == candidate)
                {
                    used = true;
                    break;
                }
            if (!used) return candidate;
            candidate = name + " " + std::to_string(suffix++);
        }
    }

    bool CreateProject(const fs::path& root, EngineState& state)
    {
        std::error_code ec;
        const std::string name = root.filename().string();

        for (const char* sub : {"scenes", "assets", "assets/models", "assets/textures",
                                "assets/audio", "assets/scripts", "assets/animators"})
            fs::create_directories(root / sub, ec);

        // Manifest.
        if (!WriteProjectManifest(root / (name + ".proj"), name))
        {
            state.AddLog("Failed to write project manifest", LogLevel::Error);
            return false;
        }

        // Starter scene (only if not already present).
        const fs::path main_scene = root / "scenes" / "Main.scene";
        if (!fs::exists(main_scene))
        {
            SceneFile s;
            s.path = main_scene;
            s.name = "Main";
            WriteSceneJson(s);
        }

        state.AddLog("Created project: " + name);
        return OpenProject(root, state);
    }

    bool OpenProject(const fs::path& root, EngineState& state)
    {
        std::error_code ec;
        if (!fs::is_directory(root, ec))
        {
            state.AddLog("Open project failed: not a folder", LogLevel::Error);
            return false;
        }

        // Find a *.proj manifest in the folder.
        fs::path manifest;
        for (const fs::directory_entry& e : fs::directory_iterator(root, ec))
        {
            if (e.path().extension() == ".proj") { manifest = e.path(); break; }
        }
        if (manifest.empty())
        {
            state.AddLog("Open project failed: no .proj manifest in " + root.filename().string(), LogLevel::Error);
            return false;
        }

        state.project_root = root;
        state.project_name = ReadProjectManifestName(manifest, manifest.stem().string());

        // Ensure the conventional asset folders exist (idempotent) so older
        // projects gain assets/scripts for Lua gameplay scripts.
        fs::create_directories(root / "assets" / "scripts", ec);
        fs::create_directories(root / "assets" / "animators", ec);

        LoadScenes(state);
        AddRecent(state, root);
        state.AddLog("Opened project: " + state.project_name);
        return true;
    }

    void CloseProject(EngineState& state)
    {
        SaveAllDirty(state);
        state.project_root.clear();
        state.project_name.clear();
        state.scenes.clear();
        state.selected_scene = -1;
        state.selected_object = -1;
        state.assets_cwd.clear();
        state.AddLog("Closed project");
    }

    void LoadScenes(EngineState& state)
    {
        state.scenes.clear();
        state.selected_scene = -1;
        state.selected_object = -1;
        state.assets_cwd.clear();

        std::error_code ec;
        const fs::path dir = state.ScenesDir();
        if (!fs::is_directory(dir, ec))
            return;

        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
        {
            if (e.path().extension() != ".scene")
                continue;
            SceneFile scene;
            scene.path = e.path();
            if (ReadSceneJson(scene))
                state.scenes.push_back(std::move(scene));
        }

        std::sort(state.scenes.begin(), state.scenes.end(),
                  [](const SceneFile& a, const SceneFile& b) { return a.name < b.name; });

        if (!state.scenes.empty())
            state.selected_scene = 0;
    }

    bool SaveScene(SceneFile& scene)
    {
        WriteSceneJson(scene);
        scene.dirty = false;
        return true;
    }

    void SaveAllDirty(EngineState& state)
    {
        for (SceneFile& s : state.scenes)
            if (s.dirty)
                SaveScene(s);
    }

    int NewScene(EngineState& state, const std::string& name)
    {
        if (!state.HasProject())
            return -1;

        std::error_code ec;
        fs::create_directories(state.ScenesDir(), ec);

        // Ensure a unique file name.
        fs::path path = state.ScenesDir() / (name + ".scene");
        std::string unique = name;
        int suffix = 1;
        while (fs::exists(path))
        {
            unique = name + "_" + std::to_string(suffix++);
            path = state.ScenesDir() / (unique + ".scene");
        }

        SceneFile scene;
        scene.path = path;
        scene.name = unique;
        WriteSceneJson(scene);

        state.scenes.push_back(std::move(scene));
        const int index = (int)state.scenes.size() - 1;
        state.selected_scene = index;
        state.selected_object = -1;
        state.AddLog("Created scene: " + unique);
        return index;
    }

    int NewObject(EngineState& state, SceneFile& scene, const std::string& name)
    {
        SceneObject obj;
        obj.name = MakeUniqueObjectName(scene, name + " " + std::to_string(scene.objects.size() + 1));
        scene.objects.push_back(obj);
        SaveScene(scene);
        state.AddLog("Created object: " + obj.name);
        return (int)scene.objects.size() - 1;
    }

    bool RenameScene(EngineState& state, int index, const std::string& new_name)
    {
        if (index < 0 || index >= (int)state.scenes.size() || new_name.empty())
            return false;

        SceneFile& scene = state.scenes[index];
        const fs::path new_path = scene.path.parent_path() / (new_name + ".scene");
        std::error_code ec;
        if (new_path != scene.path)
        {
            if (fs::exists(new_path))
            {
                state.AddLog("Rename failed: '" + new_name + "' already exists");
                return false;
            }
            fs::rename(scene.path, new_path, ec);
            if (ec)
            {
                state.AddLog("Rename failed");
                return false;
            }
            scene.path = new_path;
        }
        scene.name = new_name;
        SaveScene(scene);
        state.AddLog("Renamed scene to: " + new_name);
        return true;
    }

    int CopyScene(EngineState& state, int index)
    {
        if (index < 0 || index >= (int)state.scenes.size())
            return -1;

        const SceneFile& src = state.scenes[index];
        const std::string base = src.name + " copy";
        std::string unique = base;
        fs::path path = src.path.parent_path() / (base + ".scene");
        int n = 1;
        while (fs::exists(path))
        {
            unique = base + " " + std::to_string(++n);
            path = src.path.parent_path() / (unique + ".scene");
        }

        SceneFile copy;
        copy.path = path;
        copy.name = unique;
        copy.objects = src.objects;
        WriteSceneJson(copy);

        state.scenes.push_back(std::move(copy));
        state.AddLog("Copied scene: " + unique);
        return (int)state.scenes.size() - 1;
    }

    void DeleteScene(EngineState& state, int index)
    {
        if (index < 0 || index >= (int)state.scenes.size())
            return;

        std::error_code ec;
        fs::remove(state.scenes[index].path, ec);
        state.AddLog("Deleted scene: " + state.scenes[index].name);
        state.scenes.erase(state.scenes.begin() + index);

        if (state.selected_scene == index)
        {
            state.selected_scene = -1;
            state.selected_object = -1;
        }
        else if (state.selected_scene > index)
        {
            --state.selected_scene;
        }
    }

    void LoadRecents(EngineState& state)
    {
        state.recent_projects.clear();
        std::ifstream in(RecentsPath());
        if (!in)
            return;
        std::string line;
        while (std::getline(in, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
                line.pop_back();
            if (!line.empty())
                state.recent_projects.emplace_back(line);
        }
    }

    void SaveRecents(const EngineState& state)
    {
        std::ofstream out(RecentsPath(), std::ios::trunc);
        if (!out)
            return;
        for (const fs::path& p : state.recent_projects)
            out << p.generic_string() << '\n';
    }

    void AddRecent(EngineState& state, const fs::path& root)
    {
        const fs::path normal = root.lexically_normal();
        auto& v = state.recent_projects;
        v.erase(std::remove(v.begin(), v.end(), normal), v.end());
        v.insert(v.begin(), normal);
        if (v.size() > 10)
            v.resize(10);
        SaveRecents(state);
    }

    void RemoveRecent(EngineState& state, int index)
    {
        if (index < 0 || index >= (int)state.recent_projects.size())
            return;
        state.recent_projects.erase(state.recent_projects.begin() + index);
        SaveRecents(state);
    }
}
