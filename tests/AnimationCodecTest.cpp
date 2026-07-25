#include "anim/AnimationClip.h"
#include "anim/AnimationCodec.h"

#include <cmath>
#include <iostream>

namespace
{
    unsigned int ReadU32(const unsigned char* data)
    {
        return ((unsigned int)data[0] << 24) | ((unsigned int)data[1] << 16) |
               ((unsigned int)data[2] << 8) | data[3];
    }

    void WriteU32(unsigned char* data, unsigned int value)
    {
        data[0] = (unsigned char)(value >> 24);
        data[1] = (unsigned char)(value >> 16);
        data[2] = (unsigned char)(value >> 8);
        data[3] = (unsigned char)value;
    }
}

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    AnimationClip source;
    std::string error;
    if (!animation::BakeClip(argv[1], "Run", 30.0f, source, error)) return 2;
    std::vector<unsigned char> encoded;
    if (!animation::EncodeClipBE(source, encoded, error)) return 3;
    if (encoded.size() < 32 || ReadU32(encoded.data()) != 0x414E4D32u ||
        ReadU32(encoded.data() + 4) != 2) return 7;
    const unsigned int track_count = ReadU32(encoded.data() + 8);
    const unsigned int data_offset = ReadU32(encoded.data() + 28);
    if (track_count != source.tracks.size() ||
        data_offset != 32 + track_count * 56 ||
        ReadU32(encoded.data() + 32 + 52) != 0 ||
        ReadU32(encoded.data() + 32 + 56 + 52) != 20) return 8;
    AnimationClip decoded;
    if (!animation::DecodeClipBE(encoded.data(), encoded.size(), decoded, error)) return 4;
    if (decoded.tracks.size() != source.tracks.size() || decoded.frame_count != source.frame_count) return 5;

    std::vector<unsigned char> legacy(encoded.begin(), encoded.begin() + data_offset);
    legacy.resize(encoded.size());
    WriteU32(legacy.data(), 0x414E4D31u);
    WriteU32(legacy.data() + 4, 1);
    for (unsigned int track = 0; track < track_count; ++track)
    {
        const unsigned int legacy_offset = data_offset + track * source.frame_count * 20;
        WriteU32(legacy.data() + 32 + track * 56 + 52, legacy_offset);
        for (unsigned int frame = 0; frame < source.frame_count; ++frame)
            std::copy(encoded.begin() + data_offset + (frame * track_count + track) * 20,
                      encoded.begin() + data_offset + (frame * track_count + track + 1) * 20,
                      legacy.begin() + legacy_offset + frame * 20);
    }
    AnimationClip legacy_decoded;
    if (!animation::DecodeClipBE(legacy.data(), legacy.size(), legacy_decoded, error) ||
        legacy_decoded.tracks.size() != decoded.tracks.size()) return 9;
    std::vector<unsigned char> truncated(encoded.begin(), encoded.end() - 1);
    AnimationClip rejected;
    if (animation::DecodeClipBE(truncated.data(), truncated.size(), rejected, error)) return 10;

    float max_translation_error = 0.0f;
    float min_rotation_dot = 1.0f;
    for (size_t track = 0; track < source.tracks.size(); ++track)
        for (size_t frame = 0; frame < source.frame_count; ++frame)
        {
            const AnimationTransform& a = source.tracks[track].samples[frame];
            const AnimationTransform& b = decoded.tracks[track].samples[frame];
            const AnimationTransform& legacy_sample = legacy_decoded.tracks[track].samples[frame];
            for (int axis = 0; axis < 3; ++axis)
                max_translation_error = (std::max)(max_translation_error,
                    std::fabs(a.translation[axis] - b.translation[axis]));
            float dot = 0.0f;
            for (int component = 0; component < 4; ++component)
                dot += a.rotation[component] * b.rotation[component];
            min_rotation_dot = (std::min)(min_rotation_dot, std::fabs(dot));
            for (int axis = 0; axis < 3; ++axis)
                if (std::fabs(b.translation[axis] - legacy_sample.translation[axis]) > 1e-7f)
                    return 11;
        }
    if (max_translation_error > 0.001f || min_rotation_dot < 0.9999f) return 6;
    std::cout << "ANM2 Run: " << encoded.size() << " bytes, max translation error "
              << max_translation_error << ", min rotation dot " << min_rotation_dot << '\n';
    return 0;
}