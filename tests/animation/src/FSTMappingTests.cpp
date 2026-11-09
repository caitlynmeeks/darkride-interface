//
//  FSTMappingTests.cpp
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "FSTMappingTests.h"

#include <QtCore/QDataStream>

#include <FSTReader.h>

QTEST_MAIN(FSTMappingTests)

namespace {

const QByteArray FST_TEXT =
    "name = test\n"
    "filename = test.fbx\n"
    "texdir = textures\n"
    "script = a.js\n"
    "script = b.js\n"
    "freeJoint = LeftArm\n"
    "freeJoint = LeftForeArm\n"
    "joint = jointRoot = Hips\n"
    "joint = jointLean = Spine\n"
    "joint = jointNeck = Neck\n"
    "joint = jointHead = HeadTop_End\n"
    "bs = JawOpen = MouthOpen = 0.7\n"
    "bs = LipsFunnel = JawForeward = 0.39\n"
    "bs = LipsFunnel = Jaw_Down = 0.36\n"
    "bs = LipsFunnel = MouthNarrow_Left = 1.0\n"
    "faceTrackingBs = EyeBlink_L = Blink_Left = 1\n"
    "jointIndex = Hips = 0\n";

QString text(const QVariant& value) {
    // only a real list: Qt 6's toList() also makes a list of a text value's characters
    const int type = value.userType();
    const QVariantList list = type == QMetaType::QVariantList || type == QMetaType::QStringList ? value.toList()
                                                                                                  : QVariantList();
    if (list.isEmpty()) {
        return value.toString();
    }
    QStringList parts;
    for (const QVariant& part : list) {
        parts << part.toString();
    }
    return parts.join(" = ");
}

// Every line a mapping holds, as "key = value" or "key = name = value", sorted.
QStringList lines(const hifi::VariantMultiHash& mapping) {
    QStringList result;
    for (auto it = mapping.constBegin(); it != mapping.constEnd(); ++it) {
        const hifi::VariantMultiHash heading = hifi::multiHashFromVariant(it.value());
        if (heading.isEmpty()) {
            result << it.key() + " = " + text(it.value());
            continue;
        }
        for (auto line = heading.constBegin(); line != heading.constEnd(); ++line) {
            result << it.key() + " = " + line.key() + " = " + text(line.value());
        }
    }
    result.sort();
    return result;
}

// The mapping as the model cache's key streams it.
QByteArray streamed(const hifi::VariantMultiHash& mapping, QDataStream::Status* status) {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out << hifi::variantFromMultiHash(mapping);
    *status = out.status();
    return bytes;
}

}

void FSTMappingTests::keepsRepeatedKeys() {
    const hifi::VariantMultiHash mapping = FSTReader::readMapping(FST_TEXT);
    QCOMPARE(mapping.values(SCRIPT_FIELD).size(), 2);
    QCOMPARE(mapping.values("freeJoint").size(), 2);
    // one entry per heading, however many lines it has
    QCOMPARE(mapping.values(BLENDSHAPE_FIELD).size(), 1);
    QCOMPARE(mapping.values(JOINT_FIELD).size(), 1);

    const hifi::VariantMultiHash blendshapes = hifi::multiHashFromVariant(mapping.value(BLENDSHAPE_FIELD));
    QCOMPARE(blendshapes.values("LipsFunnel").size(), 3);
    QCOMPARE(blendshapes.values("JawOpen").size(), 1);
    QCOMPARE(text(blendshapes.value("JawOpen")), QString("MouthOpen = 0.7"));

    const hifi::VariantMultiHash joints = hifi::multiHashFromVariant(mapping.value(JOINT_FIELD));
    QCOMPARE(joints.size(), 4);
    QCOMPARE(joints.value("jointRoot").toString(), QString("Hips"));
}

void FSTMappingTests::mergesFaceTrackingShapes() {
    const hifi::VariantMultiHash mapping = FSTReader::readMapping(FST_TEXT);
    const hifi::VariantMultiHash blendshapes = FSTReader::getBlendshapeMappings(mapping);
    QCOMPARE(blendshapes.values("LipsFunnel").size(), 3);
    QCOMPARE(blendshapes.values("EyeBlink_L").size(), 1);
    QCOMPARE(FSTReader::getFaceTrackingOnlyBlendshapes(mapping), QStringList { "EyeBlink_L" });
}

void FSTMappingTests::splitsLegacyShapes() {
    const hifi::VariantMultiHash mapping = FSTReader::readMapping(
        "bs = LipsUpperUp = UpperLipUp_Left = 0.7\n"
        "bs = LipsUpperUp = UpperLipUp_Right = 0.7\n"
        "bs = JawChew = Chew = 1\n");
    const hifi::VariantMultiHash blendshapes = hifi::multiHashFromVariant(mapping.value(BLENDSHAPE_FIELD));
    QVERIFY(!blendshapes.contains("JawChew"));
    for (const QString& side : { QString("MouthUpperUp_L"), QString("MouthUpperUp_R") }) {
        const QVariantList halves = blendshapes.values(side);
        QCOMPARE(halves.size(), 2);
        for (const QVariant& half : halves) {
            QCOMPARE(half.toList().value(1).toFloat(), 0.35f);
        }
    }
}

void FSTMappingTests::writesWhatItReads() {
    const hifi::VariantMultiHash mapping = FSTReader::readMapping(FST_TEXT);
    const hifi::VariantMultiHash again = FSTReader::readMapping(FSTReader::writeMapping(mapping));
    QCOMPARE(lines(again), lines(mapping));
    QVERIFY(lines(mapping).contains("bs = LipsFunnel = Jaw_Down = 0.36"));
}

void FSTMappingTests::predictsTypeFromHeadings() {
    QCOMPARE(FSTReader::predictModelType(FSTReader::readMapping(FST_TEXT)), FSTReader::HEAD_AND_BODY_MODEL);

    // by hand, with no "bs" (readMapping always adds one, which counts as a head)
    hifi::VariantMultiHash joints;
    for (const char* name : { "jointRoot", "jointLean", "jointNeck", "jointHead" }) {
        joints.insert(name, "Bone");
    }
    hifi::VariantMultiHash body;
    body.insert(JOINT_FIELD, hifi::variantFromMultiHash(joints));
    QCOMPARE(FSTReader::predictModelType(body), FSTReader::BODY_ONLY_MODEL);

    hifi::VariantMultiHash notHeading;
    notHeading.insert(JOINT_FIELD, "Hips");
    QCOMPARE(FSTReader::predictModelType(notHeading), FSTReader::ENTITY_MODEL);
}

void FSTMappingTests::readsHeadingsInEveryForm() {
    QVariantHash plain;
    plain.insert("jointRoot", "Hips");
    QCOMPARE(hifi::multiHashFromVariant(QVariant(plain)).value("jointRoot").toString(), QString("Hips"));
    QVERIFY(hifi::multiHashFromVariant(QVariant(QString("Hips"))).isEmpty());
    QVERIFY(hifi::multiHashFromVariant(QVariant()).isEmpty());

    hifi::VariantMultiHash repeated;
    repeated.insert("LipsFunnel", 1);
    repeated.insert("LipsFunnel", 2);
    QCOMPARE(hifi::multiHashFromVariant(hifi::variantFromMultiHash(repeated)).values("LipsFunnel").size(), 2);
}

void FSTMappingTests::cacheKeyFollowsHeadings() {
    QDataStream::Status status;
    const QByteArray first = streamed(FSTReader::readMapping(FST_TEXT), &status);
    QCOMPARE(status, QDataStream::Ok);
    QCOMPARE(streamed(FSTReader::readMapping(FST_TEXT), &status), first);

    // two mappings that differ only inside a heading
    QByteArray changed = FST_TEXT;
    changed.replace("Jaw_Down = 0.36", "Jaw_Down = 0.37");
    QVERIFY(streamed(FSTReader::readMapping(changed), &status) != first);
    QCOMPARE(status, QDataStream::Ok);
}
