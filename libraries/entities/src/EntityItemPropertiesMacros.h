//
//  EntityItemPropertiesMacros.h
//  libraries/entities/src
//
//  Created by Brad Hefta-Gaub on 9/10/14.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
//
//  Entity properties are converted for two kinds of script value from one source: QtScript's QScriptValue (with a
//  QScriptEngine) and PlainScriptValue (with a PlainScriptEngine), which the WebAssembly script engine and the code
//  outside scripting use. The rule for new property code:
//  - a property goes through these macros, as before;
//  - in a hand-written line of copyToScriptValue or copyFromScriptValue, a value is `auto`, not QScriptValue, and is
//    converted with convertScriptValue(engine, value) or with the RegisteredMetaTypes.h conversion of the same name,
//    which PlainScriptValueConversions.h has for plain values;
//  - a property group's two conversions are member templates over the value and engine types (PropertyGroup.h), and
//    its .cpp ends with INSTANTIATE_PROPERTY_GROUP_CONVERSIONS(TheGroup);
//  - a conversion that only QtScript has does not compile for plain values: add its twin, under the same name, to
//    PlainScriptValueConversions.h. tests/script-values compares the two kinds for every entity type.
//


#ifndef hifi_EntityItemPropertiesMacros_h
#define hifi_EntityItemPropertiesMacros_h

#include "EntityItemID.h"
#include <PlainScriptValue.h>
#include <PlainScriptValueConversions.h>
#include <RegisteredMetaTypes.h>

#define APPEND_ENTITY_PROPERTY(P,V) \
        if (requestedProperties.getHasProperty(P)) {                \
            LevelDetails propertyLevel = packetData->startLevel();  \
            successPropertyFits = packetData->appendValue(V);       \
            if (successPropertyFits) {                              \
                propertyFlags |= P;                                 \
                propertiesDidntFit -= P;                            \
                propertyCount++;                                    \
                packetData->endLevel(propertyLevel);                \
            } else {                                                \
                packetData->discardLevel(propertyLevel);            \
                appendState = OctreeElement::PARTIAL;               \
            }                                                       \
        } else {                                                    \
            propertiesDidntFit -= P;                                \
        }

// The entity readers (readEntityDataFromBuffer, readEntitySubclassDataFromBuffer, with bytesLeftToRead, bytesRead
// and args in scope): a field that claims more bytes than are left is not read; the read is marked failed
// (args.readFailed), every later field of the packet is skipped, and the tree abandons the rest of the packet
// (EntityTree::readEntityDataFromBuffer). No field is ever read in part.
#define READ_ENTITY_PROPERTY(P,T,S)                                                \
        if (propertyFlags.getHasProperty(P) && !args.readFailed) {                 \
            T fromBuffer;                                                          \
            if (!OctreePacketData::fits(dataAt, bytesLeftToRead - bytesRead, fromBuffer)) { \
                args.readFailed = true;                                            \
            } else {                                                               \
                int bytes = OctreePacketData::unpackDataFromBytes(dataAt, fromBuffer); \
                dataAt += bytes;                                                   \
                bytesRead += bytes;                                                \
                if (overwriteLocalData) {                                          \
                    S(fromBuffer);                                                 \
                }                                                                  \
                somethingChanged = true;                                           \
            }                                                                      \
        }

#define SKIP_ENTITY_PROPERTY(P,T)                                                  \
        if (propertyFlags.getHasProperty(P) && !args.readFailed) {                 \
            T fromBuffer;                                                          \
            if (!OctreePacketData::fits(dataAt, bytesLeftToRead - bytesRead, fromBuffer)) { \
                args.readFailed = true;                                            \
            } else {                                                               \
                int bytes = OctreePacketData::unpackDataFromBytes(dataAt, fromBuffer); \
                dataAt += bytes;                                                   \
                bytesRead += bytes;                                                \
            }                                                                      \
        }

// The property groups' edit decoders (decodeFromEditPacket, with bytesToRead the whole edit's length, processedBytes
// the bytes before the group and bytesRead the group's own in scope): a field that claims more bytes than the edit
// has left refuses the edit whole.
#define READ_ENTITY_PROPERTY_FROM_EDIT(P,T,S)                                      \
        if (propertyFlags.getHasProperty(P)) {                                     \
            T fromBuffer;                                                          \
            if (!OctreePacketData::fits(dataAt, bytesToRead - processedBytes - bytesRead, fromBuffer)) { \
                return false;                                                      \
            }                                                                      \
            int bytes = OctreePacketData::unpackDataFromBytes(dataAt, fromBuffer); \
            dataAt += bytes;                                                       \
            bytesRead += bytes;                                                    \
            if (overwriteLocalData) {                                              \
                S(fromBuffer);                                                     \
            }                                                                      \
            somethingChanged = true;                                               \
        }

#define DECODE_GROUP_PROPERTY_HAS_CHANGED(P,N) \
        if (propertyFlags.getHasProperty(P)) {  \
            set##N##Changed(true); \
        }


// The edit decoder (EntityItemProperties::decodeEntityEditPacket, with bytesToRead and processedBytes in scope): a
// field that claims more bytes than the edit has left refuses the edit whole.
#define READ_ENTITY_PROPERTY_TO_PROPERTIES(P,T,O)                                  \
        if (propertyFlags.getHasProperty(P)) {                                     \
            T fromBuffer;                                                          \
            if (!OctreePacketData::fits(dataAt, bytesToRead - processedBytes, fromBuffer)) { \
                return false;                                                      \
            }                                                                      \
            int bytes = OctreePacketData::unpackDataFromBytes(dataAt, fromBuffer); \
            dataAt += bytes;                                                       \
            processedBytes += bytes;                                               \
            properties.O(fromBuffer);                                              \
        }

#define SET_ENTITY_PROPERTY_FROM_PROPERTIES(P,M)    \
    if (properties._##P##Changed) {    \
        M(properties._##P);                         \
        somethingChanged = true;                    \
    }

#define SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(G,P,p,M)  \
    if (properties.get##G().p##Changed()) {                 \
        M(properties.get##G().get##P());                    \
        somethingChanged = true;                            \
    }

#define SET_ENTITY_PROPERTY_FROM_PROPERTIES_GETTER(C,G,S)    \
    if (properties.C()) {    \
        S(properties.G());                         \
        somethingChanged = true;                    \
    }

#define COPY_ENTITY_PROPERTY_TO_PROPERTIES(P,M) \
    properties._##P = M();                      \
    properties._##P##Changed = false;

#define COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(G,P,M)  \
    properties.get##G().set##P(M());                     \
    properties.get##G().set##P##Changed(false);

#define CHECK_PROPERTY_CHANGE(P,M) \
    if (_##M##Changed) {           \
        changedProperties += P;    \
    }

inline QScriptValue convertScriptValue(QScriptEngine* e, const glm::vec2& v) { return vec2ToScriptValue(e, v); }
inline QScriptValue convertScriptValue(QScriptEngine* e, const glm::vec3& v) { return vec3ToScriptValue(e, v); }
inline QScriptValue vec3Color_convertScriptValue(QScriptEngine* e, const glm::vec3& v) { return vec3ColorToScriptValue(e, v); }
inline QScriptValue convertScriptValue(QScriptEngine* e, const glm::u8vec3& v) { return u8vec3ToScriptValue(e, v); }
inline QScriptValue u8vec3Color_convertScriptValue(QScriptEngine* e, const glm::u8vec3& v) { return u8vec3ColorToScriptValue(e, v); }
inline QScriptValue convertScriptValue(QScriptEngine* e, float v) { return QScriptValue(v); }
inline QScriptValue convertScriptValue(QScriptEngine* e, int v) { return QScriptValue(v); }
inline QScriptValue convertScriptValue(QScriptEngine* e, bool v) { return QScriptValue(v); }
inline QScriptValue convertScriptValue(QScriptEngine* e, quint16 v) { return QScriptValue(v); }
inline QScriptValue convertScriptValue(QScriptEngine* e, quint32 v) { return QScriptValue(v); }
inline QScriptValue convertScriptValue(QScriptEngine* e, quint64 v) { return QScriptValue((qsreal)v); }
inline QScriptValue convertScriptValue(QScriptEngine* e, const QString& v) { return QScriptValue(v); }

inline QScriptValue convertScriptValue(QScriptEngine* e, const glm::quat& v) { return quatToScriptValue(e, v); }
inline QScriptValue convertScriptValue(QScriptEngine* e, const QScriptValue& v) { return v; }
inline QScriptValue convertScriptValue(QScriptEngine* e, const QVector<glm::vec3>& v) {return qVectorVec3ToScriptValue(e, v); }
inline QScriptValue qVectorVec3Color_convertScriptValue(QScriptEngine* e, const QVector<glm::vec3>& v) {return qVectorVec3ColorToScriptValue(e, v); }
inline QScriptValue convertScriptValue(QScriptEngine* e, const QVector<glm::quat>& v) {return qVectorQuatToScriptValue(e, v); }
inline QScriptValue convertScriptValue(QScriptEngine* e, const QVector<bool>& v) {return qVectorBoolToScriptValue(e, v); }
inline QScriptValue convertScriptValue(QScriptEngine* e, const QVector<float>& v) { return qVectorFloatToScriptValue(e, v); }

inline QScriptValue convertScriptValue(QScriptEngine* e, const QRect& v) { return qRectToScriptValue(e, v); }

inline QScriptValue convertScriptValue(QScriptEngine* e, const QByteArray& v) {
    QByteArray b64 = v.toBase64();
    return QScriptValue(QString(b64));
}

inline QScriptValue convertScriptValue(QScriptEngine* e, const EntityItemID& v) { return QScriptValue(QUuid(v).toString()); }

inline QScriptValue convertScriptValue(QScriptEngine* e, const AACube& v) { return aaCubeToScriptValue(e, v); }

// The same for plain values (PlainScriptValue.h), member for member what a script gets: the one source of the
// conversions below compiles for QtScript and for plain values (a module, a world file, avatar-entity data).
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, const glm::vec2& v) { return vec2ToScriptValue(e, v); }
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, const glm::vec3& v) { return vec3ToScriptValue(e, v); }
inline PlainScriptValue vec3Color_convertScriptValue(PlainScriptEngine* e, const glm::vec3& v) { return vec3ColorToScriptValue(e, v); }
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, const glm::u8vec3& v) { return u8vec3ToScriptValue(e, v); }
inline PlainScriptValue u8vec3Color_convertScriptValue(PlainScriptEngine* e, const glm::u8vec3& v) { return u8vec3ColorToScriptValue(e, v); }
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, float v) { return PlainScriptValue(v); }
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, int v) { return PlainScriptValue(v); }
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, bool v) { return PlainScriptValue(v); }
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, quint16 v) { return PlainScriptValue(v); }
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, quint32 v) { return PlainScriptValue(v); }
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, quint64 v) { return PlainScriptValue((double)v); }
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, const QString& v) { return PlainScriptValue(v); }

inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, const glm::quat& v) { return quatToScriptValue(e, v); }
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, const PlainScriptValue& v) { return v; }
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, const QVector<glm::vec3>& v) { return qVectorVec3ToScriptValue(e, v); }
inline PlainScriptValue qVectorVec3Color_convertScriptValue(PlainScriptEngine* e, const QVector<glm::vec3>& v) { return qVectorVec3ColorToScriptValue(e, v); }
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, const QVector<glm::quat>& v) { return qVectorQuatToScriptValue(e, v); }
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, const QVector<bool>& v) { return qVectorBoolToScriptValue(e, v); }
inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, const QVector<float>& v) { return qVectorFloatToScriptValue(e, v); }

inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, const QRect& v) { return qRectToScriptValue(e, v); }

inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, const QByteArray& v) { return PlainScriptValue(QString(v.toBase64())); }

inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, const EntityItemID& v) { return PlainScriptValue(QUuid(v).toString()); }

inline PlainScriptValue convertScriptValue(PlainScriptEngine* e, const AACube& v) { return aaCubeToScriptValue(e, v); }

#define COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(X,G,g,P,p) \
    if ((desiredProperties.isEmpty() || desiredProperties.getHasProperty(X)) && \
        (!skipDefaults || defaultEntityProperties.get##G().get##P() != get##P())) { \
        auto groupProperties = properties.property(#g); \
        if (!groupProperties.isValid()) { \
            groupProperties = engine->newObject(); \
        } \
        auto V = convertScriptValue(engine, get##P()); \
        groupProperties.setProperty(#p, V); \
        properties.setProperty(#g, groupProperties); \
    }

#define COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE_TYPED(X,G,g,P,p,T) \
    if ((desiredProperties.isEmpty() || desiredProperties.getHasProperty(X)) && \
        (!skipDefaults || defaultEntityProperties.get##G().get##P() != get##P())) { \
        auto groupProperties = properties.property(#g); \
        if (!groupProperties.isValid()) { \
            groupProperties = engine->newObject(); \
        } \
        auto V = T##_convertScriptValue(engine, get##P()); \
        groupProperties.setProperty(#p, V); \
        properties.setProperty(#g, groupProperties); \
    }

#define COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE_GETTER(X,G,g,P,p,M)                       \
    if ((desiredProperties.isEmpty() || desiredProperties.getHasProperty(X)) &&       \
        (!skipDefaults || defaultEntityProperties.get##G().get##P() != get##P())) {   \
        auto groupProperties = properties.property(#g);                       \
        if (!groupProperties.isValid()) {                                             \
            groupProperties = engine->newObject();                                    \
        }                                                                             \
        auto V = convertScriptValue(engine, M());                             \
        groupProperties.setProperty(#p, V);                                           \
        properties.setProperty(#g, groupProperties);                                  \
    }

#define COPY_PROPERTY_TO_QSCRIPTVALUE(p,P) \
    if (((!psuedoPropertyFlagsButDesiredEmpty && _desiredProperties.isEmpty()) || _desiredProperties.getHasProperty(p)) && \
        (!skipDefaults || defaultEntityProperties._##P != _##P)) { \
        auto V = convertScriptValue(engine, _##P); \
        properties.setProperty(#P, V); \
    }

#define COPY_PROPERTY_TO_QSCRIPTVALUE_TYPED(p,P,T) \
    if ((_desiredProperties.isEmpty() || _desiredProperties.getHasProperty(p)) && \
        (!skipDefaults || defaultEntityProperties._##P != _##P)) { \
        auto V = T##_convertScriptValue(engine, _##P); \
        properties.setProperty(#P, V); \
    }

#define COPY_PROPERTY_TO_QSCRIPTVALUE_GETTER_NO_SKIP(P, G) \
    properties.setProperty(#P, G);

#define COPY_PROPERTY_TO_QSCRIPTVALUE_GETTER(p, P, G) \
    if (((!psuedoPropertyFlagsButDesiredEmpty && _desiredProperties.isEmpty()) || _desiredProperties.getHasProperty(p)) && \
        (!skipDefaults || defaultEntityProperties._##P != _##P)) { \
        auto V = convertScriptValue(engine, G); \
        properties.setProperty(#P, V); \
    }

#define COPY_PROPERTY_TO_QSCRIPTVALUE_GETTER_TYPED(p, P, G, T) \
    if ((_desiredProperties.isEmpty() || _desiredProperties.getHasProperty(p)) && \
        (!skipDefaults || defaultEntityProperties._##P != _##P)) { \
        auto V = T##_convertScriptValue(engine, G); \
        properties.setProperty(#P, V); \
    }

// same as COPY_PROPERTY_TO_QSCRIPTVALUE_GETTER but uses #X instead of #P in the setProperty() step
#define COPY_PROXY_PROPERTY_TO_QSCRIPTVALUE_GETTER(p, P, X, G) \
    if (((!psuedoPropertyFlagsButDesiredEmpty && _desiredProperties.isEmpty()) || _desiredProperties.getHasProperty(p)) && \
        (!skipDefaults || defaultEntityProperties._##P != _##P)) { \
        auto V = convertScriptValue(engine, G); \
        properties.setProperty(#X, V); \
    }

#define COPY_PROPERTY_TO_QSCRIPTVALUE_GETTER_ALWAYS(P, G) \
    if (!skipDefaults || defaultEntityProperties._##P != _##P) { \
        auto V = convertScriptValue(engine, G); \
        properties.setProperty(#P, V); \
    }

typedef QVector<glm::vec3> qVectorVec3;
typedef QVector<glm::quat> qVectorQuat;
typedef QVector<bool> qVectorBool;
typedef QVector<float> qVectorFloat;
// The readers: one body each for a QScriptValue and for a PlainScriptValue, through their shared interface
template <typename Value> inline float float_convertFromScriptValue(const Value& v, bool& isValid) { return v.toVariant().toFloat(&isValid); }
template <typename Value> inline quint64 quint64_convertFromScriptValue(const Value& v, bool& isValid) { return v.toVariant().toULongLong(&isValid); }
template <typename Value> inline quint32 quint32_convertFromScriptValue(const Value& v, bool& isValid) {
    // Use QString::toUInt() so that isValid is set to false if the number is outside the quint32 range.
    return v.toString().toUInt(&isValid);
}
template <typename Value> inline quint16 quint16_convertFromScriptValue(const Value& v, bool& isValid) { return v.toVariant().toInt(&isValid); }
template <typename Value> inline uint16_t uint16_t_convertFromScriptValue(const Value& v, bool& isValid) { return v.toVariant().toInt(&isValid); }
template <typename Value> inline uint32_t uint32_t_convertFromScriptValue(const Value& v, bool& isValid) { return v.toVariant().toInt(&isValid); }
template <typename Value> inline int int_convertFromScriptValue(const Value& v, bool& isValid) { return v.toVariant().toInt(&isValid); }
template <typename Value> inline bool bool_convertFromScriptValue(const Value& v, bool& isValid) { isValid = true; return v.toVariant().toBool(); }
template <typename Value> inline uint8_t uint8_t_convertFromScriptValue(const Value& v, bool& isValid) { isValid = true; return (uint8_t)(0xff & v.toVariant().toInt(&isValid)); }
template <typename Value> inline QString QString_convertFromScriptValue(const Value& v, bool& isValid) { isValid = true; return v.toVariant().toString().trimmed(); }
template <typename Value> inline QUuid QUuid_convertFromScriptValue(const Value& v, bool& isValid) { isValid = true; return v.toVariant().toUuid(); }
template <typename Value> inline EntityItemID EntityItemID_convertFromScriptValue(const Value& v, bool& isValid) { isValid = true; return v.toVariant().toUuid(); }

template <typename Value> inline QByteArray QByteArray_convertFromScriptValue(const Value& v, bool& isValid) {
    isValid = true;
    QString b64 = v.toVariant().toString().trimmed();
    return QByteArray::fromBase64(b64.toUtf8());
}

template <typename Value> inline glm::vec2 vec2_convertFromScriptValue(const Value& v, bool& isValid) {
    isValid = true;
    glm::vec2 vec2;
    vec2FromScriptValue(v, vec2);
    return vec2;
}

template <typename Value> inline glm::vec3 vec3_convertFromScriptValue(const Value& v, bool& isValid) {
    isValid = true;
    glm::vec3 vec3;
    vec3FromScriptValue(v, vec3);
    return vec3;
}

template <typename Value> inline glm::vec3 vec3Color_convertFromScriptValue(const Value& v, bool& isValid) {
    isValid = true;
    glm::vec3 vec3;
    vec3FromScriptValue(v, vec3);
    return vec3;
}

template <typename Value> inline glm::u8vec3 u8vec3Color_convertFromScriptValue(const Value& v, bool& isValid) {
    isValid = true;
    glm::u8vec3 vec3;
    u8vec3FromScriptValue(v, vec3);
    return vec3;
}

template <typename Value> inline AACube AACube_convertFromScriptValue(const Value& v, bool& isValid) {
    isValid = true;
    AACube result;
    aaCubeFromScriptValue(v, result);
    return result;
}

template <typename Value> inline qVectorFloat qVectorFloat_convertFromScriptValue(const Value& v, bool& isValid) {
    isValid = true;
    return qVectorFloatFromScriptValue(v);
}

template <typename Value> inline qVectorVec3 qVectorVec3_convertFromScriptValue(const Value& v, bool& isValid) {
    isValid = true;
    return qVectorVec3FromScriptValue(v);
}

template <typename Value> inline qVectorQuat qVectorQuat_convertFromScriptValue(const Value& v, bool& isValid) {
    isValid = true;
    return qVectorQuatFromScriptValue(v);
}

template <typename Value> inline qVectorBool qVectorBool_convertFromScriptValue(const Value& v, bool& isValid) {
    isValid = true;
    return qVectorBoolFromScriptValue(v);
}

template <typename Value> inline glm::quat quat_convertFromScriptValue(const Value& v, bool& isValid) {
    isValid = false; /// assume it can't be converted
    auto x = v.property("x");
    auto y = v.property("y");
    auto z = v.property("z");
    auto w = v.property("w");
    if (x.isValid() && y.isValid() && z.isValid() && w.isValid()) {
        glm::quat newValue;
        newValue.x = x.toVariant().toFloat();
        newValue.y = y.toVariant().toFloat();
        newValue.z = z.toVariant().toFloat();
        newValue.w = w.toVariant().toFloat();
        isValid = !glm::isnan(newValue.x) &&
                  !glm::isnan(newValue.y) &&
                  !glm::isnan(newValue.z) &&
                  !glm::isnan(newValue.w);
        if (isValid) {
            return newValue;
        }
    }
    return glm::quat();
}

template <typename Value> inline QRect QRect_convertFromScriptValue(const Value& v, bool& isValid) {
    isValid = true;
    QRect rect;
    qRectFromScriptValue(v, rect);
    return rect;
}

#define COPY_PROPERTY_IF_CHANGED(P) \
{                                   \
    if (other._##P##Changed) {      \
        _##P = other._##P;          \
    }                               \
}



#define COPY_PROPERTY_FROM_QSCRIPTVALUE(P, T, S)                     \
    {                                                                \
        auto V = object.property(#P);                        \
        if (V.isValid()) {                                           \
            bool isValid = false;                                    \
            T newValue = T##_convertFromScriptValue(V, isValid);     \
            if (isValid && (_defaultSettings || newValue != _##P)) { \
                S(newValue);                                         \
            }                                                        \
        }                                                            \
    }

#define COPY_PROPERTY_FROM_QSCRIPTVALUE_GETTER(P, T, S, G)      \
{                                                               \
    auto V = object.property(#P);                       \
    if (V.isValid()) {                                          \
        bool isValid = false;                                   \
        T newValue = T##_convertFromScriptValue(V, isValid);    \
        if (isValid && (_defaultSettings || newValue != G())) { \
            S(newValue);                                        \
        }                                                       \
    }                                                           \
}

#define COPY_PROPERTY_FROM_QSCRIPTVALUE_NOCHECK(P, T, S)     \
{                                                            \
    auto V = object.property(#P);                    \
    if (V.isValid()) {                                       \
        bool isValid = false;                                \
        T newValue = T##_convertFromScriptValue(V, isValid); \
        if (isValid && (_defaultSettings)) {                 \
            S(newValue);                                     \
        }                                                    \
    }                                                        \
}

#define COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(G, P, T, S)                \
    {                                                                    \
        auto G = object.property(#G);                            \
        if (G.isValid()) {                                               \
            auto V = G.property(#P);                             \
            if (V.isValid()) {                                           \
                bool isValid = false;                                    \
                T newValue = T##_convertFromScriptValue(V, isValid);     \
                if (isValid && (_defaultSettings || newValue != _##P)) { \
                    S(newValue);                                         \
                }                                                        \
            }                                                            \
        }                                                                \
    }

#define COPY_PROPERTY_FROM_QSCRIPTVALUE_ENUM(P, S)                    \
    {                                                                 \
        auto P = object.property(#P);                         \
        if (P.isValid()) {                                            \
            QString newValue = P.toVariant().toString();              \
            if (_defaultSettings || newValue != get##S##AsString()) { \
                set##S##FromString(newValue);                         \
            }                                                         \
        }                                                             \
    }

#define COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE_ENUM(G, P, S)               \
    {                                                                     \
        auto G = object.property(#G);                             \
        if (G.isValid()) {                                                \
            auto P = G.property(#P);                              \
            if (P.isValid()) {                                            \
                QString newValue = P.toVariant().toString();              \
                if (_defaultSettings || newValue != get##S##AsString()) { \
                    set##S##FromString(newValue);                         \
                }                                                         \
            }                                                             \
        }                                                                 \
    }

#define DEFINE_PROPERTY_GROUP(N, n, T)           \
    public:                                      \
        const T& get##N() const { return _##n; } \
        T& get##N() { return _##n; }             \
    private:                                     \
        T _##n;                                  \
        static T _static##N; 


#define ADD_PROPERTY_TO_MAP(P, N, n, T) \
    { \
        EntityPropertyInfo propertyInfo { makePropertyInfo<T>(P) }; \
        _propertyInfos[#n] = propertyInfo; \
		_enumsToPropertyStrings[P] = #n; \
    }

#define ADD_PROPERTY_TO_MAP_WITH_RANGE(P, N, n, T, M, X) \
    { \
        EntityPropertyInfo propertyInfo = EntityPropertyInfo(P, M, X); \
        _propertyInfos[#n] = propertyInfo; \
		_enumsToPropertyStrings[P] = #n; \
    }

#define ADD_GROUP_PROPERTY_TO_MAP(P, G, g, N, n) \
    { \
        EntityPropertyInfo propertyInfo = EntityPropertyInfo(P); \
        _propertyInfos[#g "." #n] = propertyInfo; \
		_enumsToPropertyStrings[P] = #g "." #n; \
    }

#define ADD_GROUP_PROPERTY_TO_MAP_WITH_RANGE(P, G, g, N, n, M, X) \
    { \
        EntityPropertyInfo propertyInfo = EntityPropertyInfo(P, M, X); \
        _propertyInfos[#g "." #n] = propertyInfo; \
		_enumsToPropertyStrings[P] = #g "." #n; \
    }

#define DEFINE_CORE(N, n, T, V) \
    public: \
        bool n##Changed() const { return _##n##Changed; } \
        void set##N##Changed(bool value) { _##n##Changed = value; } \
    private: \
        T _##n = V; \
        bool _##n##Changed { false };

#define DEFINE_PROPERTY(P, N, n, T, V)        \
    public: \
        T get##N() const { return _##n; } \
        void set##N(T value) { _##n = value; _##n##Changed = true; } \
    DEFINE_CORE(N, n, T, V)

#define DEFINE_PROPERTY_REF(P, N, n, T, V)        \
    public: \
        const T& get##N() const { return _##n; } \
        void set##N(const T& value) { _##n = value; _##n##Changed = true; } \
    DEFINE_CORE(N, n, T, V)

#define DEFINE_PROPERTY_REF_WITH_SETTER(P, N, n, T, V)        \
    public: \
        const T& get##N() const { return _##n; } \
        void set##N(const T& value); \
    DEFINE_CORE(N, n, T, V)

#define DEFINE_PROPERTY_REF_WITH_SETTER_AND_GETTER(P, N, n, T, V)        \
    public: \
        T get##N() const; \
        void set##N(const T& value); \
    DEFINE_CORE(N, n, T, V)

#define DEFINE_PROPERTY_REF_ENUM(P, N, n, T, V) \
    public: \
        const T& get##N() const { return _##n; } \
        void set##N(const T& value) { _##n = value; _##n##Changed = true; } \
        QString get##N##AsString() const; \
        void set##N##FromString(const QString& name); \
    DEFINE_CORE(N, n, T, V)

#define DEBUG_PROPERTY(D, P, N, n, x)                \
    D << "  " << #n << ":" << P.get##N() << x << "[changed:" << P.n##Changed() << "]\n";

#define DEBUG_PROPERTY_IF_CHANGED(D, P, N, n, x)                \
    if (P.n##Changed()) {                                       \
        D << "  " << #n << ":" << P.get##N() << x << "\n";      \
    }

#endif // hifi_EntityItemPropertiesMacros_h
