//
//  ActionDataFormatTests.cpp
//  tests/entity-actions/src
//
//  Created by scripting on 2026-11-04.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//
//  The recorded bytes (GOLDEN_*) came from a Qt 5.15 build of this code. To record them again on a Qt 5 build (never on
//  Qt 6: they are what Qt 6 must match), run the test with WIRE_FORMAT_PRINT=1 and paste the printed lines in.
//
//  An action's bytes are: its type (quint16), its id (QUuid), its own serialization version (quint16), then its fields;
//  floats and glm vectors go as 8-byte doubles, strings as UTF-16 with a byte length, the expiry as a quint64 server
//  time (0 = never). An entity's actionData is a QVector<QByteArray> of those. Nothing in the bytes says which
//  QDataStream version wrote them, which is why every stream that writes or reads them is pinned to Qt_5_15
//  (libraries/shared/src/shared/WireStream.h).
//

#include "ActionDataFormatTests.h"

#include <QtCore/QUuid>

#include <DependencyManager.h>
#include <EntityDynamicFactoryInterface.h>
#include <EntityDynamicInterface.h>
#include <EntityListMessage.h>
#include <EntityTree.h>
#include <ObjectActionOffset.h>
#include <ObjectActionTractor.h>
#include <ObjectActionTravelOriented.h>
#include <ObjectConstraintBallSocket.h>
#include <ObjectConstraintConeTwist.h>
#include <ObjectConstraintHinge.h>
#include <ObjectConstraintSlider.h>
#include <QVariantGLM.h>
#include <StreamUtils.h>
#include <shared/WireStream.h>

QTEST_MAIN(ActionDataFormatTests)

namespace {

const QUuid ACTION_ID("{3c1d8e2a-7b4f-4a6e-9d0c-1e2f3a4b5c6d}");
const QUuid SECOND_ACTION_ID("{5e6f7a8b-9c0d-4e1f-8a2b-3c4d5e6f7a8b}");
const QUuid OTHER_ID("{9a8b7c6d-5e4f-4d3c-8b2a-1f0e9d8c7b6a}");
const QUuid HOLDER_ID("{1a2b3c4d-5e6f-4a7b-8c9d-0e1f2a3b4c5d}");
const QUuid ENTITY_ID("{7f8e9d0c-1b2a-4c3d-9e8f-0a1b2c3d4e5f}");
// values a double writes exactly, so a float reads back equal
const glm::vec3 POINT(1.5f, -2.25f, 3.125f);
const glm::vec3 AXIS(0.0f, 1.0f, 0.0f);
const glm::vec3 OTHER_POINT(-0.5f, 0.75f, 2.0f);
const glm::vec3 OTHER_AXIS(1.0f, 0.0f, 0.0f);
const glm::quat ROTATION(0.5f, 0.5f, -0.5f, 0.5f);   // w, x, y, z
const float TIME_SCALE = 0.25f;
const float DISTANCE = 2.0f;
const float LOW = -0.75f;
const float HIGH = 1.5f;
const QString TAG("golden");

// recorded from the Qt 5.15 build (see the top of the file)
const char* GOLDEN_OFFSET =
    "03e83c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d00013ff8000000000000c002000000000000400900000000000040000000000000003fd00000"
    "000000000100000000000000000000000c0067006f006c00640065006e";
const char* GOLDEN_TRACTOR =
    "08343c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d00013ff8000000000000c00200000000000040090000000000003fd0000000000000013fe000"
    "0000000000bfe00000000000003fe00000000000003fe00000000000003ff80000000000000100000000000000000000000c0067006f006c"
    "00640065006e9a8b7c6d5e4f4d3c8b2a1f0e9d8c7b6a00000003";
const char* GOLDEN_TRAVEL_ORIENTED =
    "0fa03c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d000100000000000000003ff000000000000000000000000000003fd000000000000000000000"
    "000000000000000c0067006f006c00640065006e";
const char* GOLDEN_HINGE =
    "13885e6f7a8b9c0d4e1f8a2b3c4d5e6f7a8b00023ff8000000000000c002000000000000400900000000000000000000000000003ff00000"
    "0000000000000000000000009a8b7c6d5e4f4d3c8b2a1f0e9d8c7b6abfe00000000000003fe800000000000040000000000000003ff00000"
    "0000000000000000000000000000000000000000bfe80000000000003ff800000000000000000000000000000000000c0067006f006c0064"
    "0065006e";
const char* GOLDEN_SLIDER =
    "1b583c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d000100000000000000000000000c0067006f006c00640065006e3ff8000000000000c0020000"
    "00000000400900000000000000000000000000003ff000000000000000000000000000009a8b7c6d5e4f4d3c8b2a1f0e9d8c7b6abfe00000"
    "000000003fe800000000000040000000000000003ff000000000000000000000000000000000000000000000bfe80000000000003ff80000"
    "00000000c0000000000000004000000000000000";
const char* GOLDEN_BALL_SOCKET =
    "1f403c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d000100000000000000000000000c0067006f006c00640065006e3ff8000000000000c0020000"
    "0000000040090000000000009a8b7c6d5e4f4d3c8b2a1f0e9d8c7b6abfe00000000000003fe80000000000004000000000000000";
const char* GOLDEN_CONE_TWIST =
    "23283c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d000200000000000000000000000c0067006f006c00640065006e3ff8000000000000c0020000"
    "00000000400900000000000000000000000000003ff000000000000000000000000000009a8b7c6d5e4f4d3c8b2a1f0e9d8c7b6abfe00000"
    "000000003fe800000000000040000000000000003ff0000000000000000000000000000000000000000000003fd00000000000003ff80000"
    "000000004000000000000000";
const char* GOLDEN_HOLD =
    "0bb83c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d00011a2b3c4d5e6f4a7b8c9d0e1f2a3b4c5d3ff8000000000000c00200000000000040090000"
    "000000003fe0000000000000bfe00000000000003fe00000000000003fe00000000000003fd000000000000000000008006c006500660074"
    "00000000000000000000000c0067006f006c00640065006e0100";
const char* GOLDEN_FAR_GRAB =
    "17703c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d00013ff8000000000000c00200000000000040090000000000003fd0000000000000013fe000"
    "0000000000bfe00000000000003fe00000000000003fe00000000000003ff80000000000000100000000000000000000000c0067006f006c"
    "00640065006e9a8b7c6d5e4f4d3c8b2a1f0e9d8c7b6a00000003";
const char* GOLDEN_ACTION_LIST =
    "000000020000005503e83c1d8e2a7b4f4a6e9d0c1e2f3a4b5c6d00013ff8000000000000c002000000000000400900000000000040000000"
    "000000003fd00000000000000100000000000000000000000c0067006f006c00640065006e000000ac13885e6f7a8b9c0d4e1f8a2b3c4d5e"
    "6f7a8b00023ff8000000000000c002000000000000400900000000000000000000000000003ff000000000000000000000000000009a8b7c"
    "6d5e4f4d3c8b2a1f0e9d8c7b6abfe00000000000003fe800000000000040000000000000003ff00000000000000000000000000000000000"
    "0000000000bfe80000000000003ff800000000000000000000000000000000000c0067006f006c00640065006e";
const char* GOLDEN_LIST_REQUEST = "010107000000";
const char* GOLDEN_LIST_REPLY =
    "01020700000000000088789c63626060a8ef9bcb23ade5633baf9f4b5ac7d62f7e56774d6e9cbfaf4db7963cdfdc9eea2c46660607390915"
    "2e36a0da03f60c0c020718183c1c801c072e86f4fc9c94d43c85a4fc8ab8fcaaee39bc7ef25dda36be2036031a6061604470f60335db834c"
    "0099c8c47078230093791c44";

// compares with the recorded bytes, or prints them when recording
void checkGolden(const char* name, const QByteArray& bytes, const char* golden) {
    if (qEnvironmentVariableIsSet("WIRE_FORMAT_PRINT")) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QFAIL("record on a Qt 5 build only: the recorded bytes are what Qt 6 must match");
#endif
        printf("const char* GOLDEN_%s = \"%s\";\n", name, bytes.toHex().constData());
        return;
    }
    QVERIFY2(QByteArray(golden).size() > 0, qPrintable(QString("no recorded bytes for %1").arg(name)));
    QCOMPARE(bytes.toHex(), QByteArray(golden));
}

QByteArray golden(const char* hex) {
    return QByteArray::fromHex(hex);
}

// the header every action's bytes start with
void checkHeader(const QByteArray& bytes, EntityDynamicType type, const QUuid& id, quint16 version) {
    QDataStream in(bytes);
    hifi::stream::pin(in);
    EntityDynamicType readType;
    QUuid readID;
    quint16 readVersion;
    in >> readType >> readID >> readVersion;
    QCOMPARE(int(readType), int(type));
    QCOMPARE(readID, id);
    QCOMPARE(readVersion, version);
}

// the common arguments every action reads back
void checkCommon(const QVariantMap& arguments) {
    QCOMPARE(arguments["ttl"].toFloat(), 0.0f);
    QCOMPARE(arguments["tag"].toString(), TAG);
}

QVariantMap withTag(QVariantMap arguments) {
    arguments["tag"] = TAG;
    return arguments;
}

// the envelope an entity writes: the list of its actions' bytes
QByteArray envelope(const QVector<QByteArray>& actions) {
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    hifi::stream::pin(out);
    out << actions;
    return data;
}

QVector<QByteArray> openEnvelope(const QByteArray& data) {
    QVector<QByteArray> actions;
    QDataStream in(data);
    hifi::stream::pin(in);
    in >> actions;
    return actions;
}

// what a Qt 5 build serializes for each type, built through the real class
template <typename Action>
QByteArray serialized(const QUuid& id, const QVariantMap& arguments) {
    Action action(id, nullptr);
    action.updateArguments(withTag(arguments));
    return action.serialize();
}

QVariantMap offsetArguments() {
    QVariantMap arguments;
    arguments["pointToOffsetFrom"] = vec3ToQMap(POINT);
    arguments["linearDistance"] = DISTANCE;
    arguments["linearTimeScale"] = TIME_SCALE;
    return arguments;
}

QVariantMap tractorArguments() {
    QVariantMap arguments;
    arguments["targetPosition"] = vec3ToQMap(POINT);
    arguments["linearTimeScale"] = TIME_SCALE;
    arguments["targetRotation"] = quatToQMap(ROTATION);
    arguments["angularTimeScale"] = HIGH;
    arguments["otherID"] = OTHER_ID.toString();
    arguments["otherJointIndex"] = 3;
    return arguments;
}

QVariantMap hingeArguments() {
    QVariantMap arguments;
    arguments["pivot"] = vec3ToQMap(POINT);
    arguments["axis"] = vec3ToQMap(AXIS);
    arguments["otherEntityID"] = OTHER_ID.toString();
    arguments["otherPivot"] = vec3ToQMap(OTHER_POINT);
    arguments["otherAxis"] = vec3ToQMap(OTHER_AXIS);
    arguments["low"] = LOW;
    arguments["high"] = HIGH;
    return arguments;
}

// the client's factory for physics' types (the client's own also makes avatars' hold and far grab)
class TestDynamicFactory : public EntityDynamicFactoryInterface {
public:
    EntityDynamicPointer factoryBA(EntityItemPointer ownerEntity, QByteArray data) override {
        QDataStream in(data);
        hifi::stream::pin(in);
        EntityDynamicType type;
        QUuid id;
        in >> type >> id;
        EntityDynamicPointer dynamic;
        switch (type) {
            case DYNAMIC_TYPE_OFFSET:
                dynamic = std::make_shared<ObjectActionOffset>(id, ownerEntity);
                break;
            case DYNAMIC_TYPE_TRACTOR:
                dynamic = std::make_shared<ObjectActionTractor>(id, ownerEntity);
                break;
            case DYNAMIC_TYPE_TRAVEL_ORIENTED:
                dynamic = std::make_shared<ObjectActionTravelOriented>(id, ownerEntity);
                break;
            case DYNAMIC_TYPE_HINGE:
                dynamic = std::make_shared<ObjectConstraintHinge>(id, ownerEntity);
                break;
            case DYNAMIC_TYPE_SLIDER:
                dynamic = std::make_shared<ObjectConstraintSlider>(id, ownerEntity);
                break;
            case DYNAMIC_TYPE_BALL_SOCKET:
                dynamic = std::make_shared<ObjectConstraintBallSocket>(id, ownerEntity);
                break;
            case DYNAMIC_TYPE_CONE_TWIST:
                dynamic = std::make_shared<ObjectConstraintConeTwist>(id, ownerEntity);
                break;
            default:
                return nullptr;
        }
        dynamic->deserialize(data);
        return dynamic;
    }
};

}

void ActionDataFormatTests::initTestCase() {
    // as the client registers its own factory (ApplicationServices.cpp)
    DependencyManager::registerInheritance<EntityDynamicFactoryInterface, TestDynamicFactory>();
    DependencyManager::set<TestDynamicFactory>();
}

void ActionDataFormatTests::cleanupTestCase() {
    DependencyManager::destroy<TestDynamicFactory>();
}

void ActionDataFormatTests::actionList() {
    const QByteArray offset = serialized<ObjectActionOffset>(ACTION_ID, offsetArguments());
    const QByteArray hinge = serialized<ObjectConstraintHinge>(SECOND_ACTION_ID, hingeArguments());
    const QByteArray data = envelope({ offset, hinge });
    checkGolden("ACTION_LIST", data, GOLDEN_ACTION_LIST);

    const QVector<QByteArray> actions = openEnvelope(golden(GOLDEN_ACTION_LIST));
    QCOMPARE(actions.size(), 2);
    QCOMPARE(actions[0], golden(GOLDEN_OFFSET));
    QCOMPARE(actions[1], golden(GOLDEN_HINGE));

    // EntityDynamicInterface's log line reads the same envelope
    QCOMPARE(serializedDynamicsToDebugString(golden(GOLDEN_ACTION_LIST)),
             QString("offset-%1 hinge-%2 ").arg(ACTION_ID.toString(), SECOND_ACTION_ID.toString()));
    QCOMPARE(serializedDynamicsToDebugString(QByteArray()), QString());
}

// pasting or importing entities gives their actions new ids and maps the other entity's id (EntityTree)
void ActionDataFormatTests::actionListRemapped() {
    QHash<EntityItemID, EntityItemID> map;
    const QByteArray remapped = EntityTree::remapActionDataIDs(golden(GOLDEN_ACTION_LIST), map);
    const QVector<QByteArray> actions = openEnvelope(remapped);
    QCOMPARE(actions.size(), 2);

    QSet<int> types;
    for (const QByteArray& action : actions) {
        QDataStream in(action);
        hifi::stream::pin(in);
        EntityDynamicType type;
        QUuid id;
        in >> type >> id;
        types.insert(int(type));
        QVERIFY(id != ACTION_ID && id != SECOND_ACTION_ID);
        if (type == DYNAMIC_TYPE_HINGE) {
            ObjectConstraintHinge hinge(id, nullptr);
            hinge.deserialize(action);
            const QVariantMap arguments = hinge.getArguments();
            QVERIFY(map.contains(OTHER_ID));
            QCOMPARE(arguments["otherEntityID"].toUuid(), QUuid(map[OTHER_ID]));
            QCOMPARE(arguments["pivot"].toMap(), vec3ToQMap(POINT));
        }
    }
    QCOMPARE(types, QSet<int>({ DYNAMIC_TYPE_OFFSET, DYNAMIC_TYPE_HINGE }));

    QCOMPARE(EntityTree::remapActionDataIDs(QByteArray(), map), QByteArray());
}

void ActionDataFormatTests::offsetAction() {
    const QByteArray bytes = serialized<ObjectActionOffset>(ACTION_ID, offsetArguments());
    checkGolden("OFFSET", bytes, GOLDEN_OFFSET);
    checkHeader(bytes, DYNAMIC_TYPE_OFFSET, ACTION_ID, 1);

    ObjectActionOffset read(ACTION_ID, nullptr);
    read.deserialize(golden(GOLDEN_OFFSET));
    const QVariantMap arguments = read.getArguments();
    checkCommon(arguments);
    QCOMPARE(arguments["pointToOffsetFrom"].toMap(), vec3ToQMap(POINT));
    QCOMPARE(arguments["linearDistance"].toFloat(), DISTANCE);
    QCOMPARE(arguments["linearTimeScale"].toFloat(), TIME_SCALE);
    QCOMPARE(read.serialize(), golden(GOLDEN_OFFSET));
}

void ActionDataFormatTests::tractorAction() {
    const QByteArray bytes = serialized<ObjectActionTractor>(ACTION_ID, tractorArguments());
    checkGolden("TRACTOR", bytes, GOLDEN_TRACTOR);
    checkHeader(bytes, DYNAMIC_TYPE_TRACTOR, ACTION_ID, 1);

    ObjectActionTractor read(ACTION_ID, nullptr);
    read.deserialize(golden(GOLDEN_TRACTOR));
    const QVariantMap arguments = read.getArguments();
    checkCommon(arguments);
    QCOMPARE(arguments["targetPosition"].toMap(), vec3ToQMap(POINT));
    QCOMPARE(arguments["linearTimeScale"].toFloat(), TIME_SCALE);
    QCOMPARE(arguments["targetRotation"].toMap(), quatToQMap(ROTATION));
    QCOMPARE(arguments["angularTimeScale"].toFloat(), HIGH);
    QCOMPARE(arguments["otherID"].toUuid(), OTHER_ID);
    QCOMPARE(arguments["otherJointIndex"].toInt(), 3);
    QCOMPARE(read.serialize(), golden(GOLDEN_TRACTOR));
}

void ActionDataFormatTests::travelOrientedAction() {
    QVariantMap given;
    given["forward"] = vec3ToQMap(AXIS);
    given["angularTimeScale"] = TIME_SCALE;
    const QByteArray bytes = serialized<ObjectActionTravelOriented>(ACTION_ID, given);
    checkGolden("TRAVEL_ORIENTED", bytes, GOLDEN_TRAVEL_ORIENTED);
    checkHeader(bytes, DYNAMIC_TYPE_TRAVEL_ORIENTED, ACTION_ID, 1);

    ObjectActionTravelOriented read(ACTION_ID, nullptr);
    read.deserialize(golden(GOLDEN_TRAVEL_ORIENTED));
    const QVariantMap arguments = read.getArguments();
    checkCommon(arguments);
    QCOMPARE(arguments["forward"].toMap(), vec3ToQMap(AXIS));
    QCOMPARE(arguments["angularTimeScale"].toFloat(), TIME_SCALE);
    QCOMPARE(read.serialize(), golden(GOLDEN_TRAVEL_ORIENTED));
}

void ActionDataFormatTests::hingeConstraint() {
    const QByteArray bytes = serialized<ObjectConstraintHinge>(SECOND_ACTION_ID, hingeArguments());
    checkGolden("HINGE", bytes, GOLDEN_HINGE);
    checkHeader(bytes, DYNAMIC_TYPE_HINGE, SECOND_ACTION_ID, 2);

    ObjectConstraintHinge read(SECOND_ACTION_ID, nullptr);
    read.deserialize(golden(GOLDEN_HINGE));
    const QVariantMap arguments = read.getArguments();
    checkCommon(arguments);
    QCOMPARE(arguments["pivot"].toMap(), vec3ToQMap(POINT));
    QCOMPARE(arguments["axis"].toMap(), vec3ToQMap(AXIS));
    QCOMPARE(arguments["otherEntityID"].toUuid(), OTHER_ID);
    QCOMPARE(arguments["otherPivot"].toMap(), vec3ToQMap(OTHER_POINT));
    QCOMPARE(arguments["otherAxis"].toMap(), vec3ToQMap(OTHER_AXIS));
    QCOMPARE(arguments["low"].toFloat(), LOW);
    QCOMPARE(arguments["high"].toFloat(), HIGH);
    QCOMPARE(read.serialize(), golden(GOLDEN_HINGE));
}

void ActionDataFormatTests::sliderConstraint() {
    QVariantMap given;
    given["point"] = vec3ToQMap(POINT);
    given["axis"] = vec3ToQMap(AXIS);
    given["otherEntityID"] = OTHER_ID.toString();
    given["otherPoint"] = vec3ToQMap(OTHER_POINT);
    given["otherAxis"] = vec3ToQMap(OTHER_AXIS);
    given["linearLow"] = LOW;
    given["linearHigh"] = HIGH;
    given["angularLow"] = -DISTANCE;
    given["angularHigh"] = DISTANCE;
    const QByteArray bytes = serialized<ObjectConstraintSlider>(ACTION_ID, given);
    checkGolden("SLIDER", bytes, GOLDEN_SLIDER);
    checkHeader(bytes, DYNAMIC_TYPE_SLIDER, ACTION_ID, 1);

    ObjectConstraintSlider read(ACTION_ID, nullptr);
    read.deserialize(golden(GOLDEN_SLIDER));
    const QVariantMap arguments = read.getArguments();
    checkCommon(arguments);
    QCOMPARE(arguments["point"].toMap(), vec3ToQMap(POINT));
    QCOMPARE(arguments["axis"].toMap(), vec3ToQMap(AXIS));
    QCOMPARE(arguments["otherEntityID"].toUuid(), OTHER_ID);
    QCOMPARE(arguments["otherPoint"].toMap(), vec3ToQMap(OTHER_POINT));
    QCOMPARE(arguments["otherAxis"].toMap(), vec3ToQMap(OTHER_AXIS));
    QCOMPARE(arguments["linearLow"].toFloat(), LOW);
    QCOMPARE(arguments["linearHigh"].toFloat(), HIGH);
    QCOMPARE(arguments["angularLow"].toFloat(), -DISTANCE);
    QCOMPARE(arguments["angularHigh"].toFloat(), DISTANCE);
    QCOMPARE(read.serialize(), golden(GOLDEN_SLIDER));
}

void ActionDataFormatTests::ballSocketConstraint() {
    QVariantMap given;
    given["pivot"] = vec3ToQMap(POINT);
    given["otherEntityID"] = OTHER_ID.toString();
    given["otherPivot"] = vec3ToQMap(OTHER_POINT);
    const QByteArray bytes = serialized<ObjectConstraintBallSocket>(ACTION_ID, given);
    checkGolden("BALL_SOCKET", bytes, GOLDEN_BALL_SOCKET);
    checkHeader(bytes, DYNAMIC_TYPE_BALL_SOCKET, ACTION_ID, 1);

    ObjectConstraintBallSocket read(ACTION_ID, nullptr);
    read.deserialize(golden(GOLDEN_BALL_SOCKET));
    const QVariantMap arguments = read.getArguments();
    checkCommon(arguments);
    QCOMPARE(arguments["pivot"].toMap(), vec3ToQMap(POINT));
    QCOMPARE(arguments["otherEntityID"].toUuid(), OTHER_ID);
    QCOMPARE(arguments["otherPivot"].toMap(), vec3ToQMap(OTHER_POINT));
    QCOMPARE(read.serialize(), golden(GOLDEN_BALL_SOCKET));
}

void ActionDataFormatTests::coneTwistConstraint() {
    QVariantMap given;
    given["pivot"] = vec3ToQMap(POINT);
    given["axis"] = vec3ToQMap(AXIS);
    given["otherEntityID"] = OTHER_ID.toString();
    given["otherPivot"] = vec3ToQMap(OTHER_POINT);
    given["otherAxis"] = vec3ToQMap(OTHER_AXIS);
    given["swingSpan1"] = TIME_SCALE;
    given["swingSpan2"] = HIGH;
    given["twistSpan"] = DISTANCE;
    const QByteArray bytes = serialized<ObjectConstraintConeTwist>(ACTION_ID, given);
    checkGolden("CONE_TWIST", bytes, GOLDEN_CONE_TWIST);
    checkHeader(bytes, DYNAMIC_TYPE_CONE_TWIST, ACTION_ID, 2);

    ObjectConstraintConeTwist read(ACTION_ID, nullptr);
    read.deserialize(golden(GOLDEN_CONE_TWIST));
    const QVariantMap arguments = read.getArguments();
    checkCommon(arguments);
    QCOMPARE(arguments["pivot"].toMap(), vec3ToQMap(POINT));
    QCOMPARE(arguments["axis"].toMap(), vec3ToQMap(AXIS));
    QCOMPARE(arguments["otherEntityID"].toUuid(), OTHER_ID);
    QCOMPARE(arguments["otherPivot"].toMap(), vec3ToQMap(OTHER_POINT));
    QCOMPARE(arguments["otherAxis"].toMap(), vec3ToQMap(OTHER_AXIS));
    QCOMPARE(arguments["swingSpan1"].toFloat(), TIME_SCALE);
    QCOMPARE(arguments["swingSpan2"].toFloat(), HIGH);
    QCOMPARE(arguments["twistSpan"].toFloat(), DISTANCE);
    QCOMPARE(read.serialize(), golden(GOLDEN_CONE_TWIST));
}

// AvatarActionHold::serialize (interface/src/avatar/AvatarActionHold.cpp), field by field: the class needs the whole
// client, so its format is written here as it writes it, and avatars' pin of that file is held to these bytes
void ActionDataFormatTests::holdAction() {
    const quint16 holdVersion = 1;
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    hifi::stream::pin(out);
    out << DYNAMIC_TYPE_HOLD;
    out << ACTION_ID;
    out << holdVersion;
    out << HOLDER_ID;           // _holderID
    out << POINT;               // _relativePosition
    out << ROTATION;            // _relativeRotation
    out << TIME_SCALE;          // _linearTimeScale
    out << QString("left");     // _hand
    out << quint64(0);          // expires, server time (0 = never)
    out << TAG;                 // _tag
    out << true;                // _kinematic
    out << false;               // _kinematicSetVelocity
    checkGolden("HOLD", bytes, GOLDEN_HOLD);
    checkHeader(bytes, DYNAMIC_TYPE_HOLD, ACTION_ID, holdVersion);

    // read back as AvatarActionHold::deserialize does
    QDataStream in(golden(GOLDEN_HOLD));
    hifi::stream::pin(in);
    EntityDynamicType type;
    QUuid id, holderID;
    quint16 version;
    glm::vec3 relativePosition;
    glm::quat relativeRotation;
    float linearTimeScale;
    QString hand, tag;
    quint64 expires;
    bool kinematic, kinematicSetVelocity;
    in >> type >> id >> version >> holderID >> relativePosition >> relativeRotation >> linearTimeScale >> hand
       >> expires >> tag >> kinematic >> kinematicSetVelocity;
    QCOMPARE(in.status(), QDataStream::Ok);
    QVERIFY(in.atEnd());
    QCOMPARE(holderID, HOLDER_ID);
    QCOMPARE(relativePosition, POINT);
    QCOMPARE(relativeRotation, ROTATION);
    QCOMPARE(linearTimeScale, TIME_SCALE);
    QCOMPARE(hand, QString("left"));
    QCOMPARE(expires, quint64(0));
    QCOMPARE(tag, TAG);
    QCOMPARE(kinematic, true);
    QCOMPARE(kinematicSetVelocity, false);
}

// AvatarActionFarGrab::serialize writes the far-grab type, then exactly a tractor's header and fields
void ActionDataFormatTests::farGrabAction() {
    const QByteArray tractor = serialized<ObjectActionTractor>(ACTION_ID, tractorArguments());
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    hifi::stream::pin(out);
    out << DYNAMIC_TYPE_FAR_GRAB;
    bytes += tractor.mid(2);
    checkGolden("FAR_GRAB", bytes, GOLDEN_FAR_GRAB);
    checkHeader(bytes, DYNAMIC_TYPE_FAR_GRAB, ACTION_ID, 1);
    QCOMPARE(golden(GOLDEN_FAR_GRAB).mid(2), golden(GOLDEN_TRACTOR).mid(2));
}

void ActionDataFormatTests::entityListMessage() {
    const QByteArray request = EntityListMessage::encodeRequest(7);
    checkGolden("LIST_REQUEST", request, GOLDEN_LIST_REQUEST);

    QVector<EntityListMessage::Record> records(2);
    records[0].id = ENTITY_ID;
    records[0].parentID = OTHER_ID;
    records[0].type = EntityTypes::Box;
    records[0].visible = true;
    records[0].locked = true;
    records[0].created = 1700000000000000ULL;
    records[0].cube = AACube(POINT, DISTANCE);
    records[0].name = "golden box";
    records[1].id = SECOND_ACTION_ID;
    records[1].type = EntityTypes::Model;
    records[1].visible = false;
    records[1].created = 1;
    records[1].cube = AACube(OTHER_POINT, HIGH);
    records[1].name = QString::fromUtf8("ñ");
    const QByteArray reply = EntityListMessage::encodeReply(7, records);
    // the body is compressed, and the compressor may change with the build; the header is the fixed part
    checkGolden("LIST_REPLY", reply, GOLDEN_LIST_REPLY);
    QCOMPARE(reply.left(6).toHex(), golden(GOLDEN_LIST_REPLY).left(6).toHex());

    quint32 requestNumber = 0;
    QVERIFY(EntityListMessage::decodeRequest(golden(GOLDEN_LIST_REQUEST), requestNumber));
    QCOMPARE(requestNumber, quint32(7));

    QVector<EntityListMessage::Record> read;
    QVERIFY(EntityListMessage::decodeReply(golden(GOLDEN_LIST_REPLY), requestNumber, read));
    QCOMPARE(requestNumber, quint32(7));
    QCOMPARE(read.size(), 2);
    for (int i = 0; i < 2; ++i) {
        QCOMPARE(read[i].id, records[i].id);
        QCOMPARE(read[i].parentID, records[i].parentID);
        QCOMPARE(int(read[i].type), int(records[i].type));
        QCOMPARE(read[i].visible, records[i].visible);
        QCOMPARE(read[i].locked, records[i].locked);
        QCOMPARE(read[i].created, records[i].created);
        QCOMPARE(read[i].cube.getCorner(), records[i].cube.getCorner());
        QCOMPARE(read[i].cube.getScale(), records[i].cube.getScale());
        QCOMPARE(read[i].name, records[i].name);
    }
    QVERIFY(EntityListMessage::decodeReply(reply, requestNumber, read));
    QCOMPARE(read.size(), 2);
}
