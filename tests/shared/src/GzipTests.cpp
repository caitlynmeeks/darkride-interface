//
//  GzipTests.cpp
//  tests/shared/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//
//  gunzip: an ordinary stream unpacks; content larger than the caller's limit is refused with nothing kept; broken
//  and truncated streams are refused with nothing kept. The test data is packed with gzip() at run time.
//

#include "GzipTests.h"

#include <climits>

#include <QtTest/QtTest>

#include <Gzip.h>

QTEST_MAIN(GzipTests)

namespace {

const QByteArray TEXT = "Darkride gzip check: the quick brown fox jumps over the lazy dog.\n";
const int ONE_MIB = 1024 * 1024;

QByteArray packed(const QByteArray& content) {
    QByteArray result;
    if (!gzip(content, result)) {
        qFatal("gzip failed while making test data");
    }
    return result;
}

}

void GzipTests::testRoundTrip() {
    QByteArray out;
    QVERIFY(gunzip(packed(TEXT), out));
    QCOMPARE(out, TEXT);

    // many 4 KiB output chunks
    const QByteArray big(8 * ONE_MIB, 'x');
    QVERIFY(gunzip(packed(big), out));
    QCOMPARE(out, big);
}

void GzipTests::testEmpty() {
    // as before: nothing in, nothing out, and true
    QByteArray out = "left over";
    QVERIFY(gunzip(QByteArray(), out));
    QVERIFY(out.isEmpty());
}

void GzipTests::testLimitExactly() {
    const QByteArray zeros(ONE_MIB, '\0');
    QByteArray out;
    QVERIFY(gunzip(packed(zeros), out, ONE_MIB));
    QCOMPARE(out, zeros);

    QVERIFY(gunzip(packed(TEXT), out, TEXT.size()));
    QCOMPARE(out, TEXT);
}

void GzipTests::testOverLimit() {
    QByteArray out = "left over";
    QVERIFY(!gunzip(packed(QByteArray(ONE_MIB, '\0')), out, ONE_MIB - 1));
    QVERIFY(out.isEmpty());

    out = "left over";
    QVERIFY(!gunzip(packed(TEXT), out, TEXT.size() - 1));
    QVERIFY(out.isEmpty());
}

void GzipTests::testFarOverLimit() {
    // 64 MiB of content (64 KiB packed) against a 1 MiB limit: refused before the output grows past the limit
    const QByteArray source = packed(QByteArray(64 * ONE_MIB, '\0'));
    QVERIFY(source.size() < ONE_MIB);
    QByteArray out;
    QVERIFY(!gunzip(source, out, ONE_MIB));
    QVERIFY(out.isEmpty());

    // and under the default limit it unpacks
    QVERIFY(gunzip(source, out));
    QCOMPARE(out.size(), 64 * ONE_MIB);
}

void GzipTests::testBrokenStreams() {
    const QByteArray source = packed(QByteArray(ONE_MIB, 'y'));
    QByteArray out = "left over";
    QVERIFY(!gunzip(source.left(source.size() / 2), out));   // ends early
    QVERIFY(out.isEmpty());

    out = "left over";
    QVERIFY(!gunzip(QByteArray(64, '\x5a'), out));   // not gzip at all
    QVERIFY(out.isEmpty());
}

void GzipTests::testBadLimits() {
    const QByteArray source = packed(TEXT);
    QByteArray out = "left over";
    QVERIFY(!gunzip(source, out, 0));
    QVERIFY(out.isEmpty());
    QVERIFY(!gunzip(source, out, -1));
    QVERIFY(out.isEmpty());

    // a limit above the ceiling is held at the ceiling, and small content still unpacks
    QVERIFY(gunzip(source, out, INT_MAX));
    QCOMPARE(out, TEXT);
}
