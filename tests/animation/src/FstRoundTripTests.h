//
//  FstRoundTripTests.h
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_FstRoundTripTests_h
#define hifi_FstRoundTripTests_h

#include <QtTest/QtTest>

class FstRoundTripTests : public QObject {
    Q_OBJECT
private slots:
    void testPrefixedNamesBlenderAxes();
    void testVRMColliders();
    void testFace();
    void testNamesStayOnTheirLine();
};

#endif // hifi_FstRoundTripTests_h
