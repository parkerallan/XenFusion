#include "physics/PhysicsWorld.h"

#include <cassert>
#include <cmath>
#include <vector>

// Covers the world queries the Lua `physics` table and the object handle added:
// PhysicsWorld::Raycast (including that it ignores trigger ghosts) and
// GetRotation, whose Euler extraction is hand-derived as the inverse of
// EulerRowVec3x3 and so is the piece most likely to be silently wrong.

namespace
{
    bool Near(float left, float right, float tol = 1.0e-3f)
    {
        return std::fabs(left - right) < tol;
    }

    // Degrees compare modulo a full turn: atan2 can land on either end of its
    // branch cut, so +180 and -180 are the same angle.
    bool NearAngle(float left, float right, float tol = 1.0e-2f)
    {
        float diff = std::fmod(left - right, 360.0f);
        if (diff >  180.0f) diff -= 360.0f;
        if (diff < -180.0f) diff += 360.0f;
        return std::fabs(diff) < tol;
    }

    phys::BodyDesc Box(int objectIndex, float x, float y, float z, float half)
    {
        phys::BodyDesc d;
        d.kind = phys::BodyDesc::Static;
        d.shape = phys::BodyDesc::Box;
        d.halfExtents[0] = d.halfExtents[1] = d.halfExtents[2] = half;
        d.pos[0] = x; d.pos[1] = y; d.pos[2] = z;
        d.objectIndex = objectIndex;
        return d;
    }

    void TestRaycast()
    {
        std::vector<phys::BodyDesc> bodies;
        bodies.push_back(Box(7, 0.0f, 0.0f, 0.0f, 1.0f));

        // A trigger volume straddling the ray, above the box: a raycast asking
        // "what solid thing is in front of me" must shoot straight through it.
        phys::BodyDesc trigger = Box(9, 0.0f, 3.0f, 0.0f, 0.5f);
        trigger.isTrigger = true;
        bodies.push_back(trigger);

        phys::PhysicsWorld world;
        world.Build(bodies);

        const float down[3] = { 0.0f, -1.0f, 0.0f };
        const float from[3] = { 0.0f, 5.0f, 0.0f };

        phys::RayHit hit;
        assert(world.Raycast(from, down, 100.0f, hit));
        assert(hit.objectIndex == 7);           // the box, not the trigger
        assert(Near(hit.point[1], 1.0f));       // top face
        assert(Near(hit.distance, 4.0f));
        assert(Near(hit.normal[1], 1.0f));      // points back at the origin

        // Direction need not be normalized.
        const float longDown[3] = { 0.0f, -25.0f, 0.0f };
        phys::RayHit scaled;
        assert(world.Raycast(from, longDown, 100.0f, scaled));
        assert(Near(scaled.distance, 4.0f));

        // Too short to reach, and aimed away from anything.
        phys::RayHit missed;
        assert(!world.Raycast(from, down, 2.0f, missed));
        const float aside[3] = { 10.0f, 5.0f, 0.0f };
        assert(!world.Raycast(aside, down, 100.0f, missed));

        // A degenerate direction is a miss, not a crash.
        const float zero[3] = { 0.0f, 0.0f, 0.0f };
        assert(!world.Raycast(from, zero, 100.0f, missed));
    }

    // Build a body at the given authored rotation and read it straight back:
    // Build -> MakeTransformPR -> GetRotation must be the identity.
    void CheckRotationRoundTrip(float rx, float ry, float rz)
    {
        phys::BodyDesc d = Box(3, 0.0f, 0.0f, 0.0f, 0.5f);
        d.kind = phys::BodyDesc::Kinematic;
        d.rotEulerDeg[0] = rx; d.rotEulerDeg[1] = ry; d.rotEulerDeg[2] = rz;

        std::vector<phys::BodyDesc> bodies;
        bodies.push_back(d);
        phys::PhysicsWorld world;
        world.Build(bodies);

        float out[3] = { 0.0f, 0.0f, 0.0f };
        assert(world.GetRotation(3, out));
        assert(NearAngle(out[0], rx));
        assert(NearAngle(out[1], ry));
        assert(NearAngle(out[2], rz));
    }

    void TestRotation()
    {
        // Y within [-90, 90] is what asin can recover verbatim; outside that the
        // orientation is right but the triple is a different, equivalent one.
        CheckRotationRoundTrip(0.0f, 0.0f, 0.0f);
        CheckRotationRoundTrip(10.0f, 20.0f, 30.0f);
        CheckRotationRoundTrip(-45.0f, 0.0f, 0.0f);
        CheckRotationRoundTrip(0.0f, 89.0f, 0.0f);
        CheckRotationRoundTrip(0.0f, -60.0f, 15.0f);
        CheckRotationRoundTrip(90.0f, 0.0f, 0.0f);
        CheckRotationRoundTrip(179.0f, -30.0f, -179.0f);

        // Out-of-range Y: the reported triple must at least be a fixed point, so
        // feeding a script's own rotation() back through set_rotation is stable
        // rather than drifting a little further every frame.
        {
            phys::BodyDesc d = Box(3, 0.0f, 0.0f, 0.0f, 0.5f);
            d.kind = phys::BodyDesc::Kinematic;
            d.rotEulerDeg[0] = 0.0f; d.rotEulerDeg[1] = -120.0f; d.rotEulerDeg[2] = 15.0f;
            std::vector<phys::BodyDesc> bodies;
            bodies.push_back(d);
            phys::PhysicsWorld world;
            world.Build(bodies);

            float first[3] = { 0.0f, 0.0f, 0.0f };
            assert(world.GetRotation(3, first));
            assert(first[1] >= -90.0f && first[1] <= 90.0f); // canonicalized

            const float origin[3] = { 0.0f, 0.0f, 0.0f };
            world.SetTransform(3, origin, first);
            float second[3] = { 0.0f, 0.0f, 0.0f };
            assert(world.GetRotation(3, second));
            for (int i = 0; i < 3; ++i)
                assert(NearAngle(first[i], second[i]));
        }

        // Gimbal lock folds Z into X (the two axes coincide at |y| = 90), so the
        // recovered pair must preserve their difference.
        {
            phys::BodyDesc d = Box(3, 0.0f, 0.0f, 0.0f, 0.5f);
            d.kind = phys::BodyDesc::Kinematic;
            d.rotEulerDeg[0] = 20.0f; d.rotEulerDeg[1] = 90.0f; d.rotEulerDeg[2] = 40.0f;
            std::vector<phys::BodyDesc> bodies;
            bodies.push_back(d);
            phys::PhysicsWorld world;
            world.Build(bodies);

            float out[3] = { 0.0f, 0.0f, 0.0f };
            assert(world.GetRotation(3, out));
            assert(NearAngle(out[1], 90.0f));
            assert(NearAngle(out[2], 0.0f));
            assert(NearAngle(out[0], 20.0f - 40.0f));
        }

    }

    void TestHasBody()
    {
        std::vector<phys::BodyDesc> bodies;
        bodies.push_back(Box(4, 0.0f, 0.0f, 0.0f, 1.0f));
        phys::BodyDesc trigger = Box(5, 4.0f, 0.0f, 0.0f, 1.0f);
        trigger.isTrigger = true;
        bodies.push_back(trigger);

        phys::PhysicsWorld world;
        world.Build(bodies);

        assert(world.HasBody(4));
        assert(!world.HasBody(5));   // trigger volumes are ghosts, not bodies
        assert(!world.HasBody(99));  // and an unknown object has nothing
    }

    void TestCollisionEvents()
    {
        // A dynamic box resting on a static floor: enter on the first contact
        // step, stay afterwards.
        std::vector<phys::BodyDesc> bodies;
        bodies.push_back(Box(1, 0.0f, -1.0f, 0.0f, 1.0f)); // floor, top at y=0

        phys::BodyDesc faller = Box(2, 0.0f, 1.02f, 0.0f, 1.0f);
        faller.kind = phys::BodyDesc::Dynamic;
        faller.mass = 1.0f;
        bodies.push_back(faller);

        phys::PhysicsWorld world;
        world.Build(bodies);

        std::vector<phys::CollisionEvent> events;
        bool sawEnter = false, sawStay = false;
        for (int step = 0; step < 120; ++step)
        {
            world.Step(1.0f / 60.0f);
            world.DrainCollisionEvents(events);
            for (size_t i = 0; i < events.size(); ++i)
            {
                assert(events[i].aObjectIndex == 1); // reported lowest index first
                assert(events[i].bObjectIndex == 2);
                if (events[i].phase == phys::PhaseEnter)
                {
                    assert(!sawEnter);               // enter fires exactly once
                    sawEnter = true;
                }
                else if (events[i].phase == phys::PhaseStay)
                {
                    assert(sawEnter);                // never a stay before an enter
                    sawStay = true;
                }
            }
        }
        assert(sawEnter);
        assert(sawStay);
    }
}

int main()
{
    TestRaycast();
    TestRotation();
    TestHasBody();
    TestCollisionEvents();
    return 0;
}
