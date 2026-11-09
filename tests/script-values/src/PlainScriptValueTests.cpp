//
//  PlainScriptValueTests.cpp
//  tests/script-values/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "PlainScriptValueTests.h"

#include <cmath>
#include <limits>

#include <QtCore/QCborArray>
#include <QtCore/QCborMap>
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QPoint>
#include <QtCore/QUrl>
#include <QtCore/QUuid>

#include <PlainScriptValue.h>
#include <PlainScriptValueConversions.h>
#include <RegisteredMetaTypes.h>

#include "Golden.h"
#include "SameVariant.h"

QTEST_GUILESS_MAIN(PlainScriptValueTests)

namespace {

// A number set as a property on an object and read back, the way the conversions make numbers.
PlainScriptValue plainNumber(double value) {
    PlainScriptValue object = PlainScriptValue::newObject();
    object.setProperty("v", PlainScriptValue(value));
    return object.property("v");
}

const double NAN_VALUE = std::numeric_limits<double>::quiet_NaN();
const double INFINITY_VALUE = std::numeric_limits<double>::infinity();

}  // namespace

void PlainScriptValueTests::testNumbers() {
    // a script's number conversions (ECMAScript's ToString, ToBoolean, ToInt32, ToUint32, ToUint16, ToInteger), as
    // the QtScript engine made them, by example
    struct Case {
        double value;
        const char* text;
        bool boolean;
        qint32 int32;
        quint32 uint32;
        quint16 uint16;
        double integer;
        bool integral;   // the variant is an int, not a double
    };
    const Case cases[] = {
        { 0.0, "0", false, 0, 0, 0, 0.0, true },
        { 1.0, "1", true, 1, 1, 1, 1.0, true },
        { -1.0, "-1", true, -1, 4294967295u, 65535, -1.0, true },
        { 1.5, "1.5", true, 1, 1, 1, 1.0, false },
        { -1.5, "-1.5", true, -1, 4294967295u, 65535, -1.0, false },
        { 0.1 + 0.2, "0.30000000000000004", true, 0, 0, 0, 0.0, false },
        { 100.0, "100", true, 100, 100, 100, 100.0, true },
        { 2147483647.0, "2147483647", true, 2147483647, 2147483647u, 65535, 2147483647.0, true },
        { 2147483648.0, "2147483648", true, -2147483647 - 1, 2147483648u, 0, 2147483648.0, false },
        { -2147483648.0, "-2147483648", true, -2147483647 - 1, 2147483648u, 0, -2147483648.0, true },
        { -2147483649.0, "-2147483649", true, 2147483647, 2147483647u, 65535, -2147483649.0, false },
        { 4294967295.0, "4294967295", true, -1, 4294967295u, 65535, 4294967295.0, false },
        { 4294967296.0, "4294967296", true, 0, 0, 0, 4294967296.0, false },
        { 123456789012.0, "123456789012", true, -1097262572, 3197704724u, 6676, 123456789012.0, false },
        { 1e-7, "1e-7", true, 0, 0, 0, 0.0, false },
        { 0.000001, "0.000001", true, 0, 0, 0, 0.0, false },
        { 1e100, "1e+100", true, 0, 0, 0, 1e100, false },
        { 5e-324, "5e-324", true, 0, 0, 0, 0.0, false },
        { NAN_VALUE, "NaN", false, 0, 0, 0, 0.0, false },
        { INFINITY_VALUE, "Infinity", true, 0, 0, 0, INFINITY_VALUE, false },
        { -INFINITY_VALUE, "-Infinity", true, 0, 0, 0, -INFINITY_VALUE, false },
    };
    for (const Case& c : cases) {
        const PlainScriptValue plain = plainNumber(c.value);
        const QString name = QString::number(c.value, 'g', 17);
        QVERIFY2(plain.toString() == QString(c.text), qPrintable(name + ": text " + plain.toString() + " vs " + c.text));
        QVERIFY2(plain.toBool() == c.boolean, qPrintable(name + ": bool"));
        QVERIFY2(plain.toInt32() == c.int32, qPrintable(name + QString(": int32 %1 vs %2").arg(plain.toInt32()).arg(c.int32)));
        QVERIFY2(plain.toUInt32() == c.uint32, qPrintable(name + QString(": uint32 %1 vs %2").arg(plain.toUInt32()).arg(c.uint32)));
        QVERIFY2(plain.toUInt16() == c.uint16, qPrintable(name + QString(": uint16 %1 vs %2").arg(plain.toUInt16()).arg(c.uint16)));
        if (std::isnan(c.value)) {
            QVERIFY(std::isnan(plain.toNumber()));
        } else {
            QVERIFY2(plain.toNumber() == c.value, qPrintable(name + ": number"));
            QVERIFY2(plain.toInteger() == c.integer, qPrintable(name + QString(": integer %1 vs %2").arg(plain.toInteger()).arg(c.integer)));
        }
        QVERIFY2(c.integral == (plain.toVariant().userType() == QMetaType::Int), qPrintable(name + ": the variant's type " + plain.toVariant().typeName()));
        QVERIFY(plain.isNumber() && !plain.isString() && !plain.isObject());
    }
    // the sign of zero is lost in its text
    QCOMPARE(plainNumber(-0.0).toString(), QString("0"));
    QVERIFY(!plainNumber(-0.0).toBool());
    // the integer and unsigned constructors
    QCOMPARE(PlainScriptValue(7).toVariant().userType(), (int)QMetaType::Int);
    QCOMPARE(PlainScriptValue(7u).toVariant().userType(), (int)QMetaType::Int);
    QCOMPARE(PlainScriptValue(3000000000u).toVariant().userType(), (int)QMetaType::Double);
    QCOMPARE(PlainScriptValue(3000000000u).toVariant().toDouble(), 3000000000.0);
    QCOMPARE(PlainScriptValue(1.0f).toVariant().userType(), (int)QMetaType::Int);
    QCOMPARE(PlainScriptValue(0.5f).toVariant().userType(), (int)QMetaType::Double);
}

void PlainScriptValueTests::testNumberText() {
    // the specification's layout, by example
    QCOMPARE(PlainScriptValue::numberToString(1e21), QString("1e+21"));
    QCOMPARE(PlainScriptValue::numberToString(123456789012345680000.0), QString("123456789012345680000"));
    QCOMPARE(PlainScriptValue::numberToString(1e-7), QString("1e-7"));
    QCOMPARE(PlainScriptValue::numberToString(0.000001), QString("0.000001"));
    QCOMPARE(PlainScriptValue::numberToString(0.1 + 0.2), QString("0.30000000000000004"));
    QCOMPARE(PlainScriptValue::numberToString(-0.0), QString("0"));
    QCOMPARE(PlainScriptValue::numberToString(-1.5e-10), QString("-1.5e-10"));
    QCOMPARE(PlainScriptValue::numberToString(1.7976931348623157e308), QString("1.7976931348623157e+308"));
    QCOMPARE(PlainScriptValue::numberToString(5e-324), QString("5e-324"));
    QCOMPARE(PlainScriptValue::numberToString(4294967295.0), QString("4294967295"));
    QCOMPARE(PlainScriptValue::numberToString(1.0), QString("1"));
    QCOMPARE(PlainScriptValue::numberToString(12.5), QString("12.5"));
    // and over a spread of magnitudes, as recorded (what the QtScript engine printed for each)
    QStringList lines;
    for (int exponent = -12; exponent <= 24; ++exponent) {
        for (double mantissa : { 1.0, 1.5, 1.25, 9.999, 3.14159265358979, 7.0 }) {
            const double value = mantissa * std::pow(10.0, exponent);
            lines << QString("%1 -> %2").arg(value, 0, 'g', 17).arg(PlainScriptValue::numberToString(value));
        }
    }
    QString where;
    QVERIFY2(checkGolden("number-text.txt", lines.join("\n") + "\n", where), qPrintable(where));
}

void PlainScriptValueTests::testStrings() {
    // a script's string conversions (ToNumber on text, ToBoolean, ToInt32, ToUint32), by example
    struct Case {
        const char* text;
        double number;   // NaN where the text is no number
        bool boolean;
        qint32 int32;
        quint32 uint32;
    };
    const Case cases[] = {
        { "", 0.0, false, 0, 0 },
        { " ", 0.0, true, 0, 0 },
        { "12", 12.0, true, 12, 12 },
        { " 12 ", 12.0, true, 12, 12 },
        { "1.5", 1.5, true, 1, 1 },
        { "-1.5", -1.5, true, -1, 4294967295u },
        { "abc", NAN_VALUE, true, 0, 0 },
        { "0x10", 16.0, true, 16, 16 },
        { "1e3", 1000.0, true, 1000, 1000 },
        { "Infinity", INFINITY_VALUE, true, 0, 0 },
        { "-0", 0.0, true, 0, 0 },
        { "1.", 1.0, true, 1, 1 },
        { ".5", 0.5, true, 0, 0 },
        { "12abc", NAN_VALUE, true, 0, 0 },
        { "true", NAN_VALUE, true, 0, 0 },
        { "{00000000-0000-0000-0000-000000000000}", NAN_VALUE, true, 0, 0 },
    };
    for (const Case& c : cases) {
        const PlainScriptValue plain(QString(c.text));
        const QString name = QString("'%1'").arg(c.text);
        QCOMPARE(plain.toString(), QString(c.text));
        QCOMPARE(plain.toVariant(), QVariant(QString(c.text)));
        QVERIFY2(plain.toBool() == c.boolean, qPrintable(name + ": bool"));
        if (std::isnan(c.number)) {
            QVERIFY2(std::isnan(plain.toNumber()), qPrintable(name + ": NaN expected"));
        } else {
            QVERIFY2(plain.toNumber() == c.number, qPrintable(name + QString(": %1 vs %2").arg(plain.toNumber()).arg(c.number)));
        }
        QVERIFY2(plain.toInt32() == c.int32, qPrintable(name + ": int32"));
        QVERIFY2(plain.toUInt32() == c.uint32, qPrintable(name + ": uint32"));
        QVERIFY(plain.isString() && !plain.isNumber() && !plain.isObject());
    }
    // the other primitives and the absent value
    QVERIFY(!PlainScriptValue().isValid());
    QVERIFY(PlainScriptValue().toVariant().isNull());
    QCOMPARE(PlainScriptValue().toString(), QString());
    QCOMPARE(PlainScriptValue().toNumber(), 0.0);
    QCOMPARE(PlainScriptValue::undefinedValue().toString(), QString("undefined"));
    QVERIFY(std::isnan(PlainScriptValue::undefinedValue().toNumber()));
    QVERIFY(!PlainScriptValue::undefinedValue().toVariant().isValid());
    QCOMPARE(PlainScriptValue::nullValue().toString(), QString("null"));
    QCOMPARE(PlainScriptValue::nullValue().toNumber(), 0.0);
    QVERIFY(!PlainScriptValue::nullValue().toVariant().isValid());
    QCOMPARE(PlainScriptValue(true).toString(), QString("true"));
    QCOMPARE(PlainScriptValue(false).toString(), QString("false"));
    QCOMPARE(PlainScriptValue(false).toNumber(), 0.0);
    QCOMPARE(PlainScriptValue(true).toNumber(), 1.0);
    QCOMPARE(PlainScriptValue(true).toVariant(), QVariant(true));
    QCOMPARE(PlainScriptValue("text").toVariant(), QVariant(QString("text")));
}

void PlainScriptValueTests::testObjects() {
    PlainScriptValue plain = PlainScriptValue::newObject();
    // properties set in order, with every kind of value, a nested object and a replacement
    plain.setProperty("name", "box");
    plain.setProperty("count", 3);
    plain.setProperty("scale", 0.5);
    plain.setProperty("visible", true);
    plain.setProperty("nothing", PlainScriptValue::nullValue());
    plain.setProperty("unset", PlainScriptValue::undefinedValue());
    PlainScriptValue plainInner = PlainScriptValue::newObject();
    plainInner.setProperty("x", 1);
    plainInner.setProperty("y", 2.5);
    plain.setProperty("position", plainInner);
    plain.setProperty("count", 4);  // set again: the value changes, the place stays
    plain.setProperty("gone", "for now");
    plain.setProperty("gone", PlainScriptValue());  // an invalid value removes the property

    // the order of the properties, as a script's iteration gives it
    QCOMPARE(plain.propertyNames(), QStringList({ "name", "count", "scale", "visible", "nothing", "unset", "position" }));
    const QVariantMap map = plain.toVariant().toMap();
    QCOMPARE(map.value("name"), QVariant(QString("box")));
    QCOMPARE(map.value("count"), QVariant(4));
    QCOMPARE(map.value("scale"), QVariant(0.5));
    QCOMPARE(map.value("visible"), QVariant(true));
    QCOMPARE(map.value("position").toMap().value("x"), QVariant(1));
    QCOMPARE(map.value("position").toMap().value("y"), QVariant(2.5));
    QVERIFY(!map.contains("gone"));
    // reading: present, absent, through a non-object, a nested property
    QVERIFY(plain.property("name").isValid());
    QVERIFY(!plain.property("absent").isValid());
    QVERIFY(!plain.property("gone").isValid());
    QVERIFY(plain.property("nothing").isValid() && plain.property("nothing").isNull());
    QVERIFY(plain.property("unset").isValid() && plain.property("unset").isUndefined());
    QVERIFY(!plain.property("count").property("x").isValid());
    QVERIFY(!PlainScriptValue("text").property("length").isValid());
    QCOMPARE(plain.property("position").property("y").toNumber(), 2.5);
    QCOMPARE(plain.property("count").toVariant(), QVariant(4));
    QVERIFY(plain.isObject() && !plain.isArray());
    QCOMPARE(plain.toString(), QString("[object Object]"));
    QVERIFY(plain.toBool());
    QVERIFY(std::isnan(plain.toNumber()));
    // setting on a non-object does nothing
    PlainScriptValue number(5);
    number.setProperty("x", 1);
    QVERIFY(!number.property("x").isValid() && number.isNumber());
}

void PlainScriptValueTests::testArrays() {
    PlainScriptValue plain = PlainScriptValue::newArray();
    plain.setProperty(0, 1);
    plain.setProperty(1, "two");
    plain.setProperty(2, 2.5);
    plain.setProperty(4, true);  // index 3 is a hole
    PlainScriptValue plainInner = PlainScriptValue::newObject();
    plainInner.setProperty("x", 0);
    plain.setProperty(5, plainInner);

    QCOMPARE(plain.property("length").toInteger(), 6.0);
    QVERIFY(!plain.property(3).isValid());
    QVERIFY(!plain.property(9).isValid());
    QVERIFY(!plain.property("3").isValid());
    QCOMPARE(plain.property("1").toString(), QString("two"));
    QCOMPARE(plain.property(2).toNumber(), 2.5);
    QVERIFY(plain.isArray() && plain.isObject());
    QCOMPARE(plain.toString(), QString("1,two,2.5,,true,[object Object]"));
    QCOMPARE(plain.propertyNames(), QStringList({ "0", "1", "2", "4", "5" }));
    QVERIFY(plain.toVariant().userType() == QMetaType::QVariantList);
    const QVariantList list = plain.toVariant().toList();
    QCOMPARE(list.size(), 6);
    QCOMPARE(list.at(1), QVariant(QString("two")));
    QVERIFY(!list.at(3).isValid());
    QCOMPARE(list.at(4), QVariant(true));
    // the length set shorter drops the elements beyond it
    plain.setProperty("length", 2);
    QCOMPARE(plain.property("length").toInteger(), 2.0);
    QCOMPARE(plain.toVariant().toList(), QVariantList({ 1, QString("two") }));
    // a one-element array reads as its element's text, an empty one as nothing
    PlainScriptValue single = PlainScriptValue::newArray();
    single.setProperty(0, 7);
    QCOMPARE(single.toString(), QString("7"));
    QCOMPARE(single.toNumber(), 7.0);
    QCOMPARE(PlainScriptValue::newArray().toString(), QString());
    QCOMPARE(PlainScriptValue::newArray().toNumber(), 0.0);
}

void PlainScriptValueTests::testSharing() {
    // a property set through a copy is seen through the original, as a script's object is one object
    PlainScriptValue object = PlainScriptValue::newObject();
    PlainScriptValue group = PlainScriptValue::newObject();
    object.setProperty("group", group);
    PlainScriptValue again = object.property("group");
    again.setProperty("inner", 1);
    QCOMPARE(object.property("group").property("inner").toInteger(), 1.0);
    QCOMPARE(group.property("inner").toInteger(), 1.0);
    object.setProperty("group", again);  // set again, as the group macros do: the same object, the same place
    QCOMPARE(object.propertyNames(), QStringList({ "group" }));
    QCOMPARE(object.property("group").propertyNames(), QStringList({ "inner" }));
}

void PlainScriptValueTests::testCbor() {
    PlainScriptValue object = PlainScriptValue::newObject();
    object.setProperty("z", 1);
    object.setProperty("a", 2.5);
    object.setProperty("m", "text");
    object.setProperty("t", true);
    object.setProperty("n", PlainScriptValue::nullValue());
    PlainScriptValue list = PlainScriptValue::newArray();
    list.setProperty(0, 1);
    list.setProperty(1, PlainScriptValue::newObject());
    object.setProperty("list", list);
    object.setProperty("big", 3000000000.0);

    // CBOR keeps the order the properties were set in; a variant map sorts them
    const QCborValue cbor = object.toCbor();
    QVERIFY(cbor.isMap());
    QStringList keys;
    for (const auto& pair : cbor.toMap()) {
        keys.append(pair.first.toString());
    }
    QCOMPARE(keys, QStringList({ "z", "a", "m", "t", "n", "list", "big" }));
    QCOMPARE(object.toVariant().toMap().keys(), QStringList({ "a", "big", "list", "m", "n", "t", "z" }));
    QVERIFY(cbor.toMap().value("z").isInteger());
    QVERIFY(cbor.toMap().value("a").isDouble());
    QVERIFY(cbor.toMap().value("big").isDouble());
    QVERIFY(cbor.toMap().value("n").isNull());

    // and reads back as it was, value, type and order
    const PlainScriptValue back = PlainScriptValue::fromCbor(cbor);
    QString where;
    QVERIFY2(sameVariant(object.toVariant(), back.toVariant(), where), qPrintable(where));
    QCOMPARE(back.propertyNames(), object.propertyNames());
    QVERIFY(back.property("n").isNull());
    QVERIFY(back.property("list").isArray());
    QCOMPARE(back.property("list").property("length").toInteger(), 2.0);

    // what a module may send: a tagged id, bytes, a large integer, an undefined member
    QCborMap sent;
    sent.insert(QString("id"), QCborValue(QUuid("{1a2b3c4d-0000-0000-0000-000000000001}")));
    sent.insert(QString("bytes"), QCborValue(QByteArray("abc")));
    sent.insert(QString("huge"), QCborValue(qint64(1) << 40));
    sent.insert(QString("none"), QCborValue(QCborValue::Undefined));
    sent.insert(QCborValue(7), QCborValue("seven"));
    const PlainScriptValue received = PlainScriptValue::fromCbor(sent);
    QCOMPARE(received.property("id").toString(), QString("{1a2b3c4d-0000-0000-0000-000000000001}"));
    QCOMPARE(received.property("bytes").toString(), QString("YWJj"));
    QCOMPARE(received.property("huge").toVariant().userType(), (int)QMetaType::Double);
    QCOMPARE(received.property("huge").toNumber(), 1099511627776.0);
    QVERIFY(received.property("none").isUndefined());
    QCOMPARE(received.property("7").toString(), QString("seven"));
    QCOMPARE(received.propertyNames(), QStringList({ "id", "bytes", "huge", "none", "7" }));
}

void PlainScriptValueTests::testFromVariant() {
    // Every kind of QVariant a world file, avatar-entity data or an overlay's properties hold, and some that none
    // should, through fromVariant (what the callers outside scripting use): the members in the map's order, the
    // kinds a script has no value for left out
    QVariantMap nested;
    nested.insert("x", 1.0);
    nested.insert("y", 2.5);
    nested.insert("z", -3);
    const QVariantList list { 1, 2.5, "three", true, QVariantMap(), QVariantList(), nested };
    const QVariantList holes { 1, QVariant(), 3, QVariant::fromValue(QPoint(1, 2)), QVariant() };
    QVariantMap map;
    map.insert("bool", true);
    map.insert("int", 7);
    map.insert("double", 2.5);
    map.insert("integral", 3.0);
    map.insert("negativeZero", -0.0);
    map.insert("huge", 1e21);
    map.insert("float", QVariant::fromValue(0.1f));
    map.insert("uint", QVariant::fromValue(3000000000u));
    map.insert("longLong", QVariant::fromValue(qint64(123456789012345)));
    map.insert("uLongLong", QVariant::fromValue(quint64(18446744073709551615ULL)));
    map.insert("string", "text");
    map.insert("url", QUrl("https://example.invalid/a?b=c"));
    map.insert("bytes", QByteArray("12.5"));
    map.insert("letters", QByteArray("abc"));
    map.insert("char", QChar('A'));
    map.insert("uuid", QUuid("{1a2b3c4d-0000-0000-0000-000000000001}"));
    map.insert("dateTime", QDateTime::currentDateTime());
    map.insert("point", QPoint(1, 2));
    map.insert("null", QVariant());
    map.insert("nested", nested);
    map.insert("list", list);
    map.insert("holes", holes);
    map.insert("empty", QVariantMap());

    const PlainScriptValue plain = PlainScriptValue::fromVariant(map);
    // the kinds a script has no value for are left out (as the QtScript engine's conversion left them out: a
    // bool, a number, text, a URL, a map or a list cross, and anything else only when it converts to a number)
    QStringList expectedNames = map.keys();   // a QVariantMap lists its keys sorted
    for (const char* leftOut : { "null", "point", "char", "uuid", "dateTime" }) {
        expectedNames.removeAll(leftOut);
        QVERIFY2(!plain.property(leftOut).isValid(), leftOut);
    }
    QCOMPARE(plain.propertyNames(), expectedNames);
    QCOMPARE(plain.property("bool").toVariant(), QVariant(true));
    QCOMPARE(plain.property("int").toVariant(), QVariant(7));
    QCOMPARE(plain.property("double").toNumber(), 2.5);
    QCOMPARE(plain.property("string").toString(), QString("text"));
    QCOMPARE(plain.property("url").toString(), QString("https://example.invalid/a?b=c"));
    QCOMPARE(plain.property("bytes").toNumber(), 12.5);   // a byte array crosses as the number it converts to
    QCOMPARE(plain.property("letters").toNumber(), 0.0);
    QCOMPARE(plain.property("float").toNumber(), (double)0.1f);
    QCOMPARE(plain.property("uint").toNumber(), 3000000000.0);
    // a 64-bit integer goes through a float (the rule for any other type), and QJsonDocument gives one for a whole
    // number: an entity's creation time loses its low bits on a world load
    QCOMPARE(plain.property("longLong").toNumber(), (double)(float)123456789012345);
    const QVariantMap fromJson = QJsonDocument::fromJson("{\"created\": 1792340679910596}").toVariant().toMap();
    QCOMPARE(fromJson.value("created").userType(), (int)QMetaType::LongLong);
    QCOMPARE(PlainScriptValue::fromVariant(fromJson.value("created")).toNumber(), (double)(float)1792340679910596);
    QCOMPARE(plain.property("uint").toNumber(), 3000000000.0);
    QCOMPARE(plain.property("url").toString(), QString("https://example.invalid/a?b=c"));
    QVERIFY(plain.property("list").isArray());
    QCOMPARE(plain.property("list").property("length").toInteger(), 7.0);
    QCOMPARE(plain.property("list").property(6).property("y").toNumber(), 2.5);
    QCOMPARE(plain.property("holes").property("length").toInteger(), 3.0);  // the elements never set do not count
    QVERIFY(!plain.property("holes").property(1).isValid());
    QVERIFY(plain.property("empty").isObject() && plain.property("empty").propertyNames().isEmpty());

    // a PolyLine's points and widths, as Overlays reads them from a script's properties
    const QVariantList points { nested, nested };
    const QVariantList widths { 0.1, 0.2 };
    QCOMPARE(qVectorVec3FromScriptValue(PlainScriptValue::fromVariant(points)).size(), 2);
    QCOMPARE(qVectorVec3FromScriptValue(PlainScriptValue::fromVariant(points)).at(1), glm::vec3(1.0f, 2.5f, -3.0f));
    QCOMPARE(qVectorFloatFromScriptValue(PlainScriptValue::fromVariant(widths)), QVector<float>({ 0.1f, 0.2f }));
}

void PlainScriptValueTests::testJson() {
    // JSON.stringify's text (recorded from the QtScript engine's own JSON.stringify of the same object): every
    // kind of value, the escapes, the numbers' layout, undefined members left out, holes as null
    PlainScriptValue object = PlainScriptValue::newObject();
    object.setProperty("id", "{1a2b3c4d-0000-0000-0000-000000000001}");
    object.setProperty("type", "Box");
    object.setProperty("lastEdited", 1792340679910596.0);
    object.setProperty("name", "quotes \" and backslash \\ and slash / and tab \t newline \n cr \r bs \b ff \f bell \x07 esc \x1b del \x7f");
    object.setProperty("accents", QString::fromUtf8("caf\xc3\xa9 \xe2\x80\xa8 \xf0\x9f\x8c\xb4 \xe2\x82\xac"));
    object.setProperty("empty", "");
    PlainScriptValue position = PlainScriptValue::newObject();
    position.setProperty("x", -2.9272141456604004);
    position.setProperty("y", 0.20000000298023224);
    position.setProperty("z", -0.0000152587890625);
    object.setProperty("position", position);
    PlainScriptValue numbers = PlainScriptValue::newArray();
    const QList<double> values { 0.0, -0.0, 1.0, -1.0, 1.5, 100.0, 2147483648.0, 1e21, 1e-7, 0.000001, 1e100, 5e-324,
        std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() };
    for (int i = 0; i < values.size(); ++i) {
        numbers.setProperty(i, values.at(i));
    }
    object.setProperty("numbers", numbers);
    PlainScriptValue mixed = PlainScriptValue::newArray();
    mixed.setProperty(0, true);
    mixed.setProperty(1, PlainScriptValue::nullValue());
    mixed.setProperty(2, PlainScriptValue::undefinedValue());
    mixed.setProperty(4, "after a hole");
    mixed.setProperty(5, PlainScriptValue::newObject());
    mixed.setProperty(6, PlainScriptValue::newArray());
    object.setProperty("mixed", mixed);
    object.setProperty("undefinedProperty", PlainScriptValue::undefinedValue());
    object.setProperty("nullProperty", PlainScriptValue::nullValue());
    object.setProperty("false", false);
    object.setProperty("emptyObject", PlainScriptValue::newObject());
    object.setProperty("emptyArray", PlainScriptValue::newArray());
    PlainScriptValue nested = PlainScriptValue::newObject();
    PlainScriptValue deeper = PlainScriptValue::newArray();
    deeper.setProperty(0, PlainScriptValue::newObject());
    deeper.property(0).setProperty("k", "v");
    nested.setProperty("list", deeper);
    nested.setProperty("gone", PlainScriptValue::undefinedValue());
    object.setProperty("nested", nested);
    object.setProperty("key with \"quotes\"", 1);

    struct Indent { const char* name; const char* indent; };
    const Indent indents[] = { { "json-compact.txt", "" }, { "json-four-spaces.txt", "    " }, { "json-tab.txt", "\t" }, { "json-two-spaces.txt", "  " } };
    for (const Indent& indent : indents) {
        QString where;
        QVERIFY2(checkGolden(indent.name, object.toJson(indent.indent) + "\n", where), qPrintable(where));
    }
    // the parts on their own
    QCOMPARE(PlainScriptValue("a\"b").toJson(), QString("\"a\\\"b\""));
    QCOMPARE(PlainScriptValue(1e21).toJson(), QString("1e+21"));
    QCOMPARE(PlainScriptValue(-0.0).toJson(), QString("0"));
    QCOMPARE(PlainScriptValue(std::numeric_limits<double>::quiet_NaN()).toJson(), QString("null"));
    QCOMPARE(PlainScriptValue::nullValue().toJson(), QString("null"));
    QCOMPARE(PlainScriptValue::undefinedValue().toJson(), QString());
    QCOMPARE(PlainScriptValue::newObject().toJson("    "), QString("{}"));
    QCOMPARE(PlainScriptValue::newArray().toJson("    "), QString("[]"));
    QCOMPARE(object.property("nested").toJson(), QString("{\"list\":[{\"k\":\"v\"}]}"));
    QCOMPARE(object.property("mixed").toJson(), QString("[true,null,null,null,\"after a hole\",{},[]]"));
}
