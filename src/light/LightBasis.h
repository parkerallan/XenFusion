#pragma once

#include <math.h>

namespace lbasis
{
    const float kRgbmRange = 16.0f;

    struct Sample
    {
        float color[3];
        float direction[3];
        float weight;
    };

    struct Accumulator
    {
        float color[3];
        float moment[3];
        float luminance;

        Accumulator() { Clear(); }

        void Clear()
        {
            color[0] = color[1] = color[2] = 0.0f;
            moment[0] = moment[1] = moment[2] = 0.0f;
            luminance = 0.0f;
        }

        void Add(const Sample& sample)
        {
            const float weighted[3] = {
                sample.color[0] * sample.weight,
                sample.color[1] * sample.weight,
                sample.color[2] * sample.weight
            };
            const float lum = weighted[0] * 0.2126f + weighted[1] * 0.7152f + weighted[2] * 0.0722f;
            for (int axis = 0; axis < 3; ++axis)
            {
                color[axis] += weighted[axis];
                moment[axis] += sample.direction[axis] * lum;
            }
            luminance += lum;
        }
    };

    inline void Encode(const Accumulator& source, float color[3], float direction[4])
    {
        for (int axis = 0; axis < 3; ++axis)
            color[axis] = source.color[axis];
        const float length = sqrtf(source.moment[0] * source.moment[0] +
                                   source.moment[1] * source.moment[1] +
                                   source.moment[2] * source.moment[2]);
        if (length > 1.0e-6f)
        {
            direction[0] = source.moment[0] / length;
            direction[1] = source.moment[1] / length;
            direction[2] = source.moment[2] / length;
        }
        else
        {
            direction[0] = 0.0f;
            direction[1] = 0.0f;
            direction[2] = 1.0f;
        }
        direction[3] = source.luminance > 1.0e-6f ? length / source.luminance : 0.0f;
        if (direction[3] > 1.0f) direction[3] = 1.0f;
    }

    inline void EncodeRgbm(const float source[3], float encoded[4])
    {
        float maximum = source[0];
        if (source[1] > maximum) maximum = source[1];
        if (source[2] > maximum) maximum = source[2];
        if (maximum <= 0.0f)
        {
            encoded[0] = encoded[1] = encoded[2] = encoded[3] = 0.0f;
            return;
        }
        float multiplier = ceilf((maximum / kRgbmRange) * 255.0f) / 255.0f;
        if (multiplier > 1.0f) multiplier = 1.0f;
        const float scale = 1.0f / (multiplier * kRgbmRange);
        for (int channel = 0; channel < 3; ++channel)
        {
            encoded[channel] = source[channel] * scale;
            if (encoded[channel] > 1.0f) encoded[channel] = 1.0f;
            if (encoded[channel] < 0.0f) encoded[channel] = 0.0f;
        }
        encoded[3] = multiplier;
    }

    inline void DecodeRgbm(const float encoded[4], float color[3])
    {
        const float scale = encoded[3] * kRgbmRange;
        for (int channel = 0; channel < 3; ++channel) color[channel] = encoded[channel] * scale;
    }
}