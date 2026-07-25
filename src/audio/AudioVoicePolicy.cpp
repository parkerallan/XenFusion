#include "audio/AudioVoicePolicy.h"

#include <algorithm>

namespace aud
{
    namespace
    {
        struct RankedVoice
        {
            int index;
            long score;
            std::string key;
        };

        bool HigherRank(const RankedVoice& left, const RankedVoice& right)
        {
            if (left.score != right.score)
                return left.score > right.score;
            return left.key < right.key;
        }

        long ClassBias(int audioClass)
        {
            switch (audioClass)
            {
            case AudioDialogue: return 3000000L;
            case AudioMusic:    return 2000000L;
            case AudioAmbience: return 1000000L;
            default:            return 0L;
            }
        }
    }

    VoiceSelection SelectVoices(const VoiceCandidate* candidates, int count,
                                int maxVoices)
    {
        VoiceSelection selection;
        if (!candidates || count <= 0 || maxVoices <= 0)
        {
            if (count > 0)
                selection.rejected = (unsigned int)count;
            return selection;
        }

        std::vector<RankedVoice> ranked;
        ranked.reserve((size_t)count);
        for (int index = 0; index < count; ++index)
        {
            const VoiceCandidate& candidate = candidates[index];
            if (candidate.spatial && candidate.maxDistance > 0.0f &&
                candidate.distance > candidate.maxDistance)
            {
                ++selection.distanceCulled;
                continue;
            }

            float proximity = 1.0f;
            if (candidate.spatial && candidate.maxDistance > 0.0f)
            {
                proximity = 1.0f - candidate.distance / candidate.maxDistance;
                if (proximity < 0.0f) proximity = 0.0f;
                if (proximity > 1.0f) proximity = 1.0f;
            }
            RankedVoice voice;
            voice.index = index;
            voice.key = candidate.key;
            voice.score = (long)candidate.priority * 10000L +
                          ClassBias(candidate.audioClass) +
                          (long)(proximity * 1000.0f);
            if (candidate.loop && candidate.audioClass == AudioEffect)
                voice.score -= 100L;
            ranked.push_back(voice);
        }

        std::sort(ranked.begin(), ranked.end(), HigherRank);
        const int admitted = (int)ranked.size() < maxVoices
                                 ? (int)ranked.size() : maxVoices;
        selection.admitted.reserve((size_t)admitted);
        for (int index = 0; index < admitted; ++index)
            selection.admitted.push_back(ranked[index].index);
        selection.rejected = (unsigned int)(ranked.size() - admitted);
        return selection;
    }
}