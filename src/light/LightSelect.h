#pragma once

#include <math.h>
#include <stddef.h>

namespace lsel
{
    enum LightMode
    {
        Baked = 0,
        Realtime = 1,
        Mixed = 2
    };

    struct Aabb
    {
        float min[3];
        float max[3];
    };

    struct PointLight
    {
        float position[3];
        float color[3];
        float invRangeSq;
    };

    struct SpotLight
    {
        float position[3];
        float direction[3];
        float color[3];
        float invRange;
        float innerCos;
        float outerCos;
    };

    inline bool IsRealtime(int mode)
    {
        return mode == Realtime || mode == Mixed;
    }

    inline float Saturate(float value)
    {
        return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    }

    inline float Luminance(const float color[3])
    {
        return color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
    }

    inline void ClosestPoint(const Aabb& bounds, const float position[3], float result[3])
    {
        for (int axis = 0; axis < 3; ++axis)
            result[axis] = position[axis] < bounds.min[axis] ? bounds.min[axis]
                         : position[axis] > bounds.max[axis] ? bounds.max[axis]
                         : position[axis];
    }

    inline float PointAtten(const PointLight& light, const float sample[3])
    {
        const float x = light.position[0] - sample[0];
        const float y = light.position[1] - sample[1];
        const float z = light.position[2] - sample[2];
        const float distanceSq = x * x + y * y + z * z;
        const float fade = Saturate(1.0f - distanceSq * light.invRangeSq);
        return fade * fade;
    }

    inline float SpotAtten(const SpotLight& light, const float sample[3])
    {
        const float x = light.position[0] - sample[0];
        const float y = light.position[1] - sample[1];
        const float z = light.position[2] - sample[2];
        const float distanceSq = x * x + y * y + z * z;
        const float distance = sqrtf(distanceSq > 1.0e-4f ? distanceSq : 1.0e-4f);
        const float distanceFade = Saturate(1.0f - distance * light.invRange);
        const float invDistance = 1.0f / distance;
        const float coneDot = -(light.direction[0] * x + light.direction[1] * y +
                                light.direction[2] * z) * invDistance;
        float coneFade = Saturate((coneDot - light.outerCos) /
                                  ((light.innerCos - light.outerCos) > 1.0e-4f
                                      ? (light.innerCos - light.outerCos) : 1.0e-4f));
        coneFade = coneFade * coneFade * (3.0f - 2.0f * coneFade);
        return distanceFade * distanceFade * coneFade;
    }

    inline void TransformAabb(const float localMin[3], const float localMax[3],
                              const float world[16], Aabb& result)
    {
        for (int corner = 0; corner < 8; ++corner)
        {
            const float x = (corner & 1) ? localMax[0] : localMin[0];
            const float y = (corner & 2) ? localMax[1] : localMin[1];
            const float z = (corner & 4) ? localMax[2] : localMin[2];
            const float transformed[3] = {
                x * world[0] + y * world[4] + z * world[8]  + world[12],
                x * world[1] + y * world[5] + z * world[9]  + world[13],
                x * world[2] + y * world[6] + z * world[10] + world[14]
            };
            for (int axis = 0; axis < 3; ++axis)
            {
                if (corner == 0 || transformed[axis] < result.min[axis]) result.min[axis] = transformed[axis];
                if (corner == 0 || transformed[axis] > result.max[axis]) result.max[axis] = transformed[axis];
            }
        }
    }

    inline void PackPoints(const PointLight* lights, size_t count, const Aabb& bounds,
                           float positions[4][4], float colors[4][4])
    {
        float scores[4] = { 0, 0, 0, 0 };
        const PointLight* selected[4] = { 0, 0, 0, 0 };
        for (size_t index = 0; index < count; ++index)
        {
            float sample[3];
            ClosestPoint(bounds, lights[index].position, sample);
            const float score = Luminance(lights[index].color) * PointAtten(lights[index], sample);
            if (score <= 0.0f)
                continue;
            for (int slot = 0; slot < 4; ++slot)
            {
                if (!selected[slot] || score > scores[slot])
                {
                    for (int move = 3; move > slot; --move)
                    {
                        selected[move] = selected[move - 1];
                        scores[move] = scores[move - 1];
                    }
                    selected[slot] = &lights[index];
                    scores[slot] = score;
                    break;
                }
            }
        }
        for (int slot = 0; slot < 4; ++slot)
        {
            for (int axis = 0; axis < 4; ++axis)
            {
                positions[slot][axis] = 0.0f;
                colors[slot][axis] = 0.0f;
            }
            if (selected[slot])
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    positions[slot][axis] = selected[slot]->position[axis];
                    colors[slot][axis] = selected[slot]->color[axis];
                }
                positions[slot][3] = selected[slot]->invRangeSq;
            }
        }
    }

    inline void PackSpots(const SpotLight* lights, size_t count, const Aabb& bounds,
                          float positions[2][4], float directions[2][4], float colors[2][4])
    {
        float scores[2] = { 0, 0 };
        const SpotLight* selected[2] = { 0, 0 };
        for (size_t index = 0; index < count; ++index)
        {
            float sample[3];
            ClosestPoint(bounds, lights[index].position, sample);
            const float score = Luminance(lights[index].color) * SpotAtten(lights[index], sample);
            if (score <= 0.0f)
                continue;
            for (int slot = 0; slot < 2; ++slot)
            {
                if (!selected[slot] || score > scores[slot])
                {
                    for (int move = 1; move > slot; --move)
                    {
                        selected[move] = selected[move - 1];
                        scores[move] = scores[move - 1];
                    }
                    selected[slot] = &lights[index];
                    scores[slot] = score;
                    break;
                }
            }
        }
        for (int slot = 0; slot < 2; ++slot)
        {
            for (int axis = 0; axis < 4; ++axis)
            {
                positions[slot][axis] = 0.0f;
                directions[slot][axis] = 0.0f;
                colors[slot][axis] = 0.0f;
            }
            directions[slot][2] = 1.0f;
            directions[slot][3] = 1.0f;
            if (selected[slot])
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    positions[slot][axis] = selected[slot]->position[axis];
                    directions[slot][axis] = selected[slot]->direction[axis];
                    colors[slot][axis] = selected[slot]->color[axis];
                }
                positions[slot][3] = selected[slot]->invRange;
                directions[slot][3] = selected[slot]->innerCos;
                colors[slot][3] = selected[slot]->outerCos;
            }
        }
    }
}