//
//  EntityPropertiesConversionTests.cpp
//  tests/script-values/src
//
//  The entity-property conversions for plain values (what a script, a world file and avatar-entity data get):
//  every type written every way, read back from everything a script or a file may hold, and compared with the
//  recordings under tests/script-values/golden/ (Golden.h: what the conversions produced when they were proven
//  equal to the QtScript engine's, on every build until QtScript was removed).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "EntityPropertiesConversionTests.h"

#include <QtCore/QCborValue>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include <EntityItemProperties.h>
#include <EntityTypes.h>
#include <Gzip.h>
#include <PlainScriptValue.h>
#include <SharedUtil.h>

#include "Golden.h"
#include "SameVariant.h"

QTEST_GUILESS_MAIN(EntityPropertiesConversionTests)

namespace {

// a fixed creation time: the recordings must not depend on the clock
const quint64 SAMPLE_CREATED = 1700000000000000ULL;

// age and ageAsText come from the clock at the moment of the conversion, not from the properties
void stripClock(QVariantMap& map) {
    map.remove("age");
    map.remove("ageAsText");
}

// Whether every value in `part` is in `whole` with the same value, group by group (a writing without defaults carries
// only a group's changed members, so a group is compared member by member)
bool subsetOf(const QVariantMap& part, const QVariantMap& whole, QString& where) {
    for (auto it = part.cbegin(); it != part.cend(); ++it) {
        if (!whole.contains(it.key())) {
            where = it.key() + " not in the full writing";
            return false;
        }
        const QVariant& other = whole.value(it.key());
        if (it.value().userType() == QMetaType::QVariantMap && other.userType() == QMetaType::QVariantMap) {
            if (!subsetOf(it.value().toMap(), other.toMap(), where)) {
                where = it.key() + ": " + where;
                return false;
            }
        } else if (!sameVariant(other, it.value(), where)) {
            where = it.key() + ": " + where;
            return false;
        }
    }
    return true;
}

// The properties written, as a variant map (sorted keys), the clock's members taken out
QVariantMap written(const EntityItemProperties& properties, bool skipDefaults, bool allowUnknownCreateTime = true,
                    bool strictSemantics = false, EntityPsuedoPropertyFlags flags = EntityPsuedoPropertyFlags()) {
    PlainScriptEngine engine;
    QVariantMap map = properties.copyToScriptValue(&engine, skipDefaults, allowUnknownCreateTime, strictSemantics, flags).toVariant().toMap();
    stripClock(map);
    return map;
}

// The properties written, as the script value itself (the members in the order a script sees them), the clock's
// members taken out
PlainScriptValue writtenValue(const EntityItemProperties& properties, bool skipDefaults) {
    PlainScriptEngine engine;
    PlainScriptValue value = properties.copyToScriptValue(&engine, skipDefaults, true);
    value.setProperty("age", PlainScriptValue());
    value.setProperty("ageAsText", PlainScriptValue());
    return value;
}

// An entity's text in the entity server's world file: JSON.stringify(properties, null, '    ') of what the
// conversion writes, as RecurseOctreeToJSONOperator writes it
QString worldFileText(const EntityItemProperties& properties, bool skipDefaults) {
    return writtenValue(properties, skipDefaults).toJson("    ");
}

const QString& testWorldPath() {
    static const QString path = QFileInfo(__FILE__).dir().filePath("../../../tests-manual/wasm-scripting/world.json");
    return path;
}

// Every way the properties are written writes something, and the desired-properties way only those
bool writesEveryWay(const EntityItemProperties& properties, QString& where) {
    EntityPsuedoPropertyFlags pseudo;
    pseudo.set(EntityPsuedoPropertyFlag::FlagsActive);
    pseudo.set(EntityPsuedoPropertyFlag::ID);
    pseudo.set(EntityPsuedoPropertyFlag::Type);
    pseudo.set(EntityPsuedoPropertyFlag::BoundingBox);
    pseudo.set(EntityPsuedoPropertyFlag::RenderInfo);
    pseudo.set(EntityPsuedoPropertyFlag::FaceCamera);
    EntityItemProperties desired = properties;
    EntityPropertyFlags flags;
    flags << PROP_POSITION;
    flags << PROP_NAME;
    flags << PROP_COLOR;
    desired.setDesiredProperties(flags);
    struct Way {
        const char* name;
        const EntityItemProperties& properties;
        bool skipDefaults;
        bool strictSemantics;
        EntityPsuedoPropertyFlags flags;
    };
    const Way ways[] = {
        { "everything", properties, false, false, EntityPsuedoPropertyFlags() },
        { "non-defaults", properties, true, false, EntityPsuedoPropertyFlags() },
        { "strict", properties, false, true, EntityPsuedoPropertyFlags() },
        { "desired", desired, false, false, EntityPsuedoPropertyFlags() },
        { "desired, strict", desired, false, true, EntityPsuedoPropertyFlags() },
        { "pseudo-properties", properties, false, false, pseudo },
        { "desired with pseudo-properties", desired, false, false, pseudo },
    };
    QVariantMap everything;
    for (const Way& way : ways) {
        const QVariantMap map = written(way.properties, way.skipDefaults, true, way.strictSemantics, way.flags);
        if (map.isEmpty()) {
            where = QString(way.name) + ": nothing written";
            return false;
        }
        if (QString(way.name) == "everything") {
            everything = map;
        } else if (QString(way.name) == "non-defaults" || QString(way.name) == "strict") {
            // a subset of everything, with the same values
            if (!subsetOf(map, everything, where)) {
                where = QString(way.name) + ": " + where + " (differs from the full writing)";
                return false;
            }
        }
    }
    const QVariantMap onlyDesired = written(desired, false);
    if (!onlyDesired.contains("position") || !onlyDesired.contains("name") || onlyDesired.contains("dimensions")) {
        where = "desired: " + QStringList(onlyDesired.keys()).join(",");
        return false;
    }
    return true;
}

// What is read from the input, written, read again from that and written again: the same. A reading that lost
// or changed a value shows on the second pass. `first` gets the first writing.
bool readsStably(const PlainScriptValue& input, bool honorReadOnly, QString& where, QVariantMap* first = nullptr) {
    EntityItemProperties once;
    once.copyFromScriptValue(input, honorReadOnly);
    const QVariantMap a = written(once, false);
    EntityItemProperties twice;
    twice.copyFromScriptValue(PlainScriptValue::fromVariant(a), honorReadOnly);
    twice.setLastEdited(once.getLastEdited());  // the clock, not the conversion
    const QVariantMap b = written(twice, false);
    if (first) {
        *first = a;
    }
    if (!sameVariant(a, b, where)) {
        where = "the second reading differs: " + where;
        return false;
    }
    if (a.isEmpty()) {
        where = "nothing read";
        return false;
    }
    return true;
}

// A script's object, from its JSON
PlainScriptValue scriptObject(const QString& json, QString& where) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        where = "the input is not JSON: " + error.errorString();
        return PlainScriptValue();
    }
    return PlainScriptValue::fromCbor(QCborValue::fromJsonValue(document.object()));
}

bool readsStably(const QString& json, bool honorReadOnly, QString& where, QVariantMap* first = nullptr) {
    const PlainScriptValue input = scriptObject(json, where);
    if (!input.isValid()) {
        return false;
    }
    return readsStably(input, honorReadOnly, where, first);
}

// A map read as the callers outside scripting read one (a world file's entity, avatar-entity data, an avatar
// bookmark, an overlay's properties): through fromVariant
bool readsVariantStably(const QVariantMap& map, bool honorReadOnly, QString& where) {
    return readsStably(PlainScriptValue::fromVariant(map), honorReadOnly, where);
}

EntityItemProperties sampleProperties(EntityTypes::EntityType type) {
    EntityItemProperties properties;
    properties.setType(type);
    properties.setCreated(SAMPLE_CREATED);
    properties.setPosition(glm::vec3(1.5f, -2.0f, 3.25f));
    properties.setRotation(glm::quat(0.5f, 0.5f, 0.5f, 0.5f));
    properties.setDimensions(glm::vec3(0.5f, 1.0f, 2.0f));
    properties.setColor(glm::u8vec3(255, 128, 0));
    properties.setName("sample");
    properties.setScript("https://example.invalid/script.js");
    properties.setScriptsEnabled(false);
    properties.setUserData("{\"grabbable\": true}");
    properties.setParentID(QUuid("{1a2b3c4d-0000-0000-0000-000000000001}"));
    properties.setLifetime(60.0f);
    properties.setCollisionless(true);
    properties.setDynamic(true);
    properties.setModelURL("https://example.invalid/model.glb");
    properties.setImageURL("https://example.invalid/image.png");
    properties.setShapeType(SHAPE_TYPE_BOX);
    properties.setBillboardMode(BillboardMode::YAW);
    properties.setActionData(QByteArray("action bytes"));
    properties.setLimitedRun(7);
    properties.getKeyLight().setColor(glm::u8vec3(10, 20, 30));
    properties.getKeyLight().setIntensity(0.75f);
    properties.getGrab().setGrabbable(false);
    properties.getAnimation().setURL("https://example.invalid/animation.fbx");
    properties.getAnimation().setFPS(24.0f);
    properties.getPulse().setMin(0.25f);
    return properties;
}

QString typeName(int type) {
    return EntityTypes::getEntityTypeName(static_cast<EntityTypes::EntityType>(type));
}

// Every type's sample, written one way, as one recording: an object with a member per type
QString samplesText(bool skipDefaults) {
    PlainScriptValue all = PlainScriptValue::newObject();
    for (int type = EntityTypes::Box; type < EntityTypes::NUM_TYPES; ++type) {
        if (type == EntityTypes::RemovedPolyVox) {
            continue;
        }
        all.setProperty(typeName(type), writtenValue(sampleProperties(static_cast<EntityTypes::EntityType>(type)), skipDefaults));
    }
    return all.toJson("    ") + "\n";
}

}  // namespace

void EntityPropertiesConversionTests::testEveryTypeWritten() {
    for (int type = EntityTypes::Box; type < EntityTypes::NUM_TYPES; ++type) {
        if (type == EntityTypes::RemovedPolyVox) {
            continue;
        }
        EntityItemProperties properties;
        properties.setType(static_cast<EntityTypes::EntityType>(type));
        properties.setCreated(SAMPLE_CREATED);
        QString where;
        QVERIFY2(writesEveryWay(properties, where), qPrintable(typeName(type) + ": " + where));
        QCOMPARE(written(properties, false).value("type").toString(), typeName(type));
        // with no creation time, nothing is written unless allowed
        EntityItemProperties fresh;
        fresh.setType(static_cast<EntityTypes::EntityType>(type));
        PlainScriptEngine engine;
        QVERIFY(fresh.copyToScriptValue(&engine, false).toVariant().toMap().isEmpty());
        QVERIFY(!fresh.copyToScriptValue(&engine, false, true).toVariant().toMap().isEmpty());
    }
}

void EntityPropertiesConversionTests::testValuesWritten() {
    for (int type = EntityTypes::Box; type < EntityTypes::NUM_TYPES; ++type) {
        if (type == EntityTypes::RemovedPolyVox) {
            continue;
        }
        QString where;
        QVERIFY2(writesEveryWay(sampleProperties(static_cast<EntityTypes::EntityType>(type)), where), qPrintable(typeName(type) + ": " + where));
    }
    // what a script sees: the keys in the order the properties are declared, not sorted
    const QStringList names = writtenValue(sampleProperties(EntityTypes::Model), false).propertyNames();
    QVERIFY(names.indexOf("id") < names.indexOf("type") && names.indexOf("type") < names.indexOf("name"));
    QVERIFY(names.indexOf("name") < names.indexOf("position") && names.indexOf("position") < names.indexOf("modelURL"));
    QStringList sorted = names;
    sorted.sort();
    QVERIFY(names != sorted);
    // every type's sample, every property and the non-defaults only, as recorded
    QString where;
    QVERIFY2(checkGolden("samples-everything.json", samplesText(false), where), qPrintable(where));
    QVERIFY2(checkGolden("samples-nondefaults.json", samplesText(true), where), qPrintable(where));
}

void EntityPropertiesConversionTests::testReadingEdgeCases() {
    const QStringList inputs {
        // vectors: partial, a number, a colour name, an array, aliases
        "{\"type\": \"Box\", \"position\": {\"x\": 1}}",
        "{\"type\": \"Box\", \"position\": 5}",
        "{\"type\": \"Box\", \"dimensions\": [1, 2, 3]}",
        "{\"type\": \"Box\", \"dimensions\": [1, 2]}",
        "{\"type\": \"Box\", \"color\": \"red\"}",
        "{\"type\": \"Box\", \"color\": {\"r\": 1, \"g\": 2, \"b\": 3}}",
        "{\"type\": \"Box\", \"color\": [10, 20, 30]}",
        "{\"type\": \"Box\", \"color\": 7}",
        "{\"type\": \"Zone\", \"keyLight\": {\"color\": \"blue\", \"direction\": {\"x\": \"NaN\", \"y\": 1, \"z\": 0}}}",
        // quaternions: whole, partial, with a NaN
        "{\"type\": \"Box\", \"rotation\": {\"x\": 0, \"y\": 0.7071, \"z\": 0, \"w\": 0.7071}}",
        "{\"type\": \"Box\", \"rotation\": {\"x\": 1, \"y\": 1}}",
        "{\"type\": \"Box\", \"rotation\": {\"x\": \"NaN\", \"y\": 0, \"z\": 0, \"w\": 1}}",
        // the legacy names
        "{\"type\": \"Model\", \"animationURL\": \"https://example.invalid/a.fbx\", \"animationFPS\": 12}",
        "{\"type\": \"Box\", \"ignoreForCollisions\": true, \"collisionsWillMove\": true}",
        "{\"type\": \"Image\", \"textures\": \"{\\\"tex.picture\\\": \\\"https://example.invalid/p.png\\\"}\"}",
        "{\"type\": \"Image\", \"textures\": \"{\\\"tex.picture\\\": \\\"https://example.invalid/p.png\\\"}\", \"imageURL\": \"https://example.invalid/q.png\"}",
        "{\"type\": \"Text\", \"faceCamera\": true}",
        "{\"type\": \"Text\", \"isFacingAvatar\": true}",
        "{\"type\": \"Text\", \"faceCamera\": true, \"isFacingAvatar\": true}",
        "{\"type\": \"Text\", \"faceCamera\": true, \"billboardMode\": \"full\"}",
        "{\"type\": \"PolyLine\", \"faceCamera\": true}",
        // enumerations by name, good and bad
        "{\"type\": \"Shape\", \"shapeType\": \"sphere\", \"shape\": \"Cylinder\", \"billboardMode\": \"yaw\"}",
        "{\"type\": \"Shape\", \"shapeType\": \"nonsense\", \"billboardMode\": 3}",
        "{\"type\": \"Zone\", \"keyLightMode\": \"enabled\", \"shapeType\": \"box\"}",
        // numbers as text, out of range, and the unsigned readers
        "{\"type\": \"Box\", \"parentJointIndex\": \"7\", \"limitedRun\": \"4294967295\", \"editionNumber\": \"12.5\"}",
        "{\"type\": \"Box\", \"parentJointIndex\": 70000, \"limitedRun\": -1, \"lifetime\": \"abc\", \"alpha\": \"0.5\"}",
        "{\"type\": \"Box\", \"created\": 1700000000000000, \"scriptTimestamp\": \"42\"}",
        // bytes as base64, an id with and without braces
        "{\"type\": \"Box\", \"actionData\": \"YWN0aW9u\", \"parentID\": \"{1a2b3c4d-0000-0000-0000-000000000001}\"}",
        "{\"type\": \"Box\", \"parentID\": \"1a2b3c4d-0000-0000-0000-000000000002\", \"owningAvatarID\": \"not an id\"}",
        // nulls, undefined through a missing value, booleans as other things
        "{\"type\": \"Box\", \"name\": null, \"position\": null, \"visible\": null, \"color\": null, \"userData\": null}",
        "{\"type\": \"Box\", \"visible\": \"false\", \"locked\": 0, \"dynamic\": \"yes\", \"collisionless\": 1}",
        // groups: a group as a whole, a group's value of the wrong kind
        "{\"type\": \"Zone\", \"haze\": {\"hazeRange\": 100, \"hazeColor\": {\"red\": 1, \"green\": 2, \"blue\": 3}}, \"bloom\": 5}",
        "{\"type\": \"Box\", \"grab\": {\"grabbable\": false, \"equippableLeftPosition\": {\"x\": 1, \"y\": 2, \"z\": 3}}, \"pulse\": {\"min\": 0.1, \"colorMode\": \"in\"}}",
        // the type missing, unknown, or changed
        "{\"position\": {\"x\": 1, \"y\": 2, \"z\": 3}}",
        "{\"type\": \"Nonsense\", \"name\": \"x\"}",
        "{\"type\": \"Model\", \"modelURL\": \"https://example.invalid/m.glb\", \"jointRotations\": [{\"x\": 0, \"y\": 0, \"z\": 0, \"w\": 1}, {}], \"jointRotationsSet\": [true, false]}",
        // lists of numbers and strings
        "{\"type\": \"PolyLine\", \"linePoints\": [{\"x\": 0, \"y\": 0, \"z\": 0}, {\"x\": 1, \"y\": 1, \"z\": 1}], \"strokeWidths\": [0.1, \"0.2\", null], \"strokeColors\": [{\"x\": 1, \"y\": 0, \"z\": 0}]}",
        "{\"type\": \"ParticleEffect\", \"emitRate\": 10, \"emitOrientation\": {\"x\": 0, \"y\": 0, \"z\": 0, \"w\": 1}, \"particleRadius\": \"0.5\"}",
        "{}"
    };
    // each input read (read-only honoured and not): stable, and what is read as recorded (the properties that
    // are not at their defaults, which is what the input set)
    QStringList lines;
    for (const QString& input : inputs) {
        QString where;
        QVariantMap first;
        QVERIFY2(readsStably(input, true, where, &first), qPrintable(input + ": " + where));
        QVERIFY2(readsStably(input, false, where), qPrintable(input + " (read-only ignored): " + where));
        EntityItemProperties properties;
        properties.copyFromScriptValue(scriptObject(input, where), true);
        properties.setCreated(SAMPLE_CREATED);
        QVariantMap nonDefaults = written(properties, true);
        nonDefaults.remove("lastEdited");   // the clock
        lines << input + " -> " + QString::fromUtf8(QJsonDocument::fromVariant(nonDefaults).toJson(QJsonDocument::Compact));
    }
    QString where;
    QVERIFY2(checkGolden("reading-edge-cases.txt", lines.join("\n") + "\n", where), qPrintable(where));
}

void EntityPropertiesConversionTests::testReadingWorldEntities() {
    QFile file(testWorldPath());
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable("no test world at " + testWorldPath()));
    const QJsonArray entities = QJsonDocument::fromJson(file.readAll()).object().value("Entities").toArray();
    QVERIFY(!entities.isEmpty());
    for (const QJsonValue& entity : entities) {
        const QString json = QString::fromUtf8(QJsonDocument(entity.toObject()).toJson(QJsonDocument::Compact));
        QString where;
        QVERIFY2(readsStably(json, true, where), qPrintable(json.left(80) + ": " + where));
    }
}

void EntityPropertiesConversionTests::testReadingVariantMaps() {
    // what an avatar bookmark stores and reads back, and what the overlay conversion starts from: a full
    // property map as toVariant() gives it, for every entity type
    for (int type = EntityTypes::Box; type < EntityTypes::NUM_TYPES; ++type) {
        if (type == EntityTypes::RemovedPolyVox) {
            continue;
        }
        const QVariantMap map = written(sampleProperties(static_cast<EntityTypes::EntityType>(type)), false);
        QString where;
        QVERIFY2(readsVariantStably(map, false, where), qPrintable(typeName(type) + ": " + where));
        QVERIFY2(readsVariantStably(map, true, where), qPrintable(typeName(type) + " (read-only honoured): " + where));
    }

    // an overlay's properties as the Overlays API gets them from a script: ints and doubles, maps for the vectors
    // and the colour, lists, a null, text
    QVariantMap overlay;
    overlay["type"] = "Shape";
    overlay["shape"] = "Cube";
    overlay["name"] = "an overlay";
    overlay["position"] = QVariantMap({ { "x", 1 }, { "y", 2.5 }, { "z", -3 } });
    overlay["dimensions"] = QVariantMap({ { "x", 1 }, { "y", 1 }, { "z", 0.25 } });
    overlay["rotation"] = QVariantMap({ { "x", 0 }, { "y", 0.7071 }, { "z", 0 }, { "w", 0.7071 } });
    overlay["color"] = QVariantMap({ { "red", 255 }, { "green", 128 }, { "blue", 0 } });
    overlay["alpha"] = 0.5;
    overlay["visible"] = true;
    overlay["lifetime"] = -1;
    overlay["parentID"] = "{1a2b3c4d-0000-0000-0000-000000000001}";
    overlay["parentJointIndex"] = 65535;
    overlay["renderLayer"] = "hud";
    overlay["primitiveMode"] = "lines";
    overlay["ignorePickIntersection"] = false;
    overlay["linePoints"] = QVariantList({ QVariantMap({ { "x", 0 }, { "y", 0 }, { "z", 0 } }), QVariantMap({ { "x", 1 }, { "y", 1 }, { "z", 1 } }) });
    overlay["strokeWidths"] = QVariantList({ 0.1, 0.2 });
    overlay["textures"] = "{\"tex\": \"https://example.invalid/t.png\"}";
    overlay["userData"] = QVariant();
    overlay["url"] = QUrl("https://example.invalid/model.glb");
    QString where;
    QVERIFY2(readsVariantStably(overlay, true, where), qPrintable("overlay: " + where));
    EntityItemProperties shape;
    shape.copyFromScriptValue(PlainScriptValue::fromVariant(overlay), true);
    QCOMPARE(shape.getName(), QString("an overlay"));
    QCOMPARE(shape.getPosition(), glm::vec3(1.0f, 2.5f, -3.0f));
    QCOMPARE(shape.getColor(), glm::u8vec3(255, 128, 0));
    QCOMPARE(shape.getParentJointIndex(), (quint16)65535);
    overlay["type"] = "PolyLine";
    QVERIFY2(readsVariantStably(overlay, true, where), qPrintable("polyline overlay: " + where));
    EntityItemProperties line;
    line.copyFromScriptValue(PlainScriptValue::fromVariant(overlay), true);
    QCOMPARE(line.getLinePoints().size(), 2);
    QCOMPARE(line.getStrokeWidths(), QVector<float>({ 0.1f, 0.2f }));

    // the world file's entities as EntityTree::readFromMap gets them: QJsonDocument's variants, every number a double
    QFile file(testWorldPath());
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable("no test world at " + testWorldPath()));
    const QVariantList entities = QJsonDocument::fromJson(file.readAll()).toVariant().toMap().value("Entities").toList();
    QVERIFY(!entities.isEmpty());
    for (const QVariant& entity : entities) {
        const QVariantMap map = entity.toMap();
        QVERIFY2(readsVariantStably(map, true, where), qPrintable(map.value("id").toString() + ": " + where));
        QVERIFY2(readsVariantStably(map, false, where), qPrintable(map.value("id").toString() + " (read-only ignored): " + where));
    }
}

void EntityPropertiesConversionTests::testWorldFileText() {
    // the test world's entities, as read for a load and written again as the world file writes a live entity's
    // (without defaults), one recording
    QFile file(testWorldPath());
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable("no test world at " + testWorldPath()));
    const QVariantList entities = QJsonDocument::fromJson(file.readAll()).toVariant().toMap().value("Entities").toList();
    QVERIFY(!entities.isEmpty());
    PlainScriptValue all = PlainScriptValue::newArray();
    int index = 0;
    for (const QVariant& entity : entities) {
        EntityItemProperties properties;
        properties.copyFromScriptValue(PlainScriptValue::fromVariant(entity.toMap()), false);
        properties.setCreated(SAMPLE_CREATED);
        properties.setLastEdited(SAMPLE_CREATED);
        const QString text = worldFileText(properties, true);
        QVERIFY2(text.size() >= 100, qPrintable(entity.toMap().value("id").toString() + ": too short to be an entity: " + text));
        all.setProperty(index++, writtenValue(properties, true));
    }
    QString where;
    QVERIFY2(checkGolden("world-file.json", all.toJson("    ") + "\n", where), qPrintable(where));
    // the text is the writing's JSON, indented as the file is
    const QString text = worldFileText(sampleProperties(EntityTypes::Box), true);
    QVERIFY(text.startsWith("{\n    \"") && text.endsWith("\n}"));
    QVERIFY(text.contains("\"name\": \"sample\""));
}

void EntityPropertiesConversionTests::testAvatarEntityBlobs() {
    // propertiesToBlob: the writing, as JSON, the avatar's own id replaced; blobToProperties reads it back
    const QUuid myAvatarID("{aaaaaaaa-0000-0000-0000-00000000000a}");
    for (int type = EntityTypes::Box; type < EntityTypes::NUM_TYPES; ++type) {
        if (type == EntityTypes::RemovedPolyVox) {
            continue;
        }
        const QString name = typeName(type);
        EntityItemProperties properties = sampleProperties(static_cast<EntityTypes::EntityType>(type));
        properties.setParentID(myAvatarID);
        for (bool allProperties : { false, true }) {
            PlainScriptEngine engine;
            QVariant variantProperties = properties.copyToScriptValue(&engine, !allProperties).toVariant();
            QJsonObject jsonObject = QJsonDocument::fromVariant(variantProperties).object();
            if (jsonObject.contains("parentID") && QUuid(jsonObject["parentID"].toString()) == myAvatarID) {
                jsonObject["parentID"] = AVATAR_SELF_ID.toString();
            }
            jsonObject.remove("age");
            jsonObject.remove("ageAsText");
            const QByteArray expected = QJsonDocument(jsonObject).toJson(QJsonDocument::Compact);

            QByteArray blob;
            EntityItemProperties::propertiesToBlob(myAvatarID, properties, blob, allProperties);
            QJsonObject blobObject = QJsonDocument::fromJson(blob).object();
            blobObject.remove("age");
            blobObject.remove("ageAsText");
            const QByteArray actual = QJsonDocument(blobObject).toJson(QJsonDocument::Compact);
            QVERIFY2(actual == expected, qPrintable(name + ": " + actual.left(200) + " vs " + expected.left(200)));
            QVERIFY2(blobObject["parentID"].toString() == AVATAR_SELF_ID.toString(), qPrintable(name + ": the avatar's own id is not replaced"));

            // and read back: what blobToProperties gives is what reading the blob's map gives
            EntityItemProperties fromMap;
            fromMap.copyFromScriptValue(PlainScriptValue::fromVariant(QJsonDocument::fromJson(blob).toVariant().toMap()), false);
            EntityItemProperties fromBlob;
            QVERIFY2(EntityItemProperties::blobToProperties(blob, fromBlob), qPrintable(name + ": the blob does not read"));
            QVariantMap a = written(fromMap, false);
            QVariantMap b = written(fromBlob, false);
            a.remove("lastEdited");   // the clock at the end of each reading
            b.remove("lastEdited");
            QString where;
            QVERIFY2(sameVariant(a, b, where), qPrintable(name + ": " + where));
            QCOMPARE(fromBlob.getName(), QString("sample"));
        }
    }
    EntityItemProperties none;
    QVERIFY(!EntityItemProperties::blobToProperties(QByteArray(), none));
    QVERIFY(!EntityItemProperties::blobToProperties(QByteArray("[1, 2]"), none));
    QVERIFY(!EntityItemProperties::blobToProperties(QByteArray("{}"), none));
}

void EntityPropertiesConversionTests::testRoundTrip() {
    // what is written for an entity with values, read back as a script's full property map is
    for (EntityTypes::EntityType type : { EntityTypes::Model, EntityTypes::Image, EntityTypes::Zone, EntityTypes::Text, EntityTypes::Shape }) {
        const QVariantMap map = written(sampleProperties(type), false);
        const QString json = QString::fromUtf8(QJsonDocument::fromVariant(map).toJson(QJsonDocument::Compact));
        QString where;
        QVariantMap first;
        QVERIFY2(readsStably(json, true, where, &first), qPrintable(EntityTypes::getEntityTypeName(type) + ": " + where));
        QVERIFY2(readsStably(json, false, where), qPrintable(EntityTypes::getEntityTypeName(type) + " (read-only ignored): " + where));
        QCOMPARE(first.value("name").toString(), QString("sample"));
        QCOMPARE(first.value("position").toMap().value("y").toDouble(), -2.0);
    }
    // what a script writes, crossed as CBOR and read back, is what reading the value directly gives
    const PlainScriptValue plain = writtenValue(sampleProperties(EntityTypes::Model), false);
    const PlainScriptValue back = PlainScriptValue::fromCbor(plain.toCbor());
    EntityItemProperties fromCbor;
    fromCbor.copyFromScriptValue(back, true);
    EntityItemProperties direct;
    direct.copyFromScriptValue(plain, true);
    fromCbor.setLastEdited(direct.getLastEdited());
    const QVariantMap a = written(direct, false);
    const QVariantMap b = written(fromCbor, false);
    QString where;
    QVERIFY2(sameVariant(a, b, where), qPrintable(where));
    QVERIFY(a.value("name").toString() == "sample" && a.value("modelURL").toString() == "https://example.invalid/model.glb");
    // scripts off (not the default) comes through
    QVERIFY(a.contains("scriptsEnabled") && !a.value("scriptsEnabled").toBool() && !b.value("scriptsEnabled").toBool());
}

namespace {

// A car riding a track, with every vehicle property set to a value that is not its default (the wheels and seats as
// the lists WheeledVehicleSpec reads), and a Track with every track and train property set
const char* const VEHICLE_ON_A_TRACK = R"JSON({
    "type": "Box", "name": "a car on a track", "dynamic": true,
    "position": {"x": 1, "y": 2, "z": 3}, "dimensions": {"x": 2, "y": 1, "z": 4},
    "trackID": "{1a2b3c4d-0000-0000-0000-0000000000aa}", "trackOffset": 12.5,
    "vehicle": {
        "type": "car", "mass": 1200, "centerOfMass": {"x": 0, "y": -0.25, "z": 0.125},
        "wheels": "[{\"name\":\"frontLeft\",\"center\":[-0.8,-0.3,-1.3],\"radius\":0.35,\"width\":0.2,\"mass\":20,\"suspensionDistance\":0.3,\"targetPosition\":0.5,\"spring\":35000,\"damper\":4500,\"steer\":1,\"drive\":true,\"brake\":1,\"handBrake\":0,\"suspensionForcePoint\":false},{\"name\":\"rearRight\",\"center\":{\"x\":0.8,\"y\":-0.3,\"z\":1.3},\"radius\":0.35,\"steer\":0,\"drive\":true,\"handBrake\":1}]",
        "seats": "[{\"name\":\"driver\",\"position\":[-0.4,0.2,0],\"yaw\":0,\"driver\":true},{\"name\":\"passenger\",\"position\":{\"x\":0.4,\"y\":0.2,\"z\":0},\"yaw\":180}]",
        "maxSteerAngle": 35, "maxTorque": 650, "minRPM": 900, "maxRPM": 7000, "gearRatios": "[3.5,2.1,1.4,1.0,0.8]",
        "reverseGearRatio": 3.125, "finalDriveRatio": 3.875, "maxBrakeTorque": 1800, "maxHandBrakeTorque": 4200,
        "driveType": "all", "suspensionFrequency": 1.75, "suspensionDamping": 0.625, "wheel": "frontLeft", "axle": "z",
        "driver": "{1a2b3c4d-0000-0000-0000-0000000000bb}", "inputSequence": 42, "throttle": 0.5, "steering": -0.25,
        "brake": 0.125, "handBrake": 1, "engineRPM": 3200, "gear": 3, "stateTime": 123456789,
        "envelopeVolume": 5000, "centerOfBuoyancy": {"x": 0, "y": 1.5, "z": 0}, "gasDensity": 0.0625,
        "maxThrust": 20000, "maxVerticalThrust": 800, "thrustPoint": {"x": 0, "y": 0.25, "z": 0.5},
        "dragArea": {"x": 3, "y": 8, "z": 0.75}, "angularDragArea": {"x": 800, "y": 800, "z": 600}, "rudderArea": 2,
        "elevatorArea": 4, "finPoint": {"x": 0, "y": 0.5, "z": 4.5}, "maxFinAngle": 32, "ballastRate": 0.25,
        "ventRate": 0.375, "pitch": 0.125, "lift": 0.5, "trim": -0.0625, "ballast": 0.75, "fill": 0.875,
        "wingArea": 24, "wingSpan": 11, "wingPoint": {"x": 0, "y": 0.25, "z": 0.25}, "tilt": 1
    }
})JSON";

const char* const TRACK_WITH_A_TRAIN = R"JSON({
    "type": "Track", "name": "a track with a train",
    "position": {"x": 0, "y": 0, "z": 0}, "dimensions": {"x": 40, "y": 4, "z": 20},
    "trackPoints": [{"x": 0, "y": 0, "z": 0}, {"x": 10, "y": 1, "z": 0}, {"x": 20, "y": 0, "z": 5}, {"x": 10, "y": -1, "z": 10}],
    "trackBank": [0, 0.25, -0.125, 0], "trackDwell": [5, 0, 3, 0], "trackClosed": true,
    "trackSpeed": 7.5, "trackAcceleration": 1.25, "trainDistance": 33.5, "trainSpeed": 6.25, "trainReversed": true,
    "trainDwell": 2.5, "trainStation": 2, "trainTime": 987654321, "trackProfile": "none", "trackWidth": 0.75,
    "trackHeight": 0.5, "trackClearance": 1.25, "trackPylonSpacing": 12, "trackPylonFloor": -6
})JSON";

// The named properties of a writing, compared with another's
bool sameValues(const QVariantMap& expected, const QVariantMap& actual, const QStringList& names, QString& where) {
    for (const QString& name : names) {
        if (!expected.contains(name)) {
            where = name + " not written";
            return false;
        }
        if (!sameVariant(expected.value(name), actual.value(name), where)) {
            where = name + ": " + where;
            return false;
        }
    }
    return true;
}

}  // namespace

void EntityPropertiesConversionTests::testVehiclesAndTracks() {
    // what physics asked for (10-18): a round trip through copyToScriptValue and copyFromScriptValue for an entity
    // with every vehicle property set and a Track with its train properties, every value back, and the writing as
    // recorded (physics' train properties over the wire are checked by the correction trace; this is their text)
    struct Sample {
        const char* name;
        const char* recording;
        const char* json;
        QStringList properties;  // each set to a value that is not its default, each to come back
    };
    const Sample samples[] = {
        { "a car on a track", "vehicle-on-a-track.json", VEHICLE_ON_A_TRACK, { "trackID", "trackOffset", "vehicle" } },
        { "a track with a train", "track-with-a-train.json", TRACK_WITH_A_TRAIN,
          { "trackPoints", "trackBank", "trackDwell", "trackClosed", "trackSpeed", "trackAcceleration", "trainDistance",
            "trainSpeed", "trainReversed", "trainDwell", "trainStation", "trainTime", "trackProfile", "trackWidth",
            "trackHeight", "trackClearance", "trackPylonSpacing", "trackPylonFloor" } },
    };
    const int vehicleProperties = EntityItemProperties::getPropertiesOfGroup("vehicle").size();
    QVERIFY(vehicleProperties > 0);
    for (const Sample& sample : samples) {
        const QString name(sample.name);
        QString where;
        const PlainScriptValue input = scriptObject(sample.json, where);
        QVERIFY2(input.isValid(), qPrintable(name + ": " + where));
        EntityItemProperties properties;
        properties.copyFromScriptValue(input, false);
        properties.setCreated(SAMPLE_CREATED);
        properties.setLastEdited(SAMPLE_CREATED);

        // every property set: the writing without defaults carries each one, and all of the vehicle's
        const QVariantMap nonDefaults = written(properties, true);
        for (const QString& property : sample.properties) {
            QVERIFY2(nonDefaults.contains(property), qPrintable(name + ": " + property + " is at its default"));
        }
        if (sample.properties.contains("vehicle")) {
            const QVariantMap vehicle = nonDefaults.value("vehicle").toMap();
            QVERIFY2(vehicle.size() == vehicleProperties,
                     qPrintable(name + QString(": %1 of the %2 vehicle properties set").arg(vehicle.size()).arg(vehicleProperties)));
        }
        QVERIFY2(writesEveryWay(properties, where), qPrintable(name + ": " + where));

        // the round trip: written and read back, the same properties come back, and they are the values set
        const PlainScriptValue plainWritten = writtenValue(properties, false);
        for (bool honorReadOnly : { false, true }) {
            EntityItemProperties back;
            back.copyFromScriptValue(plainWritten, honorReadOnly);
            back.setLastEdited(SAMPLE_CREATED);
            const QVariantMap again = written(back, false);
            const QString way = name + (honorReadOnly ? " (read-only honoured)" : "");
            QVERIFY2(sameValues(nonDefaults, again, sample.properties, where), qPrintable(way + ", the values back: " + where));
            QVariantMap before = written(properties, false);
            QVariantMap after = again;
            for (const char* clock : { "created", "lastEdited" }) {   // the times cross as numbers, read back as the clock sets them
                before.remove(clock);
                after.remove(clock);
            }
            QVERIFY2(sameVariant(before, after, where), qPrintable(way + ", everything back: " + where));
        }

        // the readers outside scripting (a world file's entity, avatar-entity data, a bookmark) and a script's object as text
        const QVariantMap map = written(properties, false);
        QVERIFY2(readsVariantStably(map, false, where), qPrintable(name + ", read as a world file reads it: " + where));
        QVERIFY2(readsVariantStably(map, true, where), qPrintable(name + ", read as a bookmark reads it: " + where));
        const QString json = QString::fromUtf8(QJsonDocument::fromVariant(map).toJson(QJsonDocument::Compact));
        QVERIFY2(readsStably(json, true, where), qPrintable(name + ", read as a script's object: " + where));
        // the writing's text, as recorded
        QVERIFY2(checkGolden(sample.recording, worldFileText(properties, true) + "\n", where), qPrintable(where));
    }
}

void EntityPropertiesConversionTests::testWorldFiles() {
    // whole worlds as an entity server keeps them (models.json.gz, or the JSON), named in SCRIPT_VALUES_TEST_WORLDS
    // (paths separated by ':'): every entity read as EntityTree::readFromMap reads one, and written as the world
    // file and an export write one; skipped when the variable names none
    const QStringList paths = QString::fromLocal8Bit(qgetenv("SCRIPT_VALUES_TEST_WORLDS")).split(':', Qt::SkipEmptyParts);
    if (paths.isEmpty()) {
        QSKIP("SCRIPT_VALUES_TEST_WORLDS names no world files");
    }
    for (const QString& path : paths) {
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable("cannot open " + path));
        QByteArray data = file.readAll();
        if (path.endsWith(".gz")) {
            QByteArray unzipped;
            QVERIFY2(gunzip(data, unzipped), qPrintable("cannot unzip " + path));
            data = unzipped;
        }
        const QVariantList entities = QJsonDocument::fromJson(data).toVariant().toMap().value("Entities").toList();
        QVERIFY2(!entities.isEmpty(), qPrintable("no entities in " + path));
        QMap<QString, int> types;
        for (const QVariant& entity : entities) {
            const QVariantMap map = entity.toMap();
            const QString at = path + " " + map.value("id").toString() + ": ";
            QString where;
            // the load: read-only properties read too, as the world file's reader does
            QVERIFY2(readsVariantStably(map, false, where), qPrintable(at + where));
            // the save: the properties read, written as the world file (without defaults) and as an export
            // (without defaults, or every property)
            EntityItemProperties properties;
            properties.copyFromScriptValue(PlainScriptValue::fromVariant(map), false);
            if (properties.getCreated() == UNKNOWN_CREATED_TIME) {
                properties.setCreated(SAMPLE_CREATED);
            }
            QVERIFY2(worldFileText(properties, true).size() >= 100, qPrintable(at + "the world file's text is too short"));
            QVERIFY2(!written(properties, true).isEmpty(), qPrintable(at + "an export writes nothing"));
            QVERIFY2(!written(properties, false).isEmpty(), qPrintable(at + "an export of every property writes nothing"));
            types[map.value("type").toString()]++;
        }
        QStringList counts;
        for (auto it = types.cbegin(); it != types.cend(); ++it) {
            counts << QString("%1 %2").arg(it.value()).arg(it.key());
        }
        qInfo().noquote() << path << "-" << entities.size() << "entities read and written:" << counts.join(", ");
    }
}

void EntityPropertiesConversionTests::benchmarkOverlayConversions_data() {
    QTest::addColumn<bool>("edit");
    QTest::newRow("an overlay edit") << true;
    QTest::newRow("an overlay read") << false;
}

void EntityPropertiesConversionTests::benchmarkOverlayConversions() {
    // the cost of what Overlays does for every addOverlay and editOverlay (an overlay's properties to an entity's)
    // and every getProperty (an entity's to an overlay's)
    QFETCH(bool, edit);
    QVariantMap overlay;
    overlay["type"] = "Shape";
    overlay["shape"] = "Cube";
    overlay["position"] = QVariantMap({ { "x", 1 }, { "y", 2.5 }, { "z", -3 } });
    overlay["rotation"] = QVariantMap({ { "x", 0 }, { "y", 0.7071 }, { "z", 0 }, { "w", 0.7071 } });
    overlay["dimensions"] = QVariantMap({ { "x", 0.1 }, { "y", 0.1 }, { "z", 0.1 } });
    overlay["color"] = QVariantMap({ { "red", 255 }, { "green", 128 }, { "blue", 0 } });
    overlay["alpha"] = 0.5;
    overlay["visible"] = true;
    overlay["ignorePickIntersection"] = true;
    overlay["parentID"] = "{1a2b3c4d-0000-0000-0000-000000000001}";
    const EntityItemProperties entity = sampleProperties(EntityTypes::Shape);
    int written = 0;
    QBENCHMARK {
        if (edit) {
            EntityItemProperties properties;
            properties.copyFromScriptValue(PlainScriptValue::fromVariant(overlay), true);
            written += properties.getVisible() ? 1 : 0;
        } else {
            PlainScriptEngine plainEngine;
            written += entity.copyToScriptValue(&plainEngine, false).toVariant().toMap().size();
        }
    }
    QVERIFY(written > 0);
}
