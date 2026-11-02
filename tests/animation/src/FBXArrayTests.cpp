//
//  FBXArrayTests.cpp
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "FBXArrayTests.h"

#include <climits>

#include <QtCore/QBuffer>

#include <FBXSerializer.h>

QTEST_MAIN(FBXArrayTests)

namespace {

const quint32 PLAIN = 0;
const quint32 ZLIB = 1;

void append32(QByteArray& bytes, quint32 value) {
    for (int i = 0; i < 4; i++) {
        bytes.append(static_cast<char>((value >> (8 * i)) & 0xff));
    }
}

// a binary FBX 7.4 file with one top-level node "A" holding one property, given as its bytes
QByteArray fileWithProperty(const QByteArray& property) {
    QByteArray bytes("Kaydara FBX Binary  ", 20);
    bytes.append('\0');
    bytes.append('\x1a');
    bytes.append('\0');
    append32(bytes, 7400);
    const quint32 nodeHeaderBytes = 13;
    const quint32 nameBytes = 1;
    append32(bytes, bytes.size() + nodeHeaderBytes + nameBytes + property.size());
    append32(bytes, 1);
    append32(bytes, property.size());
    bytes.append(static_cast<char>(nameBytes));
    bytes.append('A');
    bytes.append(property);
    bytes.append(QByteArray(nodeHeaderBytes, '\0'));  // a null node ends the list
    return bytes;
}

// an array property: type letter, element count, encoding, the length it states, then its bytes in the file
QByteArray arrayProperty(char type, quint32 count, quint32 encoding, const QByteArray& data, qint64 statedLength = -1) {
    QByteArray bytes;
    bytes.append(type);
    append32(bytes, count);
    append32(bytes, encoding);
    append32(bytes, statedLength < 0 ? static_cast<quint32>(data.size()) : static_cast<quint32>(statedLength));
    bytes.append(data);
    return bytes;
}

// a zlib stream, as FBX stores it (qCompress's form without its 4-byte length)
QByteArray zlibStream(const QByteArray& data) {
    return qCompress(data).mid(4);
}

template <class T>
QByteArray rawBytes(const QVector<T>& values) {
    return QByteArray(reinterpret_cast<const char*>(values.constData()), values.size() * static_cast<int>(sizeof(T)));
}

FBXNode parse(const QByteArray& bytes) {
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    return FBXSerializer::parseFBX(&buffer);
}

template <class T>
QVector<T> parsedArray(const QByteArray& file) {
    const FBXNode root = parse(file);
    if (root.children.size() != 1 || root.children[0].properties.size() != 1) {
        return { };
    }
    const QVector<T>* values = root.children[0].properties[0].template array<T>();
    return values ? *values : QVector<T>();
}

} // namespace

void FBXArrayTests::readsPlainAndCompressedArrays() {
    const QVector<double> doubles { 1.5, -2.0, 3.25 };
    QCOMPARE(parsedArray<double>(fileWithProperty(arrayProperty('d', 3, PLAIN, rawBytes(doubles)))), doubles);
    QCOMPARE(parsedArray<double>(fileWithProperty(arrayProperty('d', 3, ZLIB, zlibStream(rawBytes(doubles))))), doubles);

    const QVector<float> floats { 0.5f, 7.0f };
    QCOMPARE(parsedArray<float>(fileWithProperty(arrayProperty('f', 2, ZLIB, zlibStream(rawBytes(floats))))), floats);

    QVector<qint32> ints;
    for (int i = 0; i < 1000; i++) {
        ints.append(i % 7 - 3);
    }
    QCOMPARE(parsedArray<qint32>(fileWithProperty(arrayProperty('i', 1000, ZLIB, zlibStream(rawBytes(ints))))), ints);
    QCOMPARE(parsedArray<qint32>(fileWithProperty(arrayProperty('i', 1000, PLAIN, rawBytes(ints)))), ints);

    const QVector<qint64> longs { 1, -1, 1LL << 40 };
    QCOMPARE(parsedArray<qint64>(fileWithProperty(arrayProperty('l', 3, PLAIN, rawBytes(longs)))), longs);

    const QVector<bool> bools { true, false, true, true };
    QCOMPARE(parsedArray<bool>(fileWithProperty(arrayProperty('b', 4, ZLIB, zlibStream(rawBytes(bools))))), bools);
    QCOMPARE(parsedArray<bool>(fileWithProperty(arrayProperty('b', 4, PLAIN, rawBytes(bools)))), bools);
}

void FBXArrayTests::readsEmptyCompressedArray() {
    const FBXNode root = parse(fileWithProperty(arrayProperty('d', 0, ZLIB, zlibStream(QByteArray()))));
    QCOMPARE(root.children.size(), 1);
    const QVector<double>* values = root.children[0].properties[0].array<double>();
    QVERIFY(values);
    QVERIFY(values->isEmpty());
}

void FBXArrayTests::refusesCompressedLengthBeyondFile() {
    const QByteArray body(64, 'A');
    for (char type : { 'b', 'i', 'f', 'd', 'l' }) {
        QVERIFY_EXCEPTION_THROWN(parse(fileWithProperty(arrayProperty(type, 16, ZLIB, body, INT_MAX - 3))), QString);
        QVERIFY_EXCEPTION_THROWN(parse(fileWithProperty(arrayProperty(type, 16, ZLIB, body, UINT_MAX))), QString);
        QVERIFY_EXCEPTION_THROWN(parse(fileWithProperty(arrayProperty(type, 16, ZLIB, body, body.size() + 1000))), QString);
    }
}

void FBXArrayTests::refusesPlainArrayBeyondFile() {
    const QByteArray body(16, '\0');
    QVERIFY_EXCEPTION_THROWN(parse(fileWithProperty(arrayProperty('d', 100000000, PLAIN, body))), QString);
    QVERIFY_EXCEPTION_THROWN(parse(fileWithProperty(arrayProperty('b', INT_MAX, PLAIN, body))), QString);
    QVERIFY_EXCEPTION_THROWN(parse(fileWithProperty(arrayProperty('i', UINT_MAX, PLAIN, body))), QString);
    // 40 bytes stated; the file holds 16 and the closing null node's 13
    QVERIFY_EXCEPTION_THROWN(parse(fileWithProperty(arrayProperty('i', 10, PLAIN, body))), QString);
}

void FBXArrayTests::refusesInflationBeyondDeflateLimit() {
    // 200 MB stated for a few bytes of zlib data
    const QByteArray stream = zlibStream(QByteArray(16, '\0'));
    QVERIFY_EXCEPTION_THROWN(parse(fileWithProperty(arrayProperty('i', 50000000, ZLIB, stream))), QString);
}

void FBXArrayTests::refusesCompressedDataOfWrongSize() {
    const QVector<qint32> four { 1, 2, 3, 4 };
    const QByteArray stream = zlibStream(rawBytes(four));
    QVERIFY_EXCEPTION_THROWN(parse(fileWithProperty(arrayProperty('i', 2, ZLIB, stream))), QString);  // holds more
    QVERIFY_EXCEPTION_THROWN(parse(fileWithProperty(arrayProperty('i', 8, ZLIB, stream))), QString);  // ends early
    QVERIFY_EXCEPTION_THROWN(parse(fileWithProperty(arrayProperty('i', 4, ZLIB, QByteArray(12, 'x')))), QString);  // not zlib
}

void FBXArrayTests::refusesStringBeyondFile() {
    QByteArray property;
    property.append('S');
    append32(property, 1000);
    property.append("hello");
    QVERIFY_EXCEPTION_THROWN(parse(fileWithProperty(property)), QString);
}

void FBXArrayTests::refusesArrayHeaderCutShort() {
    QByteArray file = fileWithProperty(arrayProperty('d', 1, PLAIN, QByteArray(8, '\0')));
    const int cut = file.indexOf("Ad") + 4;  // the name, the type letter and 2 bytes of the element count
    QVERIFY_EXCEPTION_THROWN(parse(file.left(cut)), QString);
}
