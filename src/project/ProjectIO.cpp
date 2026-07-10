#include "project/ProjectIO.h"

#include "state/EngineState.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;
using nlohmann::json;

namespace
{
    fs::path RecentsPath()
    {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        return fs::path(std::wstring(buf, n)).parent_path() / "recent_projects.txt";
    }

    void WriteSceneJson(const SceneFile& scene)
    {
        json j;
        j["name"] = scene.name;
        j["objects"] = json::array();
        for (const SceneObject& o : scene.objects)
        {
            json jo;
            jo["name"]     = o.name;
            jo["position"] = {o.position[0], o.position[1], o.position[2]};
            jo["rotation"] = {o.rotation[0], o.rotation[1], o.rotation[2]};
            jo["scale"]    = {o.scale[0], o.scale[1], o.scale[2]};
            jo["visible"]  = o.visible;
            j["objects"].push_back(jo);
        }

        std::ofstream out(scene.path, std::ios::binary | std::ios::trunc);
        out << j.dump(2) << '\n';
    }

    bool ReadSceneJson(SceneFile& scene)
    {
        std::ifstream in(scene.path, std::ios::binary);
        if (!in)
            return false;

        json j;
        try { in >> j; }
        catch (const std::exception&) { return false; }

        scene.name = j.value("name", scene.path.stem().string());
        scene.objects.clear();
        if (j.contains("objects") && j["objects"].is_array())
        {
            for (const json& jo : j["objects"])
            {
                SceneObject o;
                o.name = jo.value("name", std::string("Object"));
                auto read3 = [&](const char* key, float* dst, float d0, float d1, float d2)
                {
                    if (jo.contains(key) && jo[key].is_array() && jo[key].size() == 3)
                    {
                        dst[0] = jo[key][0].get<float>();
                        dst[1] = jo[key][1].get<float>();
                        dst[2] = jo[key][2].get<float>();
                    }
                    else { dst[0] = d0; dst[1] = d1; dst[2] = d2; }
                };
                read3("position", o.position, 0, 0, 0);
                read3("rotation", o.rotation, 0, 0, 0);
                read3("scale",    o.scale,    1, 1, 1);
                o.visible = jo.value("visible", true);
                scene.objects.push_back(o);
            }
        }
        scene.dirty = false;
        return true;
    }
}

namespace project
{
    bool CreateProject(const fs::path& root, EngineState& state)
    {
        std::error_code ec;
        const std::string name = root.filename().string();

        for (const char* sub : {"scenes", "assets", "assets/models", "assets/textures", "assets/audio"})
            fs::create_directories(root / sub, ec);

        // Manifest.
        {
            json j;
            j["name"]         = name;
            j["version"]      = 1;
            j["startupScene"] = "scenes/Main.scene";
            std::ofstream out(root / (name + ".proj"), std::ios::binary | std::ios::trunc);
            if (!out) { state.AddLog("Failed to write project manifest"); return false; }
            out << j.dump(2) << '\n';
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
            state.AddLog("Open project failed: not a folder");
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
            state.AddLog("Open project failed: no .proj manifest in " + root.filename().string());
            return false;
        }

        state.project_root = root;
        state.project_name = manifest.stem().string();
        try
        {
            std::ifstream in(manifest, std::ios::binary);
            json j; in >> j;
            state.project_name = j.value("name", state.project_name);
        }
        catch (const std::exception&) { /* keep filename-derived name */ }

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
        obj.name = name + " " + std::to_string(scene.objects.size() + 1);
        scene.objects.push_back(obj);
        SaveScene(scene);
        state.AddLog("Created object: " + obj.name);
        return (int)scene.objects.size() - 1;
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
