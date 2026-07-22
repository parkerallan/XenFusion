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
                ja["script_path"] = a.script_path;
                if (a.type == "Camera")
                {
                    ja["fov"]    = a.cam_fov;
                    ja["near"]   = a.cam_near;
                    ja["far"]    = a.cam_far;
                    ja["active"] = a.cam_active;
                    ja["camType"] = a.cam_type;
                    if (a.cam_type == 1) // Follow
                    {
                        ja["followTarget"]    = a.cam_follow_target;
                        ja["followOffset"]    = {a.cam_follow_offset[0], a.cam_follow_offset[1], a.cam_follow_offset[2]};
                        ja["followOrbit"]     = {a.cam_follow_orbit[0], a.cam_follow_orbit[1], a.cam_follow_orbit[2]};
                        ja["followRotOffset"] = {a.cam_follow_rot_offset[0], a.cam_follow_rot_offset[1], a.cam_follow_rot_offset[2]};
                        ja["followLock"]      = a.cam_follow_lock;
                        ja["followSmoothing"] = a.cam_follow_smoothing;
                    }
                    else if (a.cam_type == 2) // Track
                    {
                        json pts = json::array();
                        for (std::size_t p = 0; p + 2 < a.cam_track_points.size(); p += 3)
                            pts.push_back({a.cam_track_points[p], a.cam_track_points[p + 1], a.cam_track_points[p + 2]});
                        ja["trackPoints"]    = pts;
                        ja["trackSpeed"]     = a.cam_track_speed;
                        ja["trackAccel"]     = a.cam_track_accel;
                        ja["trackRotOffset"] = {a.cam_track_rot_offset[0], a.cam_track_rot_offset[1], a.cam_track_rot_offset[2]};
                    }
                }
                else if (a.type == "Directional Light" || a.type == "Point Light" ||
                         a.type == "Spot Light" || a.type == "Environment Light")
                {
                    ja["color"]     = {a.light_color[0], a.light_color[1], a.light_color[2]};
                    ja["intensity"] = a.light_intensity;
                    if (a.type == "Point Light" || a.type == "Spot Light")
                        ja["range"] = a.light_range;
                    if (a.type == "Spot Light")
                    {
                        ja["innerCone"] = a.light_inner_deg;
                        ja["outerCone"] = a.light_outer_deg;
                        ja["volumetric"] = a.light_volumetric;
                        ja["volumetricIntensity"] = a.light_volumetric_intensity;
                    }
                }
                else if (a.type == "Rigid Body")
                {
                    ja["kind"]        = a.phys_kind;
                    ja["shape"]       = a.phys_shape;
                    ja["size"]        = {a.phys_size[0], a.phys_size[1], a.phys_size[2]};
                    ja["mass"]        = a.phys_mass;
                    ja["linDamping"]  = a.phys_lin_damping;
                    ja["angDamping"]  = a.phys_ang_damping;
                    ja["restitution"] = a.phys_restitution;
                    ja["friction"]    = a.phys_friction;
                    ja["gravity"]     = a.phys_gravity;
                    ja["gravityScale"]= a.phys_gravity_scale;
                }
                else if (a.type == "Trigger Volume")
                {
                    ja["shape"] = a.trig_shape;
                    ja["size"]  = {a.trig_size[0], a.trig_size[1], a.trig_size[2]};
                }
                else if (a.type == "Video")
                {
                    ja["videoPath"]  = a.video_path;
                    ja["x"]          = a.video_x;
                    ja["y"]          = a.video_y;
                    ja["w"]          = a.video_w;
                    ja["h"]          = a.video_h;
                    ja["stretch"]    = a.video_stretch;
                    ja["lockAspect"] = a.video_lock_aspect;
                    ja["tint"]       = {a.video_tint[0], a.video_tint[1], a.video_tint[2]};
                    ja["alpha"]      = a.video_alpha;
                    ja["priority"]   = a.video_priority;
                    ja["playMode"]   = a.video_play_mode;
                    ja["volume"]     = a.video_volume;
                    ja["muted"]      = a.video_muted;
                }
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
                        a.script_path = ja.value("script_path", std::string());
                        a.cam_fov     = ja.value("fov", 45.0f);
                        a.cam_near    = ja.value("near", 0.5f);
                        a.cam_far     = ja.value("far", 100.0f);
                        a.cam_active  = ja.value("active", false);
                        a.cam_type    = ja.value("camType", 0);
                        a.cam_follow_target    = ja.value("followTarget", std::string());
                        a.cam_follow_lock      = ja.value("followLock", false);
                        a.cam_follow_smoothing = ja.value("followSmoothing", 0.0f);
                        a.cam_track_speed      = ja.value("trackSpeed", 5.0f);
                        a.cam_track_accel      = ja.value("trackAccel", 0.0f);
                        auto readA3 = [&](const char* key, float* dst)
                        {
                            if (ja.contains(key) && ja[key].is_array() && ja[key].size() == 3)
                                for (int k = 0; k < 3; ++k) dst[k] = ja[key][k].get<float>();
                        };
                        readA3("followOffset", a.cam_follow_offset);
                        readA3("followOrbit", a.cam_follow_orbit);
                        readA3("followRotOffset", a.cam_follow_rot_offset);
                        readA3("trackRotOffset", a.cam_track_rot_offset);
                        if (ja.contains("trackPoints") && ja["trackPoints"].is_array())
                            for (const json& jp : ja["trackPoints"])
                                if (jp.is_array() && jp.size() == 3)
                                    for (int k = 0; k < 3; ++k)
                                        a.cam_track_points.push_back(jp[k].get<float>());
                        if (ja.contains("color") && ja["color"].is_array() && ja["color"].size() == 3)
                        {
                            a.light_color[0] = ja["color"][0].get<float>();
                            a.light_color[1] = ja["color"][1].get<float>();
                            a.light_color[2] = ja["color"][2].get<float>();
                        }
                        a.light_intensity = ja.value("intensity", 1.0f);
                        a.light_range     = ja.value("range", 15.0f);
                        a.light_inner_deg = ja.value("innerCone", 20.0f);
                        a.light_outer_deg = ja.value("outerCone", 30.0f);
                        a.light_volumetric = ja.value("volumetric", false);
                        a.light_volumetric_intensity = ja.value("volumetricIntensity", 1.0f);

                        auto readSize = [&](float* dst)
                        {
                            if (ja.contains("size") && ja["size"].is_array() && ja["size"].size() == 3)
                            {
                                dst[0] = ja["size"][0].get<float>();
                                dst[1] = ja["size"][1].get<float>();
                                dst[2] = ja["size"][2].get<float>();
                            }
                        };
                        if (a.type == "Rigid Body")
                        {
                            a.phys_kind        = ja.value("kind", 0);
                            a.phys_shape       = ja.value("shape", 0);
                            readSize(a.phys_size);
                            a.phys_mass        = ja.value("mass", 1.0f);
                            a.phys_lin_damping = ja.value("linDamping", 0.0f);
                            a.phys_ang_damping = ja.value("angDamping", 0.0f);
                            a.phys_restitution = ja.value("restitution", 0.0f);
                            a.phys_friction    = ja.value("friction", 0.5f);
                            a.phys_gravity     = ja.value("gravity", true);
                            a.phys_gravity_scale = ja.value("gravityScale", 1.0f);
                        }
                        else if (a.type == "Trigger Volume")
                        {
                            a.trig_shape = ja.value("shape", 0);
                            readSize(a.trig_size);
                        }
                        else if (a.type == "Video")
                        {
                            a.video_path        = ja.value("videoPath", std::string());
                            a.video_x           = ja.value("x", 320.0f);
                            a.video_y           = ja.value("y", 180.0f);
                            a.video_w           = ja.value("w", 640.0f);
                            a.video_h           = ja.value("h", 360.0f);
                            a.video_stretch     = ja.value("stretch", false);
                            a.video_lock_aspect = ja.value("lockAspect", true);
                            if (ja.contains("tint") && ja["tint"].is_array() && ja["tint"].size() == 3)
                                for (int k = 0; k < 3; ++k) a.video_tint[k] = ja["tint"][k].get<float>();
                            a.video_alpha       = ja.value("alpha", 1.0f);
                            a.video_priority    = ja.value("priority", 1);
                            a.video_play_mode   = ja.value("playMode", 2);
                            a.video_volume      = ja.value("volume", 1.0f);
                            a.video_muted       = ja.value("muted", false);
                        }
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
