#include "RuntimeBoneModifiers.h"

#include "AnimatorCookFormat.h"

#include <math.h>
#include <string.h>

namespace
{
    float ClampModifierValue(float value, float minimum, float maximum)
    {
        return value < minimum ? minimum : (value > maximum ? maximum : value);
    }

    void TransformPoint(const float point[3], const float matrix[16], float output[3])
    {
        output[0] = point[0] * matrix[0] + point[1] * matrix[4] +
            point[2] * matrix[8] + matrix[12];
        output[1] = point[0] * matrix[1] + point[1] * matrix[5] +
            point[2] * matrix[9] + matrix[13];
        output[2] = point[0] * matrix[2] + point[1] * matrix[6] +
            point[2] * matrix[10] + matrix[14];
    }

    bool InverseTransformPoint(const float point[3], const float matrix[16], float output[3])
    {
        const float determinant =
            matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9]) -
            matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8]) +
            matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
        if (fabsf(determinant) <= 1e-12f) return false;
        const float inverseDeterminant = 1.0f / determinant;
        const float translated[3] = {
            point[0] - matrix[12], point[1] - matrix[13], point[2] - matrix[14]};
        output[0] = (translated[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9]) +
                     translated[1] * (matrix[2] * matrix[9] - matrix[1] * matrix[10]) +
                     translated[2] * (matrix[1] * matrix[6] - matrix[2] * matrix[5])) *
            inverseDeterminant;
        output[1] = (translated[0] * (matrix[6] * matrix[8] - matrix[4] * matrix[10]) +
                     translated[1] * (matrix[0] * matrix[10] - matrix[2] * matrix[8]) +
                     translated[2] * (matrix[2] * matrix[4] - matrix[0] * matrix[6])) *
            inverseDeterminant;
        output[2] = (translated[0] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]) +
                     translated[1] * (matrix[1] * matrix[8] - matrix[0] * matrix[9]) +
                     translated[2] * (matrix[0] * matrix[5] - matrix[1] * matrix[4])) *
            inverseDeterminant;
        return true;
    }
}

void runtime_bone_modifiers::MapPhysicsModifiers(
    const std::vector<RuntimeBoneModifier>& modifiers, const RtMesh& mesh,
    std::vector<int>& jointModifiers)
{
    jointModifiers.assign(mesh.joints.size(), -1);
    for (size_t jointIndex = 0; jointIndex < mesh.joints.size(); ++jointIndex)
        for (size_t modifierIndex = 0; modifierIndex < modifiers.size(); ++modifierIndex)
            if (modifiers[modifierIndex].type == animcook::ModifierPhysics &&
                modifiers[modifierIndex].boneHash == mesh.joints[jointIndex].nameHash)
            {
                jointModifiers[jointIndex] = (int)modifierIndex;
                break;
            }
}

void runtime_bone_modifiers::ApplyPhysics(const RuntimeBoneModifier& modifier,
                                          float deltaTime,
                                          const float parentGlobal[16],
                                          const float objectWorld[16],
                                          RuntimeBonePhysicsState& simulation,
                                          float boneGlobal[16])
{
    const float parentPosition[3] = {parentGlobal[3], parentGlobal[7], parentGlobal[11]};
    const float target[3] = {boneGlobal[3], boneGlobal[7], boneGlobal[11]};
    float targetWorld[3];
    TransformPoint(target, objectWorld, targetWorld);
    if (!simulation.initialized)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            simulation.position[axis] = targetWorld[axis];
            simulation.velocity[axis] = 0.0f;
        }
        simulation.initialized = true;
    }

    int steps = deltaTime > 0.0f ? (int)ceilf(deltaTime * 60.0f) : 0;
    if (steps > 4) steps = 4;
    const float stepTime = steps > 0 ? deltaTime / (float)steps : 0.0f;
    const float stiffness = 180.0f * ClampModifierValue(modifier.stiffness, 0.0f, 1.0f);
    const float mass = modifier.mass > 0.001f ? modifier.mass : 0.001f;
    const float damping = 2.0f * (modifier.damping > 0.0f ? modifier.damping : 0.0f) *
        sqrtf(stiffness / mass);
    const float drag = ClampModifierValue(modifier.drag, 0.0f, 1.0f);
    for (int step = 0; step < steps; ++step)
        for (int axis = 0; axis < 3; ++axis)
        {
            const float acceleration = (targetWorld[axis] - simulation.position[axis]) *
                stiffness / mass + modifier.gravityDir[axis] * modifier.gravityScale * 9.81f;
            simulation.velocity[axis] *= ClampModifierValue(1.0f - damping * stepTime, 0.0f, 1.0f);
            simulation.velocity[axis] += acceleration * stepTime;
            simulation.velocity[axis] *= 1.0f - drag * stepTime;
            simulation.position[axis] += simulation.velocity[axis] * stepTime;
        }

    const float strength = ClampModifierValue(modifier.strength, 0.0f, 2.0f);
    const float desiredWorld[3] = {
        targetWorld[0] + (simulation.position[0] - targetWorld[0]) * strength,
        targetWorld[1] + (simulation.position[1] - targetWorld[1]) * strength,
        targetWorld[2] + (simulation.position[2] - targetWorld[2]) * strength};
    float desired[3];
    if (!InverseTransformPoint(desiredWorld, objectWorld, desired)) return;

    float animatedDirection[3], simulatedDirection[3];
    float animatedLengthSquared = 0.0f, simulatedLengthSquared = 0.0f;
    for (int axis = 0; axis < 3; ++axis)
    {
        animatedDirection[axis] = target[axis] - parentPosition[axis];
        simulatedDirection[axis] = desired[axis] - parentPosition[axis];
        animatedLengthSquared += animatedDirection[axis] * animatedDirection[axis];
        simulatedLengthSquared += simulatedDirection[axis] * simulatedDirection[axis];
    }
    if (animatedLengthSquared <= 1e-10f || simulatedLengthSquared <= 1e-10f) return;

    const float animatedLength = sqrtf(animatedLengthSquared);
    const float simulatedLength = sqrtf(simulatedLengthSquared);
    for (int axis = 0; axis < 3; ++axis)
    {
        animatedDirection[axis] /= animatedLength;
        simulatedDirection[axis] /= simulatedLength;
    }
    float cosine = animatedDirection[0] * simulatedDirection[0] +
        animatedDirection[1] * simulatedDirection[1] +
        animatedDirection[2] * simulatedDirection[2];
    cosine = ClampModifierValue(cosine, -1.0f, 1.0f);
    float angle = acosf(cosine);
    const float limit = ClampModifierValue(modifier.angleLimitDeg, 0.0f, 180.0f) *
        3.14159265f / 180.0f;
    if (angle > limit) angle = limit;
    float axis[3] = {
        animatedDirection[1] * simulatedDirection[2] - animatedDirection[2] * simulatedDirection[1],
        animatedDirection[2] * simulatedDirection[0] - animatedDirection[0] * simulatedDirection[2],
        animatedDirection[0] * simulatedDirection[1] - animatedDirection[1] * simulatedDirection[0]};
    const float axisLength = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (axisLength <= 1e-6f || angle <= 1e-6f) return;
    for (int component = 0; component < 3; ++component) axis[component] /= axisLength;

    const float cosineAngle = cosf(angle), sineAngle = sinf(angle), oneMinus = 1.0f - cosineAngle;
    float rotation[9];
    rotation[0] = oneMinus*axis[0]*axis[0]+cosineAngle;
    rotation[1] = oneMinus*axis[0]*axis[1]-sineAngle*axis[2];
    rotation[2] = oneMinus*axis[0]*axis[2]+sineAngle*axis[1];
    rotation[3] = oneMinus*axis[0]*axis[1]+sineAngle*axis[2];
    rotation[4] = oneMinus*axis[1]*axis[1]+cosineAngle;
    rotation[5] = oneMinus*axis[1]*axis[2]-sineAngle*axis[0];
    rotation[6] = oneMinus*axis[0]*axis[2]-sineAngle*axis[1];
    rotation[7] = oneMinus*axis[1]*axis[2]+sineAngle*axis[0];
    rotation[8] = oneMinus*axis[2]*axis[2]+cosineAngle;
    float original[16];
    memcpy(original, boneGlobal, sizeof(original));
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            boneGlobal[row * 4 + column] = rotation[row * 3] * original[column] +
                rotation[row * 3 + 1] * original[4 + column] +
                rotation[row * 3 + 2] * original[8 + column];
    for (int row = 0; row < 3; ++row)
        boneGlobal[row * 4 + 3] = parentPosition[row] +
            (rotation[row * 3] * animatedDirection[0] +
             rotation[row * 3 + 1] * animatedDirection[1] +
             rotation[row * 3 + 2] * animatedDirection[2]) * animatedLength;
}

