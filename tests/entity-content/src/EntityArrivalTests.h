//
//  EntityArrivalTests.h
//  tests/entity-content/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_EntityArrivalTests_h
#define hifi_EntityArrivalTests_h

#include <QtTest/QtTest>

// Content addresses that name a file on this computer are emptied as an entity's data is read from the wire, from a
// world file, from another visitor's avatar blob, or from a received avatar identity, when the world isn't local.
class EntityArrivalTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void testWireReadRefuses();
    void testWireReadKeepsWhenAllowed();
    void testPartialUpdateUntouched();
    void testFreshClientTree();
    void testWorldFile();
    void testAvatarBlob();
    void testReceivedAvatarIdentity();
};

#endif  // hifi_EntityArrivalTests_h
