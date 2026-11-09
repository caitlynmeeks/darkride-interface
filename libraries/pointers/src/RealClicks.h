//
//  RealClicks.h
//  libraries/pointers/src
//
//  The entities a real press reached lately: a mouse or controller press that a pick landed on, from the input path
//  (the desktop mouse, the lasers), never a press a script sent into an entity. A world's script may open a page in
//  the visitor's tablet only within a short while after such a press on one of the world's own entities (security
//  finding #54, with editor-ui), and that press is then used up: one page per click.
//
//  The app feeds it (ApplicationServices: EntityScriptingInterface::mousePressOnEntity, skipping emissions made on a
//  world's script thread) and gives it the ownership check (EntityScriptingInterface::worldMayUse, which reads the
//  calling thread's world); the tablet asks it. Thread-safe.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once

#include <chrono>
#include <functional>

#include <QtCore/QUuid>

class RealClicks {
public:
    // true when the entity belongs to the script calling on the current thread (a world's, for this use)
    using OwnershipCheck = std::function<bool(const QUuid& entityID)>;

    // a real press landed on this entity, now
    static void note(const QUuid& entityID);

    // how "the caller's own entity" is decided; set once by the app
    static void setOwnershipCheck(OwnershipCheck check);

    // one noted press younger than maxAge on an entity the calling script owns: used up, true. Without an ownership
    // check set, or with none such press: false
    static bool takeForCaller(std::chrono::milliseconds maxAge);

    // for tests: forget every press
    static void clear();
};
