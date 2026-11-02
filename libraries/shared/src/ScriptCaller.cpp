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

namespace {
thread_local int callerWorld = 0;  // a WorldScope's
thread_local int threadWorld = 0;  // markThreadAsWorld's
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
