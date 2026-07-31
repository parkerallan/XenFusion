#pragma once

#include <array>
#include <cstddef>
#include <string>

enum class AnimatorBoneModifierType
{
    Physics = 0,
    Collision = 1,
};

struct AnimatorBoneModifier
{
    AnimatorBoneModifierType type = AnimatorBoneModifierType::Physics;
    std::string bone_name;
    float strength = 1.0f;
    float damping = 1.0f;
    float stiffness = 0.3f;
    float mass = 1.0f;
    float drag = 0.05f;
    float gravity_scale = 0.0f;
    std::array<float, 3> gravity_dir = {0.0f, -1.0f, 0.0f};
    float angle_limit_deg = 60.0f;
    float radius = 0.05f;
    bool affects_children = true;
    std::array<float, 3> box_half_extents = {0.05f, 0.05f, 0.05f};
    std::array<float, 3> box_center = {0.0f, 0.0f, 0.0f};
};

struct BonePhysicsPreset
{
    const char* name;
    float strength;
    float stiffness;
    float damping;
    float mass;
    float drag;
    float gravity_scale;
    std::array<float, 3> gravity_dir;
    float angle_limit_deg;
    bool affects_children;
};

namespace bone_modifiers
{
    const BonePhysicsPreset* PhysicsPresets(std::size_t& count);
    void ApplyPhysicsPreset(AnimatorBoneModifier& modifier, std::size_t index);
}