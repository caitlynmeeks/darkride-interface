//
//  FSTLineTests.cpp
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "FSTLineTests.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <FSTReader.h>
#include <UnityAvatar.h>
#include <UnityYaml.h>

QTEST_MAIN(FSTLineTests)

namespace {

// A model's .meta as Unity writes it, cut to its humanoid map: one joint named as joints usually are, then joint names
// holding a line break (escaped in double quotes, and as a block scalar of two lines), " = ", and a lone carriage return.
const QByteArray META = R"YAML(fileFormatVersion: 2
guid: 0123456789abcdef0123456789abcdef
ModelImporter:
  serializedVersion: 21300
  humanDescription:
    serializedVersion: 3
    human:
    - boneName: J_Hips
      humanName: Hips
      limit:
        min: {x: 0, y: 0, z: 0}
        max: {x: 0, y: 0, z: 0}
    - boneName: "J_Spine\nscale = 100"
      humanName: Spine
    - boneName: |
        J_Neck
        scale = 100
      humanName: Neck
    - boneName: J_Chest = J_Head
      humanName: Chest
    - boneName: "J_Head\rscale = 100"
      humanName: Head
)YAML";

QString boneName(const QVariantMap& humanDescription, const QString& humanName) {
    for (const QVariant& entry : humanDescription.value("human").toList()) {
        if (entry.toMap().value("humanName").toString() == humanName) {
            return entry.toMap().value("boneName").toString();
        }
    }
    return QString();
}

// The keys a mapping holds lines for, sorted (readMapping always adds a "bs" key, empty when no line names it).
QStringList keysWithLines(const hifi::VariantMultiHash& mapping) {
    QStringList keys;
    for (const QString& key : mapping.uniqueKeys()) {
        if (key != BLENDSHAPE_FIELD || !hifi::multiHashFromVariant(mapping.value(key)).isEmpty()) {
            keys << key;
        }
    }
    keys.sort();
    return keys;
}

}  // namespace

void FSTLineTests::plainNamesAreLineValues() {
    const QStringList names { "Hips", "J_Bip_C_Hips", "Armature|Hips", "mixamorig:Hips", "vrc.v_aa", "Left Eye",
                              QString::fromUtf8("まばたき"), QString::fromUtf8("口角上げ"), "" };
    for (const QString& name : names) {
        QVERIFY2(FSTReader::isOneLine(name), qPrintable(name));
        QVERIFY2(FSTReader::isLineValue(name), qPrintable(name));
    }
}

void FSTLineTests::lineBreaksAndEqualsAreNot() {
    const QStringList breaks { "a\nb", "a\rb", "a\r\nb", "a\tb", "a\vb", "a\fb", QString("a") + QChar(0) + "b",
                               QString("a") + QChar(0x7f) + "b", QString("a") + QChar(0x85) + "b",
                               QString("a") + QChar(0x2028) + "b", QString("a") + QChar(0x2029) + "b", "\n" };
    for (const QString& text : breaks) {
        QVERIFY(!FSTReader::isOneLine(text));
        QVERIFY(!FSTReader::isLineValue(text));
    }
    for (const QString& text : { QString("a = b"), QString("a=b"), QString("=") }) {
        QVERIFY(FSTReader::isOneLine(text));
        QVERIFY(!FSTReader::isLineValue(text));
    }
}

void FSTLineTests::jsonKeepsItsMeaningOnOneLine() {
    const QJsonDocument json(QJsonObject {
        { "name", "a = b\nscale = 100" },
        { "list", QJsonArray { QString("x") + QChar(0x2028) + "y", QString("z") + QChar(0x85), "plain", 1.5 } },
        { "key = with equals", true },
    });
    const QString value = FSTReader::jsonLineValue(json);
    QVERIFY(FSTReader::isLineValue(value));
    QVERIFY(QJsonDocument::fromJson(value.toUtf8()) == json);

    // and through the .fst reader, as one key's value
    const hifi::VariantMultiHash mapping = FSTReader::readMapping(("name = test\nmaterialMap = " + value + "\n").toUtf8());
    QCOMPARE(keysWithLines(mapping), QStringList({ "materialMap", "name" }));
    QVERIFY(QJsonDocument::fromJson(mapping.value("materialMap").toByteArray()) == json);
}

void FSTLineTests::plainJsonIsUnchanged() {
    // what avatars' .fst files hold today comes out byte for byte as before
    const QJsonDocument json(QJsonObject {
        { "sources", QJsonArray { QJsonArray { "J_Bip_L_UpperArm", 0.5 } } },
        { "rotation", QJsonArray { 0.0, 0.7071068, 0.0, 0.7071068 } },
        { "texture", "textures/body_2.png" },
        { "name", QString::fromUtf8("まばたき") },
    });
    QCOMPARE(FSTReader::jsonLineValue(json), QString::fromUtf8(json.toJson(QJsonDocument::Compact)));
}

void FSTLineTests::jointNamesStayOnTheirLine() {
    QString error;
    const QVariantMap human =
        unity::parseDocument(META, &error).toMap().value("ModelImporter").toMap().value("humanDescription").toMap();
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // The reader hands the names over as written: the line breaks and the " = " are in them.
    QCOMPARE(boneName(human, "Hips"), QString("J_Hips"));
    QCOMPARE(boneName(human, "Spine"), QString("J_Spine\nscale = 100"));
    QCOMPARE(boneName(human, "Neck"), QString("J_Neck\nscale = 100"));
    QCOMPARE(boneName(human, "Chest"), QString("J_Chest = J_Head"));
    QCOMPARE(boneName(human, "Head"), QString("J_Head\rscale = 100"));

    // Written onto a jointMap line as it is, such a name would end the line and start one with a key of its own.
    const hifi::VariantMultiHash unchecked =
        FSTReader::readMapping(("jointMap = Spine = " + boneName(human, "Spine") + "\n").toUtf8());
    QVERIFY(unchecked.contains("scale"));

    // The importer's lines: the one joint with a plain name, the others left out with a note each.
    QHash<QString, QString> skeletonNames;
    QStringList notes;
    const QStringList lines = avatarimport::jointMapLines(human, skeletonNames, notes);
    QCOMPARE(lines, QStringList { "jointMap = Hips = J_Hips" });
    QCOMPARE(notes.size(), 4);
    for (const QString& note : notes) {
        QVERIFY2(note.startsWith("left out: the joint for "), qPrintable(note));
        QVERIFY2(FSTReader::isOneLine(note), qPrintable(note));
    }
    QCOMPARE(skeletonNames.size(), 1);
    QCOMPARE(skeletonNames.value("J_Hips"), QString("Hips"));

    // Read back, the lines hold the jointMap and nothing else.
    const hifi::VariantMultiHash mapping = FSTReader::readMapping((lines.join('\n') + '\n').toUtf8());
    QCOMPARE(keysWithLines(mapping), QStringList { "jointMap" });
    const hifi::VariantMultiHash jointMap = hifi::multiHashFromVariant(mapping.value("jointMap"));
    QCOMPARE(jointMap.size(), 1);
    QCOMPARE(jointMap.value("Hips").toString(), QString("J_Hips"));
}

void FSTLineTests::writeMappingKeepsLinesWhole() {
    // a mapping as the model packager makes one: jointIndex's names are the model file's joint names
    hifi::VariantMultiHash jointIndex;
    jointIndex.insert("Hips", 0);
    jointIndex.insert("Spine\ntexdir = elsewhere", 1);
    hifi::VariantMultiHash joint;
    joint.insert("jointRoot", "Hips");
    joint.insert("jointLean", "Spine\ntexdir = elsewhere");
    hifi::VariantMultiHash mapping;
    mapping.insert(NAME_FIELD, "test");
    mapping.insert(FILENAME_FIELD, "test.fbx\ntexdir = elsewhere");
    mapping.insert(SCRIPT_FIELD, "a.js");
    mapping.insert(SCRIPT_FIELD, "b.js\ntexdir = elsewhere");
    mapping.insert(JOINT_INDEX_FIELD, hifi::variantFromMultiHash(jointIndex));
    mapping.insert(JOINT_FIELD, hifi::variantFromMultiHash(joint));
    hifi::VariantMultiHash blendshapes;
    blendshapes.insert("JawOpen", QVariantList { "MouthOpen", "0.7" });
    blendshapes.insert("LipsFunnel", QVariantList { QString("Mouth") + QChar(0x2028) + "texdir = elsewhere", "0.4" });
    mapping.insert(BLENDSHAPE_FIELD, hifi::variantFromMultiHash(blendshapes));

    const QByteArray text = FSTReader::writeMapping(mapping);
    for (const QByteArray& line : text.split('\n')) {
        QVERIFY2(FSTReader::isOneLine(QString::fromUtf8(line)), line.constData());
    }

    // every line that holds a line break is left out whole; the others read back as written
    const hifi::VariantMultiHash read = FSTReader::readMapping(text);
    QCOMPARE(keysWithLines(read), QStringList({ "bs", "joint", "jointIndex", "name", "script" }));
    QCOMPARE(read.value(NAME_FIELD).toString(), QString("test"));
    QCOMPARE(read.values(SCRIPT_FIELD).size(), 1);
    QCOMPARE(read.value(SCRIPT_FIELD).toString(), QString("a.js"));
    const hifi::VariantMultiHash readIndex = hifi::multiHashFromVariant(read.value(JOINT_INDEX_FIELD));
    QCOMPARE(QStringList(readIndex.uniqueKeys()), QStringList { "Hips" });
    QCOMPARE(readIndex.value("Hips").toString(), QString("0"));
    const hifi::VariantMultiHash readJoint = hifi::multiHashFromVariant(read.value(JOINT_FIELD));
    QCOMPARE(QStringList(readJoint.uniqueKeys()), QStringList { "jointRoot" });
    const hifi::VariantMultiHash readShapes = hifi::multiHashFromVariant(read.value(BLENDSHAPE_FIELD));
    QCOMPARE(QStringList(readShapes.uniqueKeys()), QStringList { "JawOpen" });
}
