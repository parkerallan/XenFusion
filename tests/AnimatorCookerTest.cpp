#include "anim/AnimationCodec.h"
#include "anim/AnimatorController.h"
#include "anim/AnimatorCooker.h"
#include "anim/FaceShapes.h"
#include "AnimatorCookFormat.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace
{
    unsigned int ReadU32(const unsigned char* data)
    {
        return ((unsigned int)data[0] << 24) | ((unsigned int)data[1] << 16) |
               ((unsigned int)data[2] << 8) | data[3];
    }
}

int main(int argc, char** argv)
{
    if (argc != 3) return 1;
    AnimatorController controller;
    std::string error;
    if (!animator::LoadController(argv[2], controller, error)) return 2;
    if (controller.states.empty())
    {
        for (const AnimatorClipReference& clip : controller.clips)
            controller.states.push_back({clip.id, clip.id, 1.0f, true});
        if (!controller.states.empty()) controller.default_state = controller.states.front().name;
    }
    AnimatorBoneModifier collision;
    collision.type = AnimatorBoneModifierType::Collision;
    collision.bone_name = "CollisionTestBone";
    controller.bone_modifiers.push_back(collision);

    FaceExpressionPose angry;
    angry.name = "Angry";
    angry.weights.push_back({"browDownLeft", 1.0f});
    angry.weights.push_back({"browDownRight", 1.0f});
    angry.weights.push_back({"notAnArkitShape", 1.0f}); // dropped at cook
    controller.face.poses.push_back(angry);
    controller.face.default_pose = "Angry";
    std::vector<unsigned char> payload;
    if (!animator::CookControllerBE(argv[1], controller, payload, error)) return 3;
    if (payload.size() < animcook::kHeaderBytes || ReadU32(payload.data()) != animcook::kMagic ||
        ReadU32(payload.data() + 4) != animcook::kVersion ||
        ReadU32(payload.data() + 20) != 3 ||
        ReadU32(payload.data() + 40) != controller.bone_modifiers.size()) return 4;
    const unsigned int modifier_table = ReadU32(payload.data() + 44);
    const unsigned int string_table = ReadU32(payload.data() + 48);
    const unsigned int string_bytes = ReadU32(payload.data() + 52);
    if (modifier_table + controller.bone_modifiers.size() * animcook::kModifierBytes > payload.size())
        return 4;
    if (string_table + string_bytes > payload.size()) return 4;
    for (unsigned int modifier = 0; modifier < controller.bone_modifiers.size(); ++modifier)
    {
        const unsigned char* record = payload.data() + modifier_table + modifier * animcook::kModifierBytes;
        if (ReadU32(record) != animation::NameHash(controller.bone_modifiers[modifier].bone_name.c_str()) ||
            ReadU32(record + 4) > animcook::ModifierCollision || ReadU32(record + 12) != 0) return 4;
        const unsigned int name_offset = ReadU32(record + 84);
        const unsigned int name_length = ReadU32(record + 88);
        if (name_offset + name_length > string_bytes ||
            std::string((const char*)payload.data() + string_table + name_offset, name_length) !=
                controller.bone_modifiers[modifier].bone_name) return 4;
    }
    const unsigned int clip_table = ReadU32(payload.data() + 32);
    for (unsigned int clip = 0; clip < 3; ++clip)
    {
        const unsigned char* record = payload.data() + clip_table + clip * animcook::kClipBytes;
        const unsigned int offset = ReadU32(record + 8);
        const unsigned int size = ReadU32(record + 12);
        AnimationClip decoded;
        if ((size_t)offset + size > payload.size() ||
            !animation::DecodeClipBE(payload.data() + offset, size, decoded, error)) return 5;
    }
    // Face block. Poses resolve to ARKit indices at cook, so the console never
    // compares a name.
    const unsigned int pose_count       = ReadU32(payload.data() + 60);
    const unsigned int pose_table       = ReadU32(payload.data() + 64);
    const unsigned int pose_target_table = ReadU32(payload.data() + 68);
    if (pose_count != controller.face.poses.size()) return 6;
    if (ReadU32(payload.data() + 56) !=
        animation::NameHash(controller.face.default_pose.c_str())) return 6;
    for (unsigned int pose = 0; pose < pose_count; ++pose)
    {
        const unsigned char* record = payload.data() + pose_table + pose * animcook::kPoseBytes;
        if (ReadU32(record) != animation::NameHash(controller.face.poses[pose].name.c_str())) return 6;
        const unsigned int first = ReadU32(record + 4);
        const unsigned int count = ReadU32(record + 8);
        if (pose_target_table + (first + count) * animcook::kPoseTargetBytes > payload.size()) return 6;
        for (unsigned int t = 0; t < count; ++t)
        {
            const unsigned char* target =
                payload.data() + pose_target_table + (first + t) * animcook::kPoseTargetBytes;
            if (target[0] >= face::kShapeCount) return 6;
        }
    }
    std::cout << "ANC3 controller: " << payload.size() << " bytes, 3 clips, "
              << pose_count << " face poses\n";
    return 0;
}