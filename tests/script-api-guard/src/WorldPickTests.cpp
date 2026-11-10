//
//  WorldPickTests.cpp
//  tests/script-api-guard
//
//  Created by scripting on 2026-11-10.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  Picks and pointers for a world's script (WorldPicks): it names only the picks and pointers a world's script made
//  (any other id is an id that doesn't exist, and the app's pick or pointer is left exactly as it was); its include
//  and ignore lists and lock end take only entities it may read; and a result that lands on an entity it may not
//  read is no hit, every field of the hit cleared. Every other caller is unaffected.
//

#include "WorldPickTests.h"

#include <cfloat>
#include <functional>

#include <QtCore/QSet>

#include <DependencyManager.h>
#include <PickManager.h>
#include <PickTransformNode.h>
#include <Pointer.h>
#include <PointerManager.h>
#include <RegisteredMetaTypes.h>
#include <ScriptCaller.h>
#include <WorldPicks.h>

QTEST_MAIN(WorldPickTests)

namespace {

const int WORLD = 5;
const int OTHER_WORLD = 6;
const int LEFT_WORLD = 99;

const QUuid WORLDS_ENTITY = QUuid::createUuid();
const QUuid HIDDEN_ENTITY = QUuid::createUuid();  // the visitor's or the app's: a world may not read it
const QUuid AN_AVATAR = QUuid::createUuid();

class TestResult : public PickResult {
public:
    explicit TestResult(const QVariantMap& map) : map(map) {}
    QVariantMap map;
    QVariantMap toVariantMap() const override { return map; }
    bool doesIntersect() const override { return map.value("intersects").toBool(); }
    bool checkOrFilterAgainstMaxDistance(float) override { return true; }
    PickResultPointer compareAndProcessNewResult(const PickResultPointer& newRes) override { return newRes; }
};

class TestPick : public PickQuery {
public:
    TestPick() : PickQuery(PickFilter(), 100.0f, true) {}
    PickType getType() const override { return Ray; }
    Transform getResultTransform() const override { return Transform(); }
};

class TestPointer : public Pointer {
public:
    explicit TestPointer(unsigned int pickID) : Pointer(pickID, true, true) {}

    int changes { 0 };  // every call that would change or re-aim the pointer

    PickQuery::PickType getType() const override { return PickQuery::PickType::Ray; }
    void setRenderState(const std::string&) override { ++changes; }
    void editRenderState(const std::string&, const QVariant&, const QVariant&, const QVariant&) override { ++changes; }
    void setLength(float) override { ++changes; }
    void setLockEndUUID(const QUuid& objectID, bool, const glm::mat4&) override { ++changes; lockEnd = objectID; }
    void updateVisuals(const PickResultPointer&) override {}

    QUuid lockEnd;

protected:
    PointerEvent buildPointerEvent(const PickedObject&, const PickResultPointer&, const std::string&, bool) override {
        return PointerEvent();
    }
    PickedObject getHoveredObject(const PickResultPointer&) override { return PickedObject(); }
    Buttons getPressedButtons(const PickResultPointer&) override { return Buttons(); }
    PickResultPointer getPickResultCopy(const PickResultPointer& pickResult) const override { return pickResult; }
};

QSharedPointer<PickManager> picks() { return DependencyManager::get<PickManager>(); }
QSharedPointer<PointerManager> pointers() { return DependencyManager::get<PointerManager>(); }

QVariantMap rayHit(const QUuid& id, IntersectionType type) {
    QVariantMap searchRay { { "origin", vec3toVariant(glm::vec3(1.0f)) } };
    return {
        { "type", type }, { "intersects", type != NONE }, { "objectID", id }, { "distance", 2.5f },
        { "intersection", vec3toVariant(glm::vec3(1.0f, 2.0f, 3.0f)) },
        { "surfaceNormal", vec3toVariant(glm::vec3(0.0f, 1.0f, 0.0f)) },
        { "searchRay", searchRay }, { "extraInfo", QVariantMap { { "subMeshIndex", 3 } } },
        { "someLaterField", 7 }
    };
}

unsigned int addPick() {
    auto pick = std::make_shared<TestPick>();
    pick->setScriptParameters({ { "made", "by the test" } });
    pick->setPickResult(std::make_shared<TestResult>(rayHit(WORLDS_ENTITY, ENTITY)));
    return picks()->addPick(PickQuery::Ray, pick);
}

struct MadePointer {
    unsigned int id { 0 };
    unsigned int pickID { 0 };
    std::shared_ptr<TestPointer> pointer;
};

MadePointer addPointer() {
    MadePointer made;
    made.pickID = addPick();
    made.pointer = std::make_shared<TestPointer>(made.pickID);
    made.id = pointers()->addPointer(made.pointer);
    return made;
}

// every PickManager call that takes a pick's id (keep in step with PickManager.h; removePick is tested apart)
void everyPickCall(unsigned int uid) {
    auto m = picks();
    m->enablePick(uid);
    m->disablePick(uid);
    QVERIFY(!m->isPickEnabled(uid));
    QVERIFY(!m->getPrevPickResult(uid));
    QVERIFY(m->getPickProperties(uid).isEmpty());
    QVERIFY(m->getPickScriptParameters(uid).isEmpty());
    m->setPrecisionPicking(uid, true);
    m->setIgnoreItems(uid, { WORLDS_ENTITY });
    m->setIncludeItems(uid, { WORLDS_ENTITY });
    QVERIFY(!m->isLeftHand(uid));
    QVERIFY(!m->isRightHand(uid));
    QVERIFY(!m->isMouse(uid));
    QCOMPARE(m->getParentTransform(uid), Transform());
    QCOMPARE(m->getResultTransform(uid), Transform());
    m->removePick(uid);
}

// every PointerManager call that takes a pointer's id (keep in step with PointerManager.h)
void everyPointerCall(unsigned int uid) {
    auto m = pointers();
    m->enablePointer(uid);
    m->disablePointer(uid);
    QVERIFY(!m->isPointerEnabled(uid));
    m->setRenderState(uid, "half");
    m->editRenderState(uid, "half", QVariant(), QVariant(), QVariant());
    QVERIFY(!m->getPrevPickResult(uid));
    QVERIFY(m->getPointerProperties(uid).isEmpty());
    QVERIFY(m->getPointerScriptParameters(uid).isEmpty());
    m->setPrecisionPicking(uid, true);
    m->setIgnoreItems(uid, { WORLDS_ENTITY });
    m->setIncludeItems(uid, { WORLDS_ENTITY });
    m->setLength(uid, 3.0f);
    m->setLockEndUUID(uid, WORLDS_ENTITY, false);
    QVERIFY(!m->isLeftHand(uid));
    QVERIFY(!m->isRightHand(uid));
    QVERIFY(!m->isMouse(uid));
    m->removePointer(uid);
}

} // namespace

void WorldPickTests::initTestCase() {
    DependencyManager::set<PickManager>();
    DependencyManager::set<PointerManager>();
}

void WorldPickTests::init() {
    WorldPicks::setReadCheck([](const QUuid& id, bool) { return id != HIDDEN_ENTITY; });
}

void WorldPickTests::worldNamesOnlyAWorldsPicks() {
    const unsigned int apps = addPick();
    unsigned int own = 0, others = 0;
    { ScriptCaller::WorldScope world(WORLD); own = addPick(); }
    { ScriptCaller::WorldScope world(OTHER_WORLD); others = addPick(); }

    QVERIFY(picks()->getPicks().contains(apps));  // the app lists all three
    QVERIFY(picks()->getPicks().contains(own));
    QVERIFY(picks()->getPicks().contains(others));
    {
        ScriptCaller::WorldScope world(WORLD);
        const auto listed = picks()->getPicks();
        QVERIFY(listed.contains(own));
        QVERIFY(!listed.contains(apps));
        QVERIFY(!listed.contains(others));
        QVERIFY(picks()->isPickEnabled(own));
        QVERIFY(picks()->getPrevPickResult(own));
        QVERIFY(!picks()->getPickScriptParameters(own).isEmpty());
        picks()->disablePick(own);
        QVERIFY(!picks()->isPickEnabled(own));
    }
    {
        // a world's caller with no world in particular (a hop no one carried the world across): any world's, not the app's
        ScriptCaller::WorldScope world(true);
        const auto listed = picks()->getPicks();
        QVERIFY(listed.contains(own));
        QVERIFY(listed.contains(others));
        QVERIFY(!listed.contains(apps));
    }
    {
        ScriptCaller::WorldScope world(WORLD);
        picks()->removePick(own);
    }
    QVERIFY(!picks()->getPicks().contains(own));
    picks()->removePick(apps);
    picks()->removePick(others);
}

void WorldPickTests::everyPickCallLeavesTheAppsPickAlone() {
    for (int maker : { 0, OTHER_WORLD }) {
        unsigned int uid = 0;
        {
            ScriptCaller::WorldScope made(maker);
            uid = addPick();
        }
        picks()->setIgnoreItems(uid, { AN_AVATAR });
        picks()->setIncludeItems(uid, { AN_AVATAR });
        {
            ScriptCaller::WorldScope world(WORLD);
            everyPickCall(uid);
            if (QTest::currentTestFailed()) {
                return;
            }
        }
        // as it was, for the app
        QVERIFY(picks()->getPicks().contains(uid));
        QVERIFY(picks()->isPickEnabled(uid));
        QVERIFY(picks()->getPrevPickResult(uid));
        QCOMPARE(picks()->getPickScriptParameters(uid).value("made").toString(), QString("by the test"));
        QCOMPARE(picks()->getPickProperties(uid).value("enabled").toBool(), true);
        picks()->removePick(uid);
        QVERIFY(!picks()->getPicks().contains(uid));
    }
}

void WorldPickTests::worldNamesOnlyAWorldsPointers() {
    const MadePointer apps = addPointer();
    MadePointer own, others;
    { ScriptCaller::WorldScope world(WORLD); own = addPointer(); }
    { ScriptCaller::WorldScope world(OTHER_WORLD); others = addPointer(); }
    {
        ScriptCaller::WorldScope world(WORLD);
        const auto listed = pointers()->getPointers();
        QVERIFY(listed.contains(own.id));
        QVERIFY(!listed.contains(apps.id));
        QVERIFY(!listed.contains(others.id));
        QVERIFY(pointers()->isPointerEnabled(own.id));
        pointers()->setRenderState(own.id, "full");
        pointers()->setLength(own.id, 2.0f);
        QCOMPARE(own.pointer->changes, 2);
        QVERIFY(!pointers()->getPointerProperties(own.id).isEmpty());  // reads its own pick too
        pointers()->removePointer(own.id);
    }
    QVERIFY(!pointers()->getPointers().contains(own.id));
    QVERIFY(pointers()->getPointers().contains(apps.id));
    pointers()->removePointer(apps.id);
    pointers()->removePointer(others.id);
}

void WorldPickTests::everyPointerCallLeavesTheAppsPointerAlone() {
    for (int maker : { 0, OTHER_WORLD }) {
        MadePointer made;
        {
            ScriptCaller::WorldScope by(maker);
            made = addPointer();
        }
        picks()->setIgnoreItems(made.pickID, { AN_AVATAR });
        {
            ScriptCaller::WorldScope world(WORLD);
            everyPointerCall(made.id);
            if (QTest::currentTestFailed()) {
                return;
            }
            everyPickCall(made.pickID);  // nor through the pointer's pick
            if (QTest::currentTestFailed()) {
                return;
            }
        }
        QVERIFY(pointers()->getPointers().contains(made.id));
        QVERIFY(pointers()->isPointerEnabled(made.id));
        QCOMPARE(made.pointer->changes, 0);
        QVERIFY(made.pointer->lockEnd.isNull());
        QVERIFY(picks()->isPickEnabled(made.pickID));
        QVERIFY(pointers()->getPrevPickResult(made.id));
        // and it still works for the app
        pointers()->setRenderState(made.id, "full");
        QCOMPARE(made.pointer->changes, 1);
        pointers()->disablePointer(made.id);
        QVERIFY(!pointers()->isPointerEnabled(made.id));
        pointers()->removePointer(made.id);
        QVERIFY(!pointers()->getPointers().contains(made.id));
    }
}

void WorldPickTests::aWorldsPickIsParentedOnlyToAWorldsPick() {
    // a pick parented to another pick starts where that pick's result is; a world's script may not start its own
    // pick from the app's (or read where the app's lands, through its own pick's origin)
    const unsigned int apps = addPick();
    unsigned int own = 0;
    { ScriptCaller::WorldScope world(WORLD); own = addPick(); }
    QVERIFY(picks()->callerMayName(apps));
    {
        ScriptCaller::WorldScope world(WORLD);
        QVERIFY(!picks()->callerMayName(apps));
        QVERIFY(picks()->callerMayName(own));
        PickTransformNode ontoApps(apps);
        QCOMPARE(ontoApps.toVariantMap().value("parentID").toUInt(), 0u);  // no parent
        PickTransformNode ontoOwn(own);
        QCOMPARE(ontoOwn.toVariantMap().value("parentID").toUInt(), own);
        // every small number but its own: none taken
        for (unsigned int uid = 1; uid <= 64; ++uid) {
            if (uid != own) {
                QCOMPARE(PickTransformNode(uid).toVariantMap().value("parentID").toUInt(), 0u);
            }
        }
    }
    // made by a world, read later by the app (as the pick manager does each frame): still no parent
    std::shared_ptr<PickTransformNode> kept;
    { ScriptCaller::WorldScope world(WORLD); kept = std::make_shared<PickTransformNode>(apps); }
    QCOMPARE(kept->toVariantMap().value("parentID").toUInt(), 0u);
    QCOMPARE(kept->getTransform(), Transform());
    // the app parents to any
    QCOMPARE(PickTransformNode(apps).toVariantMap().value("parentID").toUInt(), apps);
    QCOMPARE(PickTransformNode(own).toVariantMap().value("parentID").toUInt(), own);
    picks()->removePick(apps);
    picks()->removePick(own);
}

void WorldPickTests::addPickMarksAWorldsPick() {
    auto apps = std::make_shared<TestPick>();
    const unsigned int appsID = picks()->addPick(PickQuery::Ray, apps);
    QVERIFY(!apps->isMadeByWorld());
    auto worlds = std::make_shared<TestPick>();
    unsigned int worldsID = 0;
    { ScriptCaller::WorldScope world(WORLD); worldsID = picks()->addPick(PickQuery::Ray, worlds); }
    QVERIFY(worlds->isMadeByWorld());
    picks()->removePick(appsID);
    picks()->removePick(worldsID);
}

namespace {
    // what a cast gives: the first thing along the pick that isn't ignored
    struct Hit {
        bool intersects { false };
        QUuid entityID;
        float distance { 0.0f };
    };

    struct Along {
        QVector<QUuid> things;  // nearest first
        int casts { 0 };
        Hit cast(const QVector<QUuid>& ignore) {
            ++casts;
            for (int i = 0; i < things.size(); ++i) {
                if (!ignore.contains(things[i])) {
                    return Hit { true, things[i], float(i + 1) };
                }
            }
            return Hit();
        }
    };

    QVector<QUuid> hiddenThings(int count) {
        QVector<QUuid> things;
        for (int i = 0; i < count; ++i) {
            things.push_back(QUuid::createUuid());
        }
        return things;
    }
}

void WorldPickTests::aWorldsPickPassesThroughWhatIsHidden() {
    QSet<QUuid> hidden;
    WorldPicks::setHiddenCheck([&](const QUuid& id) { return hidden.contains(id); });
    auto through = [](Along& along, const QVector<QUuid>& ignore = {}) {
        return WorldPicks::castPastHidden(ignore, [&](const QVector<QUuid>& ignored) { return along.cast(ignored); });
    };

    // an app's local entity in front of a domain entity: the domain entity, at its own distance
    {
        Along along { hiddenThings(1) + QVector<QUuid> { WORLDS_ENTITY } };
        hidden = QSet<QUuid>(along.things.begin(), along.things.end() - 1);
        const Hit hit = through(along);
        QVERIFY(hit.intersects);
        QCOMPARE(hit.entityID, WORLDS_ENTITY);
        QCOMPARE(hit.distance, 2.0f);
        QCOMPARE(along.casts, 2);
    }
    // nothing hidden in the way: one cast, as every other pick
    {
        Along along { { WORLDS_ENTITY } };
        hidden.clear();
        QCOMPARE(through(along).entityID, WORLDS_ENTITY);
        QCOMPARE(along.casts, 1);
    }
    // only something hidden: no hit
    {
        Along along { hiddenThings(1) };
        hidden = QSet<QUuid>(along.things.begin(), along.things.end());
        const Hit hit = through(along);
        QVERIFY(!hit.intersects);
        QVERIFY(hit.entityID.isNull());
    }
    // as many hidden things as it passes through, then the world's: still found
    {
        Along along { hiddenThings(WorldPicks::MOST_PASSED_THROUGH) + QVector<QUuid> { WORLDS_ENTITY } };
        hidden = QSet<QUuid>(along.things.begin(), along.things.end() - 1);
        QCOMPARE(through(along).entityID, WORLDS_ENTITY);
    }
    // one more: no hit, and never the hidden one
    {
        Along along { hiddenThings(WorldPicks::MOST_PASSED_THROUGH + 1) + QVector<QUuid> { WORLDS_ENTITY } };
        hidden = QSet<QUuid>(along.things.begin(), along.things.end() - 1);
        const Hit hit = through(along);
        QVERIFY(!hit.intersects);
        QVERIFY(hit.entityID.isNull());
        QCOMPARE(hit.distance, 0.0f);
        QCOMPARE(along.casts, WorldPicks::MOST_PASSED_THROUGH + 1);
    }
    // the pick's own ignore list stays ignored
    {
        Along along { { AN_AVATAR, WORLDS_ENTITY } };
        hidden.clear();
        QCOMPARE(through(along, { AN_AVATAR }).entityID, WORLDS_ENTITY);
    }
    // no check set: everything is hidden, so no entity is hit
    {
        WorldPicks::setHiddenCheck(nullptr);
        Along along { { WORLDS_ENTITY } };
        QVERIFY(!through(along).intersects);
        QVERIFY(WorldPicks::isHiddenFromWorlds(WORLDS_ENTITY));
    }
    WorldPicks::setHiddenCheck(nullptr);
}

void WorldPickTests::aWorldThatWasLeftNamesNothing() {
    unsigned int own = 0;
    { ScriptCaller::WorldScope world(LEFT_WORLD); own = addPick(); }
    ScriptCaller::retireWorld(LEFT_WORLD);
    {
        ScriptCaller::WorldScope world(LEFT_WORLD);
        QVERIFY(picks()->getPicks().isEmpty());
        QVERIFY(!picks()->getPrevPickResult(own));
        picks()->removePick(own);
    }
    QVERIFY(picks()->getPicks().contains(own));
    picks()->removePick(own);
}

void WorldPickTests::listsDropWhatTheWorldMayNotRead() {
    const QVector<QUuid> asked { WORLDS_ENTITY, HIDDEN_ENTITY, AN_AVATAR };
    QCOMPARE(WorldPicks::readableByCaller(asked), asked);  // the app: all
    auto pick = std::make_shared<TestPick>();
    unsigned int uid = 0;
    {
        ScriptCaller::WorldScope world(WORLD);
        uid = picks()->addPick(PickQuery::Ray, pick);
        picks()->setIgnoreItems(uid, asked);
        picks()->setIncludeItems(uid, asked);
    }
    const QVector<QUuid> kept { WORLDS_ENTITY, AN_AVATAR };
    QCOMPARE(pick->getIgnoreItems(), kept);
    QCOMPARE(pick->getIncludeItems(), kept);
    picks()->setIgnoreItems(uid, asked);  // the app, on the same pick: all
    QCOMPARE(pick->getIgnoreItems(), asked);
    picks()->removePick(uid);
}

void WorldPickTests::lockEndOnlyOntoWhatTheWorldMayRead() {
    MadePointer own;
    {
        ScriptCaller::WorldScope world(WORLD);
        own = addPointer();
        pointers()->setLockEndUUID(own.id, HIDDEN_ENTITY, false);
        QCOMPARE(own.pointer->changes, 0);
        pointers()->setLockEndUUID(own.id, WORLDS_ENTITY, false);
        QCOMPARE(own.pointer->lockEnd, WORLDS_ENTITY);
        pointers()->setLockEndUUID(own.id, AN_AVATAR, true);
        QCOMPARE(own.pointer->lockEnd, AN_AVATAR);
    }
    pointers()->removePointer(own.id);
}

void WorldPickTests::aHitTheWorldMayNotReadIsNoHit_data() {
    QTest::addColumn<int>("type");
    QTest::newRow("an entity") << int(ENTITY);
    QTest::newRow("a local entity (a keyboard's key, the tablet)") << int(LOCAL_ENTITY);
}

void WorldPickTests::aHitTheWorldMayNotReadIsNoHit() {
    QFETCH(int, type);
    const QVariantMap hit = rayHit(HIDDEN_ENTITY, IntersectionType(type));
    QVariantMap read;
    {
        ScriptCaller::WorldScope world(WORLD);
        read = WorldPicks::resultForCaller(std::make_shared<TestResult>(hit));
    }
    QCOMPARE(read.value("type").toInt(), int(NONE));
    QCOMPARE(read.value("intersects").toBool(), false);
    QVERIFY(read.value("objectID").toUuid().isNull());
    QCOMPARE(read.value("distance").toFloat(), FLT_MAX);
    QVERIFY(read.value("intersection") != hit.value("intersection"));
    QVERIFY(read.value("surfaceNormal") != hit.value("surfaceNormal"));
    QVERIFY(read.value("extraInfo").toMap().isEmpty());
    QVERIFY(!read.contains("someLaterField"));       // a field added to a hit later is left out, not passed on
    QCOMPARE(read.value("searchRay"), hit.value("searchRay"));  // the world's own ray stays

    // a hit on what it may read is read whole
    const QVariantMap ownHit = rayHit(WORLDS_ENTITY, IntersectionType(type));
    {
        ScriptCaller::WorldScope world(WORLD);
        QCOMPARE(WorldPicks::resultForCaller(std::make_shared<TestResult>(ownHit)), ownHit);
        // an avatar or the HUD is no entity: as the pick has it
        const QVariantMap avatarHit = rayHit(AN_AVATAR, AVATAR);
        QCOMPARE(WorldPicks::resultForCaller(std::make_shared<TestResult>(avatarHit)), avatarHit);
        QVERIFY(WorldPicks::resultForCaller(PickResultPointer()).isEmpty());
    }
}

void WorldPickTests::aCollisionListLeavesOutWhatTheWorldMayNotRead() {
    auto object = [](const QUuid& id, IntersectionType type) {
        return QVariant(QVariantMap { { "id", id }, { "type", type }, { "collisionContacts", QVariantList { 1 } } });
    };
    QVariantMap region { { "shape", "sphere" } };
    QVariantMap touching {
        { "intersects", true }, { "collisionRegion", region },
        { "intersectingObjects", QVariantList { object(HIDDEN_ENTITY, ENTITY), object(WORLDS_ENTITY, ENTITY), object(AN_AVATAR, AVATAR) } }
    };
    QVariantMap onlyHidden = touching;
    onlyHidden["intersectingObjects"] = QVariantList { object(HIDDEN_ENTITY, ENTITY) };

    ScriptCaller::WorldScope world(WORLD);
    const QVariantMap read = WorldPicks::resultForCaller(std::make_shared<TestResult>(touching));
    const QVariantList kept = read.value("intersectingObjects").toList();
    QCOMPARE(kept.size(), 2);
    for (const QVariant& entry : kept) {
        QVERIFY(entry.toMap().value("id").toUuid() != HIDDEN_ENTITY);
    }
    QCOMPARE(read.value("intersects").toBool(), true);
    QCOMPARE(read.value("collisionRegion"), touching.value("collisionRegion"));

    const QVariantMap none = WorldPicks::resultForCaller(std::make_shared<TestResult>(onlyHidden));
    QVERIFY(none.value("intersectingObjects").toList().isEmpty());
    QCOMPARE(none.value("intersects").toBool(), false);
}

void WorldPickTests::otherCallersReadEveryResult() {
    const QVariantMap hit = rayHit(HIDDEN_ENTITY, LOCAL_ENTITY);
    QCOMPARE(WorldPicks::resultForCaller(std::make_shared<TestResult>(hit)), hit);
    QVERIFY(WorldPicks::callerMayRead(HIDDEN_ENTITY));
}

void WorldPickTests::theAppReachesEveryPickAndPointer() {
    // A caller with no world on its thread (the app: the keyboard's mallets, sign-in's lasers, the mouse pick; and the
    // visitor's own scripts) reaches every pick and pointer, whoever made it, with nothing filtered.
    const QVector<QUuid> any { WORLDS_ENTITY, HIDDEN_ENTITY, AN_AVATAR };
    for (int maker : { 0, WORLD }) {
        MadePointer made;
        std::shared_ptr<TestPick> pick = std::make_shared<TestPick>();
        unsigned int pickID = 0;
        {
            ScriptCaller::WorldScope by(maker);
            made = addPointer();
            pickID = picks()->addPick(PickQuery::Ray, pick);
        }
        pick->setScriptParameters({ { "made", "by the test" } });
        pick->setPickResult(std::make_shared<TestResult>(rayHit(HIDDEN_ENTITY, LOCAL_ENTITY)));

        // picks: listed, read, changed, re-aimed
        QVERIFY(picks()->getPicks().contains(pickID));
        QVERIFY(picks()->isPickEnabled(pickID));
        picks()->disablePick(pickID);
        QVERIFY(!picks()->isPickEnabled(pickID));
        picks()->enablePick(pickID);
        QVERIFY(picks()->isPickEnabled(pickID));
        QVERIFY(!picks()->getPickProperties(pickID).isEmpty());
        QCOMPARE(picks()->getPickScriptParameters(pickID).value("made").toString(), QString("by the test"));
        picks()->setIgnoreItems(pickID, any);
        picks()->setIncludeItems(pickID, any);
        QCOMPARE(pick->getIgnoreItems(), any);
        QCOMPARE(pick->getIncludeItems(), any);
        // a result on something a world may not read: whole, typed and as a script of the visitor's gets it
        auto typed = picks()->getPrevPickResultTyped<TestResult>(pickID);
        QVERIFY(typed);
        QCOMPARE(typed->map.value("objectID").toUuid(), HIDDEN_ENTITY);
        QCOMPARE(WorldPicks::resultForCaller(picks()->getPrevPickResult(pickID)), rayHit(HIDDEN_ENTITY, LOCAL_ENTITY));

        // pointers: listed, read, changed, re-aimed, locked onto anything
        QVERIFY(pointers()->getPointers().contains(made.id));
        QVERIFY(pointers()->isPointerEnabled(made.id));
        pointers()->disablePointer(made.id);
        QVERIFY(!pointers()->isPointerEnabled(made.id));
        pointers()->enablePointer(made.id);
        QVERIFY(pointers()->isPointerEnabled(made.id));
        pointers()->setRenderState(made.id, "full");
        pointers()->editRenderState(made.id, "full", QVariant(), QVariant(), QVariant());
        pointers()->setLength(made.id, 2.0f);
        pointers()->setLockEndUUID(made.id, HIDDEN_ENTITY, false);
        QCOMPARE(made.pointer->changes, 4);
        QCOMPARE(made.pointer->lockEnd, HIDDEN_ENTITY);
        QVERIFY(pointers()->getPrevPickResult(made.id));
        QVERIFY(!pointers()->getPointerProperties(made.id).isEmpty());
        pointers()->setIncludeItems(made.id, any);
        QVERIFY(picks()->getPickProperties(made.pickID).contains("enabled"));

        // removed
        pointers()->removePointer(made.id);
        QVERIFY(!pointers()->getPointers().contains(made.id));
        picks()->removePick(pickID);
        QVERIFY(!picks()->getPicks().contains(pickID));
    }
}

void WorldPickTests::withoutAReadCheckAWorldReadsNoEntity() {
    WorldPicks::setReadCheck(nullptr);
    ScriptCaller::WorldScope world(WORLD);
    QVERIFY(!WorldPicks::callerMayRead(WORLDS_ENTITY));
    const QVariantMap read = WorldPicks::resultForCaller(std::make_shared<TestResult>(rayHit(WORLDS_ENTITY, ENTITY)));
    QCOMPARE(read.value("type").toInt(), int(NONE));
    QVERIFY(WorldPicks::readableByCaller({ WORLDS_ENTITY }).isEmpty());
}
