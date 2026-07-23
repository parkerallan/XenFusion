#ifndef AUDIO_SPATIAL3D_H
#define AUDIO_SPATIAL3D_H

// X3DAudio wrapper shared by the editor (Windows SDK: merged into xaudio2) and
// the 360 runtime (XDK: x3daudio.lib). One mono emitter against the listener
// (the active camera) yields a stereo gain pair + a doppler ratio, applied by
// the AudioPlayer via SetOutputMatrix / SetFrequencyRatio. The audible model:
// full volume inside minDist, X3DAudio's default inverse (minDist / distance)
// falloff beyond it, silence past maxDist, doppler scaled per-attribute.
// Strict C++03; no X3DAudio types leak out of the .cpp.

namespace aud
{
    struct ListenerState
    {
        float pos[3];
        float fwd[3]; // normalized look direction
        float up[3];  // normalized up
        float vel[3]; // world units/sec (doppler); zero if unknown

        ListenerState()
        {
            for (int i = 0; i < 3; ++i) { pos[i] = fwd[i] = up[i] = vel[i] = 0.0f; }
            fwd[2] = 1.0f;
            up[1]  = 1.0f;
        }
    };

    struct SpatialParams
    {
        float pos[3];
        float vel[3];
        float minDist;  // full volume inside this radius
        float maxDist;  // silent beyond this
        float doppler;  // attribute's doppler factor (0 = off)

        SpatialParams() : minDist(1.0f), maxDist(50.0f), doppler(1.0f)
        {
            for (int i = 0; i < 3; ++i) { pos[i] = vel[i] = 0.0f; }
        }
    };

    class Spatial3D
    {
    public:
        // Idempotent; false only if X3DAudio can't initialize (spatial streams
        // then play as plain 2D).
        static bool Init();

        // Mono emitter -> stereo gains[2] (L, R) + playback-rate multiplier.
        static void Calculate(const ListenerState& listener,
                              const SpatialParams& emitter,
                              float gains[2], float* dopplerRatio);
    };
}

#endif // AUDIO_SPATIAL3D_H
