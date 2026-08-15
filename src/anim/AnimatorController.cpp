#include "anim/AnimatorController.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>

using nlohmann::json;

namespace animator
{
    namespace
    {
        bool ReadVector3(const json& value, const char* key, std::array<float, 3>& output)
        {
            const json::const_iterator found = value.find(key);
            if (found == value.end() || !found->is_array() || found->size() != 3)
                return false;
            for (std::size_t axis = 0; axis < 3; ++axis)
                if (!(*found)[axis].is_number()) return false;
            for (std::size_t axis = 0; axis < 3; ++axis)
                output[axis] = (*found)[axis].get<float>();
            return true;
        }

        bool FiniteVector3(const std::array<float, 3>& value)
        {
            return std::isfinite(value[0]) && std::isfinite(value[1]) &&
                   std::isfinite(value[2]);
        }
    }

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

        std::set<std::string> modifier_bones;
        for (const AnimatorBoneModifier& modifier : controller.bone_modifiers)
        {
            if (modifier.bone_name.empty())
            {
                error = "bone modifier requires a bone name";
                return false;
            }
            if (!modifier_bones.insert(modifier.bone_name).second)
            {
                error = "duplicate bone modifier: " + modifier.bone_name;
                return false;
            }
            if (!std::isfinite(modifier.strength) || !std::isfinite(modifier.damping) ||
                !std::isfinite(modifier.stiffness) || !std::isfinite(modifier.mass) ||
                !std::isfinite(modifier.drag) || !std::isfinite(modifier.gravity_scale) ||
                !std::isfinite(modifier.angle_limit_deg) || !std::isfinite(modifier.radius) ||
                modifier.mass <= 0.0f || !FiniteVector3(modifier.gravity_dir) ||
                !FiniteVector3(modifier.box_half_extents) || !FiniteVector3(modifier.box_center) ||
                modifier.box_half_extents[0] < 0.0f || modifier.box_half_extents[1] < 0.0f ||
                modifier.box_half_extents[2] < 0.0f)
            {
                error = "bone modifier '" + modifier.bone_name + "' has invalid values";
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
        {
            if (!value.is_object()) continue;
            AnimatorBoneModifier modifier;
            std::string type = value.value("type", std::string("physics"));
            std::transform(type.begin(), type.end(), type.begin(),
                [](unsigned char character) { return (char)std::tolower(character); });
            modifier.type = type == "collision" ? AnimatorBoneModifierType::Collision
                                                  : AnimatorBoneModifierType::Physics;
            modifier.bone_name = value.value("bone", value.value("bone_name", std::string()));
            modifier.strength = value.value("strength", 1.0f);
            modifier.damping = value.value("damping", 1.0f);
            modifier.stiffness = value.value("stiffness", 0.3f);
            modifier.mass = value.value("mass", 1.0f);
            modifier.drag = value.value("drag", 0.05f);
            modifier.gravity_scale = value.value("gravity_scale", 0.0f);
            ReadVector3(value, "gravity_dir", modifier.gravity_dir);
            modifier.angle_limit_deg = value.value("angle_limit_deg", 60.0f);
            modifier.radius = value.value("radius", 0.05f);
            modifier.affects_children = value.value("affects_children", true);
            ReadVector3(value, "box_half_extents", modifier.box_half_extents);
            ReadVector3(value, "box_center", modifier.box_center);
            loaded.bone_modifiers.push_back(modifier);
        }

        // Absent in controllers authored before facial animation existed.
        const json::const_iterator face = root.find("face");
        if (face != root.end() && face->is_object())
        {
            loaded.face.default_pose = face->value("default_pose", std::string());
            for (const json& value : face->value("poses", json::array()))
            {
                if (!value.is_object()) continue;
                FaceExpressionPose pose;
                pose.name = value.value("name", std::string());
                if (pose.name.empty()) continue;
                const json::const_iterator weights = value.find("weights");
                if (weights != value.end() && weights->is_object())
                    for (json::const_iterator entry = weights->begin(); entry != weights->end(); ++entry)
                        if (entry->is_number())
                            pose.weights.push_back({entry.key(), entry->get<float>()});
                loaded.face.poses.push_back(std::move(pose));
            }
        }

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
        root["bone_modifiers"] = json::array();
        for (const AnimatorBoneModifier& modifier : controller.bone_modifiers)
        {
            root["bone_modifiers"].push_back({
                {"type", modifier.type == AnimatorBoneModifierType::Collision ? "collision" : "physics"},
                {"bone", modifier.bone_name},
                {"strength", modifier.strength}, {"damping", modifier.damping},
                {"stiffness", modifier.stiffness}, {"mass", modifier.mass},
                {"drag", modifier.drag}, {"gravity_scale", modifier.gravity_scale},
                {"gravity_dir", modifier.gravity_dir},
                {"angle_limit_deg", modifier.angle_limit_deg}, {"radius", modifier.radius},
                {"affects_children", modifier.affects_children},
                {"box_half_extents", modifier.box_half_extents}, {"box_center", modifier.box_center}
            });
        }

        // Controllers that never opened the Face tab keep their existing shape.
        if (!controller.face.IsEmpty() || !controller.face.default_pose.empty())
        {
            json face;
            face["default_pose"] = controller.face.default_pose;
            face["poses"] = json::array();
            for (const FaceExpressionPose& pose : controller.face.poses)
            {
                json weights = json::object();
                for (const auto& [target, weight] : pose.weights)
                    weights[target] = weight;
                face["poses"].push_back({{"name", pose.name}, {"weights", std::move(weights)}});
            }
            root["face"] = std::move(face);
        }

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