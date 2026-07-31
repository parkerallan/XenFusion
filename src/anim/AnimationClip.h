#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct AnimationTransform
{
    float translation[3] = {0.0f, 0.0f, 0.0f};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
};

struct AnimationTrack
{
    uint32_t name_hash = 0;
    std::string name;
    std::vector<AnimationTransform> samples;
};

struct AnimationClip
{
    std::string name;
    float duration_seconds = 0.0f;
    float sample_rate = 30.0f;
    uint32_t frame_count = 0;
    std::vector<AnimationTrack> tracks;
};

namespace animation
{
    uint32_t NameHash(const char* name);
    bool DiscoverClips(const std::filesystem::path& source,
                       std::vector<std::string>& names, std::string& error);
    bool BakeClip(const std::filesystem::path& source, const std::string& clip_name,
                  float sample_rate, AnimationClip& output, std::string& error);
}