//
//  PlainScriptValueConversionsTests.cpp
//  tests/script-values/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "PlainScriptValueConversionsTests.h"

#include <cmath>
#include <limits>

#include <QtCore/QCborValue>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonValue>

#include <PlainScriptValue.h>
#include <PlainScriptValueConversions.h>
#include <RegisteredMetaTypes.h>

#include "Golden.h"

QTEST_GUILESS_MAIN(PlainScriptValueConversionsTests)

namespace {

// What a script would write, read as a plain value (through CBOR, as a script's values come).
PlainScriptValue plainInput(const QString& json) {
    if (json == "undefined") {
        return PlainScriptValue::undefinedValue();
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(("[" + json + "]").toUtf8(), &error);
    Q_ASSERT(error.error == QJsonParseError::NoError);
    return PlainScriptValue::fromCbor(QCborValue::fromJsonValue(document.array().at(0)));
}

QString vec3Text(const glm::vec3& v) {
    return QString("(%1, %2, %3)").arg(v.x).arg(v.y).arg(v.z);
}

QString vec2Text(const glm::vec2& v) {
    return QString("(%1, %2)").arg(v.x).arg(v.y);
}

QString quatText(const glm::quat& q) {
    return QString("(%1, %2, %3, %4)").arg(q.x).arg(q.y).arg(q.z).arg(q.w);
}

QString u8vec3Text(const glm::u8vec3& v) {
    return QString("(%1, %2, %3)").arg(v.x).arg(v.y).arg(v.z);
}

template <typename T> QString listText(const QVector<T>& values, QString (*each)(const T&)) {
    QStringList parts;
    for (const T& value : values) {
        parts << each(value);
    }
    return "[" + parts.join(", ") + "]";
}

QString floatText(const float& f) { return QString::number(f); }
QString boolText(const bool& b) { return b ? "true" : "false"; }
QString vec3Item(const glm::vec3& v) { return vec3Text(v); }
QString quatItem(const glm::quat& q) { return quatText(q); }

#define COMPARE_VEC3(actual, expected, input) \
    QVERIFY2((actual) == (expected), qPrintable(QString(input) + ": " + vec3Text(actual) + " vs " + vec3Text(expected)))

}  // namespace

void PlainScriptValueConversionsTests::testWriting() {
    PlainScriptEngine plain;
    const glm::vec3 vec3(1.5f, -2.0f, 0.25f);
    const glm::u8vec3 color(255, 0, 128);
    const glm::quat quat(0.5f, 0.5f, 0.5f, 0.5f);
    const glm::quat broken(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 1.0f);
    const QRect rect(1, 2, 30, 40);
    const AACube cube(glm::vec3(-1.0f, 2.0f, 3.5f), 4.0f);
    const QVector<glm::vec3> vec3s { glm::vec3(1.0f), glm::vec3(0.0f, 0.5f, 1.0f) };
    const QVector<glm::quat> quats { glm::quat(), quat };
    const QVector<bool> bools { true, false, true };
    const QVector<float> floats { 0.5f, 1.0f, -3.25f };

    // each conversion's object, as the script sees it: the members in order, with their values
    QStringList lines;
    lines << "vec2 " + vec2ToScriptValue(&plain, glm::vec2(1.0f, 2.5f)).toJson();
    lines << "vec3 " + vec3ToScriptValue(&plain, vec3).toJson();
    lines << "vec3 colour " + vec3ColorToScriptValue(&plain, vec3).toJson();
    lines << "u8vec3 " + u8vec3ToScriptValue(&plain, color).toJson();
    lines << "u8vec3 colour " + u8vec3ColorToScriptValue(&plain, color).toJson();
    lines << "quat " + quatToScriptValue(&plain, quat).toJson();
    lines << "quat with NaN " + quatToScriptValue(&plain, broken).toJson();
    lines << "rect " + qRectToScriptValue(&plain, rect).toJson();
    lines << "cube " + aaCubeToScriptValue(&plain, cube).toJson();
    lines << "vec3 list " + qVectorVec3ToScriptValue(&plain, vec3s).toJson();
    lines << "colour list " + qVectorVec3ColorToScriptValue(&plain, vec3s).toJson();
    lines << "quat list " + qVectorQuatToScriptValue(&plain, quats).toJson();
    lines << "bool list " + qVectorBoolToScriptValue(&plain, bools).toJson();
    lines << "float list " + qVectorFloatToScriptValue(&plain, floats).toJson();
    QString where;
    QVERIFY2(checkGolden("writing.txt", lines.join("\n") + "\n", where), qPrintable(where));

    // the members in the order a script sees them, spelled out
    QVERIFY(quatToScriptValue(&plain, broken).propertyNames().isEmpty());
    QCOMPARE(vec3ToScriptValue(&plain, vec3).propertyNames(), QStringList({ "x", "y", "z" }));
    QCOMPARE(u8vec3ColorToScriptValue(&plain, color).propertyNames(), QStringList({ "red", "green", "blue" }));
    QCOMPARE(quatToScriptValue(&plain, quat).propertyNames(), QStringList({ "x", "y", "z", "w" }));
    QCOMPARE(qRectToScriptValue(&plain, rect).propertyNames(), QStringList({ "x", "y", "width", "height" }));
    QCOMPARE(aaCubeToScriptValue(&plain, cube).propertyNames(), QStringList({ "x", "y", "z", "scale" }));
    QCOMPARE(vec3ToScriptValue(&plain, vec3).property("y").toNumber(), -2.0);
    QCOMPARE(u8vec3ColorToScriptValue(&plain, color).property("blue").toInteger(), 128.0);
}

void PlainScriptValueConversionsTests::testReadingVectors() {
    const QStringList inputs {
        "{\"x\": 1, \"y\": 2, \"z\": 3}", "{\"x\": 1}", "{\"r\": 1, \"g\": 2, \"b\": 3}", "{\"red\": 10, \"green\": 20, \"blue\": 30}",
        "{\"x\": 1, \"r\": 7, \"y\": \"2.5\", \"z\": null}", "5", "-1.25", "[1, 2, 3]", "[1, 2]", "[]", "\"red\"", "\"#102030\"",
        "\"nonsense\"", "\"\"", "null", "true", "{}", "undefined", "{\"u\": 1, \"v\": 2}", "{\"x\": \"abc\", \"y\": true}"
    };
    // every input read as a vec3, a vec2 and a quat (each starts from a mark, 9: a reading may leave a component as
    // it was), recorded
    QStringList lines;
    for (const QString& input : inputs) {
        const PlainScriptValue in = plainInput(input);
        glm::vec3 vec3(9.0f);
        vec3FromScriptValue(in, vec3);
        glm::vec2 vec2(9.0f);
        vec2FromScriptValue(in, vec2);
        glm::quat quat(9.0f, 9.0f, 9.0f, 9.0f);
        quatFromScriptValue(in, quat);
        lines << input + " -> vec3 " + vec3Text(vec3) + ", vec2 " + vec2Text(vec2) + ", quat " + quatText(quat);
    }
    QString where;
    QVERIFY2(checkGolden("reading-vectors.txt", lines.join("\n") + "\n", where), qPrintable(where));

    // the cases that matter most, spelled out
    glm::vec3 one;
    vec3FromScriptValue(plainInput("{\"x\": 1}"), one);
    COMPARE_VEC3(one, glm::vec3(1.0f, 0.0f, 0.0f), "{x: 1}");
    glm::vec3 five;
    vec3FromScriptValue(plainInput("5"), five);
    COMPARE_VEC3(five, glm::vec3(5.0f), "5");
    glm::vec3 red;
    vec3FromScriptValue(plainInput("\"red\""), red);
    COMPARE_VEC3(red, glm::vec3(255.0f, 0.0f, 0.0f), "red");
    glm::vec3 whole;
    vec3FromScriptValue(plainInput("{\"x\": 1, \"y\": 2, \"z\": 3}"), whole);
    COMPARE_VEC3(whole, glm::vec3(1.0f, 2.0f, 3.0f), "{x, y, z}");
    glm::quat turn;
    quatFromScriptValue(plainInput("{\"x\": 0, \"y\": 1, \"z\": 0, \"w\": 0}"), turn);
    QCOMPARE(turn.y, 1.0f);
    QCOMPARE(turn.w, 0.0f);
}

void PlainScriptValueConversionsTests::testReadingColors() {
    const QStringList inputs {
        "{\"red\": 255, \"green\": 128, \"blue\": 0}", "{\"x\": 1, \"y\": 2, \"z\": 3}", "{\"r\": 300, \"g\": -1, \"b\": 2.7}",
        "7", "[10, 20, 30]", "[10, 20]", "\"blue\"", "\"#ff8000\"", "\"nonsense\"", "null", "{}", "undefined"
    };
    QStringList lines;
    for (const QString& input : inputs) {
        glm::u8vec3 color(9);
        u8vec3FromScriptValue(plainInput(input), color);
        lines << input + " -> u8vec3 " + u8vec3Text(color);
    }
    QString where;
    QVERIFY2(checkGolden("reading-colors.txt", lines.join("\n") + "\n", where), qPrintable(where));
    glm::u8vec3 named(9);
    u8vec3FromScriptValue(plainInput("{\"red\": 255, \"green\": 128, \"blue\": 0}"), named);
    QVERIFY(named == glm::u8vec3(255, 128, 0));
    glm::u8vec3 blue(9);
    u8vec3FromScriptValue(plainInput("\"blue\""), blue);
    QVERIFY(blue == glm::u8vec3(0, 0, 255));
}

void PlainScriptValueConversionsTests::testReadingOthers() {
    const QStringList inputs {
        "{\"x\": 1, \"y\": 2, \"width\": 30, \"height\": 40, \"scale\": 2.5, \"z\": -3}", "{\"x\": \"4\", \"scale\": \"big\"}",
        "{}", "null", "5", "[1, 2, 3, 4]", "undefined"
    };
    QStringList lines;
    for (const QString& input : inputs) {
        QRect rect(7, 7, 7, 7);
        qRectFromScriptValue(plainInput(input), rect);
        AACube cube;
        aaCubeFromScriptValue(plainInput(input), cube);
        lines << input + QString(" -> rect (%1, %2, %3, %4), cube %5 scale %6").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height())
                                                                            .arg(vec3Text(cube.getCorner())).arg(cube.getScale());
    }
    QString where;
    QVERIFY2(checkGolden("reading-others.txt", lines.join("\n") + "\n", where), qPrintable(where));
    QRect rect;
    qRectFromScriptValue(plainInput("{\"x\": 1, \"y\": 2, \"width\": 30, \"height\": 40}"), rect);
    QCOMPARE(rect, QRect(1, 2, 30, 40));
}

void PlainScriptValueConversionsTests::testReadingLists() {
    const QStringList inputs {
        "[{\"x\": 1, \"y\": 2, \"z\": 3}, 4, [5, 6, 7], \"red\", null]", "[1, \"a\", 2.5, null, true, \"3\"]",
        "[true, 0, \"x\", null, \"\", 1]", "[{\"x\": 0, \"y\": 0, \"z\": 0, \"w\": 1}, {}, {\"x\": 2, \"y\": 0, \"z\": 0, \"w\": 0}]",
        "[]", "{\"length\": 2, \"0\": 1, \"1\": 2}", "5", "\"text\"", "null", "undefined"
    };
    QStringList lines;
    for (const QString& input : inputs) {
        const PlainScriptValue in = plainInput(input);
        QVector<float> every;
        qVectorFloatFromScriptValue(in, every);
        lines << input + " -> vec3s " + listText<glm::vec3>(qVectorVec3FromScriptValue(in), vec3Item)
                       + ", quats " + listText<glm::quat>(qVectorQuatFromScriptValue(in), quatItem)
                       + ", bools " + listText<bool>(qVectorBoolFromScriptValue(in), boolText)
                       + ", floats " + listText<float>(qVectorFloatFromScriptValue(in), floatText)
                       + ", every element as a float " + listText<float>(every, floatText);
    }
    QString where;
    QVERIFY2(checkGolden("reading-lists.txt", lines.join("\n") + "\n", where), qPrintable(where));
    // the selective reader keeps the numbers only, the other every element as a number
    QCOMPARE(qVectorFloatFromScriptValue(plainInput("[1, \"a\", 2.5, null, true, \"3\"]")), QVector<float>({ 1.0f, 2.5f }));
    QVector<float> every;
    qVectorFloatFromScriptValue(plainInput("[1, \"a\", 2.5, null, true, \"3\"]"), every);
    QCOMPARE(every, QVector<float>({ 1.0f, 0.0f, 2.5f, 0.0f, 1.0f, 3.0f }));
    QCOMPARE(qVectorVec3FromScriptValue(plainInput("[{\"x\": 1, \"y\": 2, \"z\": 3}, 4, [5, 6, 7], \"red\", null]")).size(), 5);
    QCOMPARE(qVectorVec3FromScriptValue(plainInput("5")).size(), 0);
}
