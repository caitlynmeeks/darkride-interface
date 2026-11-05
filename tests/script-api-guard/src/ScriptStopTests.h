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
};

#endif // hifi_ScriptStopTests_h
