//
//  WorldPicks.h
//  libraries/pointers/src
//
//  What a world's script may do with picks and pointers, in one place:
//  - it names only the picks and pointers a world's script made; any other id is, for it, an id that doesn't exist;
//  - an entity it may not read can't be put in a pick's include or ignore list, nor be a pointer's lock end;
//  - a pick it made passes through what is hidden from worlds' scripts, as if it weren't there;
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
    // True when a world's caller may read the entity with this id (asked on the caller's thread). Set once by the
    // app, before any script engine starts; without it a world's script reads no entity through a pick.
    // `isEntity`: the id is known to be an entity's (a pick hit one, or a parent was found to be one), so one that
    // can't be found any more is not readable; otherwise an id that is no entity's is left to the pick.
    using ReadCheck = std::function<bool(const QUuid& entityID, bool isEntity)>;
    void setReadCheck(ReadCheck check);

    // May the caller name the pick or pointer `uid`? `madeByWorld` is the manager's record of what worlds' scripts
    // made (id -> the world). Call with the manager's lock held.
    bool callerMayName(const std::unordered_map<unsigned int, int>& madeByWorld, unsigned int uid);

    // May the caller read this entity through a pick (true for every caller but a world's script)
    bool callerMayRead(const QUuid& entityID, bool isEntity = false);
    // The ids the caller may read (the others left out)
    QVector<QUuid> readableByCaller(const QVector<QUuid>& ids);

    // Is this entity hidden from worlds' scripts (the app's and the visitor's own local entities)? Asked on the main
    // thread where picks are evaluated, so by the entity itself, whoever asks. Set once by the app; without it every
    // entity is hidden.
    using HiddenCheck = std::function<bool(const QUuid& entityID)>;
    void setHiddenCheck(HiddenCheck check);
    bool isHiddenFromWorlds(const QUuid& entityID);

    // How many hidden entities a world's pick passes through before it gives up with no hit
    const int MOST_PASSED_THROUGH = 8;

    // The entity a pick made by a world's script hits: what is hidden from worlds' scripts is passed through, as if
    // it weren't there, so the pick's result, the picks parented to it, a pointer's drawn end and its events all see
    // the same thing. `cast(ignore)` casts the pick with these ids ignored and gives a result with `intersects` and
    // `entityID`. Each hidden hit is added to the ignored ids and the pick is cast again; past
    // MOST_PASSED_THROUGH hidden hits the answer is no hit (a default result), never the hidden one.
    template <typename ID, typename Cast>
    auto castPastHidden(QVector<ID> ignore, Cast&& cast) -> decltype(cast(ignore)) {
        for (int passed = 0;; ++passed) {
            auto hit = cast(ignore);
            if (!hit.intersects || !isHiddenFromWorlds(hit.entityID)) {
                return hit;
            }
            if (passed == MOST_PASSED_THROUGH) {
                return decltype(hit)();
            }
            ignore.push_back(ID(hit.entityID));
        }
    }

    // The result as a script gets it. For a world's caller, a hit on an entity it may not read is no hit: type none,
    // no id, no distance, point, normal or extra information; what the pick itself was (its ray, parabola, stylus
    // tip or region) stays. In a collision pick's list, such entities are left out.
    QVariantMap resultForCaller(const PickResultPointer& result);
}
