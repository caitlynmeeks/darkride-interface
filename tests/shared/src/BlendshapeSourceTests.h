//
//  BlendshapeSourceTests.h
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_BlendshapeSourceTests_h
#define hifi_BlendshapeSourceTests_h

#include <QtTest/QtTest>

class BlendshapeSourceTests : public QObject {
    Q_OBJECT
private slots:
    void testExactName();
    void testCase();
    void testAfterDot();
    void testRocketboxPrefix();
    void testPreferenceOrder();
    void testOneToOne();
    void testNone();
};

#endif // hifi_BlendshapeSourceTests_h
