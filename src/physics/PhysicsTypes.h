#pragma once

// Neutral, dependency-light physics descriptors shared by BOTH the PC editor
// and the Xbox 360 runtime. This header is compiled by the XDK toolset, so it
// must stay strict C++03: no default member initializers, no enum class, no
// nullptr, no <cstdint>. Defaults are set in constructors.
//
// The editor (from ObjectAttribute) and the runtime (from RtAttribute) each map
// their own attribute struct into these descriptors, then hand them to the same
// PhysicsWorld — so both targets simulate with identical code.

namespace phys
{
    // One rigid body OR one trigger volume to create in the world.
    struct BodyDesc
    {
        enum Kind  { Static = 0, Dynamic = 1, Kinematic = 2 };
        // Primitive shapes use halfExtents; MeshConvex/MeshExact use the mesh*
        // fields below. MeshExact (concave triangle mesh) is Static-only — Build
        // forces mass 0 for it.
        enum Shape { Box = 0, Sphere = 1, Capsule = 2, Cylinder = 3,
                     MeshConvex = 4, MeshExact = 5 };

        int   kind;            // Kind
        int   shape;           // Shape
        // halfExtents by shape (all Y-up):
        //   Box      -> [0],[1],[2] = half-extents
        //   Sphere   -> [0] = radius
        //   Capsule  -> [0] = radius, [1] = half-height of the cylindrical middle
        //   Cylinder -> [0] = radius, [1] = half-height
        float halfExtents[3];

        // Mesh geometry, only for MeshConvex / MeshExact. Pointers are valid ONLY
        // during the Build() call — PhysicsWorld copies whatever it needs.
        // Positions: 3 floats per vertex. Indices: triangle list (3 per triangle).
        const float*        meshVerts;
        int                 meshVertCount;   // vertex count (positions = 3*count)
        const unsigned int* meshIndices;     // MeshExact only
        int                 meshIndexCount;  // index count (MeshExact only)
        float mass;            // Dynamic only (Static/Kinematic force mass 0)
        float linDamping;
        float angDamping;
        float restitution;     // bounciness, 0 = dead .. 1 = fully elastic
        float friction;        // contact friction (Bullet default 0.5)
        bool  gravity;         // false = this body ignores world gravity
        float gravityScale;    // multiplier on world gravity (1 = normal, 2 = 2x
                               // faster fall, 0 = float, <0 = drift up). Applied
                               // only when gravity is true.
        bool  isTrigger;       // true = ghost/overlap-only, no contact response

        float pos[3];          // initial world position (authored)
        float rotEulerDeg[3];  // initial world rotation, degrees, X*Y*Z order

        int   objectIndex;     // index of the SceneObject this maps to

        BodyDesc()
        {
            kind = Static;
            shape = Box;
            halfExtents[0] = halfExtents[1] = halfExtents[2] = 0.5f;
            mass = 1.0f;
            linDamping = 0.0f;
            angDamping = 0.0f;
            restitution = 0.0f;
            friction = 0.5f;
            gravity = true;
            gravityScale = 1.0f;
            isTrigger = false;
            pos[0] = pos[1] = pos[2] = 0.0f;
            rotEulerDeg[0] = rotEulerDeg[1] = rotEulerDeg[2] = 0.0f;
            objectIndex = -1;
            meshVerts = 0;
            meshVertCount = 0;
            meshIndices = 0;
            meshIndexCount = 0;
        }
    };

    // A trigger overlap change since the previous step.
    struct TriggerEvent
    {
        int  triggerObjectIndex;
        int  otherObjectIndex;
        bool entered;          // true = entered, false = exited

        TriggerEvent() : triggerObjectIndex(-1), otherObjectIndex(-1), entered(false) {}
    };

    // A simulated body's world transform read back after a step. `matrix` is a
    // row-major, row-vector (D3DMATRIX) 4x4 holding rotation + translation only,
    // NO scale — the consumer composes world = Scaling(authoredScale) * matrix,
    // exactly as it composes an authored object's world matrix.
    struct Pose
    {
        int   objectIndex;
        float matrix[16];

        Pose()
        {
            objectIndex = -1;
            for (int i = 0; i < 16; ++i) matrix[i] = 0.0f;
            matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
        }
    };
}
