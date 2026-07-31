#pragma once

#include "anim/BoneModifiers.h"

#include <assimp/matrix4x4.h>

struct BoneModifierPhysicsState
{
    bool initialized = false;
    float position[3] = {};
    float velocity[3] = {};
};

namespace bone_modifiers
{
    bool ApplyPreviewPhysics(const AnimatorBoneModifier& modifier, float delta_time,
                             const aiMatrix4x4& parent_global,
                             BoneModifierPhysicsState& simulation,
                             aiMatrix4x4& bone_global);
    bool ApplyScenePhysics(const AnimatorBoneModifier& modifier, float delta_time,
                           const aiMatrix4x4& parent_global, const float object_world[16],
                           BoneModifierPhysicsState& simulation,
                           aiMatrix4x4& bone_global);
}