//
//  ScriptCaller.cpp
//  libraries/shared/src
//
//  Created by scripting on 2026-11-01.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ScriptCaller.h"

#include <atomic>

#include <QtCore/QMutex>
#include <QtCore/QSet>

#include "SharedLogging.h"

namespace {
thread_local int callerWorld = 0;  // a WorldScope's
thread_local int threadWorld = 0;  // markThreadAsWorld's
QMutex retiredLock;
QSet<int> retiredWorlds;                // under retiredLock
std::atomic<int> highestRetired { 0 };  // no world above it was left: a newer world's call never takes the lock
thread_local int refusalsLoggedFor = 0;        // the world whose refusals this thread has logged
thread_local QSet<QByteArray> refusalsLogged;  // its refused members, each logged once
}

int ScriptCaller::world() {
    return callerWorld != 0 ? callerWorld : threadWorld;
}

bool ScriptCaller::isWorld() {
    return world() != 0;
}

bool ScriptCaller::belongsTo(int tag, int world) {
    return tag != 0 && (tag == world || tag == ANY_WORLD);
}

void ScriptCaller::retireWorld(int world) {
    if (world <= 0) {
        return;
    }
    QMutexLocker locker(&retiredLock);
    retiredWorlds.insert(world);
    if (world > highestRetired.load()) {
        highestRetired.store(world);
    }
}

bool ScriptCaller::isRetired(int world) {
    if (world <= 0 || world > highestRetired.load()) {
        return false;
    }
    QMutexLocker locker(&retiredLock);
    return retiredWorlds.contains(world);
}

bool ScriptCaller::isRetired() {
    return isRetired(world());
}

void ScriptCaller::logRetiredRefusal(const char* object, const char* member) {
    const int caller = world();
    if (refusalsLoggedFor != caller) {
        refusalsLoggedFor = caller;
        refusalsLogged.clear();
    }
    const QByteArray key = QByteArray(object) + "." + member;
    if (!refusalsLogged.contains(key)) {
        refusalsLogged.insert(key);
        qCWarning(shared).nospace() << key.constData() << ": a script of a world that was left (" << caller
                                    << ") may do nothing more; refused (shown once for the world)";
    }
}

void ScriptCaller::markThreadAsWorld(int world) {
    threadWorld = world != 0 ? world : ANY_WORLD;
}

ScriptCaller::WorldScope::WorldScope(bool world) : _outer(callerWorld) {
    if (world && callerWorld == 0) {
        callerWorld = threadWorld != 0 ? threadWorld : ANY_WORLD;
    }
}

ScriptCaller::WorldScope::WorldScope(int world) : _outer(callerWorld) {
    if (world != 0 && callerWorld == 0) {
        callerWorld = world;
    }
}

ScriptCaller::WorldScope::~WorldScope() {
    callerWorld = _outer;
}
