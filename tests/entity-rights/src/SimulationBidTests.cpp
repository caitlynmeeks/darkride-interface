//
//  SimulationBidTests.cpp
//  tests/entity-rights/src
//
//  The entity server's simulation-ownership rules (EntityTree::updateEntity's server branch; vehicle master plan
//  entry 16): the physics server's bid at its own priority is taken as it is; a bid at or above that priority from any
//  other node is taken at one below it, so it never outranks the server; a bid equal to the current owner's priority
//  wins only once the lockout the entity keeps for that owner has run out. No network: the tree in server mode, the
//  senders as Node objects of the types the entity server sees.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "SimulationBidTests.h"

#include <QtCore/QThread>

#include <AddressManager.h>
#include <EntityItemProperties.h>
#include <EntityTree.h>
#include <EntityTypes.h>
#include <Node.h>
#include <NodeList.h>
#include <NodePermissions.h>
#include <SharedUtil.h>
#include <SimulationFlags.h>

QTEST_GUILESS_MAIN(SimulationBidTests)

void SimulationBidTests::initTestCase() {
    // the tree adds an entity only with a node list about (as the entity rights tests do)
    DependencyManager::set<AddressManager>();
    DependencyManager::set<NodeList>(NodeType::Agent, INVALID_PORT);
}

namespace {

SharedNodePointer makeNode(NodeType_t type) {
    SharedNodePointer node(new Node(QUuid::createUuid(), type, HifiSockAddr(), HifiSockAddr()));
    NodePermissions permissions;
    permissions.setAll(true);   // the rights are not the question here
    node->setPermissions(permissions);
    return node;
}

struct World {
    EntityTreePointer tree;
    EntityItemID carID { QUuid("{b0000000-0000-0000-0000-000000000001}") };
    EntityItemID boxID { QUuid("{b0000000-0000-0000-0000-000000000002}") };
    SharedNodePointer physicsServer { makeNode(NodeType::PhysicsServer) };
    SharedNodePointer visitor { makeNode(NodeType::Agent) };
    SharedNodePointer anotherVisitor { makeNode(NodeType::Agent) };

    World() {
        tree = EntityTreePointer(new EntityTree(true));
        tree->createRootElement();
        tree->setIsServer(true);
        add(carID, "car");
        add(boxID, "box");
    }

    void add(const EntityItemID& id, const QString& name) {
        EntityItemProperties properties;
        properties.setType(EntityTypes::Box);
        properties.setName(name);
        properties.setPosition(glm::vec3(100.0f));
        properties.setDimensions(glm::vec3(0.5f));
        properties.setDynamic(true);
        properties.setCreated(usecTimestampNow());
        tree->withWriteLock([&] { tree->addEntity(id, properties); });
    }

    EntityItemPointer find(const EntityItemID& id) {
        EntityItemPointer entity;
        tree->withReadLock([&] { entity = tree->findEntityByEntityItemID(id); });
        return entity;
    }

    // a bid for the simulation of `id`, as the entity server gets it from `sender` (a node bids only its own id)
    void bid(const EntityItemID& id, const SharedNodePointer& sender, uint8_t priority) {
        EntityItemProperties properties;
        properties.setSimulationOwner(sender->getUUID(), priority);
        properties.setLastEdited(usecTimestampNow() + 1);
        tree->withWriteLock([&] { tree->updateEntity(id, properties, sender); });
    }

    QUuid ownerOf(const EntityItemID& id) {
        auto entity = find(id);
        return entity ? entity->getSimulatorID() : QUuid();
    }

    uint8_t priorityOf(const EntityItemID& id) {
        auto entity = find(id);
        return entity ? entity->getSimulationPriority() : 0;
    }
};

}  // namespace

void SimulationBidTests::physicsServerTakesAtItsPriority() {
    World world;
    world.bid(world.carID, world.physicsServer, SERVER_SIMULATION_PRIORITY);
    QCOMPARE(world.ownerOf(world.carID), world.physicsServer->getUUID());
    QCOMPARE(world.priorityOf(world.carID), SERVER_SIMULATION_PRIORITY);
}

void SimulationBidTests::anotherNodesBidAtTheServersPriorityIsTakenBelowIt() {
    World world;
    // on something nobody simulates: the bid is taken, at one below the server's priority
    world.bid(world.boxID, world.visitor, SERVER_SIMULATION_PRIORITY);
    QCOMPARE(world.ownerOf(world.boxID), world.visitor->getUUID());
    QCOMPARE(world.priorityOf(world.boxID), uint8_t(SERVER_SIMULATION_PRIORITY - 1));
    // against the physics server's car: the same bid loses, so the car stays the server's at its priority
    world.bid(world.carID, world.physicsServer, SERVER_SIMULATION_PRIORITY);
    world.bid(world.carID, world.visitor, SERVER_SIMULATION_PRIORITY);
    QCOMPARE(world.ownerOf(world.carID), world.physicsServer->getUUID());
    QCOMPARE(world.priorityOf(world.carID), SERVER_SIMULATION_PRIORITY);
}

void SimulationBidTests::aBidBelowTheServersPriorityLosesToTheServer() {
    World world;
    world.bid(world.carID, world.physicsServer, SERVER_SIMULATION_PRIORITY);
    world.bid(world.carID, world.visitor, SERVER_SIMULATION_PRIORITY - 1);
    QCOMPARE(world.ownerOf(world.carID), world.physicsServer->getUUID());
    QCOMPARE(world.priorityOf(world.carID), SERVER_SIMULATION_PRIORITY);
}

void SimulationBidTests::anEqualBidWaitsForTheOwnersLockout() {
    World world;
    world.bid(world.boxID, world.visitor, SCRIPT_GRAB_SIMULATION_PRIORITY);
    QCOMPARE(world.ownerOf(world.boxID), world.visitor->getUUID());
    // the same priority from another node, at once: the owner's lockout is running
    world.bid(world.boxID, world.anotherVisitor, SCRIPT_GRAB_SIMULATION_PRIORITY);
    QCOMPARE(world.ownerOf(world.boxID), world.visitor->getUUID());
    // once it has run out (SimulationOwner's lockout is 200 ms): taken
    QThread::msleep(300);
    world.bid(world.boxID, world.anotherVisitor, SCRIPT_GRAB_SIMULATION_PRIORITY);
    QCOMPARE(world.ownerOf(world.boxID), world.anotherVisitor->getUUID());
    QCOMPARE(world.priorityOf(world.boxID), SCRIPT_GRAB_SIMULATION_PRIORITY);
}

void SimulationBidTests::thePhysicsServerKeepsItsPriorityOnItsOwnUpdate() {
    World world;
    world.bid(world.carID, world.physicsServer, SERVER_SIMULATION_PRIORITY);
    world.bid(world.carID, world.physicsServer, SERVER_SIMULATION_PRIORITY);
    QCOMPARE(world.ownerOf(world.carID), world.physicsServer->getUUID());
    QCOMPARE(world.priorityOf(world.carID), SERVER_SIMULATION_PRIORITY);
}
