//
//  SpaceTests.cpp
//  tests/workload/src
//
//  Created by Andrew Meadows on 2017.01.26
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "SpaceTests.h"

#include <workload/Space.h>

QTEST_GUILESS_MAIN(SpaceTests)

using namespace workload;

static_assert(Region::NUM_TRACKED_REGIONS == 3, "the tests below set a near, a mid and a far region per view");

// A view at `center` whose tracked regions R1, R2 and R3 are spheres of the given radii around it.
static View makeView(const glm::vec3& center, float near, float mid, float far) {
    View view;
    view.origin = center;
    const float radii[] = { near, mid, far };
    for (uint32_t i = 0; i < Region::NUM_TRACKED_REGIONS; ++i) {
        view.regions[i] = Sphere(center, radii[i]);
    }
    return view;
}

// Queue the transaction, close the frame and process it, as the workload engine does once per frame.
static void apply(Space& space, Transaction& transaction) {
    space.enqueueTransaction(transaction);
    space.enqueueFrame();
    space.processTransactionQueue();
    transaction.clear();
}

// One proxy walks in from outside every region to R3, R2 and R1, then is removed.
void SpaceTests::testOverlaps() {
    Space space;
    const glm::vec3 viewCenter(0.0f);
    const float near = 1.0f;
    const float mid = 2.0f;
    const float far = 3.0f;
    Views views;
    views.push_back(makeView(viewCenter, near, mid, far));
    space.setViews(views);
    QCOMPARE(space.getNumViews(), 1u);

    const float DELTA = 0.001f;
    Transaction transaction;
    Changes changes;

    // A new proxy just outside R3: known to the space but outside every tracked region, so R4 (from UNKNOWN).
    const ProxyID proxyId = space.allocateID();
    QVERIFY(space.isAllocatedID(proxyId));
    const float proxyRadius = 0.5f;
    const int OWNER_TAG = 7;
    transaction.reset(proxyId, Sphere(viewCenter + glm::vec3(0.0f, 0.0f, far + proxyRadius + DELTA), proxyRadius), Owner(OWNER_TAG));
    apply(space, transaction);
    QCOMPARE(space.getNumObjects(), 1u);
    QCOMPARE(space.getRegion(proxyId), (uint8_t)Region::UNKNOWN);
    QCOMPARE(space.getOwner(proxyId).get<int>(), OWNER_TAG);

    space.categorizeAndGetChanges(changes);
    QCOMPARE(changes.size(), (size_t)1);
    QCOMPARE(changes[0].proxyId, proxyId);
    QCOMPARE(changes[0].region, (uint8_t)Region::R4);
    QCOMPARE(changes[0].prevRegion, (uint8_t)Region::UNKNOWN);

    // Nothing moved: no change.
    changes.clear();
    space.categorizeAndGetChanges(changes);
    QVERIFY(changes.empty());

    // Into R3, R2 and R1 in turn. A proxy is in a region as soon as its sphere touches the region's sphere.
    const uint8_t regions[] = { Region::R3, Region::R2, Region::R1 };
    const float regionRadii[] = { far, mid, near };
    const float newRadius = 1.0f;
    uint8_t prevRegion = Region::R4;
    for (int step = 0; step < 3; ++step) {
        transaction.update(proxyId, Sphere(viewCenter + glm::vec3(0.0f, 0.0f, regionRadii[step] + newRadius - DELTA), newRadius));
        apply(space, transaction);
        changes.clear();
        space.categorizeAndGetChanges(changes);
        QCOMPARE(changes.size(), (size_t)1);
        QCOMPARE(changes[0].proxyId, proxyId);
        QCOMPARE(changes[0].region, regions[step]);
        QCOMPARE(changes[0].prevRegion, prevRegion);
        QCOMPARE(space.getRegion(proxyId), regions[step]);
        prevRegion = regions[step];
    }

    // Removed: the proxy is INVALID, never categorized again, and its slot is free.
    transaction.remove(proxyId);
    apply(space, transaction);
    changes.clear();
    space.categorizeAndGetChanges(changes);
    QVERIFY(changes.empty());
    QCOMPARE(space.getRegion(proxyId), (uint8_t)Region::INVALID);
    QCOMPARE(space.getNumObjects(), 0u);
}

// Two views: a proxy near the second view's origin is R1 for the space, and drops to R4 when that view moves away.
void SpaceTests::testSecondView() {
    Space space;
    const glm::vec3 farAway(100.0f, 0.0f, 0.0f);
    Views views;
    views.push_back(makeView(glm::vec3(0.0f), 1.0f, 2.0f, 3.0f));
    views.push_back(makeView(farAway, 1.0f, 2.0f, 3.0f));
    space.setViews(views);
    QCOMPARE(space.getNumViews(), 2u);

    Transaction transaction;
    const ProxyID nearSecond = space.allocateID();
    const ProxyID betweenViews = space.allocateID();
    transaction.reset(nearSecond, Sphere(farAway + glm::vec3(0.0f, 0.5f, 0.0f), 0.25f), Owner());
    transaction.reset(betweenViews, Sphere(glm::vec3(50.0f, 0.0f, 0.0f), 0.25f), Owner());
    apply(space, transaction);
    QCOMPARE(space.getNumObjects(), 2u);

    Changes changes;
    space.categorizeAndGetChanges(changes);
    QCOMPARE(changes.size(), (size_t)2);
    QCOMPARE(space.getRegion(nearSecond), (uint8_t)Region::R1);
    QCOMPARE(space.getRegion(betweenViews), (uint8_t)Region::R4);

    views[1] = makeView(farAway + glm::vec3(0.0f, 0.0f, 50.0f), 1.0f, 2.0f, 3.0f);
    space.setViews(views);
    changes.clear();
    space.categorizeAndGetChanges(changes);
    QCOMPARE(changes.size(), (size_t)1);
    QCOMPARE(changes[0].proxyId, nearSecond);
    QCOMPARE(changes[0].region, (uint8_t)Region::R4);
    QCOMPARE(changes[0].prevRegion, (uint8_t)Region::R1);
}
