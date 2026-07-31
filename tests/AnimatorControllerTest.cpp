#include "anim/AnimatorController.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    AnimatorController controller;
    controller.name = "TestAnimator";
    controller.default_state = "Idle";
    controller.clips.push_back({"idle", "assets/models/test.fbx", "Idle"});
    controller.states.push_back({"Idle", "idle", 1.0f, true});
    AnimatorBoneModifier physics;
    physics.bone_name = "Hair";
    physics.stiffness = 0.85f;
    physics.damping = 0.45f;
    physics.mass = 0.4f;
    physics.gravity_dir = {0.0f, -1.0f, 0.25f};
    controller.bone_modifiers.push_back(physics);
    AnimatorBoneModifier collision;
    collision.type = AnimatorBoneModifierType::Collision;
    collision.bone_name = "Hand";
    collision.box_half_extents = {0.1f, 0.2f, 0.3f};
    collision.box_center = {0.0f, 0.2f, 0.0f};
    controller.bone_modifiers.push_back(collision);

    std::string error;
    const fs::path round_trip = fs::temp_directory_path() / "xenfusion_animator_test.anim";
    if (!animator::SaveController(round_trip, controller, error))
        return 1;
    AnimatorController reloaded;
    if (!animator::LoadController(round_trip, reloaded, error))
        return 1;
    if (reloaded.bone_modifiers.size() != 2 ||
        reloaded.bone_modifiers[0].bone_name != "Hair" ||
        reloaded.bone_modifiers[0].stiffness != 0.85f ||
        reloaded.bone_modifiers[1].type != AnimatorBoneModifierType::Collision ||
        reloaded.bone_modifiers[1].box_half_extents[2] != 0.3f)
        return 1;

    const fs::path legacy = fs::temp_directory_path() / "xenfusion_animator_legacy_test.anim";
    std::ofstream output(legacy);
    output << R"({"name":"Legacy","clips":[],"states":[],"transitions":[],"bone_modifiers":[{"type":"Collision","bone_name":"LegacyBone","collision_mode":"Rigidbody"}]})";
    output.close();
    AnimatorController migrated;
    if (!animator::LoadController(legacy, migrated, error) ||
        migrated.bone_modifiers.size() != 1 ||
        migrated.bone_modifiers[0].bone_name != "LegacyBone" ||
        migrated.bone_modifiers[0].type != AnimatorBoneModifierType::Collision)
        return 1;

    std::error_code remove_error;
    fs::remove(round_trip, remove_error);
    fs::remove(legacy, remove_error);
    return 0;
}