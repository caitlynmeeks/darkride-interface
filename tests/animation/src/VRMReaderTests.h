//
//  VRMReaderTests.h
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_VRMReaderTests_h
#define hifi_VRMReaderTests_h

#include <QtTest/QtTest>

class VRMReaderTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void testNotVRM();
    void testHumanoid0();
    void testHumanoid1();
    void testExpressions0();
    void testExpressions1();
    void testLookAtDefaults();
    void testToon0();
    void testToon1();
    void testLoopingNodes();
    void testClashingRename();
};

#endif // hifi_VRMReaderTests_h
