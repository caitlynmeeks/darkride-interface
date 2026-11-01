//
//  WorldEntitiesTests.cpp
//  tests/script-api-guard
//
//  Created by scripting on 2026-11-01.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  Which entities a world's scripts may use (security finding #28). Each local entity records who made it when it is
//  added: a world's script, or the visitor's side (the app, the visitor's own scripts). A world's script (marked with
//  ScriptCaller, as the guards mark each of its calls) may use domain entities and the local entities a world's script
//  made, read avatar entities, and nothing else: any other local entity answers as if it didn't exist and can't be
//  changed, reparented onto or sent events. The app and the visitor's own scripts see and change everything as before.
//  An avatar entity a world's script made (a hat the world hands out) is the world's too: it may change it; the
//  visitor's own avatar entities it only reads.
//

#include "WorldEntitiesTests.h"

#include <AccountManager.h>
#include <AddressManager.h>
#include <DependencyManager.h>
#include <NodeList.h>
#include <PointerEvent.h>
#include <ScriptCaller.h>
#include <SpatialParentFinder.h>

#include <EntityItemProperties.h>
#include <EntityScriptingInterface.h>
#include <EntityTree.h>

QTEST_MAIN(WorldEntitiesTests)

namespace {

// the app's parent finder, for entities only (InterfaceParentFinder without the avatars)
class TreeParentFinder : public SpatialParentFinder {
public:
    explicit TreeParentFinder(std::weak_ptr<EntityTree> tree) : _tree(tree) {}
    SpatiallyNestableWeakPointer find(QUuid parentID, bool& success, SpatialParentTree* entityTree) const override {
        SpatiallyNestableWeakPointer parent;
        if (parentID.isNull()) {
            success = true;
            return parent;
        }
        if (entityTree) {
            parent = entityTree->findByID(parentID);
        } else if (auto tree = _tree.lock()) {
            parent = tree->findEntityByEntityItemID(parentID);
        }
        success = !parent.expired();
        return parent;
    }

private:
    std::weak_ptr<EntityTree> _tree;
};

EntityItemProperties box(const QString& name) {
    EntityItemProperties properties;
    properties.setType(EntityTypes::Box);
    properties.setName(name);
    properties.setPosition(glm::vec3(0.0f));
    properties.setDimensions(glm::vec3(1.0f));
    return properties;
}

EntityItemPointer find(const std::shared_ptr<EntityTree>& tree, const QUuid& id) {
    EntityItemPointer entity;
    tree->withReadLock([&] { entity = tree->findEntityByEntityItemID(id); });
    return entity;
}

QUuid addToTree(const std::shared_ptr<EntityTree>& tree, const EntityItemProperties& properties) {
    EntityItemID id(QUuid::createUuid());
    EntityItemPointer entity;
    tree->withWriteLock([&] { entity = tree->addEntity(id, properties); });
    return entity ? QUuid(id) : QUuid();
}

QString nameOf(const std::shared_ptr<EntityTree>& tree, const QUuid& id) {
    auto entity = find(tree, id);
    return entity ? entity->getName() : QString();
}

} // namespace

void WorldEntitiesTests::initTestCase() {
    DependencyManager::registerInheritance<LimitedNodeList, NodeList>();
    DependencyManager::set<AccountManager>();
    DependencyManager::set<AddressManager>();
    DependencyManager::set<NodeList>(NodeType::Agent, INVALID_PORT);
    _tree = std::make_shared<EntityTree>();
    _tree->createRootElement();
    _tree->setIsServerlessMode(true);  // a local file world: domain entities are added straight to the tree
    DependencyManager::set<SpatialParentFinder, TreeParentFinder>(_tree);
    _entities = DependencyManager::set<EntityScriptingInterface>(false);
    _entities->setEntityTree(_tree);
    _packetSender = std::make_unique<EntityEditPacketSender>();
    _entities->setPacketSender(_packetSender.get());  // as the app does (local entities are never sent)

    // as they arrive over the network: a domain entity, another person's avatar entity
    _domain = addToTree(_tree, box("domain"));
    auto avatarProperties = box("avatar");
    avatarProperties.setEntityHostType(entity::HostType::AVATAR);
    avatarProperties.setOwningAvatarID(QUuid::createUuid());
    _avatar = addToTree(_tree, avatarProperties);

    // made through the script API: by the visitor's own script, and by a world's script
    _visitorLocal = _entities->addEntity(box("visitor"), QString("local"));
    {
        ScriptCaller::WorldScope world(true);
        _worldLocal = _entities->addEntity(box("world"), QString("local"));
    }

    // put straight into the tree by C++ (the app's own UI entities may be made this way)
    auto straightProperties = box("straight");
    straightProperties.setEntityHostType(entity::HostType::LOCAL);
    _straightLocal = addToTree(_tree, straightProperties);

    // a local entity with no owner recorded: added as a domain entity, its host type changed afterwards
    _unownedLocal = addToTree(_tree, box("unowned"));
    QVERIFY(find(_tree, _unownedLocal));
    find(_tree, _unownedLocal)->setEntityHostType(entity::HostType::LOCAL);

    for (const auto& id : { _domain, _avatar, _visitorLocal, _worldLocal, _straightLocal, _unownedLocal }) {
        QVERIFY(!id.isNull());
        QVERIFY(find(_tree, id));
    }
}

void WorldEntitiesTests::testOwners() {
    QCOMPARE(int(find(_tree, _domain)->getLocalOwner()), int(entity::LocalOwner::NONE));
    QCOMPARE(int(find(_tree, _avatar)->getLocalOwner()), int(entity::LocalOwner::NONE));
    QCOMPARE(int(find(_tree, _visitorLocal)->getLocalOwner()), int(entity::LocalOwner::VISITOR));
    QCOMPARE(int(find(_tree, _worldLocal)->getLocalOwner()), int(entity::LocalOwner::WORLD));
    QCOMPARE(int(find(_tree, _straightLocal)->getLocalOwner()), int(entity::LocalOwner::VISITOR));
    QCOMPARE(int(find(_tree, _unownedLocal)->getLocalOwner()), int(entity::LocalOwner::NONE));

    QVERIFY(find(_tree, _domain)->isMadeByWorld());
    QVERIFY(find(_tree, _worldLocal)->isMadeByWorld());
    QVERIFY(!find(_tree, _visitorLocal)->isMadeByWorld());
    QVERIFY(!find(_tree, _avatar)->isMadeByWorld());
    QVERIFY(!find(_tree, _unownedLocal)->isMadeByWorld());

    QVERIFY(find(_tree, _visitorLocal)->isVisitorsOwn());
    QVERIFY(find(_tree, _straightLocal)->isVisitorsOwn());
    QVERIFY(!find(_tree, _worldLocal)->isVisitorsOwn());
    QVERIFY(!find(_tree, _domain)->isVisitorsOwn());
    QVERIFY(!find(_tree, _avatar)->isVisitorsOwn());  // another person's
    QVERIFY(!find(_tree, _unownedLocal)->isVisitorsOwn());
}

void WorldEntitiesTests::testWorldReads() {
    ScriptCaller::WorldScope world(true);
    // the world's own and avatar entities: read as before
    QCOMPARE(_entities->forWorldRead(_domain), _domain);
    QCOMPARE(_entities->forWorldRead(_worldLocal), _worldLocal);
    QCOMPARE(_entities->forWorldRead(_avatar), _avatar);
    QCOMPARE(_entities->getEntityProperties(_worldLocal).getName(), QString("world"));
    QCOMPARE(_entities->getEntityProperties(_domain).getName(), QString("domain"));
    QCOMPARE(_entities->getEntityProperties(_avatar).getName(), QString("avatar"));
    QCOMPARE(_entities->getEntityType(_worldLocal), QString("Box"));
    // every other local entity: as if it didn't exist (fail closed: no owner recorded is not the world's)
    for (const auto& id : { _visitorLocal, _straightLocal, _unownedLocal }) {
        QVERIFY(_entities->forWorldRead(id).isNull());
        QCOMPARE(_entities->getEntityProperties(id).getName(), QString());
        QCOMPARE(_entities->getEntityType(id), QString());
        QVERIFY(!_entities->isAddedEntity(id));
    }
    // an id that isn't an entity is left to the call
    const QUuid notAnEntity = QUuid::createUuid();
    QCOMPARE(_entities->forWorldRead(notAnEntity), notAnEntity);

    const QVector<QUuid> all { _domain, _avatar, _visitorLocal, _worldLocal, _straightLocal, _unownedLocal };
    const QVector<QUuid> readable { _domain, _avatar, _worldLocal };
    QCOMPARE(_entities->readableByWorld(all), readable);
}

void WorldEntitiesTests::testVisitorSeesEverything() {
    // no mark: the app and the visitor's own scripts
    const QVector<QUuid> all { _domain, _avatar, _visitorLocal, _worldLocal, _straightLocal, _unownedLocal };
    for (const auto& id : all) {
        QCOMPARE(_entities->forWorldRead(id), id);
    }
    QCOMPARE(_entities->readableByWorld(all), all);
    QCOMPARE(_entities->getEntityProperties(_visitorLocal).getName(), QString("visitor"));
    QVERIFY(_entities->isAddedEntity(_straightLocal));
}

void WorldEntitiesTests::testWorldChanges() {
    EntityItemProperties rename;
    rename.setName("changed by a world");
    {
        ScriptCaller::WorldScope world(true);
        for (const auto& id : { _visitorLocal, _straightLocal, _unownedLocal, _avatar }) {
            QVERIFY(_entities->editEntity(id, rename).isNull());
            _entities->deleteEntity(id);
        }
        QCOMPARE(_entities->editEntity(_worldLocal, rename), _worldLocal);
    }
    QCOMPARE(nameOf(_tree, _visitorLocal), QString("visitor"));
    QCOMPARE(nameOf(_tree, _straightLocal), QString("straight"));
    QCOMPARE(nameOf(_tree, _unownedLocal), QString("unowned"));
    QCOMPARE(nameOf(_tree, _avatar), QString("avatar"));
    QCOMPARE(nameOf(_tree, _worldLocal), QString("changed by a world"));

    // the visitor's own scripts change the visitor's entity as before
    EntityItemProperties back;
    back.setName("visitor, renamed by the visitor");
    QCOMPARE(_entities->editEntity(_visitorLocal, back), _visitorLocal);
    QCOMPARE(nameOf(_tree, _visitorLocal), QString("visitor, renamed by the visitor"));

    // a world may delete its own
    QUuid worldsSecond;
    {
        ScriptCaller::WorldScope world(true);
        worldsSecond = _entities->addEntity(box("world's second"), QString("local"));
        QVERIFY(!worldsSecond.isNull());
        _entities->deleteEntity(worldsSecond);
    }
    QVERIFY(!find(_tree, worldsSecond));
}

void WorldEntitiesTests::testWorldParents() {
    ScriptCaller::WorldScope world(true);
    // onto the visitor's local entity: refused, both when made and by an edit
    auto child = box("child");
    child.setParentID(_visitorLocal);
    QVERIFY(_entities->addEntity(child, QString("local")).isNull());
    EntityItemProperties reparent;
    reparent.setParentID(_straightLocal);
    QVERIFY(_entities->editEntity(_worldLocal, reparent).isNull());
    QVERIFY(find(_tree, _worldLocal)->getParentID().isNull());
    // onto the world's own entity: allowed
    auto ownChild = box("own child");
    ownChild.setParentID(_worldLocal);
    const QUuid ownChildID = _entities->addEntity(ownChild, QString("local"));
    QVERIFY(!ownChildID.isNull());
    QCOMPARE(find(_tree, ownChildID)->getParentID(), _worldLocal);
    // a hidden entity's children aren't listed to the world, nor is a hidden parent's
    QVERIFY(_entities->getChildrenIDs(_visitorLocal).isEmpty());
}

void WorldEntitiesTests::testWorldEvents() {
    QList<QUuid> pressed;
    auto connection = connect(_entities.data(), &EntityScriptingInterface::mousePressOnEntity, this,
                              [&](const EntityItemID& id, const PointerEvent&) { pressed.push_back(id); });
    {
        ScriptCaller::WorldScope world(true);
        _entities->sendMousePressOnEntity(EntityItemID(_visitorLocal), PointerEvent());
        _entities->sendMousePressOnEntity(EntityItemID(_straightLocal), PointerEvent());
        _entities->sendMousePressOnEntity(EntityItemID(_avatar), PointerEvent());
        _entities->sendMousePressOnEntity(EntityItemID(_worldLocal), PointerEvent());
        _entities->sendMousePressOnEntity(EntityItemID(_domain), PointerEvent());
    }
    _entities->sendMousePressOnEntity(EntityItemID(_visitorLocal), PointerEvent());  // the visitor's own script
    disconnect(connection);
    const QList<QUuid> expected { _worldLocal, _domain, _visitorLocal };
    QCOMPARE(pressed, expected);
}

void WorldEntitiesTests::testOwnerKeptOnEdit() {
    EntityItemProperties edit;
    edit.setName("edited by the visitor");
    QCOMPARE(_entities->editEntity(_worldLocal, edit), _worldLocal);
    QCOMPARE(int(find(_tree, _worldLocal)->getLocalOwner()), int(entity::LocalOwner::WORLD));
    {
        ScriptCaller::WorldScope world(true);
        edit.setName("edited by a world");
        QCOMPARE(_entities->editEntity(_worldLocal, edit), _worldLocal);
    }
    QCOMPARE(int(find(_tree, _worldLocal)->getLocalOwner()), int(entity::LocalOwner::WORLD));
    QCOMPARE(int(find(_tree, _visitorLocal)->getLocalOwner()), int(entity::LocalOwner::VISITOR));
}

void WorldEntitiesTests::testWorldAvatarEntities() {
    // one the visitor's own script made (as the visitor's settings and favourites make them), one a world's script made
    const QUuid visitorHat = _entities->addEntity(box("visitor's hat"), QString("avatar"));
    QUuid worldHat;
    {
        ScriptCaller::WorldScope world(true);
        worldHat = _entities->addEntity(box("world's hat"), QString("avatar"));
    }
    QVERIFY(!visitorHat.isNull());
    QVERIFY(!worldHat.isNull());
    QVERIFY(find(_tree, worldHat)->isMyAvatarEntity());
    QCOMPARE(int(find(_tree, visitorHat)->getLocalOwner()), int(entity::LocalOwner::NONE));
    QCOMPARE(int(find(_tree, worldHat)->getLocalOwner()), int(entity::LocalOwner::WORLD));
    QVERIFY(find(_tree, worldHat)->isMadeByWorld());
    QVERIFY(!find(_tree, worldHat)->isVisitorsOwn());
    QVERIFY(find(_tree, visitorHat)->isVisitorsOwn());
    QVERIFY(!find(_tree, visitorHat)->isMadeByWorld());

    EntityItemProperties rename;
    rename.setName("changed by a world");
    {
        ScriptCaller::WorldScope world(true);
        // the world reads both, changes its own and not the visitor's
        QCOMPARE(_entities->forWorldRead(visitorHat), visitorHat);
        QCOMPARE(_entities->getEntityProperties(visitorHat).getName(), QString("visitor's hat"));
        QVERIFY(_entities->editEntity(visitorHat, rename).isNull());
        _entities->deleteEntity(visitorHat);  // refused before anything is deleted
        QCOMPARE(_entities->editEntity(worldHat, rename), worldHat);
        // and may attach its own things to its hat
        auto feather = box("feather");
        feather.setParentID(worldHat);
        const QUuid featherID = _entities->addEntity(feather, QString("local"));
        QVERIFY(!featherID.isNull());
        QCOMPARE(find(_tree, featherID)->getParentID(), worldHat);
    }
    QCOMPARE(nameOf(_tree, visitorHat), QString("visitor's hat"));
    QCOMPARE(nameOf(_tree, worldHat), QString("changed by a world"));
    QCOMPARE(int(find(_tree, worldHat)->getLocalOwner()), int(entity::LocalOwner::WORLD));

    // the visitor's side changes either, and the owner stays
    EntityItemProperties back;
    back.setName("renamed by the visitor");
    QCOMPARE(_entities->editEntity(worldHat, back), worldHat);
    QCOMPARE(nameOf(_tree, worldHat), QString("renamed by the visitor"));
    QCOMPARE(int(find(_tree, worldHat)->getLocalOwner()), int(entity::LocalOwner::WORLD));
}

void WorldEntitiesTests::testWorldClones() {
    // a domain entity that clones as an avatar entity (a cloneable hat on a rack)
    auto rackHat = box("rack hat");
    rackHat.setCloneable(true);
    rackHat.setCloneAvatarEntity(true);
    const QUuid rack = addToTree(_tree, rackHat);
    QVERIFY(!rack.isNull());

    QUuid worldsCloneOfOwn, worldsCloneOfRack, worldsCloneOfVisitors;
    {
        ScriptCaller::WorldScope world(true);
        worldsCloneOfOwn = _entities->cloneEntity(_worldLocal);
        worldsCloneOfRack = _entities->cloneEntity(rack);
        worldsCloneOfVisitors = _entities->cloneEntity(_visitorLocal);  // hidden: as if it didn't exist
    }
    const QUuid visitorsCloneOfRack = _entities->cloneEntity(rack);
    const QUuid visitorsCloneOfOwn = _entities->cloneEntity(_visitorLocal);

    QVERIFY(worldsCloneOfVisitors.isNull());
    for (const auto& id : { worldsCloneOfOwn, worldsCloneOfRack, visitorsCloneOfRack, visitorsCloneOfOwn }) {
        QVERIFY(!id.isNull());
        QVERIFY(find(_tree, id));
    }
    QVERIFY(find(_tree, worldsCloneOfOwn)->isLocalEntity());
    QCOMPARE(int(find(_tree, worldsCloneOfOwn)->getLocalOwner()), int(entity::LocalOwner::WORLD));
    QVERIFY(find(_tree, worldsCloneOfRack)->isMyAvatarEntity());
    QCOMPARE(int(find(_tree, worldsCloneOfRack)->getLocalOwner()), int(entity::LocalOwner::WORLD));
    QVERIFY(find(_tree, visitorsCloneOfRack)->isMyAvatarEntity());
    QCOMPARE(int(find(_tree, visitorsCloneOfRack)->getLocalOwner()), int(entity::LocalOwner::NONE));
    QVERIFY(find(_tree, visitorsCloneOfOwn)->isLocalEntity());
    QCOMPARE(int(find(_tree, visitorsCloneOfOwn)->getLocalOwner()), int(entity::LocalOwner::VISITOR));
}

void WorldEntitiesTests::cleanupTestCase() {
    _entities->setEntityTree(nullptr);
    _entities->setPacketSender(nullptr);
    _tree.reset();
    DependencyManager::destroy<EntityScriptingInterface>();
}
