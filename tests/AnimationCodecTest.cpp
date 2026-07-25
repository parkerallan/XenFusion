#include "anim/AnimationClip.h"
#include "anim/AnimationCodec.h"

#include <cmath>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    AnimationClip source;
    std::string error;
    if (!animation::BakeClip(argv[1], "Run", 30.0f, source, error)) return 2;
    std::vector<unsigned char> encoded;
    if (!animation::EncodeClipBE(source, encoded, error)) return 3;
    AnimationClip decoded;
    if (!animation::DecodeClipBE(encoded.data(), encoded.size(), decoded, error)) return 4;
    if (decoded.tracks.size() != source.tracks.size() || decoded.frame_count != source.frame_count) return 5;

    float max_translation_error = 0.0f;
    float min_rotation_dot = 1.0f;
    for (size_t track = 0; track < source.tracks.size(); ++track)
        for (size_t frame = 0; frame < source.frame_count; ++frame)
        {
            const AnimationTransform& a = source.tracks[track].samples[frame];
            const AnimationTransform& b = decoded.tracks[track].samples[frame];
            for (int axis = 0; axis < 3; ++axis)
                max_translation_error = (std::max)(max_translation_error,
                    std::fabs(a.translation[axis] - b.translation[axis]));
            float dot = 0.0f;
            for (int component = 0; component < 4; ++component)
                dot += a.rotation[component] * b.rotation[component];
            min_rotation_dot = (std::min)(min_rotation_dot, std::fabs(dot));
        }
    if (max_translation_error > 0.001f || min_rotation_dot < 0.9999f) return 6;
    std::cout << "ANM1 Run: " << encoded.size() << " bytes, max translation error "
              << max_translation_error << ", min rotation dot " << min_rotation_dot << '\n';
    return 0;
}