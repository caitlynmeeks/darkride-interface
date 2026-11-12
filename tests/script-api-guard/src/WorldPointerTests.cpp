//
//  WorldPointerTests.cpp
//  tests/script-api-guard
//
//  Created by scripting on 2026-11-10.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  A pointer a world's script made delivers its hover and trigger events only into that world's own entities. Over
//  the HUD, or over an entity that isn't the world's (the visitor's own, the app's local entities such as the tablet
//  or a screen's pieces), it hovers nothing and its press, move and release go nowhere. The app's own pointers are
//  unchanged.
//

#include "WorldPointerTests.h"

#include <QtTest/QSignalSpy>

#include <DependencyManager.h>
#include <PickManager.h>
#include <Pointer.h>
#include <PointerManager.h>
#include <ScriptCaller.h>

QTEST_MAIN(WorldPointerTests)

namespace {

const QUuid WORLDS_ENTITY = QUuid::createUuid();
const QUuid APPS_ENTITY = QUuid::createUuid();

class NoPickResult : public PickResult {
public:
    bool doesIntersect() const override { return false; }
    bool checkOrFilterAgainstMaxDistance(float maxDistance) override { return true; }
    PickResultPointer compareAndProcessNewResult(const PickResultPointer& newRes) override { return newRes; }
};

// A pointer whose target and pressed buttons the test sets, frame by frame
class TestPointer : public Pointer {
public:
    TestPointer() : Pointer(1, true, true) {}

    PickedObject target;
    Buttons pressed;

    void frame(unsigned int id) { generatePointerEvents(id, std::make_shared<NoPickResult>()); }

    PickQuery::PickType getType() const override { return PickQuery::PickType::Ray; }
    void setRenderState(const std::string&) override {}
    void editRenderState(const std::string&, const QVariant&, const QVariant&, const QVariant&) override {}
    void updateVisuals(const PickResultPointer&) override {}

protected:
    PointerEvent buildPointerEvent(const PickedObject&, const PickResultPointer&, const std::string&, bool) override {
        return PointerEvent();
    }
    PickedObject getHoveredObject(const PickResultPointer&) override { return target; }
    Buttons getPressedButtons(const PickResultPointer&) override { return pressed; }
    PickResultPointer getPickResultCopy(const PickResultPointer& pickResult) const override { return pickResult; }
};

// Every signal a pointer's events leave by
struct Spies {
    explicit Spies(PointerManager* m) :
        hoverBeginEntity(m, &PointerManager::hoverBeginEntity),
        hoverContinueEntity(m, &PointerManager::hoverContinueEntity),
        hoverEndEntity(m, &PointerManager::hoverEndEntity),
        triggerBeginEntity(m, &PointerManager::triggerBeginEntity),
        triggerContinueEntity(m, &PointerManager::triggerContinueEntity),
        triggerEndEntity(m, &PointerManager::triggerEndEntity),
        hoverBeginOverlay(m, &PointerManager::hoverBeginOverlay),
        hoverContinueOverlay(m, &PointerManager::hoverContinueOverlay),
        hoverEndOverlay(m, &PointerManager::hoverEndOverlay),
        triggerBeginOverlay(m, &PointerManager::triggerBeginOverlay),
        triggerContinueOverlay(m, &PointerManager::triggerContinueOverlay),
        triggerEndOverlay(m, &PointerManager::triggerEndOverlay),
        hoverBeginHUD(m, &PointerManager::hoverBeginHUD),
        hoverContinueHUD(m, &PointerManager::hoverContinueHUD),
        hoverEndHUD(m, &PointerManager::hoverEndHUD),
        triggerBeginHUD(m, &PointerManager::triggerBeginHUD),
        triggerContinueHUD(m, &PointerManager::triggerContinueHUD),
        triggerEndHUD(m, &PointerManager::triggerEndHUD) {}

    QSignalSpy hoverBeginEntity, hoverContinueEntity, hoverEndEntity;
    QSignalSpy triggerBeginEntity, triggerContinueEntity, triggerEndEntity;
    QSignalSpy hoverBeginOverlay, hoverContinueOverlay, hoverEndOverlay;
    QSignalSpy triggerBeginOverlay, triggerContinueOverlay, triggerEndOverlay;
    QSignalSpy hoverBeginHUD, hoverContinueHUD, hoverEndHUD;
    QSignalSpy triggerBeginHUD, triggerContinueHUD, triggerEndHUD;

    int total() const {
        return hoverBeginEntity.count() + hoverContinueEntity.count() + hoverEndEntity.count()
            + triggerBeginEntity.count() + triggerContinueEntity.count() + triggerEndEntity.count()
            + hoverBeginOverlay.count() + hoverContinueOverlay.count() + hoverEndOverlay.count()
            + triggerBeginOverlay.count() + triggerContinueOverlay.count() + triggerEndOverlay.count()
            + hoverBeginHUD.count() + hoverContinueHUD.count() + hoverEndHUD.count()
            + triggerBeginHUD.count() + triggerContinueHUD.count() + triggerEndHUD.count();
    }
};

// hover, hover again, press, hold, release, then point away: six frames on one target
void hoverPressHoldReleaseLeave(TestPointer& pointer, const Pointer::PickedObject& target) {
    const unsigned int id = 7;
    pointer.target = target;
    pointer.frame(id);
    pointer.frame(id);
    pointer.pressed = { "Primary" };
    pointer.frame(id);
    pointer.frame(id);
    pointer.pressed.clear();
    pointer.frame(id);
    pointer.target = Pointer::PickedObject();
    pointer.frame(id);
}

PointerManager* manager() {
    return DependencyManager::get<PointerManager>().data();
}

} // namespace

Q_DECLARE_METATYPE(IntersectionType)

void WorldPointerTests::initTestCase() {
    qRegisterMetaType<PointerEvent>("PointerEvent");
    DependencyManager::set<PickManager>();  // a pointer's destructor removes its pick
    DependencyManager::set<PointerManager>();
}

void WorldPointerTests::init() {
    Pointer::setWorldTargetCheck([](const QUuid& entityID) { return entityID == WORLDS_ENTITY; });
}

void WorldPointerTests::addPointerMarksAWorldsPointer() {
    auto apps = std::make_shared<TestPointer>();
    const unsigned int appsID = manager()->addPointer(apps);
    QVERIFY(!apps->isMadeByWorld());
    QVERIFY(!manager()->isWorldPointer(appsID));

    auto worlds = std::make_shared<TestPointer>();
    unsigned int worldsID = 0;
    {
        ScriptCaller::WorldScope world(true);
        worldsID = manager()->addPointer(worlds);
    }
    QVERIFY(worlds->isMadeByWorld());
    QVERIFY(manager()->isWorldPointer(worldsID));
}

void WorldPointerTests::worldPointerReachesItsOwnEntity() {
    for (IntersectionType type : { ENTITY, LOCAL_ENTITY }) {
        TestPointer pointer;
        pointer.setMadeByWorld();
        Spies spies(manager());
        hoverPressHoldReleaseLeave(pointer, Pointer::PickedObject(WORLDS_ENTITY, type));
        const bool local = type == LOCAL_ENTITY;
        QCOMPARE((local ? spies.hoverBeginOverlay : spies.hoverBeginEntity).count(), 1);
        QCOMPARE((local ? spies.triggerBeginOverlay : spies.triggerBeginEntity).count(), 1);
        QCOMPARE((local ? spies.triggerContinueOverlay : spies.triggerContinueEntity).count(), 1);
        QCOMPARE((local ? spies.triggerEndOverlay : spies.triggerEndEntity).count(), 1);
        QCOMPARE((local ? spies.hoverEndOverlay : spies.hoverEndEntity).count(), 1);
    }
}

void WorldPointerTests::worldPointerReachesNothingElse_data() {
    QTest::addColumn<QUuid>("id");
    QTest::addColumn<IntersectionType>("type");
    QTest::newRow("an entity that isn't the world's") << APPS_ENTITY << ENTITY;
    QTest::newRow("an app's local entity (the tablet, a screen's piece)") << APPS_ENTITY << LOCAL_ENTITY;
    QTest::newRow("an id that is no entity") << QUuid::createUuid() << LOCAL_ENTITY;
    QTest::newRow("the HUD") << QUuid() << HUD;
}

void WorldPointerTests::worldPointerReachesNothingElse() {
    QFETCH(QUuid, id);
    QFETCH(IntersectionType, type);
    TestPointer pointer;
    pointer.setMadeByWorld();
    Spies spies(manager());
    hoverPressHoldReleaseLeave(pointer, Pointer::PickedObject(id, type));
    QCOMPARE(spies.total(), 0);
}

void WorldPointerTests::worldPointerLeavesWhenItsTargetIsNoLongerItsOwn() {
    // hovering its own entity, then the same pointer moves onto the app's: one hover end for its own, nothing after
    TestPointer pointer;
    pointer.setMadeByWorld();
    Spies spies(manager());
    pointer.target = Pointer::PickedObject(WORLDS_ENTITY, ENTITY);
    pointer.frame(7);
    pointer.frame(7);
    QCOMPARE(spies.hoverBeginEntity.count(), 1);
    pointer.target = Pointer::PickedObject(APPS_ENTITY, LOCAL_ENTITY);
    pointer.frame(7);
    QCOMPARE(spies.hoverEndEntity.count(), 1);
    QCOMPARE(spies.hoverEndEntity.first().at(0).toUuid(), WORLDS_ENTITY);
    const int before = spies.total();
    pointer.pressed = { "Primary" };
    pointer.frame(7);
    pointer.frame(7);
    pointer.pressed.clear();
    pointer.frame(7);
    QCOMPARE(spies.total(), before);
}

void WorldPointerTests::appPointerIsUnchanged_data() {
    QTest::addColumn<QUuid>("id");
    QTest::addColumn<IntersectionType>("type");
    QTest::newRow("a world's entity") << WORLDS_ENTITY << ENTITY;
    QTest::newRow("an app's local entity") << APPS_ENTITY << LOCAL_ENTITY;
    QTest::newRow("the HUD") << QUuid() << HUD;
}

void WorldPointerTests::appPointerIsUnchanged() {
    QFETCH(QUuid, id);
    QFETCH(IntersectionType, type);
    TestPointer pointer;  // not marked: the app's own
    Spies spies(manager());
    hoverPressHoldReleaseLeave(pointer, Pointer::PickedObject(id, type));
    // hover begin, hover continue (x3: the second hover frame and the press and hold frames; the release frame too),
    // trigger begin, continue, end, hover end: at least one of each kind arrives
    QSignalSpy& hoverBegin = type == ENTITY ? spies.hoverBeginEntity : type == HUD ? spies.hoverBeginHUD : spies.hoverBeginOverlay;
    QSignalSpy& triggerBegin = type == ENTITY ? spies.triggerBeginEntity : type == HUD ? spies.triggerBeginHUD : spies.triggerBeginOverlay;
    QSignalSpy& triggerContinue = type == ENTITY ? spies.triggerContinueEntity : type == HUD ? spies.triggerContinueHUD : spies.triggerContinueOverlay;
    QSignalSpy& triggerEnd = type == ENTITY ? spies.triggerEndEntity : type == HUD ? spies.triggerEndHUD : spies.triggerEndOverlay;
    QSignalSpy& hoverEnd = type == ENTITY ? spies.hoverEndEntity : type == HUD ? spies.hoverEndHUD : spies.hoverEndOverlay;
    QCOMPARE(hoverBegin.count(), 1);
    QCOMPARE(triggerBegin.count(), 1);
    QCOMPARE(triggerContinue.count(), 1);
    QCOMPARE(triggerEnd.count(), 1);
    QCOMPARE(hoverEnd.count(), 1);
}

void WorldPointerTests::withoutACheckAWorldPointerReachesNoEntity() {
    Pointer::setWorldTargetCheck(nullptr);
    TestPointer pointer;
    pointer.setMadeByWorld();
    Spies spies(manager());
    hoverPressHoldReleaseLeave(pointer, Pointer::PickedObject(WORLDS_ENTITY, ENTITY));
    QCOMPARE(spies.total(), 0);
}
