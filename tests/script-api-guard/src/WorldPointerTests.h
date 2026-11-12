//
//  WorldPointerTests.h
//  tests/script-api-guard
//
//  Created by scripting on 2026-11-10.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_WorldPointerTests_h
#define hifi_WorldPointerTests_h

#include <QtTest/QtTest>

class WorldPointerTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void addPointerMarksAWorldsPointer();
    void worldPointerReachesItsOwnEntity();
    void worldPointerReachesNothingElse_data();
    void worldPointerReachesNothingElse();
    void worldPointerLeavesWhenItsTargetIsNoLongerItsOwn();
    void appPointerIsUnchanged_data();
    void appPointerIsUnchanged();
    void withoutACheckAWorldPointerReachesNoEntity();
};

#endif // hifi_WorldPointerTests_h
