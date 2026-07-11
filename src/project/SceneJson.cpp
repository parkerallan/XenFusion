#include "project/SceneJson.h"

#include "state/EngineState.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace project
{
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
            jo["attributes"] = json::array();
            for (const ObjectAttribute& a : o.attributes)
            {
                json ja;
                ja["type"]        = a.type;
                ja["model_path"]  = a.model_path;
                ja["shader_path"] = a.shader_path;
                jo["attributes"].push_back(ja);
            }
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
                if (jo.contains("attributes") && jo["attributes"].is_array())
                {
                    for (const json& ja : jo["attributes"])
                    {
                        ObjectAttribute a;
                        a.type        = ja.value("type", std::string("3D Model"));
                        a.model_path  = ja.value("model_path", std::string());
                        a.shader_path = ja.value("shader_path", std::string());
                        o.attributes.push_back(a);
                    }
                }
                scene.objects.push_back(o);
            }
        }
        scene.dirty = false;
        return true;
    }

    bool WriteProjectManifest(const fs::path& path, const std::string& name)
    {
        json j;
        j["name"]         = name;
        j["version"]      = 1;
        j["startupScene"] = "scenes/Main.scene";

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << j.dump(2) << '\n';
        return true;
    }

    std::string ReadProjectManifestName(const fs::path& path, const std::string& fallback)
    {
        try
        {
            std::ifstream in(path, std::ios::binary);
            json j;
            in >> j;
            return j.value("name", fallback);
        }
        catch (const std::exception&)
        {
            return fallback;
        }
    }
}
