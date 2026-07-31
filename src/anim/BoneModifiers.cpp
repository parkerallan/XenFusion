#include "anim/BoneModifiers.h"

namespace
{
    const BonePhysicsPreset kPhysicsPresets[] = {
        {"Custom", 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, {0.0f, -1.0f, 0.0f}, 0.0f, false},
        {"Hair (short)", 1.0f, 0.85f, 0.45f, 0.4f, 0.04f, 0.0f, {0.0f, -1.0f, 0.0f}, 50.0f, true},
        {"Hair (long)", 1.0f, 0.55f, 0.55f, 0.9f, 0.05f, 0.05f, {0.0f, -1.0f, 0.0f}, 60.0f, true},
        {"Breast", 1.0f, 0.95f, 0.45f, 1.0f, 0.04f, 0.0f, {0.0f, -1.0f, 0.0f}, 30.0f, true},
        {"Cloth (light)", 1.0f, 0.35f, 0.85f, 0.4f, 0.10f, 0.15f, {0.0f, -1.0f, 0.0f}, 90.0f, true},
        {"Cloth (heavy)", 1.0f, 0.25f, 1.10f, 1.6f, 0.15f, 0.25f, {0.0f, -1.0f, 0.0f}, 100.0f, true},
        {"Tail / Antenna", 1.0f, 0.70f, 0.50f, 0.7f, 0.05f, 0.0f, {0.0f, -1.0f, 0.0f}, 70.0f, true},
    };
}

const BonePhysicsPreset* bone_modifiers::PhysicsPresets(std::size_t& count)
{
    count = sizeof(kPhysicsPresets) / sizeof(kPhysicsPresets[0]);
    return kPhysicsPresets;
}

void bone_modifiers::ApplyPhysicsPreset(AnimatorBoneModifier& modifier, std::size_t index)
{
    std::size_t count = 0;
    const BonePhysicsPreset* presets = PhysicsPresets(count);
    if (index == 0 || index >= count) return;
    const BonePhysicsPreset& preset = presets[index];
    modifier.strength = preset.strength;
    modifier.stiffness = preset.stiffness;
    modifier.damping = preset.damping;
    modifier.mass = preset.mass;
    modifier.drag = preset.drag;
    modifier.gravity_scale = preset.gravity_scale;
    modifier.gravity_dir = preset.gravity_dir;
    modifier.angle_limit_deg = preset.angle_limit_deg;
    modifier.affects_children = preset.affects_children;
}