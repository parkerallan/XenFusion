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

        // Add one body/trigger to the live world, or remove everything belonging
        // to an object — how a spawned object joins the simulation and a
        // destroyed one leaves it. Creates the world if there isn't one yet.
        bool AddBody(const BodyDesc& desc);
        void RemoveBody(int objectIndex);

        // Advance the simulation by dt seconds (fixed 1/60 internal sub-steps).
        void Step(float dt);

        // World matrices (rotation + translation, no scale) for every non-static
        // body, keyed by objectIndex. Static bodies keep their authored transform
        // and are not reported.
        void ReadPoses(std::vector<Pose>& out) const;

        // Enter/stay/exit events for trigger volumes since the previous call.
        void DrainTriggerEvents(std::vector<TriggerEvent>& out);

        // Enter/stay/exit contacts between non-trigger bodies since the previous
        // call. Each pair is reported once per phase change, lowest index first.
        void DrainCollisionEvents(std::vector<CollisionEvent>& out);

        // Animated bone boxes are added after Build and driven every frame.
        // Returns a stable handle, or -1 when no world exists.
        int AddBoneCollider(int ownerObjectIndex, unsigned int boneHash,
                    const float halfExtents[3]);
        void UpdateBoneCollider(int handle, const float worldMatrix[16]);

        // --- Scripting verbs: mutate/query a body by its objectIndex. All are
        // no-ops / return false if the object has no rigid body. ---

        // True when this object has a rigid body in the world. Callers use it to
        // decide whether a transform write belongs to physics or to the scene.
        bool HasBody(int objectIndex) const;

        // Apply an instantaneous central impulse (the jump). Wakes the body.
        void ApplyImpulse(int objectIndex, float x, float y, float z);
        // Apply a continuous central force / torque for this step (scale by dt
        // yourself if you want frame-rate independence). Wakes the body.
        void ApplyForce(int objectIndex, float x, float y, float z);
        void ApplyTorque(int objectIndex, float x, float y, float z);
        // Set / get linear velocity (player movement).
        void SetLinearVelocity(int objectIndex, float x, float y, float z);
        bool GetLinearVelocity(int objectIndex, float out[3]) const;
        // Set / get angular velocity, radians per second about each world axis.
        void SetAngularVelocity(int objectIndex, float x, float y, float z);
        bool GetAngularVelocity(int objectIndex, float out[3]) const;
        // Teleport / drive: set world position + rotation (degrees, X*Y*Z). For a
        // Kinematic body this is how a script moves it each frame.
        void SetTransform(int objectIndex, const float pos[3], const float rotDeg[3]);
        // Current world position / rotation (degrees, X*Y*Z) of the body.
        // GetRotation returns the CANONICAL triple for the orientation: Y is
        // recovered through asin, so it always lands in [-90, 90]. An object
        // authored at y=-120 reads back as an equivalent triple with different
        // numbers — compare orientations, not the angles themselves.
        bool GetPosition(int objectIndex, float out[3]) const;
        bool GetRotation(int objectIndex, float outDeg[3]) const;

        // Closest hit along a ray, ignoring trigger volumes and bone colliders.
        // `dir` need not be normalized. Returns false when nothing is hit.
        bool Raycast(const float origin[3], const float dir[3], float maxDistance,
                     RayHit& out) const;

        // Destroy the world and all bodies/shapes.
        void Clear();

        bool Empty() const;

    private:
        void CreateWorld(); // allocate the Bullet world if it doesn't exist yet

        struct Impl;
        Impl* m_impl;

        PhysicsWorld(const PhysicsWorld&);            // non-copyable
        PhysicsWorld& operator=(const PhysicsWorld&);
    };
}
