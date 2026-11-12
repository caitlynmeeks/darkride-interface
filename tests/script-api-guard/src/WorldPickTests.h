//
//  WorldPickTests.h
//  tests/script-api-guard
//
//  Created by scripting on 2026-11-10.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_WorldPickTests_h
#define hifi_WorldPickTests_h

#include <QtTest/QtTest>

class WorldPickTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void worldNamesOnlyAWorldsPicks();
    void everyPickCallLeavesTheAppsPickAlone();
    void worldNamesOnlyAWorldsPointers();
    void everyPointerCallLeavesTheAppsPointerAlone();
    void aWorldsPickIsParentedOnlyToAWorldsPick();
    void addPickMarksAWorldsPick();
    void aWorldsPickPassesThroughWhatIsHidden();
    void aWorldDoesNotSetThePicksTimeBudget();
    void aWorldThatWasLeftNamesNothing();
    void listsDropWhatTheWorldMayNotRead();
    void lockEndOnlyOntoWhatTheWorldMayRead();
    void aHitTheWorldMayNotReadIsNoHit_data();
    void aHitTheWorldMayNotReadIsNoHit();
    void aCollisionListLeavesOutWhatTheWorldMayNotRead();
    void otherCallersReadEveryResult();
    void theAppReachesEveryPickAndPointer();
    void withoutAReadCheckAWorldReadsNoEntity();
};

#endif // hifi_WorldPickTests_h
