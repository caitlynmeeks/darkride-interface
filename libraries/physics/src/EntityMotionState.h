//
//  EntityMotionState.h
//  libraries/entities/src
//
//  Created by Andrew Meadows on 2014.11.06
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: getCorrectionDistance (its body is here).
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//

#ifndef hifi_EntityMotionState_h
#define hifi_EntityMotionState_h

#include <EntityItem.h>
#include <EntityTypes.h>
#include <AACube.h>
#include <workload/Region.h>

#include "ObjectMotionState.h"

class TrackSimulation;
class VehicleSimulation;


// From the MotionState's perspective:
//      Inside = physics simulation
//      Outside = external agents (scripts, user interaction, other simulations)

class EntityMotionState : public ObjectMotionState {
public:
    enum class OwnershipState {
        NotLocallyOwned = 0,
        PendingBid,
        LocallyOwned,
        Unownable
    };

    EntityMotionState() = delete;
    EntityMotionState(const JPH::Shape* shape, EntityItemPointer item);
    virtual ~EntityMotionState();

    void handleDeactivation();
    virtual void handleEasyChanges(uint32_t& flags) override;

    // The chain of transforms between the entity's parent and its frame, composed: the transform of the parent
    // in the frame's coordinates. Returns false when the entity has no frame.
    bool getParentTransformInFrame(Transform& transform, glm::vec3& linearVelocity, glm::vec3& angularVelocity, bool asSimulated = false) const;
    // the body's pose as the entity's local transform, and back (the body is the simulated state of a keyframed entity)
    void bodyPoseAsLocal(Transform& local) const;
    void localToBodyPose(const Transform& local, glm::vec3& position, glm::quat& rotation) const;

    /// \return PhysicsMotionType based on params set in EntityItem
    virtual PhysicsMotionType computePhysicsMotionType() const override;

    virtual bool isMoving() const override;

    // this relays incoming position/rotation to the RigidBody
    void pushTransformToBody(float dt) override;

    // this relays outgoing position/rotation to the EntityItem
    void pullTransformFromBody() override;
    int getNestingDepth() const override;

    bool shouldSendUpdate(uint32_t simulationStep);
    // a car on a track (PHYSICS.md 4.8): driven along the curve by the client that owns it, through the TrackSimulation
    bool isTrackDriven() const { return _trackDriven; }
    bool serverTakes() const; // the physics server owns this (a car on a track, a vehicle) at SERVER priority
    void setTrackSimulation(TrackSimulation* tracks) { _tracks = tracks; }
    // a vehicle (PHYSICS.md round 6): simulated here by the VehicleSimulation, whoever the authority is
    bool isVehicle() const { return _vehicle; }
    void setVehicleSimulation(VehicleSimulation* vehicles) { _vehicles = vehicles; }
    // a correction's leftover, taken out of the shown pose and decayed with the time constant (0 clears it)
    void setCorrection(const glm::vec3& offset, const glm::quat& rotation, float blendTime);
    void sendBid(OctreeEditPacketSender* packetSender, uint32_t step);
    void sendUpdate(OctreeEditPacketSender* packetSender, uint32_t step);

    virtual uint32_t getIncomingDirtyFlags() const override;
    virtual void clearIncomingDirtyFlags(uint32_t mask = DIRTY_PHYSICS_FLAGS) override;

    virtual float getObjectRestitution() const override { return _entity->getRestitution(); }
    virtual float getObjectFriction() const override { return _entity->getFriction(); }
    virtual float getObjectLinearDamping() const override { return _entity->getDamping(); }
    virtual float getObjectAngularDamping() const override { return _entity->getAngularDamping(); }

    // In the coordinates of the entity's frame when it has one (PHYSICS.md 4.1), else the domain's.
    virtual glm::vec3 getObjectPosition() const override;
    virtual glm::quat getObjectRotation() const override;
    virtual glm::vec3 getObjectLinearVelocity() const override;
    virtual glm::vec3 getObjectAngularVelocity() const override;
    virtual glm::vec3 getObjectGravity() const override { return _entity->getGravity(); }

    QUuid getFrameID() const override;
    bool isFrame() const override;
    bool getFrameVolume(glm::vec3& minCorner, glm::vec3& maxCorner) const override;
    bool getFrameTransformInDomain(glm::vec3& position, glm::quat& rotation, glm::vec3& linearVelocity, glm::vec3& angularVelocity, bool asSimulated = false) const override;
    int getFrameDepth() const override;
    virtual glm::vec3 getObjectLinearVelocityChange() const override;

    virtual const QUuid getObjectID() const override { return _entity->getID(); }

    virtual uint8_t getSimulationPriority() const override;
    virtual QUuid getSimulatorID() const override;
    virtual void bump(uint8_t priority) override;

    // getEntity() returns a smart-pointer by reference because it is only ever used
    // to insert into lists of smart pointers, and the lists will make their own copies
    const EntityItemPointer& getEntity() const { return _entity; }

    void resetMeasuredBodyAcceleration();
    void measureBodyAcceleration();

    virtual QString getName() const override;
    ShapeType getShapeType() const override { return _entity->getShapeType(); }

    virtual void computeCollisionGroupAndMask(int32_t& group, int32_t& mask) const override;

    bool shouldSendBid() const;

    bool isLocallyOwned() const override;
    bool isLocallyOwnedOrShouldBe() const override; // aka shouldEmitCollisionEvents()
    bool isStaticPriority() const;

    friend class PhysicalEntitySimulation;
    OwnershipState getOwnershipState() const { return _ownershipState; }

    void setRegion(uint8_t region);


    // a vehicle's mass is the number on its spec sheet (vehicle.mass), not a density times a volume
    float getMass() const override;
    // FROZEN (vehicle path): see the note at the top of this file.
    // m: the correction a vehicle is still blending in (PHYSICS.md 4.6), 0 when none (the chase camera's trace)
    float getCorrectionDistance() const { return _correctionBlendTime > 0.0f ? glm::length(_correctionOffset) : 0.0f; }
protected:
    void setBody(JPH::Body* body, PhysicsWorld* world) override;

    uint8_t computeFinalBidPriority() const;
    void updateSendVelocities();
    uint64_t getNextBidExpiry() const { return _nextBidExpiry; }
    void initForBid();
    void initForOwned();
    void clearOwnershipState();
    void updateServerPhysicsVariables();
    bool remoteSimulationOutOfSync(uint32_t simulationStep);

    void workerBidPriority(); // computeNewBidPriority() with value stored in _entity

    void clearObjectVelocities() const;

    bool shouldBeInPhysicsSimulation() const;
    void setMotionType(PhysicsMotionType motionType) override;

    // EntityMotionState keeps a SharedPointer to its EntityItem which is only set in the CTOR
    // and is only cleared in the DTOR
    EntityItemPointer _entity;

    // These "_serverFoo" variables represent what we think the server knows.
    // They are used in two different modes:
    //
    // (1) For remotely owned simulation: we store the last values recieved from the server.
    //     When the body comes to rest and goes inactive we slam its final transforms to agree with the last server
    //     update. This to reduce state synchronization errors when the local simulation deviated from remote.
    //
    // (2) For locally owned simulation: we store the last values sent to the server, integrated forward over time
    //     according to how we think the server doing it.  We calculate the error between the true local transform
    //     and the remote to decide whether to send another update or not.
    //
    glm::vec3 _serverPosition;    // in simulation-frame (not world-frame)
    glm::quat _serverRotation;
    glm::vec3 _serverVelocity;
    glm::vec3 _serverAngularVelocity; // radians per second
    glm::vec3 _serverGravity;
    glm::vec3 _serverAcceleration;
    QByteArray _serverActionData;

    glm::vec3 _lastVelocity;
    glm::vec3 _measuredAcceleration;
    quint64 _nextBidExpiry { 0 };

    float _measuredDeltaTime;
    uint32_t _lastMeasureStep;
    uint32_t _lastStep; // last step of server extrapolation

    OwnershipState _ownershipState { OwnershipState::NotLocallyOwned };
    uint8_t _loopsWithoutOwner;
    mutable uint8_t _accelerationNearlyGravityCount;
    uint8_t _numInactiveUpdates { 1 };
    bool _parentChanged { false };   // the physics moved the entity to another frame: the next update says so
    uint8_t _bumpedPriority { 0 }; // the target simulation priority according to collision history
    uint8_t _region { workload::Region::INVALID };

    mutable bool _trackDriven { false }; // the entity has a trackID (refreshed with its motion type)
    TrackSimulation* _tracks { nullptr };
    uint32_t _lastTrackUpdateStep { 0 }; // the substep an owned car last sent an update at
    bool trackCarOutOfSync(uint32_t simulationStep);
    mutable bool _vehicle { false }; // the entity has a wheeled vehicle group (refreshed with its motion type)
    VehicleSimulation* _vehicles { nullptr };
    uint32_t _lastVehicleUpdateStep { 0 };
    glm::vec3 _correctionOffset { 0.0f };
    glm::quat _correctionRotation;
    float _correctionBlendTime { 0.0f };
    uint64_t _correctionTime { 0 };
    bool vehicleOutOfSync(uint32_t simulationStep, bool& handled);
    bool hasLocalVehicleControls() const;

    bool isServerlessMode();
    // the simulation changed the entity's parent (a frame crossing, round 8): the next update carries the parent too
    void setParentChanged() { _parentChanged = true; }
};

#endif // hifi_EntityMotionState_h
