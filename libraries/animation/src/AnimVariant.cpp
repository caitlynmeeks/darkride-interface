//
//  AnimVariant.cpp
//  library/animation
//
//  Created by Howard Stearns on 10/15/15.
//  Copyright (c) 2015 High Fidelity, Inc. All rights reserved.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "AnimVariant.h" // which has AnimVariant/AnimVariantMap

#include <QThread>
#include <RegisteredMetaTypes.h>

const AnimVariant AnimVariant::False = AnimVariant();

QVariantMap AnimVariantMap::animVariantMapToVariantMap(const QStringList& names, bool useNames) const {
    QVariantMap target;
    auto setOne = [&](const QString& name, const AnimVariant& value) {
        switch (value.getType()) {
            case AnimVariant::Type::Bool:
                target[name] = value.getBool();
                break;
            case AnimVariant::Type::Int:
                target[name] = value.getInt();
                break;
            case AnimVariant::Type::Float:
                target[name] = value.getFloat();
                break;
            case AnimVariant::Type::String:
                target[name] = value.getString();
                break;
            case AnimVariant::Type::Vec3:
                target[name] = vec3toVariant(value.getVec3());
                break;
            case AnimVariant::Type::Quat:
                target[name] = quatToVariant(value.getQuat());
                break;
            default:
                assert(QString("AnimVariant::Type") == QString("valid"));
        }
    };
    if (useNames) {  // only the requested names
        for (const QString& name : names) {
            auto search = _map.find(name);
            if (search != _map.end()) {
                setOne(name, search->second);
            } else if (_triggers.count(name) == 1) {
                target[name] = true;
            }
        }
    } else {
        for (auto& pair : _map) {
            setOne(pair.first, pair.second);
        }
    }
    return target;
}

void AnimVariantMap::animVariantMapFromVariantMap(const QVariantMap& source) {
    for (auto it = source.constBegin(); it != source.constEnd(); ++it) {
        const QVariant& value = it.value();
        switch (value.type()) {
            case QVariant::Bool:
                set(it.key(), value.toBool());
                break;
            case QVariant::String:
                set(it.key(), value.toString());
                break;
            case QVariant::Int:
            case QVariant::UInt:
            case QVariant::LongLong:
            case QVariant::ULongLong:
                set(it.key(), value.toInt());
                break;
            case QVariant::Double: {
                float asFloat = value.toFloat();
                int asInteger = int(asFloat);
                if (asInteger == asFloat) {
                    set(it.key(), asInteger);
                } else {
                    set(it.key(), asFloat);
                }
                break;
            }
            case QVariant::Map: {
                QVariantMap map = value.toMap();
                if (map.contains("x") && map.contains("y") && map.contains("z")) {
                    if (map.contains("w")) {
                        set(it.key(), glm::quat(map["w"].toFloat(), map["x"].toFloat(), map["y"].toFloat(), map["z"].toFloat()));
                    } else {
                        set(it.key(), glm::vec3(map["x"].toFloat(), map["y"].toFloat(), map["z"].toFloat()));
                    }
                    break;
                }
                qCWarning(animation) << "Ignoring unrecognized data" << value << "for animation property" << it.key();
                break;
            }
            default:
                qCWarning(animation) << "Ignoring unrecognized data" << value << "for animation property" << it.key();
                break;
        }
    }
}

void AnimVariantMap::copyVariantsFrom(const AnimVariantMap& other) {
    for (auto& pair : other._map) {
        _map[pair.first] = pair.second;
    }
}

std::map<QString, QString> AnimVariantMap::toDebugMap() const {
    std::map<QString, QString> result;
    for (auto& pair : _map) {
        switch (pair.second.getType()) {
        case AnimVariant::Type::Bool:
            result[pair.first] = QString("%1").arg(pair.second.getBool());
            break;
        case AnimVariant::Type::Int:
            result[pair.first] = QString("%1").arg(pair.second.getInt());
            break;
        case AnimVariant::Type::Float:
            result[pair.first] = QString::number(pair.second.getFloat(), 'f', 3);
            break;
        case AnimVariant::Type::Vec3: {
            // To prevent filling up debug stats, don't show vec3 values
            glm::vec3 value = pair.second.getVec3();
            result[pair.first] = QString("(%1, %2, %3)").
                arg(QString::number(value.x, 'f', 3)).
                arg(QString::number(value.y, 'f', 3)).
                arg(QString::number(value.z, 'f', 3));
            break;
        }
        case AnimVariant::Type::Quat: {
            // To prevent filling up the anim stats, don't show quat values
            glm::quat value = pair.second.getQuat();
            result[pair.first] = QString("(%1, %2, %3, %4)").
                arg(QString::number(value.x, 'f', 3)).
                arg(QString::number(value.y, 'f', 3)).
                arg(QString::number(value.z, 'f', 3)).
                arg(QString::number(value.w, 'f', 3));
            break;
        }
        case AnimVariant::Type::String:
            // To prevent filling up anim stats, don't show string values
            result[pair.first] = pair.second.getString();
            break;
        default:
            // invalid AnimVariant::Type
            assert(false);
        }
    }
    return result;
}
