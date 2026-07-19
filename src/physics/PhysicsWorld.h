#pragma once

#include "physics/PhysicsTypes.h"

#include <vector>

// A thin wrapper over Bullet 2.82, shared by the editor preview and the Xbox 360
// runtime so both simulate with identical code. The header hides Bullet entirely
// (opaque impl), so only PhysicsWorld.cpp needs the Bullet include path; callers
// just see PhysicsTypes. Strict C++03 (compiled by the XDK toolset).

namespace phys
{
    class PhysicsWorld
    {
    public:
        PhysicsWorld();
        ~PhysicsWorld();

        // Build a fresh world from the given descriptors (clears any previous).
        void Build(const std::vector<BodyDesc>& bodies);

        // Advance the simulation by dt seconds (fixed 1/60 internal sub-steps).
        void Step(float dt);

        // World matrices (rotation + translation, no scale) for every non-static
        // body, keyed by objectIndex. Static bodies keep their authored transform
        // and are not reported.
        void ReadPoses(std::vector<Pose>& out) const;

        // Enter/exit events for trigger volumes since the previous call.
        void DrainTriggerEvents(std::vector<TriggerEvent>& out);

        // --- Scripting verbs: mutate/query a body by its objectIndex. All are
        // no-ops / return false if the object has no rigid body. ---

        // Apply an instantaneous central impulse (the jump). Wakes the body.
        void ApplyImpulse(int objectIndex, float x, float y, float z);
        // Set / get linear velocity (player movement).
        void SetLinearVelocity(int objectIndex, float x, float y, float z);
        bool GetLinearVelocity(int objectIndex, float out[3]) const;
        // Teleport / drive: set world position + rotation (degrees, X*Y*Z). For a
        // Kinematic body this is how a script moves it each frame.
        void SetTransform(int objectIndex, const float pos[3], const float rotDeg[3]);
        // Current world position of the body.
        bool GetPosition(int objectIndex, float out[3]) const;

        // Destroy the world and all bodies/shapes.
        void Clear();

        bool Empty() const;

    private:
        struct Impl;
        Impl* m_impl;

        PhysicsWorld(const PhysicsWorld&);            // non-copyable
        PhysicsWorld& operator=(const PhysicsWorld&);
    };
}
