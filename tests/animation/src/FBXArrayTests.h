//
//  FBXArrayTests.h
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_FBXArrayTests_h
#define hifi_FBXArrayTests_h

#include <QtTest/QtTest>

// Binary FBX arrays and strings: what reads back from small hand-made files, and what is refused.
class FBXArrayTests : public QObject {
    Q_OBJECT
private slots:
    void readsPlainAndCompressedArrays();
    void readsEmptyCompressedArray();
    void refusesCompressedLengthBeyondFile();
    void refusesPlainArrayBeyondFile();
    void refusesInflationBeyondDeflateLimit();
    void refusesCompressedDataOfWrongSize();
    void refusesStringBeyondFile();
    void refusesArrayHeaderCutShort();
};

#endif // hifi_FBXArrayTests_h
