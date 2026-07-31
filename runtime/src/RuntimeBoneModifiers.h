#pragma once

#include "Content.h"

#include <string>
#include <vector>

struct RuntimeBoneModifier
{
    unsigned int boneHash, type, flags;
    float strength, damping, stiffness, mass, drag, gravityScale;
    float gravityDir[3], angleLimitDeg, radius;
    float boxHalfExtents[3], boxCenter[3];
    std::string boneName;
};

struct RuntimeBonePhysicsState
{
    bool initialized;
    float position[3], velocity[3];
    RuntimeBonePhysicsState() : initialized(false)
    {
        position[0] = position[1] = position[2] = 0.0f;
        velocity[0] = velocity[1] = velocity[2] = 0.0f;
    }
};

namespace runtime_bone_modifiers
{
    void MapPhysicsModifiers(const std::vector<RuntimeBoneModifier>& modifiers,
                             const RtMesh& mesh, std::vector<int>& jointModifiers);
    void ApplyPhysics(const RuntimeBoneModifier& modifier, float deltaTime,
                      const float parentGlobal[16],
                      const float objectWorld[16],
                      RuntimeBonePhysicsState& simulation,
                      float boneGlobal[16]);
}