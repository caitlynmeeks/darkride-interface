//
//  AvatarEntityOwnerTests.cpp
//  tests/entity-rights/src
//
//  What another avatar's data may remove from this computer's tree (EntityTree::deleteAvatarEntitiesOf, as the avatar
//  code calls it when an avatar drops an avatar entity and when it leaves): only the avatar entities that avatar wears,
//  with their local children as any delete takes them, on this computer only. The world's entities, this computer's
//  own entities and another avatar's stay, and nothing is sent to the entity server.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "AvatarEntityOwnerTests.h"

#include <AddressManager.h>
#include <EntityItemProperties.h>
#include <EntitySimulation.h>
#include <EntityTree.h>
#include <EntityTypes.h>
#include <NodeList.h>
#include <SharedUtil.h>
#include <SpatialParentFinder.h>

QTEST_GUILESS_MAIN(AvatarEntityOwnerTests)

namespace {

class TreeParentFinder : public SpatialParentFinder {
public:
    SpatiallyNestableWeakPointer find(QUuid parentID, bool& success, SpatialParentTree* entityTree) const override {
        SpatiallyNestablePointer found = entityTree ? entityTree->findByID(parentID) : SpatiallyNestablePointer();
        success = found != nullptr;
        return found;
    }
};

// counts the deletes a client tree would send to the entity server
class CountingSimulation : public EntitySimulation {
public:
    void queueEraseDomainEntity(const QUuid& id) const override { erased.push_back(id); }
    mutable std::vector<QUuid> erased;
};

const QUuid AVATAR_X("{b0000000-0000-0000-0000-00000000000a}");
const QUuid AVATAR_Y("{b0000000-0000-0000-0000-00000000000b}");

// a visitor's tree: the world's entities, this computer's own, and two other avatars' avatar entities
struct VisitorTree {
    EntityTreePointer tree;
    std::shared_ptr<CountingSimulation> simulation { std::make_shared<CountingSimulation>() };
    EntityItemID domainID { QUuid("{c0000000-0000-0000-0000-000000000001}") };
    EntityItemID localID { QUuid("{c0000000-0000-0000-0000-000000000002}") };
    EntityItemID mineID { QUuid("{c0000000-0000-0000-0000-000000000003}") };      // this computer's avatar's
    EntityItemID xWornID { QUuid("{c0000000-0000-0000-0000-000000000004}") };     // avatar X's
    EntityItemID yWornID { QUuid("{c0000000-0000-0000-0000-000000000005}") };     // avatar Y's
    EntityItemID xLocalChildID { QUuid("{c0000000-0000-0000-0000-000000000006}") };   // local, child of X's
    EntityItemID xDomainChildID { QUuid("{c0000000-0000-0000-0000-000000000007}") };  // the world's, child of X's

    VisitorTree() {
        tree = EntityTreePointer(new EntityTree());
        tree->createRootElement();
        tree->setSimulation(simulation);
        // a client tree takes a world's entity from addEntity only with rights to make one, or serverless; the world's
        // entities stand here for the ones the entity server sends
        tree->setIsServerlessMode(true);
        add(domainID, entity::HostType::DOMAIN);
        add(localID, entity::HostType::LOCAL);
        add(mineID, entity::HostType::AVATAR, AVATAR_SELF_ID);
        add(xWornID, entity::HostType::AVATAR, AVATAR_X);
        add(yWornID, entity::HostType::AVATAR, AVATAR_Y);
        add(xLocalChildID, entity::HostType::LOCAL, QUuid(), xWornID);
        add(xDomainChildID, entity::HostType::DOMAIN, QUuid(), xWornID);
    }

    void add(const EntityItemID& id, entity::HostType hostType, const QUuid& owner = QUuid(), const QUuid& parent = QUuid()) {
        EntityItemProperties properties;
        properties.setType(EntityTypes::Box);
        properties.setPosition(glm::vec3(100.0f));
        properties.setDimensions(glm::vec3(0.5f));
        properties.setEntityHostType(hostType);
        if (!owner.isNull()) {
            properties.setOwningAvatarID(owner);
        }
        if (!parent.isNull()) {
            properties.setParentID(parent);
        }
        properties.setCreated(usecTimestampNow());
        tree->withWriteLock([&] { tree->addEntity(id, properties); });
    }

    bool has(const EntityItemID& id) {
        EntityItemPointer entity;
        tree->withReadLock([&] { entity = tree->findEntityByEntityItemID(id); });
        return entity != nullptr;
    }

    EntityItemPointer find(const EntityItemID& id) {
        EntityItemPointer entity;
        tree->withReadLock([&] { entity = tree->findEntityByEntityItemID(id); });
        return entity;
    }

    std::vector<EntityItemID> everyID() const {
        return { domainID, localID, mineID, xWornID, yWornID, xLocalChildID, xDomainChildID };
    }
};

} // namespace

void AvatarEntityOwnerTests::initTestCase() {
    DependencyManager::set<AddressManager>();
    DependencyManager::set<NodeList>(NodeType::Agent, INVALID_PORT);
    DependencyManager::set<SpatialParentFinder, TreeParentFinder>();
}

void AvatarEntityOwnerTests::testAvatarEntityOf() {
    VisitorTree visitor;
    for (const auto& id : visitor.everyID()) {
        QVERIFY(visitor.has(id));
    }
    QVERIFY(visitor.find(visitor.xWornID)->isAvatarEntityOf(AVATAR_X));
    QVERIFY(!visitor.find(visitor.xWornID)->isAvatarEntityOf(AVATAR_Y));
    QVERIFY(!visitor.find(visitor.domainID)->isAvatarEntityOf(AVATAR_X));
    QVERIFY(!visitor.find(visitor.localID)->isAvatarEntityOf(AVATAR_X));
    QVERIFY(!visitor.find(visitor.mineID)->isAvatarEntityOf(AVATAR_X));
    QVERIFY(!visitor.find(visitor.mineID)->isAvatarEntityOf(AVATAR_SELF_ID));
    QVERIFY(!visitor.find(visitor.mineID)->isAvatarEntityOf(QUuid()));
}

void AvatarEntityOwnerTests::testDeleteOnlyThatAvatarsEntities() {
    VisitorTree visitor;
    // avatar X's data names every entity in the tree
    visitor.tree->deleteAvatarEntitiesOf(AVATAR_X, visitor.everyID());

    QVERIFY(!visitor.has(visitor.xWornID));
    QVERIFY(!visitor.has(visitor.xLocalChildID));  // a local child goes with its parent, as with any delete
    QVERIFY(visitor.has(visitor.domainID));
    QVERIFY(visitor.has(visitor.localID));
    QVERIFY(visitor.has(visitor.mineID));
    QVERIFY(visitor.has(visitor.yWornID));
    QVERIFY(visitor.has(visitor.xDomainChildID));
    QVERIFY(visitor.simulation->erased.empty());

    // the control: the general client delete sends a world entity's delete to the entity server
    visitor.tree->deleteEntitiesByID({ visitor.domainID }, true, true);
    QCOMPARE(visitor.simulation->erased.size(), (size_t)1);
}

void AvatarEntityOwnerTests::testNoAvatarRemovesNothing() {
    VisitorTree visitor;
    visitor.tree->deleteAvatarEntitiesOf(QUuid(), visitor.everyID());
    visitor.tree->deleteAvatarEntitiesOf(AVATAR_SELF_ID, visitor.everyID());
    for (const auto& id : visitor.everyID()) {
        QVERIFY(visitor.has(id));
    }
    QVERIFY(visitor.simulation->erased.empty());
}
