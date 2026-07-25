#include "anim/AnimationCodec.h"

#include "AnimationFormat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
    void PushU16(std::vector<unsigned char>& output, unsigned int value)
    {
        output.push_back((unsigned char)(value >> 8));
        output.push_back((unsigned char)value);
    }
    void PushU32(std::vector<unsigned char>& output, unsigned int value)
    {
        output.push_back((unsigned char)(value >> 24));
        output.push_back((unsigned char)(value >> 16));
        output.push_back((unsigned char)(value >> 8));
        output.push_back((unsigned char)value);
    }
    void PushFloat(std::vector<unsigned char>& output, float value)
    {
        unsigned int bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        PushU32(output, bits);
    }
    unsigned int ReadU16(const unsigned char* data)
    {
        return ((unsigned int)data[0] << 8) | data[1];
    }
    unsigned int ReadU32(const unsigned char* data)
    {
        return ((unsigned int)data[0] << 24) | ((unsigned int)data[1] << 16) |
               ((unsigned int)data[2] << 8) | data[3];
    }
    float ReadFloat(const unsigned char* data)
    {
        const unsigned int bits = ReadU32(data);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    unsigned int Quantize(float value, float minimum, float extent)
    {
        if (extent <= 1e-12f) return 0;
        const float normalized = (std::clamp)((value - minimum) / extent, 0.0f, 1.0f);
        return (unsigned int)std::lround(normalized * 65535.0f);
    }
    float Dequantize(unsigned int value, float minimum, float extent)
    {
        return minimum + extent * ((float)value / 65535.0f);
    }
}

namespace animation
{
    bool EncodeClipBE(const AnimationClip& clip, std::vector<unsigned char>& output,
                      std::string& error)
    {
        error.clear();
        if (clip.frame_count == 0 || clip.tracks.empty() ||
            !std::isfinite(clip.duration_seconds) || clip.duration_seconds <= 0.0f ||
            !std::isfinite(clip.sample_rate) || clip.sample_rate <= 0.0f)
        { error = "clip is empty or has invalid timing"; return false; }
        for (const AnimationTrack& track : clip.tracks)
            if (track.name_hash == 0 || track.samples.size() != clip.frame_count)
            { error = "clip has an invalid track"; return false; }

        const size_t table_bytes = clip.tracks.size() * animfmt::kTrackBytes;
        const size_t sample_bytes = (size_t)clip.frame_count * animfmt::kSampleBytes;
        if (table_bytes > std::numeric_limits<unsigned int>::max() ||
            sample_bytes * clip.tracks.size() > std::numeric_limits<unsigned int>::max())
        { error = "clip is too large"; return false; }

        struct Bounds { float tmin[3], text[3], smin[3], sext[3]; };
        std::vector<Bounds> bounds(clip.tracks.size());
        for (size_t track_index = 0; track_index < clip.tracks.size(); ++track_index)
        {
            Bounds& bound = bounds[track_index];
            for (int axis = 0; axis < 3; ++axis)
            {
                bound.tmin[axis] = bound.smin[axis] = std::numeric_limits<float>::max();
                float tmax = -std::numeric_limits<float>::max();
                float smax = -std::numeric_limits<float>::max();
                for (const AnimationTransform& sample : clip.tracks[track_index].samples)
                {
                    bound.tmin[axis] = (std::min)(bound.tmin[axis], sample.translation[axis]);
                    tmax = (std::max)(tmax, sample.translation[axis]);
                    bound.smin[axis] = (std::min)(bound.smin[axis], sample.scale[axis]);
                    smax = (std::max)(smax, sample.scale[axis]);
                }
                bound.text[axis] = tmax - bound.tmin[axis];
                bound.sext[axis] = smax - bound.smin[axis];
            }
        }

        output.clear();
        output.reserve(animfmt::kHeaderBytes + table_bytes + sample_bytes * clip.tracks.size());
        PushU32(output, animfmt::kMagic);
        PushU32(output, animfmt::kVersion);
        PushU32(output, (unsigned int)clip.tracks.size());
        PushU32(output, clip.frame_count);
        PushFloat(output, clip.duration_seconds);
        PushFloat(output, clip.sample_rate);
        PushU32(output, animfmt::kHeaderBytes);
        PushU32(output, animfmt::kHeaderBytes + (unsigned int)table_bytes);

        unsigned int sample_offset = animfmt::kHeaderBytes + (unsigned int)table_bytes;
        for (size_t track_index = 0; track_index < clip.tracks.size(); ++track_index)
        {
            PushU32(output, clip.tracks[track_index].name_hash);
            const Bounds& bound = bounds[track_index];
            for (int axis = 0; axis < 3; ++axis) PushFloat(output, bound.tmin[axis]);
            for (int axis = 0; axis < 3; ++axis) PushFloat(output, bound.text[axis]);
            for (int axis = 0; axis < 3; ++axis) PushFloat(output, bound.smin[axis]);
            for (int axis = 0; axis < 3; ++axis) PushFloat(output, bound.sext[axis]);
            PushU32(output, sample_offset);
            sample_offset += (unsigned int)sample_bytes;
        }

        for (size_t track_index = 0; track_index < clip.tracks.size(); ++track_index)
        {
            const Bounds& bound = bounds[track_index];
            for (const AnimationTransform& sample : clip.tracks[track_index].samples)
            {
                for (int axis = 0; axis < 3; ++axis)
                    PushU16(output, Quantize(sample.translation[axis], bound.tmin[axis], bound.text[axis]));
                for (int component = 0; component < 4; ++component)
                {
                    const int value = (int)std::lround((std::clamp)(sample.rotation[component], -1.0f, 1.0f) * 32767.0f);
                    PushU16(output, (unsigned short)(short)value);
                }
                for (int axis = 0; axis < 3; ++axis)
                    PushU16(output, Quantize(sample.scale[axis], bound.smin[axis], bound.sext[axis]));
            }
        }
        return true;
    }

    bool DecodeClipBE(const unsigned char* data, size_t size, AnimationClip& output,
                      std::string& error)
    {
        error.clear();
        if (!data || size < animfmt::kHeaderBytes || ReadU32(data) != animfmt::kMagic ||
            ReadU32(data + 4) != animfmt::kVersion)
        { error = "invalid ANM1 header"; return false; }
        const unsigned int track_count = ReadU32(data + 8);
        const unsigned int frame_count = ReadU32(data + 12);
        const float duration = ReadFloat(data + 16);
        const float rate = ReadFloat(data + 20);
        const unsigned int table_offset = ReadU32(data + 24);
        if (track_count == 0 || frame_count == 0 || table_offset > size ||
            (size_t)table_offset + (size_t)track_count * animfmt::kTrackBytes > size)
        { error = "invalid ANM1 table"; return false; }

        AnimationClip decoded;
        decoded.duration_seconds = duration;
        decoded.sample_rate = rate;
        decoded.frame_count = frame_count;
        decoded.tracks.resize(track_count);
        for (unsigned int track_index = 0; track_index < track_count; ++track_index)
        {
            const unsigned char* record = data + table_offset + (size_t)track_index * animfmt::kTrackBytes;
            AnimationTrack& track = decoded.tracks[track_index];
            track.name_hash = ReadU32(record);
            float tmin[3], text[3], smin[3], sext[3];
            for (int axis = 0; axis < 3; ++axis) tmin[axis] = ReadFloat(record + 4 + axis * 4);
            for (int axis = 0; axis < 3; ++axis) text[axis] = ReadFloat(record + 16 + axis * 4);
            for (int axis = 0; axis < 3; ++axis) smin[axis] = ReadFloat(record + 28 + axis * 4);
            for (int axis = 0; axis < 3; ++axis) sext[axis] = ReadFloat(record + 40 + axis * 4);
            const unsigned int sample_offset = ReadU32(record + 52);
            if ((size_t)sample_offset + (size_t)frame_count * animfmt::kSampleBytes > size)
            { error = "ANM1 sample block is truncated"; return false; }
            track.samples.resize(frame_count);
            for (unsigned int frame = 0; frame < frame_count; ++frame)
            {
                const unsigned char* source = data + sample_offset + (size_t)frame * animfmt::kSampleBytes;
                AnimationTransform& sample = track.samples[frame];
                for (int axis = 0; axis < 3; ++axis)
                    sample.translation[axis] = Dequantize(ReadU16(source + axis * 2), tmin[axis], text[axis]);
                float length_squared = 0.0f;
                for (int component = 0; component < 4; ++component)
                {
                    sample.rotation[component] = (float)(short)ReadU16(source + 6 + component * 2) / 32767.0f;
                    length_squared += sample.rotation[component] * sample.rotation[component];
                }
                const float inverse_length = length_squared > 1e-12f ? 1.0f / std::sqrt(length_squared) : 1.0f;
                for (int component = 0; component < 4; ++component)
                    sample.rotation[component] *= inverse_length;
                for (int axis = 0; axis < 3; ++axis)
                    sample.scale[axis] = Dequantize(ReadU16(source + 14 + axis * 2), smin[axis], sext[axis]);
            }
        }
        output = std::move(decoded);
        return true;
    }
}