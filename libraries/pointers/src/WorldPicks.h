//
//  WorldPicks.h
//  libraries/pointers/src
//
//  What a world's script may do with picks and pointers, in one place:
//  - it names only the picks and pointers a world's script made; any other id is, for it, an id that doesn't exist;
//  - an entity it may not read can't be put in a pick's include or ignore list, nor be a pointer's lock end;
//  - a pick result that lands on an entity it may not read reads as no hit, with every field of the hit cleared.
//  Every other caller (the app, the visitor's own scripts) is unaffected. The caller is the calling thread's
//  (ScriptCaller), so these are asked on the thread the script's call arrives on.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once

#include <functional>
#include <unordered_map>

#include <QtCore/QUuid>
#include <QtCore/QVariantMap>
#include <QtCore/QVector>

#include "Pick.h"

namespace WorldPicks {
    // True when a world's caller may read the entity with this id (asked on the caller's thread; an id that isn't an
    // entity may be read). Set once by the app; without it a world's script reads no entity through a pick.
    using ReadCheck = std::function<bool(const QUuid& entityID)>;
    void setReadCheck(ReadCheck check);

    // May the caller name the pick or pointer `uid`? `madeByWorld` is the manager's record of what worlds' scripts
    // made (id -> the world). Call with the manager's lock held.
    bool callerMayName(const std::unordered_map<unsigned int, int>& madeByWorld, unsigned int uid);

    // May the caller read this entity through a pick (true for every caller but a world's script)
    bool callerMayRead(const QUuid& entityID);
    // The ids the caller may read (the others left out)
    QVector<QUuid> readableByCaller(const QVector<QUuid>& ids);

    // The result as a script gets it. For a world's caller, a hit on an entity it may not read is no hit: type none,
    // no id, no distance, point, normal or extra information; what the pick itself was (its ray, parabola, stylus
    // tip or region) stays. In a collision pick's list, such entities are left out.
    QVariantMap resultForCaller(const PickResultPointer& result);
}
