#pragma once

#include "light/LightBasis.h"

#include <stddef.h>
#include <vector>

namespace lprobe
{
    struct Probe
    {
        float sh[12];
    };

    struct Grid
    {
        float origin[3];
        float spacing;
        unsigned int dimensions[3];
        std::vector<Probe> probes;

        Grid() : spacing(0.0f)
        {
            origin[0] = origin[1] = origin[2] = 0.0f;
            dimensions[0] = dimensions[1] = dimensions[2] = 0;
        }

        void Clear()
        {
            spacing = 0.0f;
            dimensions[0] = dimensions[1] = dimensions[2] = 0;
            probes.clear();
        }

        bool Valid() const
        {
            const size_t count = static_cast<size_t>(dimensions[0]) *
                                 dimensions[1] * dimensions[2];
            return spacing > 0.0f && count > 0 && probes.size() == count;
        }
    };

    inline float Clamp(float value, float minimum, float maximum)
    {
        return value < minimum ? minimum : (value > maximum ? maximum : value);
    }

    inline void Sample(const Grid& grid, const float position[3],
                       float color[4], float direction[4])
    {
        color[0] = color[1] = color[2] = 0.0f;
        color[3] = 0.0f;
        direction[0] = direction[1] = 0.5f;
        direction[2] = 1.0f;
        direction[3] = 0.0f;
        if (!grid.Valid()) return;

        unsigned int lower[3], upper[3];
        float fraction[3];
        for (int axis = 0; axis < 3; ++axis)
        {
            const float maximum = static_cast<float>(grid.dimensions[axis] - 1);
            const float coordinate = Clamp((position[axis] - grid.origin[axis]) / grid.spacing,
                                           0.0f, maximum);
            lower[axis] = static_cast<unsigned int>(coordinate);
            upper[axis] = lower[axis] + 1 < grid.dimensions[axis] ? lower[axis] + 1 : lower[axis];
            fraction[axis] = coordinate - static_cast<float>(lower[axis]);
        }

        float sh[12] = {0};
        for (unsigned int z = 0; z < 2; ++z)
            for (unsigned int y = 0; y < 2; ++y)
                for (unsigned int x = 0; x < 2; ++x)
                {
                    const unsigned int ix = x ? upper[0] : lower[0];
                    const unsigned int iy = y ? upper[1] : lower[1];
                    const unsigned int iz = z ? upper[2] : lower[2];
                    const float weight = (x ? fraction[0] : 1.0f - fraction[0]) *
                                         (y ? fraction[1] : 1.0f - fraction[1]) *
                                         (z ? fraction[2] : 1.0f - fraction[2]);
                    const Probe& probe = grid.probes[(static_cast<size_t>(iz) * grid.dimensions[1] + iy) *
                                                     grid.dimensions[0] + ix];
                    for (int coefficient = 0; coefficient < 12; ++coefficient)
                        sh[coefficient] += probe.sh[coefficient] * weight;
                }

        color[0] = sh[0]; color[1] = sh[1]; color[2] = sh[2]; color[3] = 1.0f;
        const float moment[3] = {
            sh[3] * 0.2126f + sh[4] * 0.7152f + sh[5] * 0.0722f,
            sh[6] * 0.2126f + sh[7] * 0.7152f + sh[8] * 0.0722f,
            sh[9] * 0.2126f + sh[10] * 0.7152f + sh[11] * 0.0722f
        };
        const float length = sqrtf(moment[0] * moment[0] + moment[1] * moment[1] + moment[2] * moment[2]);
        if (length > 1.0e-6f)
        {
            direction[0] = moment[0] / length * 0.5f + 0.5f;
            direction[1] = moment[1] / length * 0.5f + 0.5f;
            direction[2] = moment[2] / length * 0.5f + 0.5f;
        }
        const float luminance = color[0] * 0.2126f + color[1] * 0.7152f + color[2] * 0.0722f;
        direction[3] = luminance > 1.0e-6f ? Clamp(length / luminance, 0.0f, 1.0f) : 0.0f;
    }
}