//
//  PhysicsEngine.h
//  libraries/physics/src
//
//  Created by Andrew Meadows 2014.10.29
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_PhysicsEngine_h
#define hifi_PhysicsEngine_h

#include <stdint.h>

#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <QUuid>
#include <QHash>
#include <QSet>

#include <Jolt/Jolt.h>

#include "ObjectMotionState.h"
#include "ObjectAction.h"
#include "ObjectConstraint.h"
#include "PhysicsWorld.h"

namespace JPH {
    class TempAllocatorImpl;
    class JobSystemThreadPool;
    class BodyFilter;
}

const float HALF_SIMULATION_EXTENT = 512.0f; // meters

class CharacterController;
class PhysicsDebugDraw;

// The client's physics: a root world for the domain and one world per moving reference frame (PHYSICS.md
// 4.1), stepped together at 90 Hz. Every simulated object says which frame it belongs to (getFrameID) and
// lives in that frame's world, in that frame's coordinates; the frame itself is a body in its parent's
// world. The engine routes objects to worlds, feeds each frame's transform to its world before a substep,
// merges what the worlds report, and keeps the actions and constraints.
class PhysicsEngine {
public:
    // Something stepped with the worlds, before and after every substep (the vehicles, PHYSICS.md round 6).
    class SubstepListener {
    public:
        virtual ~SubstepListener() = default;
        virtual void preSubstep(uint32_t step, float dt) = 0;
        virtual void postSubstep(uint32_t step, float dt) = 0;
    };

    class Transaction {
    public:
        void clear() {
            objectsToRemove.clear();
            objectsToAdd.clear();
            objectsToReinsert.clear();
            activeStaticObjects.clear();
        }
        std::vector<ObjectMotionState*> objectsToRemove;
        std::vector<ObjectMotionState*> objectsToAdd;
        std::vector<ObjectMotionState*> objectsToReinsert;
        std::vector<ObjectMotionState*> activeStaticObjects;
    };

    PhysicsEngine(const glm::vec3& offset);
    ~PhysicsEngine();
    // maxWorkers caps Jolt's job system (0: the hardware's threads less one). The physics server passes a few: on a
    // 32-core Mac 31 workers were woken 91 times a second for nothing (PHYSICS.md round 9, the idle cost).
    void init(int maxWorkers = 0);
    // When set, a step with no active body in any world and no frame is skipped (the clock still runs): the physics
    // server's world with no vehicle moving costs nothing then. The caller (the server) says when it is allowed, since
    // the engine does not know whether a train or a vehicle is about to need a step. Never on a client: the character
    // is stepped every frame.
    void setIdleWhenStill(bool allowed) { _idleWhenStill = allowed; }
    bool isStill() const;
    QString describeActiveBodies(int maxNames) const;   // for a log: how many are awake, how many dynamic, a few names
    uint64_t getNumIdleSteps() const { return _numIdleSteps; }

    // Time dilation (PHYSICS.md round 10): a driver's client shifts its simulation clock by this much, a few percent
    // fast (positive: more substeps than the wall clock calls for, to get its inputs further ahead of the physics
    // server) or slow, until the shift is in; the step count is what moves, the substeps' stamped times stay on the
    // wall clock. The physics server never shifts.
    static constexpr float TIME_DILATION = 0.05f;
    void requestTimeShift(float seconds) { _timeShiftPending += seconds; }
    float getTimeShiftPending() const { return _timeShiftPending; }

    uint32_t getNumSubsteps() const { return _numSubsteps; }
    int32_t getNumCollisionObjects() const;

    void removeObjects(const VectorOfMotionStates& objects);
    void removeSetOfObjects(const SetOfMotionStates& objects); // only called during teardown

    void addObjects(const VectorOfMotionStates& objects);
    void reinsertObject(ObjectMotionState* object);

    void processTransaction(Transaction& transaction);

    void stepSimulation();
    void harvestPerformanceStats();
    void printPerformanceStatsToFile(const QString& filename);
    void updateContactMap();
    void doOwnershipInfectionForConstraints();

    bool hasOutgoingChanges() const { return _hasOutgoingChanges; }

    /// \return reference to list of changed MotionStates.  The list is only valid until beginning of next simulation loop.
    const VectorOfMotionStates& getChangedMotionStates();
    const VectorOfMotionStates& getDeactivatedMotionStates() const { return _deactivatedMotionStates; }

    /// \return reference to list of Collision events.  The list is only valid until beginning of next simulation loop.
    const CollisionEvents& getCollisionEvents();

    /// \brief prints timings for last simulation step
    void dumpStatsIfNecessary();
    /// \brief saves timings for last simulation step in filename
    void saveNextPhysicsStats(QString filename);

    /// \param offset position of simulation origin in domain-frame
    void setOriginOffset(const glm::vec3& offset) { _originOffset = offset; }

    /// \return position of simulation origin in domain-frame
    const glm::vec3& getOriginOffset() const { return _originOffset; }

    void setCharacterController(CharacterController* character);

    void dumpNextStats() { _dumpNextStats = true; }

    EntityDynamicPointer getDynamicByID(const QUuid& dynamicID) const;
    bool addDynamic(EntityDynamicPointer dynamic);
    void removeDynamic(const QUuid dynamicID);
    void forEachDynamic(std::function<void(EntityDynamicPointer)> actor);

    void setShowBulletWireframe(bool value);
    void setShowBulletAABBs(bool value);
    void setShowBulletContactPoints(bool value);
    void setShowBulletConstraints(bool value);
    void setShowBulletConstraintLimits(bool value);

    // Copies of these methods exist in PhysicsEntitySimulation.
    // There is a possible performance improvement in avoiding the duplication of the ShapeInfo calculation.
    // Tests the root world (the domain); a frame's interior is not searched.
    std::vector<ContactTestResult> contactTest(uint16_t mask, const ShapeInfo& regionShapeInfo, const Transform& regionTransform, uint16_t group = USER_COLLISION_GROUP_DYNAMIC, float threshold = 0.0f) const;

    void removeContacts(ObjectMotionState* motionState);

    // Ray casts in the root world's coordinates (the simulation frame), filtered as a body of the given group
    // and mask would be. A null body filter means "everything". castRayClosest tests the root world only;
    // castRayAll also goes into every frame's world (a rider can be picked), hits sorted by distance.
    bool castRayClosest(const glm::vec3& start, const glm::vec3& end, int32_t group, int32_t mask,
                        const JPH::BodyFilter* bodyFilter, RayHit& result) const;
    void castRayAll(const glm::vec3& start, const glm::vec3& end, int32_t group, int32_t mask,
                    std::vector<RayHit>& results) const;

    // Worlds. The root is the domain; a frame's world is made the first time something asks for it and
    // dropped when it is empty.
    PhysicsWorld* getRootWorld() const { return _rootWorld.get(); }
    void setSubstepListener(SubstepListener* listener) { _substepListener = listener; }
    size_t getNumFrames() const { return _frameStates.size(); }
    PhysicsWorld* getWorld(const QUuid& frameID);
    PhysicsWorld* findWorld(const QUuid& frameID) const;
    PhysicsWorld* worldFor(ObjectMotionState* motionState);
    // The frame whose membership volume contains the point (in domain coordinates), with hysteresis against
    // the frame the point is in now; null for the domain itself.
    // the frame a point belongs to, with hysteresis for the frame it is in; `supported` says whether the thing is
    // standing on something of that frame (a character on its floor), which keeps it in past the floor's edge, while
    // something unsupported there (a walker who has just stepped off the edge, with the platform waiting outside)
    // leaves at once, before it falls in a world where the platform does not exist
    QUuid frameContaining(const glm::vec3& position, const QUuid& currentFrame, bool supported = true) const;
    // the transform of a frame in the domain (its body's, or its entity's when it has no body yet)
    bool getFrameTransform(const QUuid& frameID, glm::vec3& position, glm::quat& rotation, glm::vec3& linearVelocity, glm::vec3& angularVelocity, bool asSimulated = false) const;

    JPH::TempAllocator* getTempAllocator() const;
    static ObjectMotionState* motionStateOf(const JPH::Body& body) { return PhysicsWorld::motionStateOf(body); }

private:
    // removes the dynamics on the object's body (as owner or other body) from the engine, and returns them
    std::vector<EntityDynamicPointer> removeDynamicsForObject(ObjectMotionState* motionState);
    void restoreDynamics();
    void stepOnce(float dt);
    void feedFrameTransforms();
    void dropEmptyWorlds();
    void rehomeObjects();
    void registerFrame(ObjectMotionState* frameState);
    void unregisterFrame(const QUuid& frameID);
    void refreshFrameInteriors();
    PhysicsWorld* characterWorld() const;

    SubstepListener* _substepListener { nullptr };
    bool _idleWhenStill { false };
    uint64_t _numIdleSteps { 0 };
    std::unique_ptr<JPH::TempAllocatorImpl> _tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> _jobSystem;
    std::unique_ptr<PhysicsWorld> _rootWorld;
    std::map<QUuid, std::unique_ptr<PhysicsWorld>> _frameWorlds;
    std::map<QUuid, ObjectMotionState*> _frameStates; // the frame entities' own motion states, by entity ID
    bool _framesChanged { false }; // a frame appeared or went: every object's world is rechecked
    std::unique_ptr<PhysicsDebugDraw> _physicsDebugDraw;

    CollisionEvents _collisionEvents;
    QHash<QUuid, EntityDynamicPointer> _objectDynamics;
    QHash<QUuid, PhysicsWorld*> _dynamicWorlds; // the world a constraint was added to
    std::map<std::pair<PhysicsWorld*, uint32_t>, QSet<QUuid>> _objectDynamicsByBody;
    // dynamics taken off a body that was rebuilt, which couldn't go back on at once; retried every transaction
    QHash<QUuid, EntityDynamicPointer> _dynamicsToRestore;
    VectorOfMotionStates _changedMotionStates;
    VectorOfMotionStates _deactivatedMotionStates;
    QString _statsFilename;
    glm::vec3 _originOffset;
    CharacterController* _myAvatarController { nullptr };
    uint64_t _lastStepTime { 0 };
    float _accumulatedTime { 0.0f };
    float _timeShiftPending { 0.0f };   // s of simulation still to add (or, negative, to leave out) by time dilation
    float _smoothedFrameDuration { 0.0f }; // the paced clock's frame (see stepSimulation)
    float _pacedClockLead { 0.0f }; // how far the paced clock is ahead of the wall clock
    float _lastStepDuration { 0.0f };
    uint32_t _numSubsteps { 0 };
    uint32_t _debugDrawMode { 0 };
    bool _dumpNextStats { false };
    bool _saveNextStats { false };
    bool _hasOutgoingChanges { false };
};

typedef std::shared_ptr<PhysicsEngine> PhysicsEnginePointer;

#endif // hifi_PhysicsEngine_h
