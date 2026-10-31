//
//  FrameTraceTests.h
//  tests/shared/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_FrameTraceTests_h
#define hifi_FrameTraceTests_h

#include <QtTest/QtTest>

class FrameTraceTests : public QObject {
    Q_OBJECT
private slots:
    void offWritesNothing();
    void recordsFramesAndColumns();
    void ringKeepsTheLastFrames();
    void stopKeepsRecordsAndClearDropsThem();
    void dumpWritesCsv();
    void summaryLinesShowLastAndMaximum();
};

#endif // hifi_FrameTraceTests_h
