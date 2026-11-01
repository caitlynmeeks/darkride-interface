//
//  GuardedObjectTests.h
//  tests/script-api-guard
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_GuardedObjectTests_h
#define hifi_GuardedObjectTests_h

#include <QtTest/QtTest>

class GuardedObjectTests : public QObject {
    Q_OBJECT
private slots:
    void testAllowedAndDenied();
    void testBypassAttempts();
    void testHandedBackObjects();
    void testDeniedSignals();
};

#endif // hifi_GuardedObjectTests_h
