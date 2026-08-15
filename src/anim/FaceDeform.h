#ifndef ANIM_FACEDEFORM_H
#define ANIM_FACEDEFORM_H

// Blendshape (morph target) deformation, shared by the engine and the 360
// runtime (header-only C++03, the CameraResolve.h pattern).
//
// A face deforms on the CPU and is then skinned on the GPU exactly as before --
// no shader, vertex declaration or palette change. Morph deltas are authored in
// the bind pose the skinning palette expects, so "morph, then skin" composes.
// It is affordable because targets are sparse and their vertices were permuted
// into one contiguous range at bake time.

#include "anim/FaceShapes.h" // targets are identified by ARKit index

#include <cstring>

namespace face
{
    // Position at byte 0, normal at byte 12 of the shared 44-byte MeshVertex.
    const unsigned int kVertexNormalOffset = 12;

    // Below this a target contributes under half a quantisation step.
    const float kMorphWeightEpsilon = 1.0f / 512.0f;

    // Normal deltas quantise against a fixed scale rather than a per-target one:
    // both endpoints are unit vectors, so no component can leave [-2, 2].
    const float kNormalDeltaRange = 2.0f;
    const float kNormalDeltaQuant = 127.0f / kNormalDeltaRange; // delta -> int8
    const float kNormalDeltaScale = kNormalDeltaRange / 127.0f; // int8 -> delta

    // `vertex` is relative to MorphView::firstVertex, capping a morph region at
    // 65536 vertices.
    struct MorphDelta
    {
        unsigned short vertex;
        short px, py, pz;
        signed char nx, ny, nz;
        signed char pad;
    }; // 12 bytes, naturally aligned

    // Cooked payloads are memcpy'd straight into arrays of this struct, so a
    // compiler that padded it would silently corrupt every face.
    typedef char MorphDeltaLayoutCheck[(sizeof(MorphDelta) == 12) ? 1 : -1];

    struct MorphTarget
    {
        const MorphDelta* deltas;
        unsigned int      deltaCount;
        float             positionScale; // int16 delta -> model units
        int               shape;         // ARKit index, or kShapeNone
    };

    struct MorphView
    {
        const MorphTarget* targets;
        unsigned int       targetCount;
        unsigned int       firstVertex;
        unsigned int       vertexCount;

        MorphView() : targets(0), targetCount(0), firstVertex(0), vertexCount(0) {}
        bool Valid() const { return targets != 0 && targetCount != 0 && vertexCount != 0; }
    };

    // True when any target is driven hard enough to be worth deforming for. At
    // rest the deformed result IS the base mesh, so callers draw the mesh's
    // shared vertex buffer and an idle character costs nothing.
    inline bool AnyActive(const MorphView& morph, const float* weights, unsigned int weightCount)
    {
        if (!morph.Valid() || weights == 0) return false;
        for (unsigned int t = 0; t < morph.targetCount; ++t)
        {
            const MorphTarget& target = morph.targets[t];
            if (target.shape < 0 || (unsigned int)target.shape >= weightCount) continue;
            const float weight = weights[target.shape];
            if (weight > kMorphWeightEpsilon || weight < -kMorphWeightEpsilon) return true;
        }
        return false;
    }

    // `base` and `dst` both point at the START OF THE REGION -- vertex
    // firstVertex, not vertex zero -- and span vertexCount vertices. `dst` is
    // written, never read: console vertex memory is write-combined.
    inline void Deform(const unsigned char* base, unsigned int stride,
                       const MorphView& morph, const float* weights, unsigned int weightCount,
                       unsigned char* dst)
    {
        if (!morph.Valid() || base == 0 || dst == 0) return;

        std::memcpy(dst, base, (size_t)morph.vertexCount * stride);
        if (weights == 0) return;

        for (unsigned int t = 0; t < morph.targetCount; ++t)
        {
            const MorphTarget& target = morph.targets[t];
            if (target.shape < 0 || (unsigned int)target.shape >= weightCount) continue;
            const float weight = weights[target.shape];
            if (weight <= kMorphWeightEpsilon && weight >= -kMorphWeightEpsilon) continue;
            if (target.deltas == 0) continue;

            const float positionWeight = weight * target.positionScale;
            const float normalWeight   = weight * kNormalDeltaScale;

            for (unsigned int d = 0; d < target.deltaCount; ++d)
            {
                const MorphDelta& delta = target.deltas[d];
                if (delta.vertex >= morph.vertexCount) continue; // corrupt blob
                float* position = (float*)(dst + (size_t)delta.vertex * stride);
                position[0] += (float)delta.px * positionWeight;
                position[1] += (float)delta.py * positionWeight;
                position[2] += (float)delta.pz * positionWeight;

                float* normal = (float*)((unsigned char*)position + kVertexNormalOffset);
                normal[0] += (float)delta.nx * normalWeight;
                normal[1] += (float)delta.ny * normalWeight;
                normal[2] += (float)delta.nz * normalWeight;
            }
        }
    }
}

#endif // ANIM_FACEDEFORM_H
