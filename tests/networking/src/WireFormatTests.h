//
//  WireFormatTests.h
//  tests/networking/src
//
//  The networking library's wire formats, byte for byte as a Qt 5.15 build writes them (the Qt 6 move's stage 0): each
//  type is written through a pinned stream and compared with bytes recorded from the Qt 5 build, then read back.
//
//  Created by servers on 2026-11-03.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_WireFormatTests_h
#define hifi_WireFormatTests_h

#pragma once

#include <QtTest/QtTest>

class WireFormatTests : public QObject {
    Q_OBJECT
private slots:
    void thePinIsQt5_15WithDoubleFloats();
    void pinnedBytesMatchQt5Defaults();
    void hifiSockAddr();
    void nodePermissions();
    void node();
    void assignment();
    void anAssignmentRequestReads();
};

#endif // hifi_WireFormatTests_h
