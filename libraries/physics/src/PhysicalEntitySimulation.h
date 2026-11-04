//
//  PhysicalEntitySimulation.h
//  libraries/physics/src
//
//  Created by Andrew Meadows 2015.04.27
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_PhysicalEntitySimulation_h
#define hifi_PhysicalEntitySimulation_h

#include <stdint.h>
#include <map>
#include <set>
#include <memory>


#include <EntityDynamicInterface.h>
#include <EntityItem.h>
#include <EntitySimulation.h>
#include <workload/Space.h>

#include "PhysicsEngine.h"
#include "EntityMotionState.h"

class PhysicalEntitySimulation;
class TrackSimulation;
class VehicleSimulation;
using PhysicalEntitySimulationPointer = std::shared_ptr<PhysicalEntitySimulation>;
using SetOfEntityMotionStates = QSet<EntityMotionState*>;

class VectorOfEntityMotionStates: public std::vector<EntityMotionState*> {
public:
    void remove(uint32_t index) {
        assert(index < size());
        if (index < size() - 1) {
            (*this)[index] = back();
        }
        pop_back();
    }
    void removeFirst(EntityMotionState* state) {
        for (uint32_t i = 0; i < size(); ++i) {
            if ((*this)[i] == state) {
                remove(i);
                break;
            }
        }
    }
};

class PhysicalEntitySimulation : public EntitySimulation {
    Q_OBJECT
public:
    PhysicalEntitySimulation();
    ~PhysicalEntitySimulation();

    void init(EntityTreePointer tree, PhysicsEnginePointer engine, EntityEditPacketSender* packetSender);
    void setWorkloadSpace(const workload::SpacePointer space) { _space = space; }

    void addDynamic(EntityDynamicPointer dynamic) override;
    void removeDynamic(const QUuid dynamicID) override;
    void applyDynamicChanges() override;

    void takeDeadAvatarEntities(SetOfEntities& deadEntities);

    virtual void clearEntities() override;
    QVariantMap getVehicleStats() const override;
    void queueEraseDomainEntity(const QUuid& id) const override;

signals:
    void entityCollisionWithEntity(const EntityItemID& idA, const EntityItemID& idB, const Collision& collision);

private slots:
    // the physics server's state stream (PHYSICS.md round 9): a client receives the states, the server the requests
    void handleVehicleStatePacket(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode);
    void handleVehicleStateRequest(QSharedPointer<ReceivedMessage> message, SharedNodePointer senderNode);


protected: // only called by EntitySimulation
    // overrides for EntitySimulation
    void addEntityToInternalLists(EntityItemPointer entity) override;
    void removeEntityFromInternalLists(EntityItemPointer entity) override;
    void processChangedEntity(const EntityItemPointer& entity) override;
    void processDeadEntities() override;

    void removeOwnershipData(EntityMotionState* motionState);
    void clearOwnershipData();

public:
    virtual void prepareEntityForDelete(EntityItemPointer entity) override;
    void removeDeadEntities();

    void buildPhysicsTransaction(PhysicsEngine::Transaction& transaction);
    void handleProcessedPhysicsTransaction(PhysicsEngine::Transaction& transaction);

    void handleDeactivatedMotionStates(const VectorOfMotionStates& motionStates);
    void handleChangedMotionStates(const VectorOfMotionStates& motionStates);
    void handleCollisionEvents(const CollisionEvents& collisionEvents);

    EntityEditPacketSender* getPacketSender() { return _entityPacketSender; }

    // the trains on the tracks (PHYSICS.md 4.8); the physics server takes their state publications from here
    TrackSimulation* getTrackSimulation() const { return _trackSimulation.get(); }
    // the driven vehicles (PHYSICS.md round 6); the physics server hands it the drivers' inputs
    VehicleSimulation* getVehicleSimulation() const { return _vehicleSimulation.get(); }

    void addOwnershipBid(EntityMotionState* motionState);
    void addOwnership(EntityMotionState* motionState);
    void sendOwnershipBids(uint32_t numSubsteps);
    void sendOwnedUpdates(uint32_t numSubsteps);

private:
    // A dynamic entity this process simulates crosses between frames by where it is (PHYSICS.md 4.1, round 8): out of
    // a frame's volume into the frame that holds it, or the domain; its pose and velocities are handed over exactly
    // and its body is reinserted in the other world by the next transaction.
    void updateFrameMembership(const VectorOfMotionStates& motionStates);
    void buildMotionStatesForEntitiesThatNeedThem();
    // the workload region an entity is in; without a workload space (the physics server has none) everything is R1
    uint8_t regionOf(const EntityItemPointer& entity) const;

    class ShapeRequest {
    public:
        ShapeRequest() { }
        ShapeRequest(const EntityItemPointer& e) : entity(e) { }
        bool operator<(const ShapeRequest& other) const { return entity.get() < other.entity.get(); }
        bool operator==(const ShapeRequest& other) const { return entity.get() == other.entity.get(); }
        EntityItemPointer entity { nullptr };
        mutable uint64_t shapeHash { 0 };
    };
    SetOfEntities _entitiesToAddToPhysics; // we could also call this: _entitiesThatNeedMotionStates
    SetOfEntities _entitiesToRemoveFromPhysics;
    SetOfEntityMotionStates _incomingChanges; // EntityMotionStates changed by external events
    SetOfMotionStates _physicalObjects; // MotionStates of entities in PhysicsEngine

    using ShapeRequests = std::set<ShapeRequest>;
    ShapeRequests _shapeRequests;

    PhysicsEnginePointer _physicsEngine = nullptr;
    EntityEditPacketSender* _entityPacketSender = nullptr;

    VectorOfEntityMotionStates _owned;
    VectorOfEntityMotionStates _bids;
    SetOfEntities _deadAvatarEntities; // to remove from Avatar's lists
    std::vector<EntityItemPointer> _entitiesToDeleteLater;

    QList<EntityDynamicPointer> _dynamicsToAdd;
    QSet<QUuid> _dynamicsToRemove;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QRecursiveMutex _dynamicsMutex;   // Qt 6 has no QMutex::Recursive; the same lock as below
#else
    QMutex _dynamicsMutex { QMutex::Recursive };
#endif

    workload::SpacePointer _space;
    std::unique_ptr<TrackSimulation> _trackSimulation; // the trains on the tracks (PHYSICS.md 4.8)
    std::unique_ptr<VehicleSimulation> _vehicleSimulation; // the driven vehicles (PHYSICS.md round 6)
    uint64_t _nextBidExpiry;
    uint32_t _lastStepSendPackets { 0 };
    bool _streamListenersRegistered { false };

    uint32_t _lastWorkDeliveryCount { 0 };
};


typedef std::shared_ptr<PhysicalEntitySimulation> PhysicalEntitySimulationPointer;

#endif // hifi_PhysicalEntitySimulation_h
