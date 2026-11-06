//
//  OffscreenSurfaceTests.h
//  tests/qml/src
//
//  Created by scripting on 2026-11-05.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_OffscreenSurfaceTests_h
#define hifi_OffscreenSurfaceTests_h

#include <QtTest/QtTest>

/// The filter the offscreen UI puts on the 3D view: which mouse moves stop in the QML, and which go on to the app.
class OffscreenSurfaceTests : public QObject {
    Q_OBJECT
private slots:
    void cleanup();
    void testAHoverLeavesTheMoveToTheApp();
    void testADragOnTheViewLeavesTheMoveToTheApp();
    void testAnItemHoldingTheMouseKeepsTheMove();
    void testTheNextClickEndsAHoldWhoseReleaseWentMissing();
};

#endif // hifi_OffscreenSurfaceTests_h
