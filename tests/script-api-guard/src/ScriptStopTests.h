//
//  ScriptStopTests.h
//  tests/script-api-guard
//
//  Created by scripting on 2026-11-05.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_ScriptStopTests_h
#define hifi_ScriptStopTests_h

#include <QtTest/QtTest>

/// A script stopped from another thread, as the app stops scripts at a quit and a reload: its scriptEnding handlers
/// run to their end with the host's answers reaching them, and only the deadline at a quit ends them early. A world
/// that was left runs nothing more.
class ScriptStopTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void testAStopLeavesRunningHandlersAlone();
    void testHandlersRunAfterAStopEndedACall();
    void testALeftWorldRunsNoHandlers();
    void testAnEndlessHandlerEndsAtTheDeadline();
    void testStuckHandlersShareOneDeadline();
    void testAnInterruptEndsALeftWorldsHandler();
    void testASignalHandlersCallFinishes();
    void testARunawaySignalHandlerEndsAfterTwoSeconds();
};

/// A signal for a script to connect to: a call that reaches it through a connected signal, as Messages and Controller
/// events do, not through one of the engine's own calls.
class StopTestPoker : public QObject {
    Q_OBJECT
signals:
    void poked();
};

#endif // hifi_ScriptStopTests_h
