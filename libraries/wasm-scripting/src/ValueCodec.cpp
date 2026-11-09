//
//  ValueCodec.cpp
//  libraries/wasm-scripting/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ValueCodec.h"

#include <cmath>
#include <cstring>
#include <limits>

#include <QtCore/QCborArray>
#include <QtCore/QCborMap>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QMetaEnum>
#include <QtCore/QMetaObject>
#include <QtCore/QMutex>
#include <QtCore/QPointF>
#include <QtCore/QReadWriteLock>
#include <QtCore/QRectF>
#include <QtCore/QSet>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtCore/QAssociativeIterable>
#include <QtCore/QSequentialIterable>
#endif
#include <QtCore/QSizeF>
#include <QtCore/QUrl>
#include <QtCore/QUuid>
#include <QtGui/QColor>
#include <QtGui/QQuaternion>
#include <QtGui/QVector2D>
#include <QtGui/QVector3D>
#include <QtGui/QVector4D>

#include <RegisteredMetaTypes.h>

#include "ObjectTable.h"

const QString ValueCodec::OBJECT_KEY = QStringLiteral("@object");
const QString ValueCodec::CLASS_KEY = QStringLiteral("@class");
const QString ValueCodec::CALLBACK_KEY = QStringLiteral("@callback");


namespace {

struct RegisteredType {
    ValueCodec::Encoder encoder;
    ValueCodec::Decoder decoder;
};

QReadWriteLock& registryLock() {
    static QReadWriteLock lock;
    return lock;
}

QHash<int, RegisteredType>& registry() {
    static QHash<int, RegisteredType> types;
    return types;
}

int scriptCallbackTypeId() {
    static const int id = qRegisterMetaType<ScriptCallback>();
    return id;
}

// One warning per type the codec could not convert, so a script that leans on one does not flood the log.
void warnOnce(const char* what, int typeId) {
    static QMutex mutex;
    static QSet<QString> warned;
    QString key = QString("%1:%2").arg(what).arg(typeId);
    QMutexLocker lock(&mutex);
    if (!warned.contains(key)) {
        warned.insert(key);
        qWarning() << "ValueCodec:" << what << "for type" << QMetaType::typeName(typeId) << "(" << typeId << ")";
    }
}

QCborMap xyz(double x, double y, double z) {
    QCborMap map;
    map.insert(QStringLiteral("x"), x);
    map.insert(QStringLiteral("y"), y);
    map.insert(QStringLiteral("z"), z);
    return map;
}

QCborMap xyzw(double x, double y, double z, double w) {
    QCborMap map = xyz(x, y, z);
    map.insert(QStringLiteral("w"), w);
    return map;
}

double number(const QCborValue& value, double fallback = 0.0) {
    if (value.isInteger()) {
        return double(value.toInteger());
    }
    if (value.isDouble()) {
        return value.toDouble();
    }
    if (value.isBool()) {
        return value.toBool() ? 1.0 : 0.0;
    }
    if (value.isString()) {
        bool ok = false;
        double d = value.toString().toDouble(&ok);
        return ok ? d : fallback;
    }
    return fallback;
}

// A map member by any of its names ("x", "r", "red"), as the vec3 conversions accept today.
QCborValue member(const QCborMap& map, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        QCborValue value = map.value(QLatin1String(name));
        if (!value.isUndefined()) {
            return value;
        }
    }
    return QCborValue();
}

bool colorFromName(const QString& name, glm::vec3& out) {
    QColor color(name);
    if (!color.isValid()) {
        return false;
    }
    out = glm::vec3(color.red(), color.green(), color.blue());
    return true;
}

// --- the containers the API uses (QVector<float>, QVector<QUuid>, QVector<glm::vec3>...) --------------------
// Encoding needs no table: a registered container converts to a QSequentialIterable. Decoding does, since
// QVariant cannot build a container from a list by itself.

using ContainerDecoder = std::function<QVariant(const ValueCodec& codec, const QCborValue& value, bool& ok)>;

template <typename Container, typename Element>
QVariant decodeContainer(const ValueCodec& codec, const QCborValue& value, bool& ok) {
    Container out;
    if (value.isArray()) {
        const QCborArray array = value.toArray();
        out.reserve(int(array.size()));
        for (const QCborValue& item : array) {
            bool itemOk = true;
            out.append(codec.fromCbor(item, qMetaTypeId<Element>(), &itemOk).template value<Element>());
            ok = ok && itemOk;
        }
    } else if (!value.isNull() && !value.isUndefined()) {
        ok = false;
    }
    return QVariant::fromValue(out);
}

template <typename Element>
void addContainers(QHash<int, ContainerDecoder>& table) {
    // registering also gives QVariant the sequential converter that encoding relies on
    table.insert(qRegisterMetaType<QVector<Element>>(), &decodeContainer<QVector<Element>, Element>);
    table.insert(qRegisterMetaType<QList<Element>>(), &decodeContainer<QList<Element>, Element>);
}

const QHash<int, ContainerDecoder>& containerDecoders() {
    static const QHash<int, ContainerDecoder> table = [] {
        QHash<int, ContainerDecoder> t;
        addContainers<float>(t);
        addContainers<double>(t);
        addContainers<int>(t);
        addContainers<unsigned int>(t);
        addContainers<bool>(t);
        addContainers<QString>(t);
        addContainers<QUuid>(t);
        addContainers<QUrl>(t);
        addContainers<QVariant>(t);
        addContainers<glm::vec2>(t);
        addContainers<glm::vec3>(t);
        addContainers<glm::vec4>(t);
        addContainers<glm::quat>(t);
        return t;
    }();
    return table;
}

} // namespace

void ValueCodec::registerType(int typeId, Encoder encoder, Decoder decoder) {
    QWriteLocker lock(&registryLock());
    registry().insert(typeId, RegisteredType { std::move(encoder), std::move(decoder) });
}

bool ValueCodec::hasRegisteredType(int typeId) {
    QReadLocker lock(&registryLock());
    return registry().contains(typeId);
}

ValueCodec::ValueCodec() {
    scriptCallbackTypeId();
    containerDecoders();
}

ValueCodec::ValueCodec(ObjectTable* objects) : _objects(objects) {
    scriptCallbackTypeId();
    containerDecoders();
}

void ValueCodec::setFallback(EncodeFallback encode, DecodeFallback decode) {
    _encodeFallback = std::move(encode);
    _decodeFallback = std::move(decode);
}

bool ValueCodec::isBuiltinType(int typeId) {
    switch (typeId) {
        case QMetaType::Nullptr:
        case QMetaType::Bool:
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
        case QMetaType::Double:
        case QMetaType::Float:
        case QMetaType::Short:
        case QMetaType::UShort:
        case QMetaType::Long:
        case QMetaType::ULong:
        case QMetaType::Char:
        case QMetaType::UChar:
        case QMetaType::SChar:
        case QMetaType::QChar:
        case QMetaType::QString:
        case QMetaType::QByteArray:
        case QMetaType::QUuid:
        case QMetaType::QUrl:
        case QMetaType::QDateTime:
        case QMetaType::QDate:
        case QMetaType::QTime:
        case QMetaType::QStringList:
        case QMetaType::QVariantList:
        case QMetaType::QVariantMap:
        case QMetaType::QVariantHash:
        case QMetaType::QVariant:
        case QMetaType::QJsonValue:
        case QMetaType::QJsonObject:
        case QMetaType::QJsonArray:
        case QMetaType::QJsonDocument:
        case QMetaType::QColor:
        case QMetaType::QSize:
        case QMetaType::QSizeF:
        case QMetaType::QPoint:
        case QMetaType::QPointF:
        case QMetaType::QRect:
        case QMetaType::QRectF:
        case QMetaType::QVector2D:
        case QMetaType::QVector3D:
        case QMetaType::QVector4D:
        case QMetaType::QQuaternion:
        case QMetaType::QObjectStar:
            return true;
        default:
            break;
    }
    return typeId == qMetaTypeId<glm::vec2>() || typeId == qMetaTypeId<glm::vec3>() ||
           typeId == qMetaTypeId<glm::vec4>() || typeId == qMetaTypeId<glm::quat>() ||
           typeId == qMetaTypeId<glm::u8vec3>() || typeId == qMetaTypeId<glm::mat4>() ||
           typeId == qMetaTypeId<AACube>() || typeId == qMetaTypeId<PickRay>() ||
           typeId == qMetaTypeId<ScriptCallback>() || containerDecoders().contains(typeId) ||
           (QMetaType::typeFlags(typeId) & (QMetaType::PointerToQObject | QMetaType::IsEnumeration));
}

// --- encoding ----------------------------------------------------------------------------------------------

QCborValue ValueCodec::encodeObject(QObject* object, bool owned, std::function<bool(QObject*)> lingers) const {
    if (!object) {
        return QCborValue(nullptr);
    }
    if (!_objects) {
        warnOnce("no object table, cannot encode an object", QMetaType::QObjectStar);
        return QCborValue(QCborValue::Undefined);
    }
    QCborMap map;
    map.insert(OBJECT_KEY, qint64(owned ? _objects->adopt(object, std::move(lingers)) : _objects->handleFor(object)));
    map.insert(CLASS_KEY, QString::fromUtf8(object->metaObject()->className()));
    return map;
}

QCborValue ValueCodec::toCborOwned(QObject* object, std::function<bool(QObject*)> lingers) const {
    return encodeObject(object, true, std::move(lingers));
}

bool ValueCodec::encodeRegistered(const QVariant& value, QCborValue& out) const {
    Encoder encoder;
    {
        QReadLocker lock(&registryLock());
        auto registered = registry().find(value.userType());
        if (registered == registry().end()) {
            return false;
        }
        encoder = registered.value().encoder;
    }
    out = encoder(*this, value);
    return true;
}

bool ValueCodec::decodeRegistered(const QCborValue& value, int targetType, QVariant& out, bool& ok) const {
    Decoder decoder;
    {
        QReadLocker lock(&registryLock());
        auto registered = registry().find(targetType);
        if (registered == registry().end()) {
            return false;
        }
        decoder = registered.value().decoder;
    }
    if (!decoder(*this, value, out)) {
        ok = false;
        out = variantOfType(targetType, nullptr);
    }
    return true;
}

QCborValue ValueCodec::encodeSequence(const QVariant& value) const {
    QCborArray array;
    auto iterable = value.value<QSequentialIterable>();
    for (auto it = iterable.begin(); it != iterable.end(); ++it) {
        array.append(toCbor(*it));
    }
    return array;
}

QCborValue ValueCodec::encodeAssociative(const QVariant& value) const {
    QVariantMap items;
    auto iterable = value.value<QAssociativeIterable>();
    for (auto it = iterable.begin(); it != iterable.end(); ++it) {
        items.insert(it.key().toString(), it.value());
    }
    return encodeVariantMap(items);
}

namespace {

// A QVariantMap is sorted by key, so a colour that came through one reads {blue, green, red}. The shapes
// scripts know by their member order are written in it: {x, y, z}, {x, y, z, w}, {red, green, blue}. Null for
// any other map, written in its own order.
const QStringList* memberOrder(const QVariantMap& items) {
    static const QStringList VEC2 { "x", "y" };
    static const QStringList VEC3 { "x", "y", "z" };
    static const QStringList QUAT { "x", "y", "z", "w" };
    static const QStringList COLOR { "red", "green", "blue" };
    const QStringList* order = nullptr;
    const int count = items.size();
    if (count == 2 && items.contains("x") && items.contains("y")) {
        order = &VEC2;
    } else if (count == 3 && items.contains("x") && items.contains("z")) {
        order = &VEC3;
    } else if (count == 4 && items.contains("w") && items.contains("x")) {
        order = &QUAT;
    } else if (count == 3 && items.contains("red") && items.contains("blue")) {
        order = &COLOR;
    }
    if (order) {
        for (const QString& key : *order) {
            if (!items.contains(key)) {
                return nullptr;
            }
        }
    }
    return order;
}

} // namespace

QCborValue ValueCodec::encodeVariantMap(const QVariantMap& items) const {
    QCborMap map;
    if (const QStringList* order = memberOrder(items)) {
        for (const QString& key : *order) {
            map.insert(key, toCbor(items.value(key)));
        }
        return map;
    }
    for (auto it = items.constBegin(); it != items.constEnd(); ++it) {
        map.insert(it.key(), toCbor(it.value()));
    }
    return map;
}

QCborValue ValueCodec::toCbor(const QVariant& value) const {
    const int type = value.userType();
    // a CBOR item made by the caller (an ordered map, say) crosses as it is
    if (type == QMetaType::QCborValue) {
        return value.value<QCborValue>();
    }
    if (type == QMetaType::QCborMap) {
        return QCborValue(value.value<QCborMap>());
    }
    if (type == QMetaType::QCborArray) {
        return QCborValue(value.value<QCborArray>());
    }
    switch (type) {
        case QMetaType::UnknownType:
            return QCborValue(QCborValue::Undefined);
        case QMetaType::Nullptr:
            return QCborValue(nullptr);
        case QMetaType::Bool:
            return QCborValue(value.toBool());
        case QMetaType::Int:
        case QMetaType::Short:
        case QMetaType::Char:
        case QMetaType::SChar:
        case QMetaType::Long:
        case QMetaType::LongLong:
            return QCborValue(value.toLongLong());
        case QMetaType::UInt:
        case QMetaType::UShort:
        case QMetaType::UChar:
        case QMetaType::ULong:
        case QMetaType::ULongLong: {
            qulonglong unsignedValue = value.toULongLong();
            if (unsignedValue <= qulonglong(std::numeric_limits<qint64>::max())) {
                return QCborValue(qint64(unsignedValue));
            }
            return QCborValue(double(unsignedValue));
        }
        case QMetaType::Double:
        case QMetaType::Float:
            return QCborValue(value.toDouble());
        case QMetaType::QChar:
            return QCborValue(QString(value.toChar()));
        case QMetaType::QString:
            return QCborValue(value.toString());
        case QMetaType::QByteArray:
            return QCborValue(value.toByteArray());
        case QMetaType::QUuid: {
            QUuid uuid = value.toUuid();
            return uuid.isNull() ? QCborValue(nullptr) : QCborValue(uuid.toString());
        }
        case QMetaType::QUrl:
            return QCborValue(value.toUrl().toString());
        case QMetaType::QDateTime:
            return QCborValue(value.toDateTime().toString(Qt::ISODateWithMs));
        case QMetaType::QDate:
            return QCborValue(value.toDate().toString(Qt::ISODate));
        case QMetaType::QTime:
            return QCborValue(value.toTime().toString(Qt::ISODateWithMs));
        case QMetaType::QStringList: {
            QCborArray array;
            for (const QString& item : value.toStringList()) {
                array.append(item);
            }
            return array;
        }
        case QMetaType::QVariantList: {
            QCborArray array;
            for (const QVariant& item : value.toList()) {
                array.append(toCbor(item));
            }
            return array;
        }
        case QMetaType::QVariantMap:
            return encodeVariantMap(value.toMap());
        case QMetaType::QVariantHash: {
            QCborMap map;
            const QVariantHash items = value.toHash();
            for (auto it = items.constBegin(); it != items.constEnd(); ++it) {
                map.insert(it.key(), toCbor(it.value()));
            }
            return map;
        }
        case QMetaType::QVariant:
            return toCbor(value.value<QVariant>());
        case QMetaType::QJsonValue:
            return toCbor(value.toJsonValue().toVariant());
        case QMetaType::QJsonObject:
            return toCbor(QVariant(value.toJsonObject().toVariantMap()));
        case QMetaType::QJsonArray:
            return toCbor(QVariant(value.toJsonArray().toVariantList()));
        case QMetaType::QJsonDocument:
            return toCbor(value.toJsonDocument().toVariant());
        case QMetaType::QColor: {
            QColor color = value.value<QColor>();
            QCborMap map;
            map.insert(QStringLiteral("red"), color.red());
            map.insert(QStringLiteral("green"), color.green());
            map.insert(QStringLiteral("blue"), color.blue());
            map.insert(QStringLiteral("alpha"), color.alpha());
            return map;
        }
        case QMetaType::QSize:
        case QMetaType::QSizeF: {
            QSizeF size = value.toSizeF();
            QCborMap map;
            map.insert(QStringLiteral("width"), size.width());
            map.insert(QStringLiteral("height"), size.height());
            return map;
        }
        case QMetaType::QPoint:
        case QMetaType::QPointF: {
            QPointF point = value.toPointF();
            QCborMap map;
            map.insert(QStringLiteral("x"), point.x());
            map.insert(QStringLiteral("y"), point.y());
            return map;
        }
        case QMetaType::QRect:
        case QMetaType::QRectF: {
            QRectF rect = value.toRectF();
            QCborMap map;
            map.insert(QStringLiteral("x"), rect.x());
            map.insert(QStringLiteral("y"), rect.y());
            map.insert(QStringLiteral("width"), rect.width());
            map.insert(QStringLiteral("height"), rect.height());
            return map;
        }
        case QMetaType::QVector2D: {
            QVector2D v = value.value<QVector2D>();
            QCborMap map;
            map.insert(QStringLiteral("x"), v.x());
            map.insert(QStringLiteral("y"), v.y());
            return map;
        }
        case QMetaType::QVector3D: {
            QVector3D v = value.value<QVector3D>();
            return xyz(v.x(), v.y(), v.z());
        }
        case QMetaType::QVector4D: {
            QVector4D v = value.value<QVector4D>();
            return xyzw(v.x(), v.y(), v.z(), v.w());
        }
        case QMetaType::QQuaternion: {
            QQuaternion q = value.value<QQuaternion>();
            return xyzw(q.x(), q.y(), q.z(), q.scalar());
        }
        case QMetaType::QObjectStar:
            return encodeObject(value.value<QObject*>());
        default:
            break;
    }

    if (type == qMetaTypeId<glm::vec3>()) {
        glm::vec3 v = value.value<glm::vec3>();
        return xyz(v.x, v.y, v.z);
    }
    if (type == qMetaTypeId<glm::quat>()) {
        glm::quat q = value.value<glm::quat>();
        return xyzw(q.x, q.y, q.z, q.w);
    }
    if (type == qMetaTypeId<glm::vec2>()) {
        glm::vec2 v = value.value<glm::vec2>();
        QCborMap map;
        map.insert(QStringLiteral("x"), v.x);
        map.insert(QStringLiteral("y"), v.y);
        return map;
    }
    if (type == qMetaTypeId<glm::vec4>()) {
        glm::vec4 v = value.value<glm::vec4>();
        return xyzw(v.x, v.y, v.z, v.w);
    }
    if (type == qMetaTypeId<glm::u8vec3>()) {
        glm::u8vec3 v = value.value<glm::u8vec3>();
        QCborMap map;
        map.insert(QStringLiteral("red"), v.x);
        map.insert(QStringLiteral("green"), v.y);
        map.insert(QStringLiteral("blue"), v.z);
        return map;
    }
    if (type == qMetaTypeId<glm::mat4>()) {
        return toCbor(mat4ToVariant(value.value<glm::mat4>()));
    }
    if (type == qMetaTypeId<AACube>()) {
        AACube cube = value.value<AACube>();
        QCborMap map = xyz(cube.getCorner().x, cube.getCorner().y, cube.getCorner().z);
        map.insert(QStringLiteral("scale"), cube.getScale());
        return map;
    }
    if (type == qMetaTypeId<PickRay>()) {
        return toCbor(QVariant(value.value<PickRay>().toVariantMap()));
    }
    if (type == qMetaTypeId<ScriptCallback>()) {
        QCborMap map;
        map.insert(CALLBACK_KEY, qint64(value.value<ScriptCallback>().id));
        return map;
    }

    {
        QCborValue registered;
        if (encodeRegistered(value, registered)) {
            return registered;
        }
    }

    const QMetaType::TypeFlags flags = QMetaType::typeFlags(type);
    if (flags & QMetaType::PointerToQObject) {
        return encodeObject(*reinterpret_cast<QObject* const*>(value.constData()));
    }
    if (flags & (QMetaType::TrackingPointerToQObject | QMetaType::SharedPointerToQObject | QMetaType::WeakPointerToQObject)) {
        // a QPointer, QSharedPointer or QWeakPointer to an object (Desktop.createWindow's window, Graphics' models):
        // the object, through the conversion to QObject* Qt registers for these
        return encodeObject(value.value<QObject*>());
    }
    if (flags & QMetaType::IsEnumeration) {
        QVariant number = value;
        if (number.convert(QMetaType::LongLong)) {
            return QCborValue(number.toLongLong());
        }
    }

    // a registered container (QVector<float>, QVector<glm::vec3>, QHash<QString, int>...)
    if (value.canConvert<QVariantList>()) {
        return encodeSequence(value);
    }
    if (value.canConvert<QVariantMap>()) {
        return encodeAssociative(value);
    }

    if (_encodeFallback) {
        QCborValue out;
        if (_encodeFallback(value, out)) {
            return out;
        }
    }
    warnOnce("no encoding", type);
    return QCborValue(QCborValue::Undefined);
}

namespace {

// CBOR as RFC 8949 writes it, the head in its shortest form, as Qt's writer does.
void appendHead(QByteArray& out, uint8_t major, quint64 value) {
    const char type = char(major << 5);
    if (value < 24) {
        out.append(char(type | char(value)));
        return;
    }
    int bytes = value <= 0xff ? 1 : value <= 0xffff ? 2 : value <= 0xffffffffu ? 4 : 8;
    out.append(char(type | char(bytes == 1 ? 24 : bytes == 2 ? 25 : bytes == 4 ? 26 : 27)));
    for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8) {
        out.append(char((value >> shift) & 0xff));
    }
}

void appendInteger(QByteArray& out, qint64 value) {
    if (value >= 0) {
        appendHead(out, 0, quint64(value));
    } else {
        appendHead(out, 1, quint64(-1 - value));
    }
}

void appendDouble(QByteArray& out, double value) {
    quint64 bits;
    memcpy(&bits, &value, sizeof(bits));
    out.append(char(0xfb));
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.append(char((bits >> shift) & 0xff));
    }
}

void appendText(QByteArray& out, const QString& text) {
    QByteArray utf8 = text.toUtf8();
    appendHead(out, 3, quint64(utf8.size()));
    out.append(utf8);
}

// A map of numbers under one-letter keys ("x", "y", "z", "w"), as xyz() and xyzw() build it.
void appendLetters(QByteArray& out, const char* keys, const double* values, int count) {
    appendHead(out, 5, quint64(count));
    for (int i = 0; i < count; ++i) {
        out.append(char(0x61));
        out.append(keys[i]);
        appendDouble(out, values[i]);
    }
}

} // namespace

bool ValueCodec::encodeDirectly(const QVariant& value, QByteArray& out) const {
    // the same cases, in the same order of precedence, as toCbor: the built-in types, then the glm types, all
    // before the registry, so no registered conversion can be skipped here
    const int type = value.userType();
    switch (type) {
        case QMetaType::UnknownType:
            out.append(char(0xf7));
            return true;
        case QMetaType::Nullptr:
            out.append(char(0xf6));
            return true;
        case QMetaType::Bool:
            out.append(char(value.toBool() ? 0xf5 : 0xf4));
            return true;
        case QMetaType::Int:
        case QMetaType::Short:
        case QMetaType::Char:
        case QMetaType::SChar:
        case QMetaType::Long:
        case QMetaType::LongLong:
            appendInteger(out, value.toLongLong());
            return true;
        case QMetaType::UInt:
        case QMetaType::UShort:
        case QMetaType::UChar:
        case QMetaType::ULong:
        case QMetaType::ULongLong: {
            qulonglong unsignedValue = value.toULongLong();
            if (unsignedValue <= qulonglong(std::numeric_limits<qint64>::max())) {
                appendInteger(out, qint64(unsignedValue));
            } else {
                appendDouble(out, double(unsignedValue));
            }
            return true;
        }
        case QMetaType::Double:
        case QMetaType::Float:
            appendDouble(out, value.toDouble());
            return true;
        case QMetaType::QString:
            appendText(out, value.toString());
            return true;
        case QMetaType::QUuid: {
            QUuid uuid = value.toUuid();
            if (uuid.isNull()) {
                out.append(char(0xf6));
            } else {
                appendText(out, uuid.toString());
            }
            return true;
        }
        // Lists and maps item by item, each item directly or, when it isn't one of these types, through the tree.
        // Building a QCborMap costs a lookup of every key as it goes in (it is a flat array), so a map of n keys
        // cost n^2 compares: a host call returning a large map many times a second (the controller dispatcher's
        // timers) spent most of its thread there.
        case QMetaType::QStringList: {
            const QStringList items = value.toStringList();
            appendHead(out, 4, quint64(items.size()));
            for (const QString& item : items) {
                appendText(out, item);
            }
            return true;
        }
        case QMetaType::QVariantList: {
            const QVariantList items = value.toList();
            appendHead(out, 4, quint64(items.size()));
            for (const QVariant& item : items) {
                encodeItem(item, out);
            }
            return true;
        }
        case QMetaType::QVariantMap: {
            const QVariantMap items = value.toMap();
            appendHead(out, 5, quint64(items.size()));
            if (const QStringList* order = memberOrder(items)) {
                for (const QString& key : *order) {
                    appendText(out, key);
                    encodeItem(items.value(key), out);
                }
            } else {
                for (auto it = items.constBegin(); it != items.constEnd(); ++it) {
                    appendText(out, it.key());
                    encodeItem(it.value(), out);
                }
            }
            return true;
        }
        case QMetaType::QVariantHash: {
            const QVariantHash items = value.toHash();
            appendHead(out, 5, quint64(items.size()));
            for (auto it = items.constBegin(); it != items.constEnd(); ++it) {
                appendText(out, it.key());
                encodeItem(it.value(), out);
            }
            return true;
        }
        default:
            break;
    }
    if (type == qMetaTypeId<glm::vec3>()) {
        glm::vec3 v = value.value<glm::vec3>();
        const double values[] = { v.x, v.y, v.z };
        appendLetters(out, "xyz", values, 3);
        return true;
    }
    if (type == qMetaTypeId<glm::quat>()) {
        glm::quat q = value.value<glm::quat>();
        const double values[] = { q.x, q.y, q.z, q.w };
        appendLetters(out, "xyzw", values, 4);
        return true;
    }
    if (type == qMetaTypeId<glm::vec2>()) {
        glm::vec2 v = value.value<glm::vec2>();
        const double values[] = { v.x, v.y };
        appendLetters(out, "xy", values, 2);
        return true;
    }
    if (type == qMetaTypeId<glm::vec4>()) {
        glm::vec4 v = value.value<glm::vec4>();
        const double values[] = { v.x, v.y, v.z, v.w };
        appendLetters(out, "xyzw", values, 4);
        return true;
    }
    return false;
}

void ValueCodec::encodeItem(const QVariant& value, QByteArray& out) const {
    if (!encodeDirectly(value, out)) {
        out.append(toCbor(value).toCbor());
    }
}

QByteArray ValueCodec::encode(const QVariant& value) const {
    QByteArray bytes;
    if (encodeDirectly(value, bytes)) {
        return bytes;
    }
    return toCbor(value).toCbor();
}

// --- decoding ----------------------------------------------------------------------------------------------

QVariant ValueCodec::fromCbor(const QCborValue& value) const {
    switch (value.type()) {
        case QCborValue::Integer: {
            qint64 i = value.toInteger();
            if (i >= std::numeric_limits<int>::min() && i <= std::numeric_limits<int>::max()) {
                return QVariant(int(i));
            }
            return QVariant(qlonglong(i));
        }
        case QCborValue::Double:
            return QVariant(value.toDouble());
        case QCborValue::True:
        case QCborValue::False:
            return QVariant(value.toBool());
        case QCborValue::Null:
            return QVariant::fromValue(nullptr);
        case QCborValue::Undefined:
        case QCborValue::Invalid:
            return QVariant();
        case QCborValue::String:
            return QVariant(value.toString());
        case QCborValue::ByteArray:
            return QVariant(value.toByteArray());
        case QCborValue::Array: {
            QVariantList list;
            const QCborArray array = value.toArray();
            list.reserve(int(array.size()));
            for (const QCborValue& item : array) {
                list.append(fromCbor(item));
            }
            return list;
        }
        case QCborValue::Map: {
            const QCborMap map = value.toMap();
            QCborValue handle = map.value(OBJECT_KEY);
            if (handle.isInteger()) {
                QObject* object = _objects ? _objects->object(ObjectTable::Handle(handle.toInteger())) : nullptr;
                return QVariant::fromValue(object);
            }
            QCborValue callback = map.value(CALLBACK_KEY);
            if (callback.isInteger()) {
                return QVariant::fromValue(ScriptCallback { int32_t(callback.toInteger()), _callbackTarget });
            }
            QVariantMap out;
            for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
                QString key = it.key().isString() ? it.key().toString() : fromCbor(it.key()).toString();
                out.insert(key, fromCbor(it.value()));
            }
            return out;
        }
        case QCborValue::DateTime:
            return QVariant(value.toDateTime());
        case QCborValue::Url:
            return QVariant(value.toUrl());
        case QCborValue::Uuid:
            return QVariant(value.toUuid());
        case QCborValue::Tag:
            return fromCbor(value.taggedValue());
        default:
            return QVariant();
    }
}

int ValueCodec::genericTypeOf(const QCborValue& value) const {
    // fromCbor(value)'s cases, the type of each
    switch (value.type()) {
        case QCborValue::Integer: {
            qint64 i = value.toInteger();
            return (i >= std::numeric_limits<int>::min() && i <= std::numeric_limits<int>::max()) ? int(QMetaType::Int)
                                                                                                    : int(QMetaType::LongLong);
        }
        case QCborValue::Double:
            return QMetaType::Double;
        case QCborValue::True:
        case QCborValue::False:
            return QMetaType::Bool;
        case QCborValue::Null:
            return QMetaType::Nullptr;
        case QCborValue::String:
            return QMetaType::QString;
        case QCborValue::ByteArray:
            return QMetaType::QByteArray;
        case QCborValue::Array:
            return QMetaType::QVariantList;
        case QCborValue::Map: {
            const QCborMap map = value.toMap();
            if (map.value(OBJECT_KEY).isInteger()) {
                return QMetaType::QObjectStar;
            }
            if (map.value(CALLBACK_KEY).isInteger()) {
                return scriptCallbackTypeId();
            }
            return QMetaType::QVariantMap;
        }
        case QCborValue::DateTime:
            return QMetaType::QDateTime;
        case QCborValue::Url:
            return QMetaType::QUrl;
        case QCborValue::Uuid:
            return QMetaType::QUuid;
        case QCborValue::Tag:
            return genericTypeOf(value.taggedValue());
        default:
            return QMetaType::UnknownType;
    }
}

QVariant ValueCodec::fromCbor(const QCborValue& value, int targetType) const {
    return fromCbor(value, targetType, nullptr);
}

QVariant ValueCodec::fromCbor(const QCborValue& value, int targetType, bool* ok) const {
    bool converted = true;
    QVariant result = decodeTyped(value, targetType, converted);
    if (ok) {
        *ok = converted;
    }
    return result;
}

QVariant ValueCodec::decodeNumber(const QCborValue& value, int targetType, bool& ok) const {
    double d = 0.0;
    qint64 i = 0;
    bool integral = false;
    if (value.isInteger()) {
        i = value.toInteger();
        d = double(i);
        integral = true;
    } else if (value.isDouble()) {
        d = value.toDouble();
    } else if (value.isBool()) {
        i = value.toBool() ? 1 : 0;
        d = double(i);
        integral = true;
    } else if (value.isString()) {
        bool parsed = false;
        d = value.toString().toDouble(&parsed);
        if (!parsed) {
            ok = false;
            d = 0.0;
        }
    } else if (value.isNull() || value.isUndefined()) {
        d = 0.0;
    } else {
        ok = false;
    }
    if (!integral) {
        // a fractional or out-of-range double to an integer: what a JavaScript ToInt32 does for the usual range
        i = std::isfinite(d) ? qint64(std::trunc(d)) : 0;
    }
    switch (targetType) {
        case QMetaType::Int:
            return QVariant(int(i));
        case QMetaType::UInt:
            return QVariant(uint(i));
        case QMetaType::LongLong:
            return QVariant(qlonglong(i));
        case QMetaType::ULongLong:
            return QVariant(qulonglong(i));
        case QMetaType::Double:
            return QVariant(d);
        case QMetaType::Float:
            return QVariant(float(d));
        case QMetaType::Short:
            return QVariant::fromValue(short(i));
        case QMetaType::UShort:
            return QVariant::fromValue(ushort(i));
        case QMetaType::Long:
            return QVariant::fromValue(long(i));
        case QMetaType::ULong:
            return QVariant::fromValue(ulong(i));
        case QMetaType::Char:
            return QVariant::fromValue(char(i));
        case QMetaType::UChar:
            return QVariant::fromValue(uchar(i));
        case QMetaType::SChar:
            return QVariant::fromValue(static_cast<signed char>(i));
        default:
            ok = false;
            return QVariant();
    }
}

QVariant ValueCodec::decodeEnum(const QCborValue& value, int targetType, bool& ok) const {
    qint64 number = 0;
    if (value.isInteger() || value.isDouble() || value.isBool()) {
        number = qint64(::number(value));
    } else if (value.isString()) {
        // the key of the enumerator, as "Mode" or "Class::Mode"
        const QMetaObject* meta = QMetaType::metaObjectForType(targetType);
        QByteArray enumName = QMetaType::typeName(targetType);
        int scope = enumName.lastIndexOf("::");
        if (scope >= 0) {
            enumName = enumName.mid(scope + 2);
        }
        int index = meta ? meta->indexOfEnumerator(enumName.constData()) : -1;
        bool found = false;
        if (index >= 0) {
            QByteArray key = value.toString().toUtf8();
            int keyScope = key.lastIndexOf("::");
            if (keyScope >= 0) {
                key = key.mid(keyScope + 2);
            }
            int enumValue = meta->enumerator(index).keyToValue(key.constData(), &found);
            number = enumValue;
        }
        if (!found) {
            ok = false;
            return variantOfType(targetType, nullptr);
        }
    } else {
        ok = false;
        return variantOfType(targetType, nullptr);
    }
    switch (QMetaType::sizeOf(targetType)) {
        case 1: {
            qint8 v = qint8(number);
            return variantOfType(targetType, &v);
        }
        case 2: {
            qint16 v = qint16(number);
            return variantOfType(targetType, &v);
        }
        case 4: {
            qint32 v = qint32(number);
            return variantOfType(targetType, &v);
        }
        case 8: {
            return variantOfType(targetType, &number);
        }
        default:
            ok = false;
            return variantOfType(targetType, nullptr);
    }
}

QVariant ValueCodec::decodeObjectPointer(const QCborValue& value, int targetType, bool& ok) const {
    QObject* object = nullptr;
    if (value.isMap()) {
        QCborValue handle = value.toMap().value(OBJECT_KEY);
        if (handle.isInteger() && _objects) {
            object = _objects->object(ObjectTable::Handle(handle.toInteger()));
        }
        if (!object) {
            ok = false;
        } else {
            const QMetaObject* wanted = QMetaType::metaObjectForType(targetType);
            if (wanted && !object->metaObject()->inherits(wanted)) {
                ok = false;
                object = nullptr;
            }
        }
    } else if (!value.isNull() && !value.isUndefined()) {
        ok = false;
    }
    if (targetType == QMetaType::QObjectStar) {
        return QVariant::fromValue(object);
    }
    return variantOfType(targetType, &object);
}

QVariant ValueCodec::decodeTyped(const QCborValue& value, int targetType, bool& ok) const {
    switch (targetType) {
        case QMetaType::UnknownType:
        case QMetaType::QVariant:
            return fromCbor(value);
        case QMetaType::Bool:
            if (value.isBool()) {
                return QVariant(value.toBool());
            }
            if (value.isInteger() || value.isDouble()) {
                double d = number(value);
                return QVariant(d != 0.0 && !std::isnan(d));
            }
            if (value.isString()) {
                return QVariant(!value.toString().isEmpty());
            }
            if (value.isNull() || value.isUndefined()) {
                return QVariant(false);
            }
            return QVariant(true);
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
        case QMetaType::Double:
        case QMetaType::Float:
        case QMetaType::Short:
        case QMetaType::UShort:
        case QMetaType::Long:
        case QMetaType::ULong:
        case QMetaType::Char:
        case QMetaType::UChar:
        case QMetaType::SChar:
            return decodeNumber(value, targetType, ok);
        case QMetaType::QString:
            if (value.isString()) {
                return QVariant(value.toString());
            }
            if (value.isInteger()) {
                return QVariant(QString::number(value.toInteger()));
            }
            if (value.isDouble()) {
                return QVariant(QVariant(value.toDouble()).toString());
            }
            if (value.isBool()) {
                return QVariant(value.toBool() ? QStringLiteral("true") : QStringLiteral("false"));
            }
            if (value.isNull() || value.isUndefined()) {
                return QVariant(QString());
            }
            if (value.isByteArray()) {
                return QVariant(QString::fromUtf8(value.toByteArray()));
            }
            return QVariant(QString::fromUtf8(QJsonDocument::fromVariant(fromCbor(value)).toJson(QJsonDocument::Compact)));
        case QMetaType::QChar: {
            QString text = decodeTyped(value, QMetaType::QString, ok).toString();
            return QVariant(text.isEmpty() ? QChar() : text.at(0));
        }
        case QMetaType::QByteArray:
            if (value.isByteArray()) {
                return QVariant(value.toByteArray());
            }
            if (value.isString()) {
                return QVariant(value.toString().toUtf8());
            }
            ok = value.isNull() || value.isUndefined();
            return QVariant(QByteArray());
        case QMetaType::QUuid:
            if (value.isString()) {
                return QVariant(QUuid(value.toString()));
            }
            if (value.isUuid()) {
                return QVariant(value.toUuid());
            }
            ok = value.isNull() || value.isUndefined();
            return QVariant(QUuid());
        case QMetaType::QUrl:
            if (value.isUrl()) {
                return QVariant(value.toUrl());
            }
            ok = value.isString();
            return QVariant(QUrl(value.isString() ? value.toString() : QString()));
        case QMetaType::QDateTime:
            if (value.isDateTime()) {
                return QVariant(value.toDateTime());
            }
            if (value.isString()) {
                return QVariant(QDateTime::fromString(value.toString(), Qt::ISODateWithMs));
            }
            if (value.isInteger() || value.isDouble()) {
                return QVariant(QDateTime::fromMSecsSinceEpoch(qint64(number(value))));
            }
            ok = false;
            return QVariant(QDateTime());
        case QMetaType::QStringList: {
            QStringList list;
            if (value.isArray()) {
                for (const QCborValue& item : value.toArray()) {
                    list.append(decodeTyped(item, QMetaType::QString, ok).toString());
                }
            } else if (value.isString()) {
                list.append(value.toString());
            } else {
                ok = value.isNull() || value.isUndefined();
            }
            return QVariant(list);
        }
        case QMetaType::QVariantList:
            if (value.isArray()) {
                return fromCbor(value);
            }
            ok = value.isNull() || value.isUndefined();
            return QVariant(QVariantList());
        case QMetaType::QVariantMap:
            if (value.isMap()) {
                QVariant map = fromCbor(value);
                if (map.userType() == QMetaType::QVariantMap) {
                    return map;
                }
            }
            ok = value.isNull() || value.isUndefined();
            return QVariant(QVariantMap());
        case QMetaType::QVariantHash: {
            QVariant map = decodeTyped(value, QMetaType::QVariantMap, ok);
            QVariantHash hash;
            const QVariantMap items = map.toMap();
            for (auto it = items.constBegin(); it != items.constEnd(); ++it) {
                hash.insert(it.key(), it.value());
            }
            return QVariant(hash);
        }
        case QMetaType::QJsonValue:
            return QVariant(QJsonValue::fromVariant(fromCbor(value)));
        case QMetaType::QJsonObject:
            return QVariant(QJsonObject::fromVariantMap(decodeTyped(value, QMetaType::QVariantMap, ok).toMap()));
        case QMetaType::QJsonArray:
            return QVariant(QJsonArray::fromVariantList(decodeTyped(value, QMetaType::QVariantList, ok).toList()));
        case QMetaType::QJsonDocument:
            return QVariant(QJsonDocument::fromVariant(fromCbor(value)));
        case QMetaType::QColor: {
            QColor color;
            if (value.isMap()) {
                QCborMap map = value.toMap();
                QCborValue alpha = member(map, { "alpha", "a" });
                color.setRgb(int(number(member(map, { "red", "r", "x" }))), int(number(member(map, { "green", "g", "y" }))),
                             int(number(member(map, { "blue", "b", "z" }))), alpha.isUndefined() ? 255 : int(number(alpha)));
            } else if (value.isString()) {
                color.setNamedColor(value.toString());
                ok = color.isValid();
            } else if (value.isInteger() || value.isDouble()) {
                color.setRgb(QRgb(quint32(number(value))));
            } else {
                ok = false;
            }
            return QVariant(color);
        }
        case QMetaType::QSize:
        case QMetaType::QSizeF: {
            QSizeF size;
            if (value.isMap()) {
                QCborMap map = value.toMap();
                size = QSizeF(number(map.value(QStringLiteral("width"))), number(map.value(QStringLiteral("height"))));
            } else {
                ok = false;
            }
            return targetType == QMetaType::QSize ? QVariant(size.toSize()) : QVariant(size);
        }
        case QMetaType::QPoint:
        case QMetaType::QPointF: {
            QPointF point;
            if (value.isMap()) {
                QCborMap map = value.toMap();
                point = QPointF(number(map.value(QStringLiteral("x"))), number(map.value(QStringLiteral("y"))));
            } else {
                ok = false;
            }
            return targetType == QMetaType::QPoint ? QVariant(point.toPoint()) : QVariant(point);
        }
        case QMetaType::QRect:
        case QMetaType::QRectF: {
            QRectF rect;
            if (value.isMap()) {
                QCborMap map = value.toMap();
                rect = QRectF(number(map.value(QStringLiteral("x"))), number(map.value(QStringLiteral("y"))),
                              number(map.value(QStringLiteral("width"))), number(map.value(QStringLiteral("height"))));
            } else {
                ok = false;
            }
            return targetType == QMetaType::QRect ? QVariant(rect.toRect()) : QVariant(rect);
        }
        case QMetaType::QVector2D: {
            glm::vec2 v = decodeTyped(value, qMetaTypeId<glm::vec2>(), ok).value<glm::vec2>();
            return QVariant(QVector2D(v.x, v.y));
        }
        case QMetaType::QVector3D: {
            glm::vec3 v = decodeTyped(value, qMetaTypeId<glm::vec3>(), ok).value<glm::vec3>();
            return QVariant(QVector3D(v.x, v.y, v.z));
        }
        case QMetaType::QVector4D: {
            glm::vec4 v = decodeTyped(value, qMetaTypeId<glm::vec4>(), ok).value<glm::vec4>();
            return QVariant(QVector4D(v.x, v.y, v.z, v.w));
        }
        case QMetaType::QQuaternion: {
            glm::quat q = decodeTyped(value, qMetaTypeId<glm::quat>(), ok).value<glm::quat>();
            return QVariant(QQuaternion(q.w, q.x, q.y, q.z));
        }
        case QMetaType::QObjectStar:
            return decodeObjectPointer(value, targetType, ok);
        default:
            break;
    }

    if (targetType == qMetaTypeId<glm::vec3>()) {
        glm::vec3 v(0.0f);
        if (value.isInteger() || value.isDouble()) {
            v = glm::vec3(float(number(value)));
        } else if (value.isString()) {
            ok = colorFromName(value.toString(), v);
        } else if (value.isArray()) {
            QCborArray array = value.toArray();
            if (array.size() == 3) {
                v = glm::vec3(float(number(array[0])), float(number(array[1])), float(number(array[2])));
            } else {
                ok = false;
            }
        } else if (value.isMap()) {
            QCborMap map = value.toMap();
            v = glm::vec3(float(number(member(map, { "x", "r", "red" }))), float(number(member(map, { "y", "g", "green" }))),
                          float(number(member(map, { "z", "b", "blue" }))));
        } else if (!value.isNull() && !value.isUndefined()) {
            ok = false;  // undefined and null read as zeros, as they did on QtScript
        }
        return QVariant::fromValue(v);
    }
    if (targetType == qMetaTypeId<glm::quat>()) {
        glm::quat q(1.0f, 0.0f, 0.0f, 0.0f);
        if (value.isMap()) {
            QCborMap map = value.toMap();
            q = glm::quat(float(number(map.value(QStringLiteral("w")))), float(number(map.value(QStringLiteral("x")))),
                          float(number(map.value(QStringLiteral("y")))), float(number(map.value(QStringLiteral("z")))));
        } else if (value.isArray() && value.toArray().size() == 4) {
            QCborArray array = value.toArray();
            q = glm::quat(float(number(array[3])), float(number(array[0])), float(number(array[1])), float(number(array[2])));
        } else if (!value.isNull() && !value.isUndefined()) {
            ok = false;  // undefined and null read as the identity, as they did on QtScript
        }
        // enforce a unit quaternion, as the conversion does today
        float length = glm::length(q);
        q = length > std::numeric_limits<float>::epsilon() ? q / length : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        return QVariant::fromValue(q);
    }
    if (targetType == qMetaTypeId<glm::vec2>()) {
        glm::vec2 v(0.0f);
        if (value.isInteger() || value.isDouble()) {
            v = glm::vec2(float(number(value)));
        } else if (value.isArray() && value.toArray().size() == 2) {
            QCborArray array = value.toArray();
            v = glm::vec2(float(number(array[0])), float(number(array[1])));
        } else if (value.isMap()) {
            QCborMap map = value.toMap();
            v = glm::vec2(float(number(member(map, { "x", "u" }))), float(number(member(map, { "y", "v" }))));
        } else if (!value.isNull() && !value.isUndefined()) {
            ok = false;
        }
        return QVariant::fromValue(v);
    }
    if (targetType == qMetaTypeId<glm::vec4>()) {
        glm::vec4 v(0.0f);
        if (value.isMap()) {
            QCborMap map = value.toMap();
            v = glm::vec4(float(number(map.value(QStringLiteral("x")))), float(number(map.value(QStringLiteral("y")))),
                          float(number(map.value(QStringLiteral("z")))), float(number(map.value(QStringLiteral("w")))));
        } else if (value.isArray() && value.toArray().size() == 4) {
            QCborArray array = value.toArray();
            v = glm::vec4(float(number(array[0])), float(number(array[1])), float(number(array[2])), float(number(array[3])));
        } else {
            ok = false;
        }
        return QVariant::fromValue(v);
    }
    if (targetType == qMetaTypeId<glm::u8vec3>()) {
        glm::vec3 v(0.0f);
        bool converted = true;
        v = decodeTyped(value, qMetaTypeId<glm::vec3>(), converted).value<glm::vec3>();
        ok = ok && converted;
        return QVariant::fromValue(glm::u8vec3(uint8_t(v.x), uint8_t(v.y), uint8_t(v.z)));
    }
    if (targetType == qMetaTypeId<glm::mat4>()) {
        glm::mat4 m(1.0f);
        if (value.isMap()) {
            bool valid = false;
            m = mat4FromVariant(fromCbor(value), valid);
            ok = ok && valid;
        } else if (value.isArray() && value.toArray().size() == 16) {
            QCborArray array = value.toArray();
            for (int i = 0; i < 16; ++i) {
                m[i / 4][i % 4] = float(number(array[i]));
            }
        } else {
            ok = false;
        }
        return QVariant::fromValue(m);
    }
    if (targetType == qMetaTypeId<AACube>()) {
        AACube cube;
        if (value.isMap()) {
            QCborMap map = value.toMap();
            cube.setBox(glm::vec3(float(number(map.value(QStringLiteral("x")))), float(number(map.value(QStringLiteral("y")))),
                                  float(number(map.value(QStringLiteral("z"))))),
                        float(number(map.value(QStringLiteral("scale")))));
        } else {
            ok = false;
        }
        return QVariant::fromValue(cube);
    }
    if (targetType == qMetaTypeId<PickRay>()) {
        if (value.isMap()) {
            return QVariant::fromValue(PickRay(fromCbor(value).toMap()));
        }
        ok = false;
        return QVariant::fromValue(PickRay());
    }
    if (targetType == qMetaTypeId<ScriptCallback>()) {
        ScriptCallback callback;
        callback.target = _callbackTarget;
        if (value.isMap()) {
            QCborValue id = value.toMap().value(CALLBACK_KEY);
            if (id.isInteger()) {
                callback.id = int32_t(id.toInteger());
            } else {
                ok = false;
            }
        } else if (!value.isNull() && !value.isUndefined()) {
            ok = false;
        }
        return QVariant::fromValue(callback);
    }

    {
        const auto& containers = containerDecoders();
        auto container = containers.find(targetType);
        if (container != containers.end()) {
            return container.value()(*this, value, ok);
        }
    }

    {
        QVariant registered;
        if (decodeRegistered(value, targetType, registered, ok)) {
            return registered;
        }
    }

    const QMetaType::TypeFlags flags = QMetaType::typeFlags(targetType);
    if (flags & QMetaType::PointerToQObject) {
        return decodeObjectPointer(value, targetType, ok);
    }
    if (flags & QMetaType::IsEnumeration) {
        return decodeEnum(value, targetType, ok);
    }

    // the conversions QVariant itself knows (QMetaType::registerConverter)
    QVariant plain = fromCbor(value);
    if (plain.canConvert(targetType)) {
        QVariant converted = plain;
        if (converted.convert(targetType)) {
            return converted;
        }
    }

    if (_decodeFallback) {
        QVariant out;
        if (_decodeFallback(value, targetType, out)) {
            return out;
        }
    }
    warnOnce("no decoding", targetType);
    ok = false;
    return QVariant();
}

bool ValueCodec::decode(const QByteArray& bytes, QVariant& out, QString& error) const {
    return decode(bytes, QMetaType::UnknownType, out, error);
}

bool ValueCodec::decode(const QByteArray& bytes, int targetType, QVariant& out, QString& error) const {
    QCborParserError parseError;
    QCborValue value = QCborValue::fromCbor(bytes, &parseError);
    if (parseError.error != QCborError::NoError) {
        error = QString("malformed CBOR at byte %1: %2").arg(parseError.offset).arg(parseError.errorString());
        return false;
    }
    out = fromCbor(value, targetType);
    return true;
}

bool ValueCodec::decodeArguments(const QByteArray& bytes, const QList<int>& types, QVariantList& out, QString& error) const {
    QCborParserError parseError;
    QCborValue value = QCborValue::fromCbor(bytes, &parseError);
    if (parseError.error != QCborError::NoError) {
        error = QString("malformed CBOR at byte %1: %2").arg(parseError.offset).arg(parseError.errorString());
        return false;
    }
    if (!value.isArray()) {
        error = "arguments must be a CBOR array";
        return false;
    }
    const QCborArray array = value.toArray();
    out.clear();
    out.reserve(qMax(types.size(), int(array.size())));
    for (int i = 0; i < types.size(); ++i) {
        out.append(i < array.size() ? fromCbor(array.at(i), types[i]) : QVariant());
    }
    for (qsizetype i = types.size(); i < array.size(); ++i) {
        out.append(fromCbor(array.at(i)));
    }
    return true;
}
