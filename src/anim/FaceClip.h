#ifndef ANIM_FACECLIP_H
#define ANIM_FACECLIP_H

// A cooked facial performance: ARKit blendshape weights on a uniform frame
// grid, shared by the engine and the 360 runtime (header-only C++03).
//
// Uniform frames rather than keyframe curves because a capture IS dense —
// sampling is then two array reads and a lerp with no search, which is what a
// console wants.
//
// On-disk (u32 fields big-endian, so the console reads them natively; the
// weight bytes need no swapping):
//   u32 magic 'FCL1' | u32 version | u32 frameCount | u32 targetCount |
//   u32 frameRateMilli | i32 audioOffsetMs | u32 audioPathBytes
//   targetCount bytes: the ARKit shape each target drives, padded to 4
//   frameCount * targetCount bytes: weights, FRAME-major, 255 = 1.0
//   pad to 4, then audioPathBytes of project-relative audio path
//
// Frame-major keeps one sampled frame contiguous. The clip carries its own
// audio path because playing a performance means playing that audio and
// following it; a clip whose audio the caller must know separately is a clip
// that can fall out of sync.

#include "anim/FaceShapes.h"

namespace face
{
    const unsigned int kClipMagic        = 0x46434C31; // 'FCL1'
    const unsigned int kClipVersion      = 1;
    const unsigned int kClipHeaderBytes  = 28;         // 7 * u32
    const unsigned int kClipMaxFrames    = 1u << 20;
    const unsigned int kClipMaxAudioPath = 512;
    const float        kClipDefaultRate  = 30.0f;

    // A cooked clip as a read-only view over bytes someone else owns.
    struct ClipView
    {
        const unsigned char* shapes;
        const unsigned char* weights;   // frameCount * targetCount, frame-major
        const char*          audioPath; // not NUL-terminated
        unsigned int         audioPathBytes;
        int                  audioOffsetMs;
        unsigned int         frameCount;
        unsigned int         targetCount;
        float                frameRate;

        ClipView() : shapes(0), weights(0), audioPath(0), audioPathBytes(0),
                     audioOffsetMs(0), frameCount(0), targetCount(0),
                     frameRate(kClipDefaultRate) {}
        bool Valid() const
        {
            return shapes != 0 && weights != 0 && frameCount != 0 &&
                   targetCount != 0 && frameRate > 0.0f;
        }
        // The last frame is a sample, not an interval.
        float Duration() const
        {
            return Valid() ? (float)(frameCount - 1) / frameRate : 0.0f;
        }
    };

    inline unsigned int LoadClipU32BE(const unsigned char* p)
    {
        return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
               ((unsigned int)p[2] << 8)  |  (unsigned int)p[3];
    }

    // False for anything that does not add up, so a corrupt clip costs a silent
    // mouth rather than a crash.
    inline bool ParseClip(const unsigned char* data, unsigned int size, ClipView& out)
    {
        out = ClipView();
        if (data == 0 || size < kClipHeaderBytes) return false;
        if (LoadClipU32BE(data) != kClipMagic) return false;
        if (LoadClipU32BE(data + 4) != kClipVersion) return false;

        const unsigned int frameCount  = LoadClipU32BE(data + 8);
        const unsigned int targetCount = LoadClipU32BE(data + 12);
        const unsigned int rateMilli   = LoadClipU32BE(data + 16);
        const int          offsetMs    = (int)LoadClipU32BE(data + 20);
        const unsigned int audioBytes  = LoadClipU32BE(data + 24);
        if (frameCount == 0 || frameCount > kClipMaxFrames) return false;
        if (targetCount == 0 || targetCount > (unsigned int)kShapeCount) return false;
        if (rateMilli == 0 || audioBytes > kClipMaxAudioPath) return false;

        const unsigned int shapeBytes  = (targetCount + 3u) & ~3u;
        const unsigned int weightBytes = frameCount * targetCount;
        // The audio path is aligned, so the weights are followed by padding --
        // but only when there is a path to align.
        const unsigned int audioOffset =
            ((unsigned int)kClipHeaderBytes + shapeBytes + weightBytes + 3u) & ~3u;
        if ((unsigned int)kClipHeaderBytes + shapeBytes + weightBytes > size) return false;
        if (audioBytes && audioOffset + audioBytes > size) return false;

        out.shapes         = data + kClipHeaderBytes;
        out.weights        = out.shapes + shapeBytes;
        out.audioPath      = audioBytes ? (const char*)(data + audioOffset) : 0;
        out.audioPathBytes = audioBytes;
        out.audioOffsetMs  = offsetMs;
        out.frameCount     = frameCount;
        out.targetCount    = targetCount;
        out.frameRate      = (float)rateMilli / 1000.0f;
        return true;
    }

    // Samples into `out`, writing ONLY the shapes this clip drives -- which is
    // what lets a performance layer over an expression pose. `touched` marks
    // them so the caller can tell "driven to zero" from "not driven".
    inline void SampleClip(const ClipView& clip, float seconds, bool loop,
                           float* out, unsigned int outCount, unsigned char* touched)
    {
        if (!clip.Valid() || out == 0) return;

        const float duration = clip.Duration();
        float time = seconds;
        if (loop && duration > 0.0f)
        {
            // fmod without <cmath>: this header stays dependency-free.
            const float wraps = time / duration;
            time -= duration * (float)(int)(wraps >= 0.0f ? wraps : wraps - 1.0f);
        }
        if (time < 0.0f) time = 0.0f;
        if (time > duration) time = duration;

        const float exact = time * clip.frameRate;
        unsigned int frame = (unsigned int)exact;
        if (frame >= clip.frameCount - 1) frame = clip.frameCount - (clip.frameCount > 1 ? 2 : 1);
        const float blend = exact - (float)frame;

        const unsigned char* a = clip.weights + (size_t)frame * clip.targetCount;
        const unsigned char* b = (clip.frameCount > 1)
            ? clip.weights + (size_t)(frame + 1) * clip.targetCount : a;

        for (unsigned int t = 0; t < clip.targetCount; ++t)
        {
            const int shape = (int)clip.shapes[t];
            if (shape < 0 || (unsigned int)shape >= outCount) continue;
            const float low  = (float)a[t] * (1.0f / 255.0f);
            const float high = (float)b[t] * (1.0f / 255.0f);
            out[shape] = low + (high - low) * blend;
            if (touched) touched[shape] = 1;
        }
    }
}

#endif // ANIM_FACECLIP_H
