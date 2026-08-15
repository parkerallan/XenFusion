#include "anim/FaceRuntime.h"

#include <string.h>

namespace face
{
    namespace
    {
        float Clamp01(float value)
        {
            return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        }

        float Clamp(float value, float low, float high)
        {
            return value < low ? low : (value > high ? high : value);
        }
    }

    Layer::Layer()
    {
        m_random = 0x9E3779B9u;
        Reset();
    }

    void Layer::Reset()
    {
        memset(m_weights, 0, sizeof(m_weights));
        memset(m_poseCurrent, 0, sizeof(m_poseCurrent));
        memset(m_poseTarget, 0, sizeof(m_poseTarget));
        m_poseSpeed = 0.0f;
        memset(m_clipTouched, 0, sizeof(m_clipTouched));
        m_clip = ClipView();
        m_clipPlaying = false;
        m_clipLoop = false;
        m_clipTime = 0.0f;
        m_blinkEnabled = true;
        m_blinkTimer = NextBlinkInterval();
        m_blinkPhase = -1.0f;
        m_gazeActive = false;
        m_gazeYaw = m_gazePitch = 0.0f;
        m_gazeYawSmoothed = m_gazePitchSmoothed = 0.0f;
        m_active = false;
    }

    void Layer::SetSeed(unsigned int seed)
    {
        // Never zero: the generator below would then be stuck there forever.
        m_random = seed ? seed : 0x9E3779B9u;
        m_blinkTimer = NextBlinkInterval();
    }

    float Layer::NextBlinkInterval()
    {
        // xorshift32: deterministic, and not the CRT's shared global rand.
        m_random ^= m_random << 13;
        m_random ^= m_random >> 17;
        m_random ^= m_random << 5;
        const float unit = (float)(m_random & 0xFFFFFF) / (float)0xFFFFFF;
        return kBlinkMinInterval + unit * (kBlinkMaxInterval - kBlinkMinInterval);
    }

    void Layer::SetPose(const Pose* pose, float weight, float speed)
    {
        memset(m_poseTarget, 0, sizeof(m_poseTarget));
        if (pose)
        {
            const float scale = Clamp01(weight) * (1.0f / 255.0f);
            for (size_t i = 0; i < pose->targets.size(); ++i)
            {
                const PoseTarget& target = pose->targets[i];
                if (target.shape < kShapeCount)
                    m_poseTarget[target.shape] = (float)target.weight * scale;
            }
        }
        m_poseSpeed = speed > 0.0f ? speed : 0.0f;
        if (m_poseSpeed <= 0.0f)
            memcpy(m_poseCurrent, m_poseTarget, sizeof(m_poseCurrent)); // a cut
        m_active = true;
    }


    void Layer::PlayClip(const ClipView& clip, bool loop)
    {
        if (!clip.Valid())
            return;
        m_clip = clip;
        m_clipPlaying = true;
        m_clipLoop = loop;
        m_clipTime = 0.0f;
        m_active = true;
    }

    void Layer::StopClip()
    {
        m_clipPlaying = false;
        m_clipTime = 0.0f;
        memset(m_clipTouched, 0, sizeof(m_clipTouched));
    }

    void Layer::SetGaze(float yaw, float pitch)
    {
        m_gazeYaw = Clamp(yaw, -1.0f, 1.0f);
        m_gazePitch = Clamp(pitch, -1.0f, 1.0f);
        m_gazeActive = true;
        m_active = true;
    }

    void Layer::ClearGaze()
    {
        // The eyes ease back rather than snapping: a glance away, not a reset.
        m_gazeYaw = m_gazePitch = 0.0f;
        m_gazeActive = false;
    }

    void Layer::SetBlinkEnabled(bool enabled)
    {
        m_blinkEnabled = enabled;
        if (!enabled)
            m_blinkPhase = -1.0f;
    }

    void Layer::ApplyPose()
    {
        for (int shape = 0; shape < kShapeCount; ++shape)
            m_weights[shape] = m_poseCurrent[shape];
    }

    void Layer::ApplyBlink(float deltaSeconds)
    {
        if (!m_blinkEnabled)
            return;

        if (m_blinkPhase >= 0.0f)
        {
            m_blinkPhase += deltaSeconds;
            const float total = kBlinkCloseSeconds + kBlinkHoldSeconds + kBlinkOpenSeconds;
            if (m_blinkPhase >= total)
            {
                m_blinkPhase = -1.0f;
                m_blinkTimer = NextBlinkInterval();
            }
        }
        else
        {
            m_blinkTimer -= deltaSeconds;
            if (m_blinkTimer <= 0.0f)
                m_blinkPhase = 0.0f;
        }

        if (m_blinkPhase < 0.0f)
            return;

        float closed;
        if (m_blinkPhase < kBlinkCloseSeconds)
            closed = m_blinkPhase / kBlinkCloseSeconds;
        else if (m_blinkPhase < kBlinkCloseSeconds + kBlinkHoldSeconds)
            closed = 1.0f;
        else
            closed = 1.0f - (m_blinkPhase - kBlinkCloseSeconds - kBlinkHoldSeconds) / kBlinkOpenSeconds;
        closed = Clamp01(closed);

        // A captured performance blinks on its own; where the clip drives the
        // lids, leave them alone rather than adding a second blink on top.
        static const char* const kLids[2] = {"eyeBlinkLeft", "eyeBlinkRight"};
        for (int side = 0; side < 2; ++side)
        {
            const int shape = ShapeIndex(kLids[side]);
            if (shape == kShapeNone || m_clipTouched[shape])
                continue;
            if (closed > m_weights[shape])
                m_weights[shape] = closed;
        }
        if (closed > 0.0f)
            m_active = true;
    }

    void Layer::ApplyGaze(float deltaSeconds)
    {
        // Runs even when inactive so ClearGaze eases back to centre.
        const float blend = Clamp01(kGazeSmoothing * deltaSeconds);
        m_gazeYawSmoothed   += (m_gazeYaw - m_gazeYawSmoothed) * blend;
        m_gazePitchSmoothed += (m_gazePitch - m_gazePitchSmoothed) * blend;

        const float yaw = m_gazeYawSmoothed;
        const float pitch = m_gazePitchSmoothed;
        if (yaw == 0.0f && pitch == 0.0f)
            return;

        // ARKit names these relative to the nose: looking left takes the left
        // eye OUT and the right eye IN.
        const int lookOutLeft  = ShapeIndex("eyeLookOutLeft");
        const int lookInLeft   = ShapeIndex("eyeLookInLeft");
        const int lookOutRight = ShapeIndex("eyeLookOutRight");
        const int lookInRight  = ShapeIndex("eyeLookInRight");
        const int lookUpLeft   = ShapeIndex("eyeLookUpLeft");
        const int lookUpRight  = ShapeIndex("eyeLookUpRight");
        const int lookDownLeft = ShapeIndex("eyeLookDownLeft");
        const int lookDownRight = ShapeIndex("eyeLookDownRight");

        const float horizontal = yaw < 0.0f ? -yaw : yaw;
        const int outward = yaw > 0.0f ? lookOutLeft : lookInLeft;
        const int inward  = yaw > 0.0f ? lookInRight : lookOutRight;
        if (outward != kShapeNone) m_weights[outward] = horizontal;
        if (inward  != kShapeNone) m_weights[inward]  = horizontal;

        const float vertical = pitch < 0.0f ? -pitch : pitch;
        const int upDownLeft  = pitch > 0.0f ? lookUpLeft : lookDownLeft;
        const int upDownRight = pitch > 0.0f ? lookUpRight : lookDownRight;
        if (upDownLeft  != kShapeNone) m_weights[upDownLeft]  = vertical;
        if (upDownRight != kShapeNone) m_weights[upDownRight] = vertical;

        m_active = true;
    }

    void Layer::Update(float deltaSeconds, float audioSeconds)
    {
        if (deltaSeconds < 0.0f)
            deltaSeconds = 0.0f;

        if (m_poseSpeed > 0.0f)
        {
            const float blend = Clamp01(m_poseSpeed * deltaSeconds);
            for (int shape = 0; shape < kShapeCount; ++shape)
                m_poseCurrent[shape] += (m_poseTarget[shape] - m_poseCurrent[shape]) * blend;
        }

        ApplyPose();
        memset(m_clipTouched, 0, sizeof(m_clipTouched));

        if (m_clipPlaying)
        {
            if (audioSeconds >= 0.0f)
                m_clipTime = audioSeconds;
            else
                m_clipTime += deltaSeconds;

            SampleClip(m_clip, m_clipTime, m_clipLoop, m_weights, kShapeCount, m_clipTouched);
            if (!m_clipLoop && m_clipTime >= m_clip.Duration())
                StopClip();
        }

        ApplyBlink(deltaSeconds);
        ApplyGaze(deltaSeconds);
    }
}
