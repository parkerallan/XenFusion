#ifndef ANIM_FACERUNTIME_H
#define ANIM_FACERUNTIME_H

// One character's face, evaluated the same way in the engine's viewport and on
// the console (shared C++03, compiled into both). Three layers -- expression
// pose, procedural blink, gaze -- resolve to the float[52] in ARKit order
// (FaceShapes.h) that the deformer applies to the mesh.

#include "anim/FaceShapes.h"

#include <vector>

namespace face
{
    // Poses are sparse: only the shapes they drive are listed.
    struct PoseTarget
    {
        unsigned char shape;  // ARKit index
        unsigned char weight; // 255 = 1.0
    };

    struct Pose
    {
        unsigned int            nameHash;
        std::vector<PoseTarget> targets;

        Pose() : nameHash(0) {}
    };

    // A metronome blink reads as a twitch, so the interval is jittered.
    const float kBlinkCloseSeconds = 0.06f;
    const float kBlinkHoldSeconds  = 0.02f;
    const float kBlinkOpenSeconds  = 0.09f;
    const float kBlinkMinInterval  = 2.0f;
    const float kBlinkMaxInterval  = 6.0f;

    const float kGazeSmoothing = 12.0f; // fraction closed per second

    class Layer
    {
    public:
        Layer();

        // `speed` is pose-units per second; 0 snaps, which is what a cut wants.
        void SetPose(const Pose* pose, float weight, float speed);

        // Character space: yaw positive toward its LEFT, pitch positive up,
        // both [-1, 1]. The caller resolves a world point into these.
        void SetGaze(float yaw, float pitch);
        void ClearGaze();
        bool GazeActive() const { return m_gazeActive; }

        void SetBlinkEnabled(bool enabled);
        bool BlinkEnabled() const { return m_blinkEnabled; }

        // Two faces sharing a seed blink in unison, so pass something per-object.
        void SetSeed(unsigned int seed);

        void Update(float deltaSeconds);

        const float* Weights() const { return m_weights; }
        // False until something drives this face, so it can skip deformation.
        bool Active() const { return m_active; }

        void Reset();

    private:
        void ApplyPose();
        void ApplyBlink(float deltaSeconds);
        void ApplyGaze(float deltaSeconds);
        float NextBlinkInterval();

        float m_weights[kShapeCount];
        float m_poseCurrent[kShapeCount]; // showing
        float m_poseTarget[kShapeCount];  // where SetPose asked it to go
        float m_poseSpeed;

        bool  m_blinkEnabled;
        float m_blinkTimer;     // counts down to the next blink
        float m_blinkPhase;     // seconds into the current blink, < 0 = not blinking
        unsigned int m_random;

        bool  m_gazeActive;
        float m_gazeYaw, m_gazePitch;             // requested
        float m_gazeYawSmoothed, m_gazePitchSmoothed;

        bool  m_active;
    };
}

#endif // ANIM_FACERUNTIME_H
