#include "render/BoneModifiers.h"

#include <algorithm>
#include <cmath>

namespace
{
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
        if (std::fabs(determinant) <= 1e-12f) return false;
        const float inverse_determinant = 1.0f / determinant;
        const float translated[3] = {
            point[0] - matrix[12], point[1] - matrix[13], point[2] - matrix[14]};
        output[0] = (translated[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9]) +
                     translated[1] * (matrix[2] * matrix[9] - matrix[1] * matrix[10]) +
                     translated[2] * (matrix[1] * matrix[6] - matrix[2] * matrix[5])) *
            inverse_determinant;
        output[1] = (translated[0] * (matrix[6] * matrix[8] - matrix[4] * matrix[10]) +
                     translated[1] * (matrix[0] * matrix[10] - matrix[2] * matrix[8]) +
                     translated[2] * (matrix[2] * matrix[4] - matrix[0] * matrix[6])) *
            inverse_determinant;
        output[2] = (translated[0] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]) +
                     translated[1] * (matrix[1] * matrix[8] - matrix[0] * matrix[9]) +
                     translated[2] * (matrix[0] * matrix[5] - matrix[1] * matrix[4])) *
            inverse_determinant;
        return true;
    }

    bool RotateBoneToward(const AnimatorBoneModifier& modifier,
                          const float parent[3], const float target[3],
                          const float desired[3], aiMatrix4x4& bone_global)
    {
        aiVector3D animated(target[0] - parent[0], target[1] - parent[1], target[2] - parent[2]);
        aiVector3D simulated(desired[0] - parent[0], desired[1] - parent[1], desired[2] - parent[2]);
        const float animated_length = animated.Length();
        const float simulated_length = simulated.Length();
        if (animated_length <= 1e-5f || simulated_length <= 1e-5f) return false;

        animated /= animated_length;
        simulated /= simulated_length;
        const float direction_dot = animated.x * simulated.x +
            animated.y * simulated.y + animated.z * simulated.z;
        float angle = std::acos((std::clamp)(direction_dot, -1.0f, 1.0f));
        angle = (std::min)(angle, (std::clamp)(modifier.angle_limit_deg, 0.0f, 180.0f) *
            3.14159265f / 180.0f);
        aiVector3D axis(animated.y * simulated.z - animated.z * simulated.y,
                       animated.z * simulated.x - animated.x * simulated.z,
                       animated.x * simulated.y - animated.y * simulated.x);
        if (axis.SquareLength() <= 1e-10f || angle <= 1e-5f) return false;

        axis.Normalize();
        aiMatrix4x4 rotation;
        aiMatrix4x4::Rotation(angle, axis, rotation);
        aiMatrix4x4 negative, positive;
        aiMatrix4x4::Translation(aiVector3D(-parent[0], -parent[1], -parent[2]), negative);
        aiMatrix4x4::Translation(aiVector3D(parent[0], parent[1], parent[2]), positive);
        bone_global = positive * rotation * negative * bone_global;
        return true;
    }
}

bool bone_modifiers::ApplyPreviewPhysics(const AnimatorBoneModifier& modifier,
                                         float delta_time,
                                         const aiMatrix4x4& parent_global,
                                         BoneModifierPhysicsState& simulation,
                                         aiMatrix4x4& bone_global)
{
    if (modifier.type != AnimatorBoneModifierType::Physics) return false;
    const float parent[3] = {parent_global.a4, parent_global.b4, parent_global.c4};
    const float target[3] = {bone_global.a4, bone_global.b4, bone_global.c4};
    if (!simulation.initialized)
    {
        for (int axis = 0; axis < 3; ++axis) simulation.position[axis] = target[axis];
        simulation.initialized = true;
    }

    int steps = delta_time > 0.0f ? (int)std::ceil(delta_time * 60.0f) : 0;
    steps = (std::min)(steps, 4);
    const float step_dt = steps > 0 ? delta_time / (float)steps : 0.0f;
    const float stiffness = 180.0f * (std::clamp)(modifier.stiffness, 0.0f, 1.0f);
    const float mass = (std::max)(0.001f, modifier.mass);
    const float damping = 2.0f * (std::max)(0.0f, modifier.damping) *
        std::sqrt(stiffness / mass);
    const float drag = (std::clamp)(modifier.drag, 0.0f, 1.0f);
    for (int step = 0; step < steps; ++step)
        for (int axis = 0; axis < 3; ++axis)
        {
            const float acceleration = (target[axis] - simulation.position[axis]) *
                stiffness / mass + modifier.gravity_dir[axis] * modifier.gravity_scale * 9.81f;
            simulation.velocity[axis] *= (std::clamp)(1.0f - damping * step_dt, 0.0f, 1.0f);
            simulation.velocity[axis] += acceleration * step_dt;
            simulation.velocity[axis] *= 1.0f - drag * step_dt;
            simulation.position[axis] += simulation.velocity[axis] * step_dt;
        }

    const float desired[3] = {
        target[0] + (simulation.position[0] - target[0]) * modifier.strength,
        target[1] + (simulation.position[1] - target[1]) * modifier.strength,
        target[2] + (simulation.position[2] - target[2]) * modifier.strength};
    return RotateBoneToward(modifier, parent, target, desired, bone_global);
}

bool bone_modifiers::ApplyScenePhysics(const AnimatorBoneModifier& modifier,
                                       float delta_time,
                                       const aiMatrix4x4& parent_global,
                                       const float object_world[16],
                                       BoneModifierPhysicsState& simulation,
                                       aiMatrix4x4& bone_global)
{
    if (modifier.type != AnimatorBoneModifierType::Physics) return false;
    const float parent[3] = {parent_global.a4, parent_global.b4, parent_global.c4};
    const float target[3] = {bone_global.a4, bone_global.b4, bone_global.c4};
    float target_world[3];
    TransformPoint(target, object_world, target_world);
    if (!simulation.initialized)
    {
        for (int axis = 0; axis < 3; ++axis) simulation.position[axis] = target_world[axis];
        simulation.initialized = true;
    }

    int steps = delta_time > 0.0f ? (int)std::ceil(delta_time * 60.0f) : 0;
    steps = (std::min)(steps, 4);
    const float step_dt = steps > 0 ? delta_time / (float)steps : 0.0f;
    const float stiffness = 180.0f * (std::clamp)(modifier.stiffness, 0.0f, 1.0f);
    const float mass = (std::max)(0.001f, modifier.mass);
    const float damping = 2.0f * (std::max)(0.0f, modifier.damping) *
        std::sqrt(stiffness / mass);
    const float drag = (std::clamp)(modifier.drag, 0.0f, 1.0f);
    for (int step = 0; step < steps; ++step)
        for (int axis = 0; axis < 3; ++axis)
        {
            const float acceleration = (target_world[axis] - simulation.position[axis]) *
                stiffness / mass + modifier.gravity_dir[axis] * modifier.gravity_scale * 9.81f;
            simulation.velocity[axis] *= (std::clamp)(1.0f - damping * step_dt, 0.0f, 1.0f);
            simulation.velocity[axis] += acceleration * step_dt;
            simulation.velocity[axis] *= 1.0f - drag * step_dt;
            simulation.position[axis] += simulation.velocity[axis] * step_dt;
        }

    const float desired_world[3] = {
        target_world[0] + (simulation.position[0] - target_world[0]) * modifier.strength,
        target_world[1] + (simulation.position[1] - target_world[1]) * modifier.strength,
        target_world[2] + (simulation.position[2] - target_world[2]) * modifier.strength};
    float desired_model[3];
    if (!InverseTransformPoint(desired_world, object_world, desired_model)) return false;
    return RotateBoneToward(modifier, parent, target, desired_model, bone_global);
}

