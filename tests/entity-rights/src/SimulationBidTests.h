//
//  SimulationBidTests.h
//  tests/entity-rights/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#pragma once

#include <QtTest/QtTest>

// The entity server's simulation-ownership rules (EntityTree::updateEntity, vehicle master plan entry 16): only the
// physics server simulates at its priority, and an equal bid waits for the current owner's lockout.
class SimulationBidTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void physicsServerTakesAtItsPriority();
    void anotherNodesBidAtTheServersPriorityIsTakenBelowIt();
    void aBidBelowTheServersPriorityLosesToTheServer();
    void anEqualBidWaitsForTheOwnersLockout();
    void thePhysicsServerKeepsItsPriorityOnItsOwnUpdate();
};
