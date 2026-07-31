#include "anim/AnimationClip.h"

#include <cmath>
#include <iostream>

namespace
{
    int Fail(const std::string& message)
    {
        std::cerr << "AnimationClipTest: " << message << '\n';
        return 1;
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return Fail("expected the path to Fox.gltf");

    const char* names[] = {"Survey", "Walk", "Run"};
    std::vector<std::string> discovered;
    std::string discovery_error;
    if (!animation::DiscoverClips(argv[1], discovered, discovery_error))
        return Fail(discovery_error);
    if (discovered.size() != 3 || discovered[0] != names[0] ||
        discovered[1] != names[1] || discovered[2] != names[2])
        return Fail("clip discovery did not return Survey, Walk, Run");
    for (const char* name : names)
    {
        AnimationClip clip;
        std::string error;
        if (!animation::BakeClip(argv[1], name, 30.0f, clip, error))
            return Fail(error);
        if (clip.frame_count < 2 || clip.tracks.empty() || clip.duration_seconds <= 0.0f)
            return Fail(std::string(name) + " produced no samples");

        bool moved = false;
        for (const AnimationTrack& track : clip.tracks)
        {
            if (track.name_hash == 0 || track.samples.size() != clip.frame_count)
                return Fail(std::string(name) + " has an invalid track");
            const AnimationTransform& first = track.samples.front();
            for (const AnimationTransform& sample : track.samples)
            {
                for (float value : sample.translation) if (!std::isfinite(value)) return Fail("non-finite translation");
                for (float value : sample.rotation) if (!std::isfinite(value)) return Fail("non-finite rotation");
                for (float value : sample.scale) if (!std::isfinite(value)) return Fail("non-finite scale");
                if (std::fabs(sample.translation[0] - first.translation[0]) > 1e-4f ||
                    std::fabs(sample.translation[1] - first.translation[1]) > 1e-4f ||
                    std::fabs(sample.translation[2] - first.translation[2]) > 1e-4f ||
                    std::fabs(sample.rotation[0] - first.rotation[0]) > 1e-4f ||
                    std::fabs(sample.rotation[1] - first.rotation[1]) > 1e-4f ||
                    std::fabs(sample.rotation[2] - first.rotation[2]) > 1e-4f ||
                    std::fabs(sample.rotation[3] - first.rotation[3]) > 1e-4f)
                    moved = true;
            }
        }
        if (!moved)
            return Fail(std::string(name) + " contains no motion");
        std::cout << name << ": " << clip.duration_seconds << "s, "
                  << clip.frame_count << " frames, " << clip.tracks.size() << " tracks\n";
    }
    return 0;
}