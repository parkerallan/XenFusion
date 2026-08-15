#pragma once

#include "anim/BoneModifiers.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

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

// Sparse: only the shapes the pose drives are listed.
struct FaceExpressionPose
{
    std::string name;
    std::vector<std::pair<std::string, float>> weights; // ARKit target -> weight
};

// Evaluated as its own layer, independent of the skeletal state machine.
struct AnimatorFaceConfig
{
    std::string default_pose;                  // applied when a script sets none
    std::vector<FaceExpressionPose> poses;
    bool IsEmpty() const { return poses.empty(); }
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
    std::vector<AnimatorBoneModifier> bone_modifiers;
    AnimatorFaceConfig face;
};

namespace animator
{
    bool LoadController(const std::filesystem::path& path, AnimatorController& output,
                        std::string& error);
    bool SaveController(const std::filesystem::path& path, const AnimatorController& controller,
                        std::string& error);
    bool ValidateController(const AnimatorController& controller, std::string& error);
}