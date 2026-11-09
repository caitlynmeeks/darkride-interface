//
//  ImageTests.h
//  tests/image/src
//
//  Created by the Darkride rendering session on 2026-11-04.
//  Copyright 2026 Tivoli Cloud VR, Inc.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE or https://opensource.org/licenses/MIT
//

#pragma once

#include <QtCore/QObject>

// Pictures and EXR files that are not what their headers say are refused, never a crash: a truncated EXR, an EXR
// whose window is absurd, a picture whose header claims a size past the limits.
class ImageTests : public QObject {
    Q_OBJECT
private slots:
    void testExrReadsWhole();
    void testExrTruncated();
    void testExrHugeWindow();
    void testPictureHeaderSize();
    void testFormatsReportSize();
    void testTgaBlackWhite();
};
