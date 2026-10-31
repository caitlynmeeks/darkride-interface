//
//  FrameTraceTests.cpp
//  tests/shared/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "FrameTraceTests.h"

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include <FrameTrace.h>

QTEST_MAIN(FrameTraceTests)

void FrameTraceTests::offWritesNothing() {
    FrameTrace::stop();
    FrameTrace::clear();
    QVERIFY(!FrameTrace::enabled());
    QCOMPARE(FrameTrace::beginFrame(), (uint64_t)0);
    QCOMPARE(FrameTrace::currentFrame(), (uint64_t)0);
    FrameTrace::set("test.off", 1.0);
    FrameTrace::setText("test.off_text", "x");
    QCOMPARE(FrameTrace::frames(), 0);
    QVERIFY(FrameTrace::rows().isEmpty());
    QVERIFY(FrameTrace::summaryLines().isEmpty());
}

void FrameTraceTests::recordsFramesAndColumns() {
    FrameTrace::start(8);
    QVERIFY(FrameTrace::enabled());
    const uint64_t first = FrameTrace::beginFrame();
    QVERIFY(first > 0);
    QCOMPARE(FrameTrace::currentFrame(), first);
    FrameTrace::set("test.a", 1.5);
    FrameTrace::set("test.b", 2.0);
    FrameTrace::set("test.b", 3.0);   // the last value of a frame wins
    FrameTrace::setText("test.id", "{abc}");
    const uint64_t second = FrameTrace::beginFrame();
    QCOMPARE(second, first + 1);
    FrameTrace::set("test.a", 4.0);
    FrameTrace::set(first, "test.c", 7.0);   // a frame written by its index, as the present thread does
    const uint64_t third = FrameTrace::beginFrame();
    FrameTrace::set(third - 1, "test.b", 5.0);
    FrameTrace::set(third + 1, "test.a", 9.0);   // not a frame yet: dropped
    FrameTrace::set("test.c", 2.5);   // a number into a number column: fine
    FrameTrace::set("test.id", 1.0);   // a number into a text column: dropped

    QCOMPARE(FrameTrace::columns(), QStringList({ "frame", "t_us", "test.a", "test.b", "test.id", "test.c" }));
    QCOMPARE(FrameTrace::frames(), 3);
    const auto rows = FrameTrace::rows();
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows[0].size(), 6);
    QCOMPARE(rows[0][0].toDouble(), (double)first);
    QVERIFY(rows[0][1].toDouble() > 0.0);
    QCOMPARE(rows[0][2].toDouble(), 1.5);
    QCOMPARE(rows[0][3].toDouble(), 3.0);
    QCOMPARE(rows[0][4].toString(), QString("{abc}"));
    QCOMPARE(rows[0][5].toDouble(), 7.0);
    QCOMPARE(rows[1][2].toDouble(), 4.0);
    QCOMPARE(rows[1][3].toDouble(), 5.0);
    QVERIFY(!rows[1][4].isValid());   // unset
    QVERIFY(!rows[1][5].isValid());
    QVERIFY(!rows[2][2].isValid());
    QVERIFY(!rows[2][4].isValid());
    QCOMPARE(rows[2][5].toDouble(), 2.5);
    FrameTrace::stop();
}

void FrameTraceTests::ringKeepsTheLastFrames() {
    FrameTrace::start(4);
    uint64_t frames[6];
    for (int i = 0; i < 6; i++) {
        frames[i] = FrameTrace::beginFrame();
        FrameTrace::set("test.i", (double)i);
    }
    FrameTrace::set(frames[1], "test.i", 100.0);   // left the ring: dropped
    FrameTrace::set(frames[2], "test.i", 200.0);   // the oldest held
    QCOMPARE(FrameTrace::frames(), 4);
    const auto rows = FrameTrace::rows();
    QCOMPARE(rows.size(), 4);
    QCOMPARE(rows[0][0].toDouble(), (double)frames[2]);
    QCOMPARE(rows[3][0].toDouble(), (double)frames[5]);
    const int column = FrameTrace::columns().indexOf("test.i");
    QVERIFY(column > 1);
    QCOMPARE(rows[0][column].toDouble(), 200.0);
    QCOMPARE(rows[1][column].toDouble(), 3.0);
    QCOMPARE(rows[3][column].toDouble(), 5.0);
    FrameTrace::stop();
}

void FrameTraceTests::stopKeepsRecordsAndClearDropsThem() {
    FrameTrace::start(8);
    FrameTrace::beginFrame();
    FrameTrace::set("test.a", 1.0);
    FrameTrace::stop();
    QVERIFY(!FrameTrace::enabled());
    QCOMPARE(FrameTrace::beginFrame(), (uint64_t)0);
    FrameTrace::set("test.a", 2.0);   // off: dropped
    QCOMPARE(FrameTrace::frames(), 1);
    const int column = FrameTrace::columns().indexOf("test.a");
    QCOMPARE(FrameTrace::rows()[0][column].toDouble(), 1.0);
    FrameTrace::clear();
    QCOMPARE(FrameTrace::frames(), 0);
    QVERIFY(FrameTrace::rows().isEmpty());
    QVERIFY(FrameTrace::columns().contains("test.a"));   // the names stay, so a later CSV's columns hold still
    // take: the rows and the clear in one step
    FrameTrace::start(8);
    FrameTrace::beginFrame();
    FrameTrace::set("test.a", 3.0);
    FrameTrace::beginFrame();
    const auto taken = FrameTrace::take();
    QCOMPARE(taken.size(), 2);
    QCOMPARE(taken[0][column].toDouble(), 3.0);
    QCOMPARE(FrameTrace::frames(), 0);
    QVERIFY(FrameTrace::enabled());   // still on
    FrameTrace::stop();
}

void FrameTraceTests::dumpWritesCsv() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("trace.csv");
    FrameTrace::clear();
    FrameTrace::start(8);
    const uint64_t first = FrameTrace::beginFrame();
    FrameTrace::set("test.a", 0.25);
    FrameTrace::setText("test.id", "a,b \"c\"");
    FrameTrace::beginFrame();
    FrameTrace::set("test.b", -3.0);
    FrameTrace::stop();
    QCOMPARE(FrameTrace::dump(path), 2);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList lines = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 3);
    QCOMPARE(lines[0], FrameTrace::columns().join(','));
    QVERIFY(lines[1].startsWith(QString::number((qulonglong)first) + ","));
    // the two rows' cells after t_us, built from the column order: a = 0.25 and the text quoted with its quote doubled
    // in the first frame, b = -3 in the second, every other cell blank
    const QStringList columns = FrameTrace::columns();
    QString row1, row2;
    for (int i = 2; i < columns.size(); i++) {
        row1 += ",";
        row2 += ",";
        if (columns[i] == "test.a") {
            row1 += "0.25";
        } else if (columns[i] == "test.id") {
            row1 += "\"a,b \"\"c\"\"\"";
        } else if (columns[i] == "test.b") {
            row2 += "-3";
        }
    }
    QVERIFY2(lines[1].endsWith(row1), qPrintable(lines[1] + " should end with " + row1));
    QVERIFY2(lines[2].endsWith(row2), qPrintable(lines[2] + " should end with " + row2));
    QCOMPARE(FrameTrace::dump(dir.filePath("no/such/dir/trace.csv")), -1);
}

void FrameTraceTests::summaryLinesShowLastAndMaximum() {
    FrameTrace::clear();
    FrameTrace::start(8);
    FrameTrace::beginFrame();
    FrameTrace::set("test.a", 10.0);
    FrameTrace::beginFrame();
    FrameTrace::set("test.a", 4.0);
    FrameTrace::setText("test.id", "{id}");
    FrameTrace::beginFrame();   // the current frame, still being written: not the "last"
    FrameTrace::set("test.a", 99.0);
    const QStringList lines = FrameTrace::summaryLines();
    QVERIFY(lines.size() >= 3);
    QVERIFY(lines[0].startsWith("Frame trace: on, 3 frames"));
    QVERIFY(lines.contains("test.a: 4 (max 1 s 10)"));
    QVERIFY(lines.contains("test.id: {id}"));
    FrameTrace::stop();
    FrameTrace::clear();
}
