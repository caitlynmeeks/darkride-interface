//
//  WorldMessagesTests.h
//  tests/script-api-guard
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_WorldMessagesTests_h
#define hifi_WorldMessagesTests_h

#include <QtTest/QtTest>

class WorldMessagesTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void testAppChannels();
    void testSenderMark();
    void testWorldRelay();
    void testWorldLeft();
};

#endif // hifi_WorldMessagesTests_h
