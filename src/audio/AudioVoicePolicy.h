#ifndef AUDIO_AUDIOVOICEPOLICY_H
#define AUDIO_AUDIOVOICEPOLICY_H

#include <string>
#include <vector>

namespace aud
{
    enum AudioClass
    {
        AudioEffect = 0,
        AudioMusic = 1,
        AudioAmbience = 2,
        AudioDialogue = 3
    };

    enum AudioLoadMode
    {
        AudioLoadAuto = 0,
        AudioLoadResident = 1,
        AudioLoadStream = 2
    };

    struct VoiceCandidate
    {
        std::string key;
        int audioClass;
        int priority;
        bool spatial;
        bool loop;
        float distance;
        float maxDistance;

        VoiceCandidate()
            : audioClass(AudioEffect), priority(0), spatial(false), loop(false),
              distance(0.0f), maxDistance(0.0f) {}
    };

    struct VoiceSelection
    {
        std::vector<int> admitted;
        unsigned int distanceCulled;
        unsigned int rejected;

        VoiceSelection() : distanceCulled(0), rejected(0) {}
    };

    // Select at most maxVoices candidates. Results are returned in descending
    // policy rank, with key ordering as the final deterministic tie-breaker.
    VoiceSelection SelectVoices(const VoiceCandidate* candidates, int count,
                                int maxVoices);
}

#endif // AUDIO_AUDIOVOICEPOLICY_H