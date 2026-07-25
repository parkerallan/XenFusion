#include "anim/AnimatorController.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <set>

using nlohmann::json;

namespace animator
{
    bool ValidateController(const AnimatorController& controller, std::string& error)
    {
        error.clear();
        std::set<std::string> clip_ids;
        for (const AnimatorClipReference& clip : controller.clips)
        {
            if (clip.id.empty() || clip.clip_name.empty() || clip.source_model_path.empty())
            {
                error = "each clip requires id, clip_name, and source_model_path";
                return false;
            }
            if (!clip_ids.insert(clip.id).second)
            {
                error = "duplicate clip id: " + clip.id;
                return false;
            }
        }

        std::set<std::string> state_names;
        for (const AnimatorStateDefinition& state : controller.states)
        {
            if (state.name.empty() || !state_names.insert(state.name).second)
            {
                error = state.name.empty() ? "state name cannot be empty"
                                           : "duplicate state: " + state.name;
                return false;
            }
            if (clip_ids.find(state.clip_id) == clip_ids.end())
            {
                error = "state '" + state.name + "' references missing clip '" + state.clip_id + "'";
                return false;
            }
            if (!std::isfinite(state.playback_speed) || state.playback_speed <= 0.0f)
            {
                error = "state '" + state.name + "' has invalid playback speed";
                return false;
            }
        }
        if (!controller.states.empty() && state_names.find(controller.default_state) == state_names.end())
        {
            error = "default state does not exist: " + controller.default_state;
            return false;
        }

        for (const AnimatorTransitionDefinition& transition : controller.transitions)
        {
            if (state_names.find(transition.from_state) == state_names.end() ||
                state_names.find(transition.to_state) == state_names.end())
            {
                error = "transition references a missing state";
                return false;
            }
            if (!std::isfinite(transition.blend_duration) || transition.blend_duration < 0.0f ||
                !std::isfinite(transition.exit_time) || transition.exit_time < 0.0f)
            {
                error = "transition from '" + transition.from_state + "' has invalid timing";
                return false;
            }
        }
        return true;
    }

    bool LoadController(const std::filesystem::path& path, AnimatorController& output,
                        std::string& error)
    {
        std::ifstream input(path, std::ios::binary);
        json root = json::parse(input, nullptr, false);
        if (!input || root.is_discarded() || !root.is_object())
        {
            error = "controller is missing or is not valid JSON";
            return false;
        }

        AnimatorController loaded;
        loaded.version = root.value("version", 1);
        loaded.name = root.value("name", std::string("NewAnimator"));
        loaded.default_state = root.value("default_state", std::string());
        loaded.preview_model_path = root.value("preview_model_path", std::string());
        for (const json& value : root.value("clips", json::array()))
        {
            AnimatorClipReference clip;
            clip.id = value.value("id", std::string());
            clip.source_model_path = value.value("source_model_path", std::string());
            clip.clip_name = value.value("clip_name", std::string());
            loaded.clips.push_back(clip);
        }
        for (const json& value : root.value("states", json::array()))
        {
            AnimatorStateDefinition state;
            state.name = value.value("name", std::string());
            state.clip_id = value.value("clip_id", std::string());
            state.playback_speed = value.value("playback_speed", 1.0f);
            state.loop = value.value("loop", true);
            loaded.states.push_back(state);
        }
        for (const json& value : root.value("transitions", json::array()))
        {
            AnimatorTransitionDefinition transition;
            transition.from_state = value.value("from_state", std::string());
            transition.to_state = value.value("to_state", std::string());
            transition.condition = value.value("condition", std::string());
            transition.blend_duration = value.value("blend_duration", 0.15f);
            transition.has_exit_time = value.value("has_exit_time", false);
            transition.exit_time = value.value("exit_time", 1.0f);
            loaded.transitions.push_back(transition);
        }
        for (const json& value : root.value("bone_modifiers", json::array()))
            loaded.bone_modifiers.push_back(value);

        if (!ValidateController(loaded, error))
            return false;
        output = std::move(loaded);
        return true;
    }

    bool SaveController(const std::filesystem::path& path, const AnimatorController& controller,
                        std::string& error)
    {
        if (!ValidateController(controller, error))
            return false;

        json root;
        root["version"] = controller.version;
        root["name"] = controller.name;
        root["default_state"] = controller.default_state;
        if (!controller.preview_model_path.empty())
            root["preview_model_path"] = controller.preview_model_path;
        root["clips"] = json::array();
        for (const AnimatorClipReference& clip : controller.clips)
            root["clips"].push_back({{"id", clip.id}, {"source_model_path", clip.source_model_path},
                                     {"clip_name", clip.clip_name}});
        root["states"] = json::array();
        for (const AnimatorStateDefinition& state : controller.states)
            root["states"].push_back({{"name", state.name}, {"clip_id", state.clip_id},
                                      {"playback_speed", state.playback_speed}, {"loop", state.loop}});
        root["transitions"] = json::array();
        for (const AnimatorTransitionDefinition& transition : controller.transitions)
            root["transitions"].push_back({{"from_state", transition.from_state},
                {"to_state", transition.to_state}, {"condition", transition.condition},
                {"blend_duration", transition.blend_duration},
                {"has_exit_time", transition.has_exit_time}, {"exit_time", transition.exit_time}});
        root["bone_modifiers"] = controller.bone_modifiers;

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            error = "cannot write controller: " + path.string();
            return false;
        }
        output << root.dump(2) << '\n';
        return !!output;
    }
}