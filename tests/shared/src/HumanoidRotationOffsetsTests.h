//
//  HumanoidRotationOffsetsTests.h
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_HumanoidRotationOffsetsTests_h
#define hifi_HumanoidRotationOffsetsTests_h

#include <QtTest/QtTest>

class HumanoidRotationOffsetsTests : public QObject {
    Q_OBJECT
private slots:
    void testDefaultAvatarTPose();
    void testDefaultAvatarAPose();
    void testPosedNeck();
    void testBlenderAxes();
    void testFacingMinusZ();
};

#endif // hifi_HumanoidRotationOffsetsTests_h
