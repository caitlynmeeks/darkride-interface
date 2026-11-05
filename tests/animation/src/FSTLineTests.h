//
//  FSTLineTests.h
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_FSTLineTests_h
#define hifi_FSTLineTests_h

#include <QtTest/QtTest>

// Names from model files and Unity packages on .fst lines: what an .fst line can hold, the Unity importer's jointMap
// lines from a hand-made .meta, and FSTReader::writeMapping's lines.
class FSTLineTests : public QObject {
    Q_OBJECT
private slots:
    void plainNamesAreLineValues();
    void lineBreaksAndEqualsAreNot();
    void jsonKeepsItsMeaningOnOneLine();
    void plainJsonIsUnchanged();
    void jointNamesStayOnTheirLine();
    void writeMappingKeepsLinesWhole();
};

#endif // hifi_FSTLineTests_h
