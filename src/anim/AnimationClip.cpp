#include "anim/AnimationClip.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cmath>

namespace
{
    template <typename Key>
    unsigned FindKey(const Key* keys, unsigned count, double time)
    {
        for (unsigned i = 0; i + 1 < count; ++i)
            if (time < keys[i + 1].mTime)
                return i;
        return count - 1;
    }

    aiVector3D SampleVector(const aiVectorKey* keys, unsigned count, double time,
                            const aiVector3D& fallback)
    {
        if (count == 0) return fallback;
        if (count == 1 || time <= keys[0].mTime) return keys[0].mValue;
        const unsigned index = FindKey(keys, count, time);
        if (index + 1 >= count) return keys[index].mValue;
        const double span = keys[index + 1].mTime - keys[index].mTime;
        const float t = span > 0.0 ? (float)((time - keys[index].mTime) / span) : 0.0f;
        return keys[index].mValue + (keys[index + 1].mValue - keys[index].mValue) * t;
    }

    aiQuaternion SampleRotation(const aiQuatKey* keys, unsigned count, double time,
                                const aiQuaternion& fallback)
    {
        if (count == 0) return fallback;
        if (count == 1 || time <= keys[0].mTime) return keys[0].mValue;
        const unsigned index = FindKey(keys, count, time);
        if (index + 1 >= count) return keys[index].mValue;
        const double span = keys[index + 1].mTime - keys[index].mTime;
        const float t = span > 0.0 ? (float)((time - keys[index].mTime) / span) : 0.0f;
        aiQuaternion result;
        aiQuaternion::Interpolate(result, keys[index].mValue, keys[index + 1].mValue, t);
        result.Normalize();
        return result;
    }
}

namespace animation
{
    uint32_t NameHash(const char* name)
    {
        uint32_t hash = 2166136261u;
        for (; name && *name; ++name)
        {
            char c = *name;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c == '\\') c = '/';
            hash ^= (unsigned char)c;
            hash *= 16777619u;
        }
        return hash;
    }

    bool DiscoverClips(const std::filesystem::path& source,
                       std::vector<std::string>& names, std::string& error)
    {
        names.clear();
        error.clear();
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(source.string(), aiProcess_ConvertToLeftHanded);
        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE))
        {
            error = importer.GetErrorString();
            return false;
        }
        names.reserve(scene->mNumAnimations);
        for (unsigned index = 0; index < scene->mNumAnimations; ++index)
        {
            std::string name = scene->mAnimations[index]->mName.C_Str();
            if (name.empty()) name = "Animation_" + std::to_string(index + 1);
            names.push_back(name);
        }
        return true;
    }

    bool BakeClip(const std::filesystem::path& source, const std::string& clip_name,
                  float sample_rate, AnimationClip& output, std::string& error)
    {
        error.clear();
        if (!std::isfinite(sample_rate) || sample_rate <= 0.0f)
        {
            error = "sample rate must be positive";
            return false;
        }

        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(source.string(), aiProcess_ConvertToLeftHanded);
        if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE))
        {
            error = importer.GetErrorString();
            return false;
        }

        const aiAnimation* source_clip = nullptr;
        for (unsigned i = 0; i < scene->mNumAnimations; ++i)
            if (clip_name == scene->mAnimations[i]->mName.C_Str())
            {
                source_clip = scene->mAnimations[i];
                break;
            }
        if (!source_clip)
        {
            error = "animation not found: " + clip_name;
            return false;
        }

        const double ticks_per_second = source_clip->mTicksPerSecond > 0.0
            ? source_clip->mTicksPerSecond : 25.0;
        const double duration = source_clip->mDuration / ticks_per_second;
        if (!(duration > 0.0) || !std::isfinite(duration))
        {
            error = "animation has invalid duration: " + clip_name;
            return false;
        }

        AnimationClip baked;
        baked.name = clip_name;
        baked.duration_seconds = (float)duration;
        baked.sample_rate = sample_rate;
        baked.frame_count = (std::max)(2u, (uint32_t)std::ceil(duration * sample_rate) + 1u);
        baked.tracks.reserve(source_clip->mNumChannels);

        for (unsigned channel_index = 0; channel_index < source_clip->mNumChannels; ++channel_index)
        {
            const aiNodeAnim* channel = source_clip->mChannels[channel_index];
            const aiNode* node = scene->mRootNode->FindNode(channel->mNodeName);
            aiVector3D bind_scale(1.0f), bind_translation(0.0f);
            aiQuaternion bind_rotation;
            if (node)
                node->mTransformation.Decompose(bind_scale, bind_rotation, bind_translation);

            AnimationTrack track;
            track.name = channel->mNodeName.C_Str();
            track.name_hash = NameHash(track.name.c_str());
            track.samples.resize(baked.frame_count);
            for (uint32_t frame = 0; frame < baked.frame_count; ++frame)
            {
                const double seconds = (std::min)(duration, (double)frame / sample_rate);
                const double ticks = seconds * ticks_per_second;
                const aiVector3D translation = SampleVector(channel->mPositionKeys,
                    channel->mNumPositionKeys, ticks, bind_translation);
                const aiQuaternion rotation = SampleRotation(channel->mRotationKeys,
                    channel->mNumRotationKeys, ticks, bind_rotation);
                const aiVector3D scale = SampleVector(channel->mScalingKeys,
                    channel->mNumScalingKeys, ticks, bind_scale);
                AnimationTransform& sample = track.samples[frame];
                sample.translation[0] = translation.x;
                sample.translation[1] = translation.y;
                sample.translation[2] = translation.z;
                sample.rotation[0] = rotation.x;
                sample.rotation[1] = rotation.y;
                sample.rotation[2] = rotation.z;
                sample.rotation[3] = rotation.w;
                sample.scale[0] = scale.x;
                sample.scale[1] = scale.y;
                sample.scale[2] = scale.z;
            }
            baked.tracks.push_back(std::move(track));
        }
        if (baked.tracks.empty())
        {
            error = "animation has no tracks: " + clip_name;
            return false;
        }
        output = std::move(baked);
        return true;
    }
}