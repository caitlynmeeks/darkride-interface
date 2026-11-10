//
//  WorldPicks.cpp
//  libraries/pointers/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "WorldPicks.h"

#include <cfloat>
#include <cmath>

#include <QtCore/QSet>

#include <RegisteredMetaTypes.h>
#include <ScriptCaller.h>

namespace {
    WorldPicks::ReadCheck& readCheck() {
        static WorldPicks::ReadCheck check;
        return check;
    }

    bool isEntityHit(const QVariant& type) {
        const int t = type.toInt();
        return t == ENTITY || t == LOCAL_ENTITY;
    }

    // what the pick itself was: the caller's own input, kept when a hit is cleared
    const QSet<QString>& queryKeys() {
        static const QSet<QString> keys { "searchRay", "parabola", "stylusTip", "collisionRegion" };
        return keys;
    }

    // The map of a result with no hit, keeping the keys the result's kind has
    QVariantMap withoutHit(const QVariantMap& hit) {
        QVariantMap none;
        for (auto it = hit.constBegin(); it != hit.constEnd(); ++it) {
            const QString& key = it.key();
            if (queryKeys().contains(key)) {
                none[key] = it.value();
            } else if (key == "type") {
                none[key] = NONE;
            } else if (key == "intersects") {
                none[key] = false;
            } else if (key == "objectID") {
                none[key] = QUuid();
            } else if (key == "distance" || key == "parabolicDistance") {
                none[key] = FLT_MAX;
            } else if (key == "intersection" || key == "surfaceNormal") {
                none[key] = vec3toVariant(glm::vec3(NAN));
            } else if (key == "extraInfo") {
                none[key] = QVariantMap();
            }
            // any other field of a hit is left out
        }
        return none;
    }
}

void WorldPicks::setReadCheck(ReadCheck check) {
    readCheck() = std::move(check);
}

bool WorldPicks::callerMayName(const std::unordered_map<unsigned int, int>& madeByWorld, unsigned int uid) {
    const int caller = ScriptCaller::world();
    if (caller == 0) {
        return true;
    }
    if (ScriptCaller::isRetired()) {
        return false;
    }
    auto tag = madeByWorld.find(uid);
    return tag != madeByWorld.end() &&
           (caller == ScriptCaller::ANY_WORLD || ScriptCaller::belongsTo(tag->second, caller));
}

bool WorldPicks::callerMayRead(const QUuid& entityID, bool isEntity) {
    if (!ScriptCaller::isWorld()) {
        return true;
    }
    const auto& check = readCheck();
    return check && check(entityID, isEntity);
}

QVector<QUuid> WorldPicks::readableByCaller(const QVector<QUuid>& ids) {
    if (!ScriptCaller::isWorld()) {
        return ids;
    }
    QVector<QUuid> readable;
    readable.reserve(ids.size());
    for (const QUuid& id : ids) {
        if (callerMayRead(id)) {
            readable.push_back(id);
        }
    }
    return readable;
}

QVariantMap WorldPicks::resultForCaller(const PickResultPointer& result) {
    if (!result) {
        return QVariantMap();
    }
    QVariantMap map = result->toVariantMap();
    if (!ScriptCaller::isWorld()) {
        return map;
    }
    if (map.contains("intersectingObjects")) {
        // a collision pick: a list of what it touches
        QVariantList kept;
        const QVariantList all = map.value("intersectingObjects").toList();
        for (const QVariant& entry : all) {
            const QVariantMap object = entry.toMap();
            if (isEntityHit(object.value("type")) && !callerMayRead(object.value("id").toUuid(), true)) {
                continue;
            }
            kept.append(entry);
        }
        if (kept.size() != all.size()) {
            map["intersectingObjects"] = kept;
            map["intersects"] = !kept.isEmpty();
        }
        return map;
    }
    if (isEntityHit(map.value("type")) && !callerMayRead(map.value("objectID").toUuid(), true)) {
        return withoutHit(map);
    }
    return map;
}
