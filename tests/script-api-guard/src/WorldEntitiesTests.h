//
//  WorldEntitiesTests.h
//  tests/script-api-guard
//
//  Created by scripting on 2026-11-01.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_WorldEntitiesTests_h
#define hifi_WorldEntitiesTests_h

#include <memory>

#include <QtCore/QSharedPointer>
#include <QtCore/QUuid>
#include <QtTest/QtTest>

#include <EntityEditPacketSender.h>

class EntityScriptingInterface;
class EntityTree;

class WorldEntitiesTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void testOwners();
    void testWorldReads();
    void testVisitorSeesEverything();
    void testWorldChanges();
    void testWorldParents();
    void testWorldEvents();
    void testOwnerKeptOnEdit();
    void testWorldAvatarEntities();
    void testWorldClones();
    void testWorldThread();
    void testWorldSignalDeliveries();
    void testWorldChangeErase();
    void testReplacedEngineIsFreed();
    void testWorldMappingsGoWithEngine();
    void testWorldIds();
    void cleanupTestCase();

private:
    std::unique_ptr<EntityEditPacketSender> _packetSender;  // made after the NodeList it needs
    std::shared_ptr<EntityTree> _tree;
    QSharedPointer<EntityScriptingInterface> _entities;
    QUuid _domain;        // a domain entity, as it arrives from the entity server
    QUuid _avatar;        // another person's avatar entity
    QUuid _visitorLocal;  // a local entity the visitor's own script made
    QUuid _worldLocal;    // a local entity a world's script made
    QUuid _straightLocal; // a local entity C++ put straight into the tree (no addEntityInternal)
    QUuid _unownedLocal;  // a local entity with no owner recorded (a host type changed after it was added)
};

#endif // hifi_WorldEntitiesTests_h
