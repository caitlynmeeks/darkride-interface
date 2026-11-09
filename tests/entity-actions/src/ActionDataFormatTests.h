//
//  ActionDataFormatTests.h
//  tests/entity-actions/src
//
//  Created by scripting on 2026-11-04.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_ActionDataFormatTests_h
#define hifi_ActionDataFormatTests_h

#include <QtTest/QtTest>

// An entity's actionData: its actions and constraints as QDataStream bytes, sent in entity packets and saved,
// base64-encoded, in world files. One recorded golden per action type (what a Qt 5.15 build writes), read back
// through the real classes, so a Qt 6 build is held to the same bytes.
class ActionDataFormatTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    // the envelope every entity writes: the list of its actions' bytes (EntityItem, EntityTree, the debug string)
    void actionList();
    void actionListRemapped();

    // physics' action and constraint types
    void offsetAction();
    void tractorAction();
    void travelOrientedAction();
    void hingeConstraint();
    void sliderConstraint();
    void ballSocketConstraint();
    void coneTwistConstraint();

    // avatars' client-only types, by their field lists (the classes need the whole client)
    void holdAction();
    void farGrabAction();

    // the editor's entity list (its own little-endian format on the Messages mixer)
    void entityListMessage();
};

#endif // hifi_ActionDataFormatTests_h
