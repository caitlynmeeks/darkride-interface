//
//  WasmTypeConversions.cpp
//  libraries/script-engine/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "WasmTypeConversions.h"

#include <mutex>

#include <QtCore/QCborArray>
#include <QtCore/QCborMap>
#include <QtCore/QDebug>
#include <QtCore/QJsonDocument>

#include <AudioInjectorOptions.h>
#include <AvatarData.h>
#include <BoxBase.h>
#include <EntityItemID.h>
#include <EntityItemProperties.h>
#include <EntityScriptingInterface.h>
#include <PlainScriptValue.h>
#include <PointerEvent.h>
#include <RegisteredMetaTypes.h>
#include <ResourceCache.h>
#include <ScriptAvatarData.h>
#include <Sound.h>
#include <ObjectTable.h>
#include <ValueCodec.h>
#include <controllers/UserInputMapper.h>

#include "KeyEvent.h"
#include "MenuItemProperties.h"
#include "MIDIEvent.h"
#include "MouseEvent.h"
#include "HttpRequestObject.h"
#include "ScriptAudioInjector.h"
#include "SpatialEvent.h"
#include "TouchEvent.h"
#include "WebSocketObject.h"
#include "WheelEvent.h"

namespace {

// --- helpers: a script object's members as the QtScript conversions read them ---------------------------------

// A member as QScriptValue::toVariant() gives it (what most QtScript conversions read with .toVariant().toX()).
QVariant variantOf(const ValueCodec& codec, const QCborMap& object, const char* name) {
    return codec.fromCbor(object.value(QLatin1String(name)));
}

// Whether the object has the member at all (QScriptValue::property(name).isValid()).
bool has(const QCborMap& object, const char* name) {
    return object.contains(QLatin1String(name));
}

// A member as JavaScript's ToBoolean reads it (QScriptValue::toBool()).
bool truthy(const ValueCodec& codec, const QCborValue& value) {
    return codec.fromCbor(value, QMetaType::Bool).toBool();
}

// A value converted as the codec converts a parameter of the type: vec3FromScriptValue, quatFromScriptValue and
// the other shared conversions have their codec equivalents built in.
template <typename T>
T typed(const ValueCodec& codec, const QCborValue& value) {
    return codec.fromCbor(value, qMetaTypeId<T>()).template value<T>();
}

template <typename T>
QCborValue cborOf(const ValueCodec& codec, const T& value) {
    return codec.toCbor(QVariant::fromValue(value));
}

QCborValue uuidOf(const QUuid& uuid) {
    // quuidToScriptValue: the text with braces, null for the null uuid
    return uuid.isNull() ? QCborValue(nullptr) : QCborValue(uuid.toString());
}

template <typename T>
using Encode = QCborValue (*)(const ValueCodec& codec, const T& value);
template <typename T>
using Decode = bool (*)(const ValueCodec& codec, const QCborValue& value, T& out);

template <typename T>
void add(Encode<T> encode, Decode<T> decode) {
    ValueCodec::registerType(qMetaTypeId<T>(),
        [encode](const ValueCodec& codec, const QVariant& value) { return encode(codec, value.value<T>()); },
        [decode](const ValueCodec& codec, const QCborValue& value, QVariant& out) {
            T result;
            if (!decode(codec, value, result)) {
                return false;
            }
            out = QVariant::fromValue(result);
            return true;
        });
}

// A sequence of a registered element type (qScriptRegisterSequenceMetaType): an array of the elements' encodings,
// and back from an array (null or undefined give an empty one, as for the codec's own containers).
template <typename Container, typename Element>
void addSequence(Encode<Element> encode, Decode<Element> decode) {
    ValueCodec::registerType(qMetaTypeId<Container>(),
        [encode](const ValueCodec& codec, const QVariant& value) {
            QCborArray array;
            for (const Element& element : value.value<Container>()) {
                array.append(encode(codec, element));
            }
            return QCborValue(array);
        },
        [decode](const ValueCodec& codec, const QCborValue& value, QVariant& out) {
            Container container;
            if (value.isArray()) {
                for (const QCborValue& item : value.toArray()) {
                    Element element;
                    if (!decode(codec, item, element)) {
                        return false;
                    }
                    container.append(element);
                }
            } else if (!value.isNull() && !value.isUndefined()) {
                return false;
            }
            out = QVariant::fromValue(container);
            return true;
        });
}

// An object pointer the QtScript engine hands to the script with ScriptOwnership (newQObject(..., ScriptOwnership)):
// the engine's object table owns it, so it is deleted when the script releases it or stops; `lingers` may keep one
// the script released while it still has work to do (see ObjectTable).
template <typename T>
void addOwnedObject(ObjectTable::Lingers lingers = ObjectTable::Lingers()) {
    ValueCodec::registerType(qMetaTypeId<T*>(),
        [lingers](const ValueCodec& codec, const QVariant& value) { return codec.toCborOwned(value.value<T*>(), lingers); },
        [](const ValueCodec& codec, const QCborValue& value, QVariant& out) {
            bool ok = true;
            QObject* object = codec.fromCbor(value, QMetaType::QObjectStar, &ok).value<QObject*>();
            T* typedObject = qobject_cast<T*>(object);
            if (!ok || (object && !typedObject)) {
                return false;
            }
            out = QVariant::fromValue(typedObject);
            return true;
        });
}

// --- the event types (KeyEvent.cpp, MouseEvent.cpp, TouchEvent.cpp, WheelEvent.cpp, SpatialEvent.cpp) ----------------

QCborValue encodeKeyEvent(const ValueCodec&, const KeyEvent& event) {
    QCborMap map;
    map.insert(QStringLiteral("key"), event.key);
    map.insert(QStringLiteral("text"), event.text);
    map.insert(QStringLiteral("isShifted"), event.isShifted);
    map.insert(QStringLiteral("isMeta"), event.isMeta);
    map.insert(QStringLiteral("isControl"), event.isControl);
    map.insert(QStringLiteral("isAlt"), event.isAlt);
    map.insert(QStringLiteral("isKeypad"), event.isKeypad);
    map.insert(QStringLiteral("isAutoRepeat"), event.isAutoRepeat);
    return map;
}

bool decodeKeyEvent(const ValueCodec& codec, const QCborValue& value, KeyEvent& event) {
    const QCborMap object = value.toMap();
    event.isValid = false;
    event.isMeta = variantOf(codec, object, "isMeta").toBool();
    event.isControl = variantOf(codec, object, "isControl").toBool();
    event.isAlt = variantOf(codec, object, "isAlt").toBool();
    event.isKeypad = variantOf(codec, object, "isKeypad").toBool();
    event.isAutoRepeat = variantOf(codec, object, "isAutoRepeat").toBool();
    if (has(object, "key")) {
        event.key = variantOf(codec, object, "key").toInt();
        event.text = QString(QChar(event.key));
        event.isValid = true;
    } else if (has(object, "text")) {
        event.text = variantOf(codec, object, "text").toString();
        event.key = KeyEvent::keyFromText(event.text, event.isKeypad);
        event.isValid = true;
    }
    if (has(object, "isShifted")) {
        event.isShifted = variantOf(codec, object, "isShifted").toBool();
    } else if (!event.text.isEmpty()) {
        // if no isShifted was included, get it from the text: an upper-case letter or a shifted symbol
        QChar character = event.text.at(0);
        if ((character.isLetter() && character.isUpper()) || QString("~!@#$%^&*()_+{}|:\"<>?").contains(character)) {
            event.isShifted = true;
        }
    }
    return true;
}

QCborValue encodeMouseEvent(const ValueCodec&, const MouseEvent& event) {
    QCborMap map;
    map.insert(QStringLiteral("x"), event.x);
    map.insert(QStringLiteral("y"), event.y);
    map.insert(QStringLiteral("button"), event.button);
    map.insert(QStringLiteral("isLeftButton"), event.isLeftButton);
    map.insert(QStringLiteral("isRightButton"), event.isRightButton);
    map.insert(QStringLiteral("isMiddleButton"), event.isMiddleButton);
    map.insert(QStringLiteral("isShifted"), event.isShifted);
    map.insert(QStringLiteral("isMeta"), event.isMeta);
    map.insert(QStringLiteral("isControl"), event.isControl);
    map.insert(QStringLiteral("isAlt"), event.isAlt);
    return map;
}

bool decodeNothing(const ValueCodec&, const QCborValue&, MouseEvent&) {
    return true;  // MouseEvent::fromScriptValue: "nothing for now"
}

QCborValue encodeTouchEvent(const ValueCodec& codec, const TouchEvent& event) {
    QCborMap map;
    map.insert(QStringLiteral("x"), double(event.x));
    map.insert(QStringLiteral("y"), double(event.y));
    map.insert(QStringLiteral("isPressed"), event.isPressed);
    map.insert(QStringLiteral("isMoved"), event.isMoved);
    map.insert(QStringLiteral("isStationary"), event.isStationary);
    map.insert(QStringLiteral("isReleased"), event.isReleased);
    map.insert(QStringLiteral("isShifted"), event.isShifted);
    map.insert(QStringLiteral("isMeta"), event.isMeta);
    map.insert(QStringLiteral("isControl"), event.isControl);
    map.insert(QStringLiteral("isAlt"), event.isAlt);
    map.insert(QStringLiteral("touchPoints"), event.touchPoints);
    QCborArray points;
    for (const glm::vec2& point : event.points) {
        points.append(cborOf(codec, point));
    }
    map.insert(QStringLiteral("points"), points);
    map.insert(QStringLiteral("radius"), double(event.radius));
    map.insert(QStringLiteral("isPinching"), event.isPinching);
    map.insert(QStringLiteral("isPinchOpening"), event.isPinchOpening);
    map.insert(QStringLiteral("angle"), double(event.angle));
    map.insert(QStringLiteral("deltaAngle"), double(event.deltaAngle));
    QCborArray angles;
    for (float angle : event.angles) {
        angles.append(double(angle));
    }
    map.insert(QStringLiteral("angles"), angles);
    map.insert(QStringLiteral("isRotating"), event.isRotating);
    map.insert(QStringLiteral("rotating"), event.rotating);
    return map;
}

bool decodeNothing(const ValueCodec&, const QCborValue&, TouchEvent&) {
    return true;  // TouchEvent::fromScriptValue: "nothing for now"
}

QCborValue encodeWheelEvent(const ValueCodec&, const WheelEvent& event) {
    QCborMap map;
    map.insert(QStringLiteral("x"), event.x);
    map.insert(QStringLiteral("y"), event.y);
    map.insert(QStringLiteral("delta"), event.delta);
    map.insert(QStringLiteral("orientation"), event.orientation);
    map.insert(QStringLiteral("isLeftButton"), event.isLeftButton);
    map.insert(QStringLiteral("isRightButton"), event.isRightButton);
    map.insert(QStringLiteral("isMiddleButton"), event.isMiddleButton);
    map.insert(QStringLiteral("isShifted"), event.isShifted);
    map.insert(QStringLiteral("isMeta"), event.isMeta);
    map.insert(QStringLiteral("isControl"), event.isControl);
    map.insert(QStringLiteral("isAlt"), event.isAlt);
    return map;
}

bool decodeNothing(const ValueCodec&, const QCborValue&, WheelEvent&) {
    return true;  // WheelEvent::fromScriptValue: "nothing for now"
}

QCborValue encodeSpatialEvent(const ValueCodec& codec, const SpatialEvent& event) {
    QCborMap map;
    map.insert(QStringLiteral("locTranslation"), cborOf(codec, event.locTranslation));
    map.insert(QStringLiteral("locRotation"), cborOf(codec, event.locRotation));
    map.insert(QStringLiteral("absTranslation"), cborOf(codec, event.absTranslation));
    map.insert(QStringLiteral("absRotation"), cborOf(codec, event.absRotation));
    return map;
}

bool decodeNothing(const ValueCodec&, const QCborValue&, SpatialEvent&) {
    return true;  // SpatialEvent::fromScriptValue: "nothing for now"
}

// --- PointerEvent (libraries/shared/src/PointerEvent.cpp) -----------------------------------------------------------------

QCborValue encodePointerEvent(const ValueCodec& codec, const PointerEvent& event) {
    QCborMap map;
    switch (event.getType()) {
        case PointerEvent::Press:
            map.insert(QStringLiteral("type"), QStringLiteral("Press"));
            break;
        case PointerEvent::DoublePress:
            map.insert(QStringLiteral("type"), QStringLiteral("DoublePress"));
            break;
        case PointerEvent::Release:
            map.insert(QStringLiteral("type"), QStringLiteral("Release"));
            break;
        default:
        case PointerEvent::Move:
            map.insert(QStringLiteral("type"), QStringLiteral("Move"));
            break;
    }
    map.insert(QStringLiteral("id"), qint64(event.getID()));
    map.insert(QStringLiteral("pos2D"), cborOf(codec, event.getPos2D()));
    map.insert(QStringLiteral("pos3D"), cborOf(codec, event.getPos3D()));
    map.insert(QStringLiteral("normal"), cborOf(codec, event.getNormal()));
    map.insert(QStringLiteral("direction"), cborOf(codec, event.getDirection()));
    bool isPrimaryButton = false;
    bool isSecondaryButton = false;
    bool isTertiaryButton = false;
    switch (event.getButton()) {
        case PointerEvent::NoButtons:
            map.insert(QStringLiteral("button"), QStringLiteral("None"));
            break;
        case PointerEvent::PrimaryButton:
            map.insert(QStringLiteral("button"), QStringLiteral("Primary"));
            isPrimaryButton = true;
            break;
        case PointerEvent::SecondaryButton:
            map.insert(QStringLiteral("button"), QStringLiteral("Secondary"));
            isSecondaryButton = true;
            break;
        case PointerEvent::TertiaryButton:
            map.insert(QStringLiteral("button"), QStringLiteral("Tertiary"));
            isTertiaryButton = true;
            break;
    }
    if (isPrimaryButton) {
        map.insert(QStringLiteral("isPrimaryButton"), true);
        map.insert(QStringLiteral("isLeftButton"), true);
    }
    if (isSecondaryButton) {
        map.insert(QStringLiteral("isSecondaryButton"), true);
        map.insert(QStringLiteral("isRightButton"), true);
    }
    if (isTertiaryButton) {
        map.insert(QStringLiteral("isTertiaryButton"), true);
        map.insert(QStringLiteral("isMiddleButton"), true);
    }
    const uint32_t buttons = event.getButtons();
    map.insert(QStringLiteral("isPrimaryHeld"), (buttons & PointerEvent::PrimaryButton) == PointerEvent::PrimaryButton);
    map.insert(QStringLiteral("isSecondaryHeld"), (buttons & PointerEvent::SecondaryButton) == PointerEvent::SecondaryButton);
    map.insert(QStringLiteral("isTertiaryHeld"), (buttons & PointerEvent::TertiaryButton) == PointerEvent::TertiaryButton);
    map.insert(QStringLiteral("keyboardModifiers"), qint64(uint32_t(event.getKeyboardModifiers())));
    return map;
}

bool decodePointerEvent(const ValueCodec& codec, const QCborValue& value, PointerEvent& event) {
    if (!value.isMap()) {
        return true;  // the event is left as it was, as for QtScript
    }
    const QCborMap object = value.toMap();
    const QCborValue type = object.value(QLatin1String("type"));
    const QString typeName = type.isString() ? type.toString() : QStringLiteral("Move");
    PointerEvent::EventType eventType = PointerEvent::Move;
    if (typeName == "Press") {
        eventType = PointerEvent::Press;
    } else if (typeName == "DoublePress") {
        eventType = PointerEvent::DoublePress;
    } else if (typeName == "Release") {
        eventType = PointerEvent::Release;
    }
    const QCborValue id = object.value(QLatin1String("id"));
    const uint32_t pointerID = (id.isInteger() || id.isDouble()) ? uint32_t(id.toDouble()) : 0;
    // the QtScript conversion reads the button only when the type is given as a string
    const QString buttonName = type.isString() ? codec.fromCbor(object.value(QLatin1String("button")), QMetaType::QString).toString()
                                               : QStringLiteral("NoButtons");
    PointerEvent::Button button = PointerEvent::NoButtons;
    if (buttonName == "Primary") {
        button = PointerEvent::PrimaryButton;
    } else if (buttonName == "Secondary") {
        button = PointerEvent::SecondaryButton;
    } else if (buttonName == "Tertiary") {
        button = PointerEvent::TertiaryButton;
    }
    uint32_t buttons = 0;
    if (truthy(codec, object.value(QLatin1String("isPrimaryHeld")))) {
        buttons |= PointerEvent::PrimaryButton;
    }
    if (truthy(codec, object.value(QLatin1String("isSecondaryHeld")))) {
        buttons |= PointerEvent::SecondaryButton;
    }
    if (truthy(codec, object.value(QLatin1String("isTertiaryHeld")))) {
        buttons |= PointerEvent::TertiaryButton;
    }
    const auto modifiers = Qt::KeyboardModifiers(codec.fromCbor(object.value(QLatin1String("keyboardModifiers")), QMetaType::UInt).toUInt());
    event = PointerEvent(eventType, pointerID, typed<glm::vec2>(codec, object.value(QLatin1String("pos2D"))),
                         typed<glm::vec3>(codec, object.value(QLatin1String("pos3D"))),
                         typed<glm::vec3>(codec, object.value(QLatin1String("normal"))),
                         typed<glm::vec3>(codec, object.value(QLatin1String("direction"))), button, buttons, modifiers);
    return true;
}

// --- MIDIEvent.cpp, MenuItemProperties.cpp ---------------------------------------------------------------------------

QCborValue encodeMIDIEvent(const ValueCodec&, const MIDIEvent& event) {
    QCborMap map;
    map.insert(QStringLiteral("deltaTime"), event.deltaTime);
    map.insert(QStringLiteral("type"), event.type);
    map.insert(QStringLiteral("data1"), event.data1);
    map.insert(QStringLiteral("data2"), event.data2);
    return map;
}

bool decodeMIDIEvent(const ValueCodec& codec, const QCborValue& value, MIDIEvent& event) {
    const QCborMap object = value.toMap();
    event.deltaTime = variantOf(codec, object, "deltaTime").toDouble();
    event.type = variantOf(codec, object, "type").toUInt();
    event.data1 = variantOf(codec, object, "data1").toUInt();
    event.data2 = variantOf(codec, object, "data2").toUInt();
    return true;
}

QCborValue encodeMenuItemProperties(const ValueCodec&, const MenuItemProperties&) {
    return QCborMap();  // menuItemPropertiesToScriptValue: "not supported"
}

bool decodeMenuItemProperties(const ValueCodec& codec, const QCborValue& value, MenuItemProperties& properties) {
    const QCborMap object = value.toMap();
    properties.menuName = variantOf(codec, object, "menuName").toString();
    properties.menuItemName = variantOf(codec, object, "menuItemName").toString();
    properties.isCheckable = variantOf(codec, object, "isCheckable").toBool();
    properties.isChecked = variantOf(codec, object, "isChecked").toBool();
    properties.isSeparator = variantOf(codec, object, "isSeparator").toBool();
    // the shortcut key options in order
    if (has(object, "shortcutKey")) {
        properties.shortcutKey = variantOf(codec, object, "shortcutKey").toString();
        properties.shortcutKeySequence = properties.shortcutKey;
    } else if (has(object, "shortcutKeyEvent")) {
        decodeKeyEvent(codec, object.value(QLatin1String("shortcutKeyEvent")), properties.shortcutKeyEvent);
        properties.shortcutKeySequence = properties.shortcutKeyEvent;
    }
    if (has(object, "position")) {
        properties.position = variantOf(codec, object, "position").toInt();
    }
    properties.beforeItem = variantOf(codec, object, "beforeItem").toString();
    properties.afterItem = variantOf(codec, object, "afterItem").toString();
    properties.grouping = variantOf(codec, object, "grouping").toString();
    return true;
}

// --- audio (libraries/audio: AudioInjectorOptions.cpp, Sound.cpp) -------------------------------------------------------

QCborValue encodeAudioInjectorOptions(const ValueCodec& codec, const AudioInjectorOptions& options) {
    QCborMap map;
    if (options.positionSet) {
        map.insert(QStringLiteral("position"), cborOf(codec, options.position));
    }
    map.insert(QStringLiteral("volume"), double(options.volume));
    map.insert(QStringLiteral("loop"), options.loop);
    map.insert(QStringLiteral("orientation"), cborOf(codec, options.orientation));
    map.insert(QStringLiteral("ignorePenumbra"), options.ignorePenumbra);
    map.insert(QStringLiteral("localOnly"), options.localOnly);
    map.insert(QStringLiteral("secondOffset"), double(options.secondOffset));
    map.insert(QStringLiteral("pitch"), double(options.pitch));
    return map;
}

bool decodeAudioInjectorOptions(const ValueCodec& codec, const QCborValue& value, AudioInjectorOptions& options) {
    if (!value.isMap()) {
        qWarning() << "Audio injector options is not an object.";
        return true;
    }
    if (options.positionSet == false) {
        qWarning() << "Audio injector options: injectorOptionsFromScriptValue() called more than once?";
    }
    options.positionSet = false;
    const QCborMap object = value.toMap();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString name = it.key().toString();
        const QCborValue member = it.value();
        const bool isNumber = member.isInteger() || member.isDouble();
        if (name == "position") {
            options.position = typed<glm::vec3>(codec, member);
            options.positionSet = true;
        } else if (name == "orientation") {
            options.orientation = typed<glm::quat>(codec, member);
        } else if (name == "volume") {
            if (isNumber) {
                options.volume = float(member.toDouble());
            } else {
                qWarning() << "Audio injector options: volume is not a number";
            }
        } else if (name == "loop") {
            if (member.isBool()) {
                options.loop = member.toBool();
            } else {
                qWarning() << "Audio injector options: loop is not a boolean";
            }
        } else if (name == "ignorePenumbra") {
            if (member.isBool()) {
                options.ignorePenumbra = member.toBool();
            } else {
                qWarning() << "Audio injector options: ignorePenumbra is not a boolean";
            }
        } else if (name == "localOnly") {
            if (member.isBool()) {
                options.localOnly = member.toBool();
            } else {
                qWarning() << "Audio injector options: localOnly is not a boolean";
            }
        } else if (name == "secondOffset") {
            if (isNumber) {
                options.secondOffset = float(member.toDouble());
            } else {
                qWarning() << "Audio injector options: secondOffset is not a number";
            }
        } else if (name == "pitch") {
            if (isNumber) {
                options.pitch = float(member.toDouble());
            } else {
                qWarning() << "Audio injector options: pitch is not a number";
            }
        } else {
            qWarning() << "Unknown audio injector option:" << name;
        }
    }
    return true;
}

// An injector the script let go of while its sound plays: deleting it would stop the sound (~ScriptAudioInjector),
// which a script that plays a sound and keeps no reference does not mean (QtScript's collector stopped it at a
// time of its own, usually after the end). It goes at the engine's first sweep after the sound has finished, or
// when the script stops.
bool injectorLingers(QObject* object) {
    auto injector = qobject_cast<ScriptAudioInjector*>(object);
    return injector && injector->isPlaying();
}

// A request the script let go of while it was still under way (sent, its reply not yet come) is kept until it is
// done; one opened and never sent is not.
bool requestLingers(QObject* object) {
    auto request = qobject_cast<HttpRequestObject*>(object);
    return request && request->isSending();
}

QCborValue encodeSound(const ValueCodec& codec, const SharedSoundPointer& sound) {
    // a new wrapper for each crossing, which the script owns (soundSharedPointerToScriptValue)
    return codec.toCborOwned(new SoundScriptingInterface(sound));
}

bool decodeSound(const ValueCodec& codec, const QCborValue& value, SharedSoundPointer& sound) {
    if (auto wrapper = qobject_cast<SoundScriptingInterface*>(codec.fromCbor(value).value<QObject*>())) {
        sound = wrapper->getSound();
    }
    return true;
}

// --- meshes (libraries/shared/src/RegisteredMetaTypes.cpp) ---------------------------------------------------------------

QCborValue encodeMeshFace(const ValueCodec& codec, const MeshFace& face) {
    QCborMap map;
    map.insert(QStringLiteral("vertices"), cborOf(codec, face.vertexIndices));
    return map;
}

bool decodeMeshFace(const ValueCodec& codec, const QCborValue& value, MeshFace& face) {
    face.vertexIndices = typed<QVector<uint32_t>>(codec, value.toMap().value(QLatin1String("vertices")));
    return true;
}

// meshesToScriptValue, meshesFromScriptValue: the meshes as handles (QtOwnership); a null one is left out
QCborValue encodeMeshProxyList(const ValueCodec& codec, const MeshProxyList& meshes) {
    QCborArray array;
    for (MeshProxy* mesh : meshes) {
        array.append(cborOf<QObject*>(codec, mesh));
    }
    return array;
}

bool decodeMeshProxyList(const ValueCodec& codec, const QCborValue& value, MeshProxyList& meshes) {
    for (const QCborValue& item : value.toArray()) {
        MeshProxy* mesh = qobject_cast<MeshProxy*>(codec.fromCbor(item).value<QObject*>());
        if (mesh) {
            meshes.append(mesh);
        } else {
            qDebug() << "null meshProxy";
        }
    }
    return true;
}

// --- the entity types (libraries/entities) -----------------------------------------------------------------------------

QCborValue encodeEntityItemID(const ValueCodec&, const EntityItemID& id) {
    return uuidOf(id);
}

bool decodeEntityItemID(const ValueCodec& codec, const QCborValue& value, EntityItemID& id) {
    bool ok = true;
    id = EntityItemID(codec.fromCbor(value, QMetaType::QUuid, &ok).toUuid());
    return ok;
}

// The entity properties, by the conversion bodies compiled for plain values (EntityItemProperties.h): a module gets
// the map a script gets, member for member and in the same order, and what it sends is read as a script's object is
// (QtScript's pair is EntityItemPropertiesToScriptValue and EntityItemPropertiesFromScriptValueHonorReadOnly)
QCborValue encodeEntityItemProperties(const ValueCodec&, const EntityItemProperties& properties) {
    PlainScriptEngine engine;
    return properties.copyToScriptValue(&engine, false).toCbor();
}

bool decodeEntityItemProperties(const ValueCodec&, const QCborValue& value, EntityItemProperties& properties) {
    if (!value.isMap()) {
        return false;
    }
    properties.copyFromScriptValue(PlainScriptValue::fromCbor(value), true);
    return true;
}

QCborValue encodeEntityPropertyFlags(const ValueCodec&, const EntityPropertyFlags&) {
    return QCborMap();  // EntityItemProperties::entityPropertyFlagsToScriptValue gives an empty object
}

// EntityItemProperties::entityPropertyFlagsFromScriptValue: a property name or a list of them, by its rule
// (addPropertyFlagsByName): a property's name ("position", "grab.grabbable") names one flag, a group's name alone
// ("grab", "vehicle") every property of the group
bool decodeEntityPropertyFlags(const ValueCodec& codec, const QCborValue& value, EntityPropertyFlags& flags) {
    QStringList names;
    if (value.isString()) {
        names.append(value.toString());
    } else if (value.isArray()) {
        for (const QCborValue& item : value.toArray()) {
            names.append(codec.fromCbor(item, QMetaType::QString).toString());
        }
    }
    for (const QString& name : names) {
        EntityPropertyInfo propertyInfo;
        if (EntityItemProperties::getPropertyInfo(name, propertyInfo)) {
            flags << propertyInfo.propertyEnum;
        } else if (!name.contains('.')) {
            for (EntityPropertyList property : EntityItemProperties::getPropertiesOfGroup(name)) {
                flags << property;
            }
        }
    }
    return true;
}

QCborValue encodeEntityPropertyInfo(const ValueCodec&, const EntityPropertyInfo& info) {
    QCborMap map;
    map.insert(QStringLiteral("propertyEnum"), qint64(info.propertyEnum));
    map.insert(QStringLiteral("minimum"), info.minimum.toString());
    map.insert(QStringLiteral("maximum"), info.maximum.toString());
    return map;
}

bool decodeEntityPropertyInfo(const ValueCodec& codec, const QCborValue& value, EntityPropertyInfo& info) {
    const QCborMap object = value.toMap();
    info.propertyEnum = EntityPropertyList(variantOf(codec, object, "propertyEnum").toUInt());
    info.minimum = variantOf(codec, object, "minimum");
    info.maximum = variantOf(codec, object, "maximum");
    return true;
}

QCborValue encodeRayToEntityIntersectionResult(const ValueCodec& codec, const RayToEntityIntersectionResult& result) {
    QCborMap map;
    map.insert(QStringLiteral("intersects"), result.intersects);
    map.insert(QStringLiteral("accurate"), result.accurate);
    map.insert(QStringLiteral("entityID"), uuidOf(result.entityID));
    map.insert(QStringLiteral("distance"), double(result.distance));
    map.insert(QStringLiteral("face"), boxFaceToString(result.face));
    map.insert(QStringLiteral("intersection"), cborOf(codec, result.intersection));
    map.insert(QStringLiteral("surfaceNormal"), cborOf(codec, result.surfaceNormal));
    map.insert(QStringLiteral("extraInfo"), codec.toCbor(result.extraInfo));
    return map;
}

bool decodeRayToEntityIntersectionResult(const ValueCodec& codec, const QCborValue& value, RayToEntityIntersectionResult& result) {
    const QCborMap object = value.toMap();
    result.intersects = variantOf(codec, object, "intersects").toBool();
    result.accurate = variantOf(codec, object, "accurate").toBool();
    result.entityID = typed<QUuid>(codec, object.value(QLatin1String("entityID")));
    result.distance = variantOf(codec, object, "distance").toFloat();
    result.face = boxFaceFromString(variantOf(codec, object, "face").toString());
    if (has(object, "intersection")) {
        result.intersection = typed<glm::vec3>(codec, object.value(QLatin1String("intersection")));
    }
    if (has(object, "surfaceNormal")) {
        result.surfaceNormal = typed<glm::vec3>(codec, object.value(QLatin1String("surfaceNormal")));
    }
    result.extraInfo = variantOf(codec, object, "extraInfo").toMap();
    return true;
}

// --- the avatar types (libraries/avatars/src/AvatarData.cpp) ----------------------------------------------------------------

QCborValue encodeRayToAvatarIntersectionResult(const ValueCodec& codec, const RayToAvatarIntersectionResult& result) {
    QCborMap map;
    map.insert(QStringLiteral("intersects"), result.intersects);
    map.insert(QStringLiteral("avatarID"), uuidOf(result.avatarID));
    map.insert(QStringLiteral("distance"), double(result.distance));
    map.insert(QStringLiteral("face"), boxFaceToString(result.face));
    map.insert(QStringLiteral("intersection"), cborOf(codec, result.intersection));
    map.insert(QStringLiteral("surfaceNormal"), cborOf(codec, result.surfaceNormal));
    map.insert(QStringLiteral("jointIndex"), result.jointIndex);
    map.insert(QStringLiteral("extraInfo"), codec.toCbor(result.extraInfo));
    return map;
}

bool decodeRayToAvatarIntersectionResult(const ValueCodec& codec, const QCborValue& value, RayToAvatarIntersectionResult& result) {
    const QCborMap object = value.toMap();
    result.intersects = variantOf(codec, object, "intersects").toBool();
    result.avatarID = typed<QUuid>(codec, object.value(QLatin1String("avatarID")));
    result.distance = variantOf(codec, object, "distance").toFloat();
    result.face = boxFaceFromString(variantOf(codec, object, "face").toString());
    if (has(object, "intersection")) {
        result.intersection = typed<glm::vec3>(codec, object.value(QLatin1String("intersection")));
    }
    if (has(object, "surfaceNormal")) {
        result.surfaceNormal = typed<glm::vec3>(codec, object.value(QLatin1String("surfaceNormal")));
    }
    result.jointIndex = codec.fromCbor(object.value(QLatin1String("jointIndex")), QMetaType::Int).toInt();
    result.extraInfo = variantOf(codec, object, "extraInfo").toMap();
    return true;
}

// An object with the avatar entities' ids as keys and their properties (the JSON the map holds) as values.
QCborValue encodeAvatarEntityMap(const ValueCodec& codec, const AvatarEntityMap& entities) {
    QCborMap map;
    for (auto it = entities.constBegin(); it != entities.constEnd(); ++it) {
        QJsonValue properties = QJsonDocument::fromJson(it.value()).toVariant().toJsonValue();
        if (!properties.isObject()) {
            qDebug() << "bad AvatarEntityData in AvatarEntityMap" << QString(it.value().toHex());
        }
        map.insert(it.key().toString(), codec.toCbor(QVariant(properties.toVariant().toMap())));
    }
    return map;
}

bool decodeAvatarEntityMap(const ValueCodec& codec, const QCborValue& value, AvatarEntityMap& entities) {
    const QCborMap object = value.toMap();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        QVariant properties = codec.fromCbor(it.value());
        entities[QUuid(it.key().toString())] = properties.toJsonDocument().toJson(QJsonDocument::Compact);
    }
    return true;
}

// AttachmentData crosses with the members its QtScript prototype (AttachmentDataObject) shows a script.
QCborValue encodeAttachmentData(const ValueCodec& codec, const AttachmentData& attachment) {
    QCborMap map;
    map.insert(QStringLiteral("modelURL"), attachment.modelURL.toString());
    map.insert(QStringLiteral("jointName"), attachment.jointName);
    map.insert(QStringLiteral("translation"), cborOf(codec, attachment.translation));
    map.insert(QStringLiteral("rotation"), cborOf(codec, attachment.rotation));
    map.insert(QStringLiteral("scale"), double(attachment.scale));
    map.insert(QStringLiteral("isSoft"), attachment.isSoft);
    return map;
}

bool decodeAttachmentData(const ValueCodec& codec, const QCborValue& value, AttachmentData& attachment) {
    if (!value.isMap()) {
        return false;
    }
    const QCborMap object = value.toMap();
    attachment.modelURL = QUrl(variantOf(codec, object, "modelURL").toString());
    attachment.jointName = variantOf(codec, object, "jointName").toString();
    attachment.translation = typed<glm::vec3>(codec, object.value(QLatin1String("translation")));
    attachment.rotation = typed<glm::quat>(codec, object.value(QLatin1String("rotation")));
    if (has(object, "scale")) {
        attachment.scale = variantOf(codec, object, "scale").toFloat();
    }
    attachment.isSoft = variantOf(codec, object, "isSoft").toBool();
    return true;
}

// --- shared types (libraries/shared/src/RegisteredMetaTypes.cpp) ----------------------------------------------------------

QCborValue encodeCollision(const ValueCodec& codec, const Collision& collision) {
    QCborMap map;
    map.insert(QStringLiteral("type"), int(collision.type));
    map.insert(QStringLiteral("idA"), uuidOf(collision.idA));
    map.insert(QStringLiteral("idB"), uuidOf(collision.idB));
    map.insert(QStringLiteral("penetration"), cborOf(codec, collision.penetration));
    map.insert(QStringLiteral("contactPoint"), cborOf(codec, collision.contactPoint));
    map.insert(QStringLiteral("velocityChange"), cborOf(codec, collision.velocityChange));
    return map;
}

bool decodeNothing(const ValueCodec&, const QCborValue&, Collision&) {
    return true;  // collisionFromScriptValue: not implemented
}

QCborValue encodeAnimationDetails(const ValueCodec&, const AnimationDetails& details) {
    QCborMap map;
    map.insert(QStringLiteral("role"), details.role);
    map.insert(QStringLiteral("url"), details.url.toString());
    map.insert(QStringLiteral("fps"), double(details.fps));
    map.insert(QStringLiteral("priority"), double(details.priority));
    map.insert(QStringLiteral("loop"), details.loop);
    map.insert(QStringLiteral("hold"), details.hold);
    map.insert(QStringLiteral("startAutomatically"), details.startAutomatically);
    map.insert(QStringLiteral("firstFrame"), double(details.firstFrame));
    map.insert(QStringLiteral("lastFrame"), double(details.lastFrame));
    map.insert(QStringLiteral("running"), details.running);
    map.insert(QStringLiteral("currentFrame"), double(details.currentFrame));
    map.insert(QStringLiteral("allowTranslation"), details.allowTranslation);
    return map;
}

bool decodeNothing(const ValueCodec&, const QCborValue&, AnimationDetails&) {
    return true;  // animationDetailsFromScriptValue: "nothing for now"
}

// --- animations (libraries/animation/src/AnimationObject.cpp) -------------------------------------------------------------

QCborValue encodeAnimation(const ValueCodec& codec, const AnimationPointer& animation) {
    // a new wrapper for each crossing, which the script owns, as the QtScript engine's variant was
    return codec.toCborOwned(new AnimationWrapper(animation));
}

bool decodeAnimation(const ValueCodec&, const QCborValue&, AnimationPointer&) {
    return false;  // no method takes an animation from a script
}

// AnimationFrameObject, the frame's prototype: its rotations
QCborValue encodeAnimationFrame(const ValueCodec& codec, const HFMAnimationFrame& frame) {
    QCborMap map;
    map.insert(QStringLiteral("rotations"), cborOf(codec, frame.rotations));
    return map;
}

bool decodeAnimationFrame(const ValueCodec& codec, const QCborValue& value, HFMAnimationFrame& frame) {
    frame.rotations = typed<QVector<glm::quat>>(codec, value.toMap().value(QLatin1String("rotations")));
    return true;
}

// --- the controller types (libraries/controllers: Pose.cpp, UserInputMapper.cpp) ---------------------------------------

QCborValue encodePose(const ValueCodec& codec, const controller::Pose& pose) {
    QCborMap map;
    if (pose.isValid()) {
        map.insert(QStringLiteral("translation"), cborOf(codec, pose.translation));
        map.insert(QStringLiteral("rotation"), cborOf(codec, pose.rotation));
        map.insert(QStringLiteral("velocity"), cborOf(codec, pose.velocity));
        map.insert(QStringLiteral("angularVelocity"), cborOf(codec, pose.angularVelocity));
        map.insert(QStringLiteral("valid"), true);
    } else {
        map.insert(QStringLiteral("valid"), false);
    }
    return map;
}

bool decodePose(const ValueCodec& codec, const QCborValue& value, controller::Pose& pose) {
    const QCborMap object = value.toMap();
    if (has(object, "translation") && has(object, "rotation") && has(object, "velocity") && has(object, "angularVelocity")) {
        pose.translation = typed<glm::vec3>(codec, object.value(QLatin1String("translation")));
        pose.rotation = typed<glm::quat>(codec, object.value(QLatin1String("rotation")));
        pose.velocity = typed<glm::vec3>(codec, object.value(QLatin1String("velocity")));
        pose.angularVelocity = typed<glm::vec3>(codec, object.value(QLatin1String("angularVelocity")));
        pose.valid = true;
    } else {
        pose.valid = false;
    }
    return true;
}

QCborValue encodeInput(const ValueCodec&, const controller::Input& input) {
    QCborMap map;
    map.insert(QStringLiteral("device"), input.getDevice());
    map.insert(QStringLiteral("channel"), input.getChannel());
    map.insert(QStringLiteral("type"), int(input.getType()));
    map.insert(QStringLiteral("id"), qint64(input.getID()));
    return map;
}

bool decodeInput(const ValueCodec& codec, const QCborValue& value, controller::Input& input) {
    input.id = uint32_t(codec.fromCbor(value.toMap().value(QLatin1String("id")), QMetaType::Int).toInt());
    return true;
}

QCborValue encodeAction(const ValueCodec&, const controller::Action& action) {
    QCborMap map;
    map.insert(QStringLiteral("action"), int(action));
    map.insert(QStringLiteral("actionName"), DependencyManager::get<controller::UserInputMapper>()->getActionName(action));
    return map;
}

bool decodeAction(const ValueCodec& codec, const QCborValue& value, controller::Action& action) {
    action = controller::Action(variantOf(codec, value.toMap(), "action").toInt());
    return true;
}

QCborValue encodeInputPair(const ValueCodec& codec, const controller::Input::NamedPair& pair) {
    QCborMap map;
    map.insert(QStringLiteral("input"), encodeInput(codec, pair.first));
    map.insert(QStringLiteral("inputName"), pair.second);
    return map;
}

bool decodeInputPair(const ValueCodec& codec, const QCborValue& value, controller::Input::NamedPair& pair) {
    const QCborMap object = value.toMap();
    decodeInput(codec, object.value(QLatin1String("input")), pair.first);
    pair.second = variantOf(codec, object, "inputName").toString();
    return true;
}

} // namespace

void WasmTypeConversions::registerAll() {
    static std::once_flag once;
    std::call_once(once, [] {
        add<KeyEvent>(&encodeKeyEvent, &decodeKeyEvent);
        add<MouseEvent>(&encodeMouseEvent, &decodeNothing);
        add<TouchEvent>(&encodeTouchEvent, &decodeNothing);
        add<WheelEvent>(&encodeWheelEvent, &decodeNothing);
        add<SpatialEvent>(&encodeSpatialEvent, &decodeNothing);
        add<PointerEvent>(&encodePointerEvent, &decodePointerEvent);
        add<MIDIEvent>(&encodeMIDIEvent, &decodeMIDIEvent);
        add<MenuItemProperties>(&encodeMenuItemProperties, &decodeMenuItemProperties);

        add<AudioInjectorOptions>(&encodeAudioInjectorOptions, &decodeAudioInjectorOptions);
        add<SharedSoundPointer>(&encodeSound, &decodeSound);
        addOwnedObject<ScriptAudioInjector>(&injectorLingers);

        add<MeshFace>(&encodeMeshFace, &decodeMeshFace);
        addSequence<QVector<MeshFace>, MeshFace>(&encodeMeshFace, &decodeMeshFace);
        add<MeshProxyList>(&encodeMeshProxyList, &decodeMeshProxyList);

        add<EntityItemID>(&encodeEntityItemID, &decodeEntityItemID);
        addSequence<QVector<EntityItemID>, EntityItemID>(&encodeEntityItemID, &decodeEntityItemID);
        add<EntityItemProperties>(&encodeEntityItemProperties, &decodeEntityItemProperties);
        add<EntityPropertyFlags>(&encodeEntityPropertyFlags, &decodeEntityPropertyFlags);
        add<EntityPropertyInfo>(&encodeEntityPropertyInfo, &decodeEntityPropertyInfo);
        add<RayToEntityIntersectionResult>(&encodeRayToEntityIntersectionResult, &decodeRayToEntityIntersectionResult);

        add<RayToAvatarIntersectionResult>(&encodeRayToAvatarIntersectionResult, &decodeRayToAvatarIntersectionResult);
        add<AvatarEntityMap>(&encodeAvatarEntityMap, &decodeAvatarEntityMap);
        add<AttachmentData>(&encodeAttachmentData, &decodeAttachmentData);
        addSequence<QVector<AttachmentData>, AttachmentData>(&encodeAttachmentData, &decodeAttachmentData);
        addOwnedObject<ScriptAvatarData>();

        add<Collision>(&encodeCollision, &decodeNothing);
        add<AnimationDetails>(&encodeAnimationDetails, &decodeNothing);
        addOwnedObject<ScriptableResource>();
        // what the engine's network objects are made of (the guest's XMLHttpRequest, WebSocket, WebSocketServer):
        // the script owns them, so one it has let go of is deleted
        addOwnedObject<HttpRequestObject>(&requestLingers);
        addOwnedObject<WebSocketObject>();
        addOwnedObject<WebSocketServerObject>();

        add<AnimationPointer>(&encodeAnimation, &decodeAnimation);
        add<HFMAnimationFrame>(&encodeAnimationFrame, &decodeAnimationFrame);
        addSequence<QVector<HFMAnimationFrame>, HFMAnimationFrame>(&encodeAnimationFrame, &decodeAnimationFrame);

        add<controller::Pose>(&encodePose, &decodePose);
        add<controller::Input>(&encodeInput, &decodeInput);
        add<controller::Action>(&encodeAction, &decodeAction);
        addSequence<QVector<controller::Action>, controller::Action>(&encodeAction, &decodeAction);
        add<controller::Input::NamedPair>(&encodeInputPair, &decodeInputPair);
        addSequence<controller::Input::NamedVector, controller::Input::NamedPair>(&encodeInputPair, &decodeInputPair);
    });
}
