//
//  FBX.h
//  libraries/fbx/src
//
//  Created by Ryan Huffman on 9/5/17.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_FBX_h_
#define hifi_FBX_h_

#include <type_traits>
#include <variant>

#include <QDebug>
#include <QMetaType>
#include <QVariant>
#include <QVector>

#include <glm/glm.hpp>

#include <shared/HifiTypes.h>

// See comment in FBXSerializer::parseFBX().
static const int FBX_HEADER_BYTES_BEFORE_VERSION = 23;
static const hifi::ByteArray FBX_BINARY_PROLOG("Kaydara FBX Binary  ");
static const hifi::ByteArray FBX_BINARY_PROLOG2("\0\x1a\0", 3);
static const quint32 FBX_VERSION_2015 = 7400;
static const quint32 FBX_VERSION_2016 = 7500;

static const int32_t FBX_PROPERTY_UNCOMPRESSED_FLAG = 0;
static const int32_t FBX_PROPERTY_COMPRESSED_FLAG = 1;

// The version of the FBX node containing the draco mesh. See also: DRACO_MESH_VERSION in HFM.h
static const int FBX_DRACO_MESH_VERSION = 2;

class FBXNode;
using FBXNodeList = QList<FBXNode>;

template <class T> struct FBXIsArray : std::false_type {};
template <> struct FBXIsArray<QVector<float>> : std::true_type {};
template <> struct FBXIsArray<QVector<double>> : std::true_type {};
template <> struct FBXIsArray<QVector<qint64>> : std::true_type {};
template <> struct FBXIsArray<QVector<qint32>> : std::true_type {};
template <> struct FBXIsArray<QVector<bool>> : std::true_type {};

/// A property of an FBX node: a number, a string or bytes, held in a QVariant of one of Qt's own types, or an array of
/// numbers, held as its typed QVector. An array is kept out of QVariant because a QVariant of a custom type such as
/// QVector<float> takes Qt 5's global metatype registry lock on every copy and destroy, and loader threads parsing
/// FBX at once (an avatar's animations on arrival) queued on that lock and held up the main thread with them. The
/// reads mean what QVariant's do: an array reads only as its own type, and as nothing (0, "", false) otherwise.
class FBXProperty {
public:
    FBXProperty() {}
    FBXProperty(const QVariant& value);  // an array in a QVariant is taken out of it
    FBXProperty(bool value) : _value(value) {}
    FBXProperty(int value) : _value(value) {}
    FBXProperty(qint64 value) : _value(value) {}
    FBXProperty(float value) : _value(value) {}
    FBXProperty(double value) : _value(value) {}
    FBXProperty(const QString& value) : _value(value) {}
    FBXProperty(const QByteArray& value) : _value(value) {}
    FBXProperty(const char* value) : _value(value) {}
    FBXProperty(QVector<float> values) : _array(std::move(values)) {}
    FBXProperty(QVector<double> values) : _array(std::move(values)) {}
    FBXProperty(QVector<qint64> values) : _array(std::move(values)) {}
    FBXProperty(QVector<qint32> values) : _array(std::move(values)) {}
    FBXProperty(QVector<bool> values) : _array(std::move(values)) {}

    bool isArray() const { return _array.index() != 0; }
    /// the array of T (float, double, qint64, qint32 or bool), or null if this isn't one
    template <class T> const QVector<T>* array() const { return std::get_if<QVector<T>>(&_array); }
    /// a number, string or bytes as a QVariant of Qt's own type; an invalid QVariant for an array
    const QVariant& scalar() const { return _value; }
    /// the scalar's QMetaType, or QMetaType::UnknownType for an array
    int userType() const { return isArray() ? QMetaType::UnknownType : _value.userType(); }

    QString toString() const { return isArray() ? QString() : _value.toString(); }
    QByteArray toByteArray() const { return isArray() ? QByteArray() : _value.toByteArray(); }
    bool toBool() const { return isArray() ? false : _value.toBool(); }
    int toInt(bool* ok = nullptr) const { return isArray() ? notScalar<int>(ok) : _value.toInt(ok); }
    uint toUInt(bool* ok = nullptr) const { return isArray() ? notScalar<uint>(ok) : _value.toUInt(ok); }
    qlonglong toLongLong(bool* ok = nullptr) const {
        return isArray() ? notScalar<qlonglong>(ok) : _value.toLongLong(ok);
    }
    float toFloat(bool* ok = nullptr) const { return isArray() ? notScalar<float>(ok) : _value.toFloat(ok); }
    double toDouble(bool* ok = nullptr) const { return isArray() ? notScalar<double>(ok) : _value.toDouble(ok); }

    template <class T> T value() const {
        if constexpr (FBXIsArray<T>::value) {
            const T* values = std::get_if<T>(&_array);
            return values ? *values : T();  // one array type never converts to another, nor a scalar to an array
        } else {
            return isArray() ? T() : _value.value<T>();
        }
    }

private:
    template <class T> static T notScalar(bool* ok) {
        if (ok) {
            *ok = false;
        }
        return T();
    }

    QVariant _value;
    using Array = std::variant<std::monostate, QVector<float>, QVector<double>, QVector<qint64>, QVector<qint32>,
                               QVector<bool>>;
    Array _array;
};
QDebug operator<<(QDebug debug, const FBXProperty& property);

// compared as QVariants compare: an array equals no scalar
inline bool operator==(const FBXProperty& property, const QVariant& value) {
    return !property.isArray() && property.scalar() == value;
}
inline bool operator!=(const FBXProperty& property, const QVariant& value) {
    return !(property == value);
}
inline bool operator==(const FBXProperty& property, const char* value) {
    return property == QVariant(value);
}
inline bool operator!=(const FBXProperty& property, const char* value) {
    return !(property == value);
}

using FBXPropertyList = QVector<FBXProperty>;

/// A node within an FBX document.
class FBXNode {
public:
    hifi::ByteArray name;
    FBXPropertyList properties;
    FBXNodeList children;
};

#endif // hifi_FBX_h_
