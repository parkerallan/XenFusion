#include "physics/PhysicsWorld.h"

#include "btBulletDynamicsCommon.h"
#include "BulletCollision/CollisionDispatch/btGhostObject.h"

#include <map>
#include <math.h>
#include <utility>

// Bullet 2.82 wrapper. Shared verbatim by the editor and the Xbox 360 runtime.
// Strict C++03. The SIMD backend is chosen by btScalar.h from the platform
// macros (MSVC -> SSE, XDK's _XBOX -> VMX128); nothing here forces it. We never
// touch Bullet's serialization, so this stays endian-clean on big-endian PPC.

namespace phys
{
    namespace
    {
        // Build the renderer's row-vector rotation 3x3 (RotX*RotY*RotZ, degrees)
        // into r[row][col], matching RtMath / SceneRenderer::ComposeWorld exactly.
        void EulerRowVec3x3(const float rotDeg[3], float r[3][3])
        {
            const float d2r = 3.14159265358979323846f / 180.0f;
            const float ax = rotDeg[0] * d2r, ay = rotDeg[1] * d2r, az = rotDeg[2] * d2r;
            const float cx = cosf(ax), sx = sinf(ax);
            const float cy = cosf(ay), sy = sinf(ay);
            const float cz = cosf(az), sz = sinf(az);

            // RotationX: rows {1,0,0}{0,c,s}{0,-s,c}
            float X[3][3] = { {1,0,0}, {0,cx,sx}, {0,-sx,cx} };
            // RotationY: rows {c,0,-s}{0,1,0}{s,0,c}
            float Y[3][3] = { {cy,0,-sy}, {0,1,0}, {sy,0,cy} };
            // RotationZ: rows {c,s,0}{-s,c,0}{0,0,1}
            float Z[3][3] = { {cz,sz,0}, {-sz,cz,0}, {0,0,1} };

            float XY[3][3];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    XY[i][j] = X[i][0]*Y[0][j] + X[i][1]*Y[1][j] + X[i][2]*Y[2][j];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    r[i][j] = XY[i][0]*Z[0][j] + XY[i][1]*Z[1][j] + XY[i][2]*Z[2][j];
        }

        // Initial world transform for a body: Bullet basis is the transpose of the
        // renderer's row-vector rotation, so a read-back before any step reproduces
        // the authored orientation exactly (no pop at Play).
        btTransform MakeTransformPR(const float pos[3], const float rotDeg[3])
        {
            float r[3][3];
            EulerRowVec3x3(rotDeg, r);
            btMatrix3x3 basis(r[0][0], r[1][0], r[2][0],
                              r[0][1], r[1][1], r[2][1],
                              r[0][2], r[1][2], r[2][2]);
            btTransform xf;
            xf.setBasis(basis);
            xf.setOrigin(btVector3(pos[0], pos[1], pos[2]));
            return xf;
        }

        btTransform MakeTransform(const BodyDesc& d)
        {
            return MakeTransformPR(d.pos, d.rotEulerDeg);
        }

        btTransform MatrixToTransform(const float matrix[16])
        {
            btMatrix3x3 basis(matrix[0], matrix[4], matrix[8],
                              matrix[1], matrix[5], matrix[9],
                              matrix[2], matrix[6], matrix[10]);
            return btTransform(basis, btVector3(matrix[12], matrix[13], matrix[14]));
        }

        // Override Bullet's default material combine (which MULTIPLIES the two
        // bodies' restitution/friction, so both must be non-zero) with MAX — the
        // higher value wins, so setting bounce/friction on just the object that
        // should bounce or grip behaves as expected.
        bool PhysMaterialCombine(btManifoldPoint& cp,
                                 const btCollisionObjectWrapper* w0, int, int,
                                 const btCollisionObjectWrapper* w1, int, int)
        {
            const btCollisionObject* b0 = w0->getCollisionObject();
            const btCollisionObject* b1 = w1->getCollisionObject();
            const btScalar r0 = b0->getRestitution(), r1 = b1->getRestitution();
            const btScalar f0 = b0->getFriction(),    f1 = b1->getFriction();
            cp.m_combinedRestitution = (r0 > r1) ? r0 : r1;
            cp.m_combinedFriction    = (f0 > f1) ? f0 : f1;
            return true;
        }

        // Closest-hit ray callback that ignores ghosts. Trigger volumes and bone
        // colliders are btGhostObjects with no contact response — a script asking
        // "what is in front of me" means solid geometry, not a trigger it is
        // standing inside.
        struct SolidRayCallback : public btCollisionWorld::ClosestRayResultCallback
        {
            SolidRayCallback(const btVector3& from, const btVector3& to)
                : btCollisionWorld::ClosestRayResultCallback(from, to) {}

            virtual bool needsCollision(btBroadphaseProxy* proxy0) const
            {
                const btCollisionObject* o = (const btCollisionObject*)proxy0->m_clientObject;
                if (o && o->getInternalType() == btCollisionObject::CO_GHOST_OBJECT)
                    return false;
                return btCollisionWorld::ClosestRayResultCallback::needsCollision(proxy0);
            }
        };

        // Write a Bullet transform back into a row-major, row-vector D3DMATRIX-style
        // float[16] (rotation + translation, no scale). Inverse of MakeTransform's
        // basis mapping: D3D 3x3 = transpose(Bullet basis).
        void TransformToMatrix(const btTransform& xf, float m[16])
        {
            const btMatrix3x3& b = xf.getBasis();
            const btVector3& o = xf.getOrigin();
            for (int i = 0; i < 16; ++i) m[i] = 0.0f;
            for (int i = 0; i < 3; ++i)      // Bullet row
                for (int k = 0; k < 3; ++k)  // Bullet col
                    m[k * 4 + i] = b[i][k];
            m[12] = o.x(); m[13] = o.y(); m[14] = o.z(); m[15] = 1.0f;
        }
    }

    struct PhysicsWorld::Impl
    {
        btDefaultCollisionConfiguration*     config;
        btCollisionDispatcher*               dispatcher;
        btDbvtBroadphase*                    broadphase;
        btSequentialImpulseConstraintSolver* solver;
        btDiscreteDynamicsWorld*             world;
        btGhostPairCallback*                 ghostCb;

        std::vector<btCollisionShape*>   shapes;
        std::vector<btDefaultMotionState*> motionStates;
        std::vector<btRigidBody*>        rigidBodies;

        struct ReadRec { btRigidBody* body; int objectIndex; };
        std::vector<ReadRec> readback;

        struct TriggerRec { btGhostObject* ghost; int objectIndex; unsigned int boneHash; };
        std::vector<TriggerRec> triggers;

        struct BoneRec { btGhostObject* object; };
        std::vector<BoneRec> boneColliders;

        // Stable storage for MeshExact colliders: btBvhTriangleMeshShape references
        // the interface, which references these vertex/index buffers — all must
        // outlive the shape (freed in Clear).
        std::vector<btTriangleIndexVertexArray*> meshIfaces;
        std::vector<std::vector<float>*>         meshVertStore;
        std::vector<std::vector<int>*>           meshIndexStore;

        // Per-trigger last-seen overlap sets (index into triggers[]) -> set of
        // other object indices, for enter/exit diffing.
        std::vector<std::map<int, char> > lastOverlap;

        // collision object -> objectIndex (for resolving overlap partners).
        std::map<const btCollisionObject*, int> objIndexOf;

        // objectIndex -> rigid body (for scripting verbs to find a body by index).
        std::map<int, btRigidBody*> bodyByIndex;
        std::map<int, char> staticBodyByIndex;

        // Last step's contacting body pairs (lowest object index first), for the
        // same enter/exit diffing DrainTriggerEvents does with lastOverlap.
        std::map<std::pair<int, int>, char> lastContacts;
        Impl()
                        : config(0), dispatcher(0), broadphase(0), solver(0), world(0), ghostCb(0) {}
    };

    PhysicsWorld::PhysicsWorld() : m_impl(new Impl()) {}
    PhysicsWorld::~PhysicsWorld() { Clear(); delete m_impl; }

    bool PhysicsWorld::Empty() const
    {
        return m_impl->world == 0 ||
               (m_impl->rigidBodies.empty() && m_impl->triggers.empty());
    }

    void PhysicsWorld::Clear()
    {
        Impl& s = *m_impl;
        if (s.world)
        {
            for (size_t i = 0; i < s.triggers.size(); ++i)
                s.world->removeCollisionObject(s.triggers[i].ghost);
            for (size_t i = 0; i < s.rigidBodies.size(); ++i)
                s.world->removeRigidBody(s.rigidBodies[i]);
        }
        for (size_t i = 0; i < s.triggers.size(); ++i)   delete s.triggers[i].ghost;
        for (size_t i = 0; i < s.rigidBodies.size(); ++i) delete s.rigidBodies[i];
        for (size_t i = 0; i < s.motionStates.size(); ++i) delete s.motionStates[i];
        for (size_t i = 0; i < s.shapes.size(); ++i)      delete s.shapes[i];
        // Mesh interfaces reference the vertex/index stores — delete after shapes.
        for (size_t i = 0; i < s.meshIfaces.size(); ++i)     delete s.meshIfaces[i];
        for (size_t i = 0; i < s.meshVertStore.size(); ++i)  delete s.meshVertStore[i];
        for (size_t i = 0; i < s.meshIndexStore.size(); ++i) delete s.meshIndexStore[i];
        s.meshIfaces.clear();
        s.meshVertStore.clear();
        s.meshIndexStore.clear();
        s.triggers.clear();
        s.rigidBodies.clear();
        s.motionStates.clear();
        s.shapes.clear();
        s.readback.clear();
        s.lastOverlap.clear();
        s.objIndexOf.clear();
        s.bodyByIndex.clear();
        s.staticBodyByIndex.clear();
        s.lastContacts.clear();
        s.boneColliders.clear();

        delete s.world;      s.world = 0;
        delete s.solver;     s.solver = 0;
        delete s.broadphase; s.broadphase = 0;
        delete s.dispatcher; s.dispatcher = 0;
        delete s.config;     s.config = 0;
        delete s.ghostCb;    s.ghostCb = 0;
    }

    void PhysicsWorld::CreateWorld()
    {
        Impl& s = *m_impl;
        if (s.world) return;

        s.config     = new btDefaultCollisionConfiguration();
        s.dispatcher = new btCollisionDispatcher(s.config);
        s.broadphase = new btDbvtBroadphase();
        s.solver     = new btSequentialImpulseConstraintSolver();
        s.world      = new btDiscreteDynamicsWorld(s.dispatcher, s.broadphase, s.solver, s.config);
        s.world->setGravity(btVector3(0.0f, -9.81f, 0.0f));

        // Max-combine restitution/friction (see PhysMaterialCombine). Bodies opt in
        // via CF_CUSTOM_MATERIAL_CALLBACK below; the callback fires if either body
        // in a contact has the flag.
        gContactAddedCallback = PhysMaterialCombine;

        // Required for btGhostObject to accumulate overlapping pairs.
        s.ghostCb = new btGhostPairCallback();
        s.broadphase->getOverlappingPairCache()->setInternalGhostPairCallback(s.ghostCb);
    }

    void PhysicsWorld::Build(const std::vector<BodyDesc>& bodies)
    {
        Clear();
        CreateWorld();
        for (size_t i = 0; i < bodies.size(); ++i)
            AddBody(bodies[i]);
    }

    bool PhysicsWorld::AddBody(const BodyDesc& desc)
    {
        Impl& s = *m_impl;
        CreateWorld(); // spawning into an empty world is legal
        {
            const BodyDesc& d = desc;

            // MeshExact is concave -> Static only (Bullet can't run it dynamic).
            bool forceStatic = false;

            btCollisionShape* shape = 0;
            if (d.shape == BodyDesc::Sphere)
                shape = new btSphereShape(d.halfExtents[0]);
            else if (d.shape == BodyDesc::Capsule)
                // btCapsuleShape(radius, height): height = cylindrical section
                // length between the two hemisphere centers = 2 * half-height.
                shape = new btCapsuleShape(d.halfExtents[0], d.halfExtents[1] * 2.0f);
            else if (d.shape == BodyDesc::Cylinder)
                // Y-up cylinder: half-extents (radius, half-height, radius).
                shape = new btCylinderShape(btVector3(d.halfExtents[0], d.halfExtents[1], d.halfExtents[0]));
            else if (d.shape == BodyDesc::MeshConvex && d.meshVerts && d.meshVertCount > 0)
            {
                // Convex hull of the mesh vertices. The (points, count, stride)
                // constructor copies the points and computes the local AABB.
                shape = new btConvexHullShape(d.meshVerts, d.meshVertCount, 3 * (int)sizeof(float));
            }
            else if (d.shape == BodyDesc::MeshExact && d.meshVerts && d.meshIndices && d.meshIndexCount >= 3)
            {
                // Exact concave triangle mesh — Static only. The interface
                // references heap copies kept alive until Clear.
                std::vector<float>* vs = new std::vector<float>(d.meshVerts, d.meshVerts + (size_t)d.meshVertCount * 3);
                std::vector<int>*   is = new std::vector<int>((size_t)d.meshIndexCount);
                for (int k = 0; k < d.meshIndexCount; ++k) (*is)[k] = (int)d.meshIndices[k];
                s.meshVertStore.push_back(vs);
                s.meshIndexStore.push_back(is);
                btTriangleIndexVertexArray* iface = new btTriangleIndexVertexArray(
                    d.meshIndexCount / 3, &(*is)[0], 3 * (int)sizeof(int),
                    d.meshVertCount,      &(*vs)[0], 3 * (int)sizeof(float));
                s.meshIfaces.push_back(iface);
                shape = new btBvhTriangleMeshShape(iface, true);
                forceStatic = true;
            }
            else if (d.shape == BodyDesc::Box || d.shape < BodyDesc::MeshConvex)
                shape = new btBoxShape(btVector3(d.halfExtents[0], d.halfExtents[1], d.halfExtents[2]));
            else
                // Mesh shape requested but no geometry supplied — safe fallback.
                shape = new btBoxShape(btVector3(0.5f, 0.5f, 0.5f));
            s.shapes.push_back(shape);

            const btTransform xf = MakeTransform(d);

            if (d.isTrigger)
            {
                btGhostObject* ghost = new btGhostObject();
                ghost->setCollisionShape(shape);
                ghost->setWorldTransform(xf);
                ghost->setCollisionFlags(ghost->getCollisionFlags() |
                                         btCollisionObject::CF_NO_CONTACT_RESPONSE);
                s.world->addCollisionObject(ghost);

                Impl::TriggerRec tr; tr.ghost = ghost; tr.objectIndex = d.objectIndex; tr.boneHash = 0;
                s.triggers.push_back(tr);
                s.lastOverlap.resize(s.triggers.size());
                s.objIndexOf[ghost] = d.objectIndex;
                return true;
            }

            const bool isDynamic = (d.kind == BodyDesc::Dynamic) && !forceStatic;
            const btScalar mass  = isDynamic ? (btScalar)d.mass : (btScalar)0.0;
            btVector3 inertia(0.0f, 0.0f, 0.0f);
            if (mass > 0.0f)
                shape->calculateLocalInertia(mass, inertia);

            btDefaultMotionState* ms = new btDefaultMotionState(xf);
            s.motionStates.push_back(ms);

            btRigidBody::btRigidBodyConstructionInfo ci(mass, ms, shape, inertia);
            ci.m_linearDamping  = d.linDamping;
            ci.m_angularDamping = d.angDamping;
            ci.m_restitution    = d.restitution;
            ci.m_friction       = d.friction;
            btRigidBody* body = new btRigidBody(ci);
            if (isDynamic)
                body->setAngularFactor(btVector3(d.lockRotation[0] ? 0.0f : 1.0f,
                                                 d.lockRotation[1] ? 0.0f : 1.0f,
                                                 d.lockRotation[2] ? 0.0f : 1.0f));

            // Opt into the max-combine material callback for this body's contacts.
            body->setCollisionFlags(body->getCollisionFlags() |
                                    btCollisionObject::CF_CUSTOM_MATERIAL_CALLBACK);

            if (d.kind == BodyDesc::Kinematic && !forceStatic)
            {
                body->setCollisionFlags(body->getCollisionFlags() |
                                        btCollisionObject::CF_KINEMATIC_OBJECT);
                body->setActivationState(DISABLE_DEACTIVATION);
            }

            s.world->addRigidBody(body);

            // Per-body gravity: world gravity * gravityScale (or 0 when disabled).
            // BT_DISABLE_WORLD_GRAVITY stops the world from overwriting it.
            {
                const btScalar gy = d.gravity ? (btScalar)(-9.81f * d.gravityScale) : (btScalar)0.0;
                body->setFlags(body->getFlags() | BT_DISABLE_WORLD_GRAVITY);
                body->setGravity(btVector3(0.0f, gy, 0.0f));
            }

            s.rigidBodies.push_back(body);
            s.objIndexOf[body] = d.objectIndex;
            s.bodyByIndex[d.objectIndex] = body;
            if (d.kind == BodyDesc::Static || forceStatic)
                s.staticBodyByIndex[d.objectIndex] = 1;
            if (d.kind != BodyDesc::Static && !forceStatic)
            {
                Impl::ReadRec rr; rr.body = body; rr.objectIndex = d.objectIndex;
                s.readback.push_back(rr);
            }
        }
        return true;
    }

    void PhysicsWorld::RemoveBody(int objectIndex)
    {
        Impl& s = *m_impl;
        if (!s.world) return;

        // Everything belonging to the object goes: it may own both a Rigid Body
        // and a Trigger Volume. Shapes and motion states are dropped from the
        // owning vectors too, so repeated spawn/destroy doesn't grow the world.
        std::map<int, btRigidBody*>::iterator bodyIt = s.bodyByIndex.find(objectIndex);
        if (bodyIt != s.bodyByIndex.end() && bodyIt->second)
        {
            btRigidBody* body = bodyIt->second;
            s.world->removeRigidBody(body);

            btCollisionShape*     shape = body->getCollisionShape();
            btDefaultMotionState* motion =
                (btDefaultMotionState*)body->getMotionState();

            for (size_t i = 0; i < s.rigidBodies.size(); ++i)
                if (s.rigidBodies[i] == body) { s.rigidBodies.erase(s.rigidBodies.begin() + i); break; }
            for (size_t i = 0; i < s.readback.size(); ++i)
                if (s.readback[i].body == body) { s.readback.erase(s.readback.begin() + i); break; }
            s.objIndexOf.erase(body);
            s.bodyByIndex.erase(bodyIt);
            s.staticBodyByIndex.erase(objectIndex);
            delete body;

            if (motion)
                for (size_t i = 0; i < s.motionStates.size(); ++i)
                    if (s.motionStates[i] == motion)
                    { delete motion; s.motionStates.erase(s.motionStates.begin() + i); break; }
            // Mesh shapes reference the interface/vertex stores, which are freed
            // only by Clear — so leave those alone and drop primitive shapes only.
            if (shape && !shape->isConcave())
                for (size_t i = 0; i < s.shapes.size(); ++i)
                    if (s.shapes[i] == shape)
                    { delete shape; s.shapes.erase(s.shapes.begin() + i); break; }
        }

        // Trigger ghosts (skipping bone colliders, which belong to the animation
        // system and are torn down with it).
        for (size_t t = s.triggers.size(); t-- > 0; )
        {
            if (s.triggers[t].objectIndex != objectIndex || s.triggers[t].boneHash != 0)
                continue;
            btGhostObject* ghost = s.triggers[t].ghost;
            s.world->removeCollisionObject(ghost);
            s.objIndexOf.erase(ghost);
            btCollisionShape* shape = ghost->getCollisionShape();
            delete ghost;
            if (shape && !shape->isConcave())
                for (size_t i = 0; i < s.shapes.size(); ++i)
                    if (s.shapes[i] == shape)
                    { delete shape; s.shapes.erase(s.shapes.begin() + i); break; }
            s.triggers.erase(s.triggers.begin() + t);
            if (t < s.lastOverlap.size()) s.lastOverlap.erase(s.lastOverlap.begin() + t);
        }

        // Any contact pair involving the object is stale now; dropping it means
        // the next drain reports no phantom exit for a body that no longer exists.
        for (std::map<std::pair<int, int>, char>::iterator it = s.lastContacts.begin();
             it != s.lastContacts.end(); )
        {
            if (it->first.first == objectIndex || it->first.second == objectIndex)
            {
                std::map<std::pair<int, int>, char>::iterator dead = it++;
                s.lastContacts.erase(dead);
            }
            else ++it;
        }
    }

    int PhysicsWorld::AddBoneCollider(int ownerObjectIndex, unsigned int boneHash,
                                      const float halfExtents[3])
    {
        Impl& s = *m_impl;
        if (!s.world) return -1;
        btBoxShape* shape = new btBoxShape(btVector3(
            halfExtents[0], halfExtents[1], halfExtents[2]));
        s.shapes.push_back(shape);
        btTransform transform;
        transform.setIdentity();
        Impl::BoneRec record;
        btGhostObject* ghost = new btGhostObject();
        ghost->setCollisionShape(shape);
        ghost->setWorldTransform(transform);
        ghost->setCollisionFlags(ghost->getCollisionFlags() |
                                 btCollisionObject::CF_NO_CONTACT_RESPONSE);
        s.world->addCollisionObject(ghost);
        Impl::TriggerRec triggerRecord;
        triggerRecord.ghost = ghost;
        triggerRecord.objectIndex = ownerObjectIndex;
        triggerRecord.boneHash = boneHash;
        s.triggers.push_back(triggerRecord);
        s.lastOverlap.push_back(std::map<int, char>());
        s.objIndexOf[ghost] = ownerObjectIndex;
        record.object = ghost;
        s.boneColliders.push_back(record);
        return (int)s.boneColliders.size() - 1;
    }

    void PhysicsWorld::UpdateBoneCollider(int handle, const float worldMatrix[16])
    {
        Impl& s = *m_impl;
        if (handle < 0 || (size_t)handle >= s.boneColliders.size() ||
            !s.boneColliders[(size_t)handle].object) return;
        const btTransform transform = MatrixToTransform(worldMatrix);
        Impl::BoneRec& record = s.boneColliders[(size_t)handle];
        record.object->setWorldTransform(transform);
        record.object->activate(true);
        if (s.world) s.world->updateSingleAabb(record.object);
    }

    void PhysicsWorld::Step(float dt)
    {
        if (!m_impl->world) return;
        // Fixed 1/60 internal steps; allow a few sub-steps to absorb hitches.
        m_impl->world->stepSimulation((btScalar)dt, 4, (btScalar)(1.0 / 60.0));
    }

    void PhysicsWorld::ReadPoses(std::vector<Pose>& out) const
    {
        const Impl& s = *m_impl;
        out.clear();
        out.reserve(s.readback.size());
        for (size_t i = 0; i < s.readback.size(); ++i)
        {
            btTransform xf;
            s.readback[i].body->getMotionState()->getWorldTransform(xf);
            Pose p;
            p.objectIndex = s.readback[i].objectIndex;
            TransformToMatrix(xf, p.matrix);
            out.push_back(p);
        }
    }

    void PhysicsWorld::DrainTriggerEvents(std::vector<TriggerEvent>& out)
    {
        Impl& s = *m_impl;
        out.clear();
        for (size_t t = 0; t < s.triggers.size(); ++t)
        {
            btGhostObject* ghost = s.triggers[t].ghost;
            std::map<int, char> current;
            const int n = ghost->getNumOverlappingObjects();
            for (int i = 0; i < n; ++i)
            {
                const btCollisionObject* other = ghost->getOverlappingObject(i);
                std::map<const btCollisionObject*, int>::const_iterator it =
                    s.objIndexOf.find(other);
                const int otherIndex = (it != s.objIndexOf.end()) ? it->second : -1;
                if (otherIndex == s.triggers[t].objectIndex) continue;
                current[otherIndex] = 1;
            }

            std::map<int, char>& last = s.lastOverlap[t];
            // Entered: in current, not in last. Still overlapping: in both.
            for (std::map<int, char>::const_iterator it = current.begin(); it != current.end(); ++it)
            {
                const bool wasThere = last.find(it->first) != last.end();
                TriggerEvent e;
                e.triggerObjectIndex = s.triggers[t].objectIndex;
                e.otherObjectIndex   = it->first;
                e.boneHash           = s.triggers[t].boneHash;
                e.entered            = !wasThere;
                e.phase              = wasThere ? PhaseStay : PhaseEnter;
                out.push_back(e);
            }
            // Exited: in last, not in current.
            for (std::map<int, char>::const_iterator it = last.begin(); it != last.end(); ++it)
            {
                if (current.find(it->first) == current.end())
                {
                    TriggerEvent e;
                    e.triggerObjectIndex = s.triggers[t].objectIndex;
                    e.otherObjectIndex   = it->first;
                    e.boneHash           = s.triggers[t].boneHash;
                    e.entered            = false;
                    e.phase              = PhaseExit;
                    out.push_back(e);
                }
            }
            last.swap(current);
        }
    }

    void PhysicsWorld::DrainCollisionEvents(std::vector<CollisionEvent>& out)
    {
        Impl& s = *m_impl;
        out.clear();
        if (!s.world || !s.dispatcher) { s.lastContacts.clear(); return; }

        // This step's touching pairs. Ghosts (trigger volumes and bone colliders)
        // also produce manifolds, but they report through DrainTriggerEvents —
        // skip them here so a trigger never looks like a collision.
        std::map<std::pair<int, int>, char> current;
        const int manifolds = s.dispatcher->getNumManifolds();
        for (int i = 0; i < manifolds; ++i)
        {
            const btPersistentManifold* m = s.dispatcher->getManifoldByIndexInternal(i);
            if (!m || m->getNumContacts() <= 0) continue;
            const btCollisionObject* b0 = m->getBody0();
            const btCollisionObject* b1 = m->getBody1();
            if (!b0 || !b1) continue;
            if (b0->getInternalType() == btCollisionObject::CO_GHOST_OBJECT ||
                b1->getInternalType() == btCollisionObject::CO_GHOST_OBJECT) continue;

            std::map<const btCollisionObject*, int>::const_iterator i0 = s.objIndexOf.find(b0);
            std::map<const btCollisionObject*, int>::const_iterator i1 = s.objIndexOf.find(b1);
            if (i0 == s.objIndexOf.end() || i1 == s.objIndexOf.end()) continue;
            int a = i0->second, b = i1->second;
            if (a == b) continue;
            if (a > b) { const int t = a; a = b; b = t; }
            current[std::make_pair(a, b)] = 1;
        }

        typedef std::map<std::pair<int, int>, char>::const_iterator PairIt;
        for (PairIt it = current.begin(); it != current.end(); ++it)
        {
            CollisionEvent e;
            e.aObjectIndex = it->first.first;
            e.bObjectIndex = it->first.second;
            e.phase = (s.lastContacts.find(it->first) != s.lastContacts.end())
                          ? PhaseStay : PhaseEnter;
            out.push_back(e);
        }
        for (PairIt it = s.lastContacts.begin(); it != s.lastContacts.end(); ++it)
        {
            if (current.find(it->first) != current.end()) continue;
            CollisionEvent e;
            e.aObjectIndex = it->first.first;
            e.bObjectIndex = it->first.second;
            e.phase        = PhaseExit;
            out.push_back(e);
        }
        s.lastContacts.swap(current);
    }

    // --- Scripting verbs -----------------------------------------------------

    bool PhysicsWorld::HasBody(int objectIndex) const
    {
        std::map<int, btRigidBody*>::const_iterator it = m_impl->bodyByIndex.find(objectIndex);
        return it != m_impl->bodyByIndex.end() && it->second != 0;
    }

    void PhysicsWorld::ApplyImpulse(int objectIndex, float x, float y, float z)
    {
        std::map<int, btRigidBody*>::iterator it = m_impl->bodyByIndex.find(objectIndex);
        if (it == m_impl->bodyByIndex.end() || !it->second) return;
        it->second->activate(true);
        it->second->applyCentralImpulse(btVector3(x, y, z));
    }

    void PhysicsWorld::ApplyForce(int objectIndex, float x, float y, float z)
    {
        std::map<int, btRigidBody*>::iterator it = m_impl->bodyByIndex.find(objectIndex);
        if (it == m_impl->bodyByIndex.end() || !it->second) return;
        it->second->activate(true);
        it->second->applyCentralForce(btVector3(x, y, z));
    }

    void PhysicsWorld::ApplyTorque(int objectIndex, float x, float y, float z)
    {
        std::map<int, btRigidBody*>::iterator it = m_impl->bodyByIndex.find(objectIndex);
        if (it == m_impl->bodyByIndex.end() || !it->second) return;
        it->second->activate(true);
        it->second->applyTorque(btVector3(x, y, z));
    }

    void PhysicsWorld::SetLinearVelocity(int objectIndex, float x, float y, float z)
    {
        std::map<int, btRigidBody*>::iterator it = m_impl->bodyByIndex.find(objectIndex);
        if (it == m_impl->bodyByIndex.end() || !it->second) return;
        it->second->activate(true);
        it->second->setLinearVelocity(btVector3(x, y, z));
    }

    bool PhysicsWorld::GetLinearVelocity(int objectIndex, float out[3]) const
    {
        std::map<int, btRigidBody*>::const_iterator it = m_impl->bodyByIndex.find(objectIndex);
        if (it == m_impl->bodyByIndex.end() || !it->second) return false;
        const btVector3& v = it->second->getLinearVelocity();
        out[0] = v.x(); out[1] = v.y(); out[2] = v.z();
        return true;
    }

    void PhysicsWorld::SetAngularVelocity(int objectIndex, float x, float y, float z)
    {
        std::map<int, btRigidBody*>::iterator it = m_impl->bodyByIndex.find(objectIndex);
        if (it == m_impl->bodyByIndex.end() || !it->second) return;
        it->second->activate(true);
        it->second->setAngularVelocity(btVector3(x, y, z));
    }

    bool PhysicsWorld::GetAngularVelocity(int objectIndex, float out[3]) const
    {
        std::map<int, btRigidBody*>::const_iterator it = m_impl->bodyByIndex.find(objectIndex);
        if (it == m_impl->bodyByIndex.end() || !it->second) return false;
        const btVector3& v = it->second->getAngularVelocity();
        out[0] = v.x(); out[1] = v.y(); out[2] = v.z();
        return true;
    }

    void PhysicsWorld::SetTransform(int objectIndex, const float pos[3], const float rotDeg[3])
    {
        std::map<int, btRigidBody*>::iterator it = m_impl->bodyByIndex.find(objectIndex);
        if (it == m_impl->bodyByIndex.end() || !it->second) return;
        btRigidBody* body = it->second;
        const btTransform xf = MakeTransformPR(pos, rotDeg);
        body->setWorldTransform(xf);
        // Kinematic bodies are driven from their motion state each step, so update
        // it too (this is how a script continuously moves a kinematic player).
        if (body->getMotionState())
            body->getMotionState()->setWorldTransform(xf);
        body->activate(true);
        // Teleport semantics: drop residual momentum so it appears, not launches.
        body->setLinearVelocity(btVector3(0.0f, 0.0f, 0.0f));
        body->setAngularVelocity(btVector3(0.0f, 0.0f, 0.0f));
    }

    void PhysicsWorld::SetHierarchyTransform(int objectIndex, const float pos[3], const float rotDeg[3])
    {
        Impl& s = *m_impl;
        const btTransform xf = MakeTransformPR(pos, rotDeg);
        if (s.staticBodyByIndex.find(objectIndex) != s.staticBodyByIndex.end())
        {
            std::map<int, btRigidBody*>::iterator bodyIt = s.bodyByIndex.find(objectIndex);
            if (bodyIt != s.bodyByIndex.end() && bodyIt->second)
            {
                bodyIt->second->setWorldTransform(xf);
                if (bodyIt->second->getMotionState())
                    bodyIt->second->getMotionState()->setWorldTransform(xf);
                s.world->updateSingleAabb(bodyIt->second);
            }
        }
        for (size_t i = 0; i < s.triggers.size(); ++i)
        {
            Impl::TriggerRec& trigger = s.triggers[i];
            if (trigger.objectIndex != objectIndex || trigger.boneHash != 0) continue;
            trigger.ghost->setWorldTransform(xf);
            s.world->updateSingleAabb(trigger.ghost);
        }
    }

    bool PhysicsWorld::GetPosition(int objectIndex, float out[3]) const
    {
        std::map<int, btRigidBody*>::const_iterator it = m_impl->bodyByIndex.find(objectIndex);
        if (it == m_impl->bodyByIndex.end() || !it->second) return false;
        const btVector3& o = it->second->getWorldTransform().getOrigin();
        out[0] = o.x(); out[1] = o.y(); out[2] = o.z();
        return true;
    }

    bool PhysicsWorld::GetRotation(int objectIndex, float outDeg[3]) const
    {
        std::map<int, btRigidBody*>::const_iterator it = m_impl->bodyByIndex.find(objectIndex);
        if (it == m_impl->bodyByIndex.end() || !it->second) return false;
        // Bullet's basis is the transpose of the renderer's row-vector rotation
        // (see MakeTransformPR), so r[i][j] = basis[j][i). Inverting X*Y*Z:
        //   r02 = -sin(y), r12 = sin(x)cos(y), r22 = cos(x)cos(y),
        //   r00 = cos(y)cos(z), r01 = cos(y)sin(z).
        const btMatrix3x3& b = it->second->getWorldTransform().getBasis();
        float r[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r[i][j] = (float)b[j][i];

        float s = -r[0][2];
        if (s < -1.0f) s = -1.0f;
        if (s >  1.0f) s =  1.0f;
        const float ay = asinf(s);
        float ax, az;
        if (fabsf(s) > 0.99999f) // gimbal lock: cos(y) ~ 0, fold z into x
        {
            ax = atan2f(-r[2][1], r[1][1]);
            az = 0.0f;
        }
        else
        {
            ax = atan2f(r[1][2], r[2][2]);
            az = atan2f(r[0][1], r[0][0]);
        }
        const float r2d = 180.0f / 3.14159265358979323846f;
        outDeg[0] = ax * r2d; outDeg[1] = ay * r2d; outDeg[2] = az * r2d;
        return true;
    }

    bool PhysicsWorld::Raycast(const float origin[3], const float dir[3],
                               float maxDistance, RayHit& out) const
    {
        const Impl& s = *m_impl;
        if (!s.world) return false;

        btVector3 d((btScalar)dir[0], (btScalar)dir[1], (btScalar)dir[2]);
        const btScalar len = d.length();
        if (len <= SIMD_EPSILON) return false;
        d /= len;
        if (!(maxDistance > 0.0f)) maxDistance = 1000.0f;

        const btVector3 from((btScalar)origin[0], (btScalar)origin[1], (btScalar)origin[2]);
        const btVector3 to = from + d * (btScalar)maxDistance;

        SolidRayCallback cb(from, to);
        s.world->rayTest(from, to, cb);
        if (!cb.hasHit()) return false;

        std::map<const btCollisionObject*, int>::const_iterator it =
            s.objIndexOf.find(cb.m_collisionObject);
        out.objectIndex = (it != s.objIndexOf.end()) ? it->second : -1;
        out.point[0]  = cb.m_hitPointWorld.x();
        out.point[1]  = cb.m_hitPointWorld.y();
        out.point[2]  = cb.m_hitPointWorld.z();
        out.normal[0] = cb.m_hitNormalWorld.x();
        out.normal[1] = cb.m_hitNormalWorld.y();
        out.normal[2] = cb.m_hitNormalWorld.z();
        out.distance  = (cb.m_hitPointWorld - from).length();
        return true;
    }
}
