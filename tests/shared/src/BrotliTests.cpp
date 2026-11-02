//
//  BrotliTests.cpp
//  tests/shared/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//
//  brotliDecompress: an ordinary stream decodes; content larger than the caller's limit is refused with nothing kept;
//  a large-window stream (not standard HTTP "br") and broken streams are refused. The streams below were made with
//  the brotli 1.2.0 command line tool.
//

#include "BrotliTests.h"

#include <climits>

#include <QtTest/QtTest>

#include <Brotli.h>

QTEST_MAIN(BrotliTests)

namespace {

QByteArray bytes(std::initializer_list<unsigned char> list) {
    QByteArray result;
    result.reserve(static_cast<int>(list.size()));
    for (unsigned char c : list) {
        result.append(static_cast<char>(c));
    }
    return result;
}

const QByteArray TEXT = "Darkride brotli check: the quick brown fox jumps over the lazy dog.\n";

// brotli -q 11 of TEXT
const QByteArray TEXT_BR = bytes({ 0xa1, 0x18, 0x02, 0x00, 0x2f, 0xea, 0xb6, 0xfe, 0x92, 0x16, 0xd2, 0xe9, 0x66, 0x53,
    0x51, 0xae, 0x72, 0x14, 0x06, 0xd3, 0x20, 0x38, 0xe4, 0xc0, 0xe1, 0x5b, 0xd2, 0x3c, 0x0f, 0x30, 0x10, 0xad, 0x51,
    0xaa, 0x58, 0xce, 0xad, 0x4d, 0x5b, 0x19, 0x03, 0x5d, 0x7c, 0x27, 0x01, 0x73, 0xd9, 0x8e, 0x24, 0x8f, 0x3a, 0xf9,
    0xe1, 0x93, 0x0b, 0x9a, 0xc6, 0x55, 0x07, 0xc1, 0xe5, 0x06 });

// brotli -q 5 --large_window=30 of TEXT: the large-window format, which standard decoders refuse
const QByteArray TEXT_LARGE_WINDOW_BR = bytes({ 0x11, 0x5e, 0x0c, 0x01, 0x00, 0x10, 0x9f, 0x76, 0xaf, 0x27, 0xf4, 0x80,
    0x3a, 0x8f, 0x23, 0xd9, 0xfb, 0x0f, 0xd8, 0x80, 0x03, 0xf6, 0x26, 0x5b, 0x60, 0xe0, 0x95, 0xd7, 0xc1, 0x21, 0x27,
    0xf4, 0x43, 0x01, 0xb5, 0xcc, 0xc5, 0x4c, 0x6d, 0x13, 0x72, 0xb6, 0x9d, 0xc3, 0x42, 0x5c, 0xb3, 0xef, 0x8b, 0x8a,
    0x02, 0xfb, 0x4f, 0x85, 0x6d, 0x2f, 0xe2, 0x29, 0x7d, 0xa1, 0x5d, 0x33, 0x95, 0x91, 0xc5, 0xff, 0x41, 0x37, 0x47,
    0x09 });

// brotli -q 11 of 1 MiB of zero bytes: 14 bytes that expand 75,000 times
const int ZEROS_1M_SIZE = 1024 * 1024;
const QByteArray ZEROS_1M_BR = bytes({ 0x59, 0xff, 0xff, 0x8f, 0x7f, 0x02, 0x20, 0x1e, 0x0b, 0x04, 0x72, 0xef, 0x1f,
    0x00 });

// brotli -q 11 of 64 MiB of zero bytes
const int ZEROS_64M_SIZE = 64 * 1024 * 1024;
const QByteArray ZEROS_64M_BR = bytes({ 0xcf, 0xff, 0xff, 0x7f, 0xf8, 0x27, 0x00, 0xe2, 0xb1, 0x40, 0x20, 0xf7, 0xfe,
    0x9f, 0xff, 0xff, 0xff, 0xf0, 0x4f, 0x00, 0xc4, 0x61, 0x01, 0x80, 0xee, 0xfd, 0x3f, 0xff, 0xff, 0xff, 0xe1, 0x9f,
    0x00, 0x88, 0xc3, 0x22, 0x00, 0xdd, 0xfb, 0x7f, 0xfe, 0xff, 0xff, 0xc3, 0x3f, 0x01, 0x10, 0x87, 0x05, 0x00, 0xba,
    0xf7, 0xff, 0x03 });

bool allZero(const QByteArray& data) {
    for (char c : data) {
        if (c != 0) {
            return false;
        }
    }
    return true;
}

}

void BrotliTests::testText() {
    QByteArray out;
    QVERIFY(brotliDecompress(TEXT_BR, out));
    QCOMPARE(out, TEXT);
}

void BrotliTests::testLimitExactly() {
    // content exactly as large as the limit is accepted
    QByteArray out;
    QVERIFY(brotliDecompress(ZEROS_1M_BR, out, ZEROS_1M_SIZE));
    QCOMPARE(out.size(), ZEROS_1M_SIZE);
    QVERIFY(allZero(out));

    QVERIFY(brotliDecompress(TEXT_BR, out, TEXT.size()));
    QCOMPARE(out, TEXT);
}

void BrotliTests::testOverLimit() {
    // one byte too many is refused, and nothing is kept
    QByteArray out = "left over";
    QVERIFY(!brotliDecompress(ZEROS_1M_BR, out, ZEROS_1M_SIZE - 1));
    QVERIFY(out.isEmpty());

    out = "left over";
    QVERIFY(!brotliDecompress(TEXT_BR, out, TEXT.size() - 1));
    QVERIFY(out.isEmpty());
}

void BrotliTests::testFarOverLimit() {
    // 64 MiB of content against a 64 KiB limit: refused once the buffer reaches the limit, never grown past it
    QByteArray out;
    QVERIFY(!brotliDecompress(ZEROS_64M_BR, out, 64 * 1024));
    QVERIFY(out.isEmpty());

    QVERIFY(!brotliDecompress(ZEROS_64M_BR, out, ZEROS_1M_SIZE));
    QVERIFY(out.isEmpty());
}

void BrotliTests::testManyGrowSteps() {
    // under the default limit, 64 MiB decodes through many buffer doublings
    QByteArray out;
    QVERIFY(brotliDecompress(ZEROS_64M_BR, out));
    QCOMPARE(out.size(), ZEROS_64M_SIZE);
    QVERIFY(allZero(out));
}

void BrotliTests::testLargeWindowRefused() {
    QByteArray out = "left over";
    QVERIFY(!brotliDecompress(TEXT_LARGE_WINDOW_BR, out));
    QVERIFY(out.isEmpty());
}

void BrotliTests::testBrokenStreams() {
    QByteArray out = "left over";
    QVERIFY(!brotliDecompress(TEXT_BR.left(TEXT_BR.size() / 2), out));   // ends early
    QVERIFY(out.isEmpty());

    QVERIFY(!brotliDecompress(QByteArray(), out));
    QVERIFY(out.isEmpty());

    QVERIFY(!brotliDecompress(QByteArray(64, '\xff'), out));
    QVERIFY(out.isEmpty());
}

void BrotliTests::testBadLimits() {
    QByteArray out = "left over";
    QVERIFY(!brotliDecompress(TEXT_BR, out, 0));
    QVERIFY(out.isEmpty());
    QVERIFY(!brotliDecompress(TEXT_BR, out, -1));
    QVERIFY(out.isEmpty());

    // a limit above the ceiling is held at the ceiling, and small content still decodes
    QVERIFY(brotliDecompress(TEXT_BR, out, INT_MAX));
    QCOMPARE(out, TEXT);
}
