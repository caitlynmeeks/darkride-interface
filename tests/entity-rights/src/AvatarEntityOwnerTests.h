//
//  AvatarEntityOwnerTests.h
//  tests/entity-rights/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_AvatarEntityOwnerTests_h
#define hifi_AvatarEntityOwnerTests_h

#include <QtTest/QtTest>

// What another avatar's data may remove from this computer's tree: its own avatar entities only, locally.
class AvatarEntityOwnerTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void testAvatarEntityOf();
    void testDeleteOnlyThatAvatarsEntities();
    void testNoAvatarRemovesNothing();
};

#endif // hifi_AvatarEntityOwnerTests_h
