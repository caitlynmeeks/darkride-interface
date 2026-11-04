//
//  EngineGlobalsTests.h
//  tests/script-api-guard
//
//  Created by scripting on 2026-11-04.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_EngineGlobalsTests_h
#define hifi_EngineGlobalsTests_h

#include <QtTest/QtTest>

/// The globals the script engine itself gives a script of each context (beside the hosts' registrations): which
/// context gets what, so that a change can't quietly give a world's scripts more than they should have.
class EngineGlobalsTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void testListeningSocketsAreAnAgentsAlone();
    void testNetworkObjectsInEveryContext();
};

#endif // hifi_EngineGlobalsTests_h
