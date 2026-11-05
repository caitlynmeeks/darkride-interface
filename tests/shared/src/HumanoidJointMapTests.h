//
//  HumanoidJointMapTests.h
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_HumanoidJointMapTests_h
#define hifi_HumanoidJointMapTests_h

#include <QtTest/QtTest>

class HumanoidJointMapTests : public QObject {
    Q_OBJECT
private slots:
    void testMixamo();
    void testMixamoPrefixed();
    void testUnrealMannequin();
    void testUnityHumanoid();
    void testBiped();
    void testSecondLifeBento();
    void testVRoid();
    void testSynty();
    void testMMD();
    void testQuadruped();
    void testScenery();
    void testFacingMinusZ();
};

#endif // hifi_HumanoidJointMapTests_h
