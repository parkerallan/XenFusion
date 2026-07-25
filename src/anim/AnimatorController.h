#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct AnimatorClipReference
{
    std::string id;
    std::string source_model_path;
    std::string clip_name;
};

struct AnimatorStateDefinition
{
    std::string name;
    std::string clip_id;
    float playback_speed = 1.0f;
    bool loop = true;
};

struct AnimatorTransitionDefinition
{
    std::string from_state;
    std::string to_state;
    std::string condition;
    float blend_duration = 0.15f;
    bool has_exit_time = false;
    float exit_time = 1.0f;
};

struct AnimatorController
{
    int version = 1;
    std::string name = "NewAnimator";
    std::string default_state;
    std::string preview_model_path;
    std::vector<AnimatorClipReference> clips;
    std::vector<AnimatorStateDefinition> states;
    std::vector<AnimatorTransitionDefinition> transitions;
    // Bone modifiers are deliberately out of runtime scope, but retaining the
    // authored JSON prevents this editor from erasing future Vulkan data.
    std::vector<nlohmann::json> bone_modifiers;
};

namespace animator
{
    bool LoadController(const std::filesystem::path& path, AnimatorController& output,
                        std::string& error);
    bool SaveController(const std::filesystem::path& path, const AnimatorController& controller,
                        std::string& error);
    bool ValidateController(const AnimatorController& controller, std::string& error);
}