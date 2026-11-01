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
thread_local bool callerIsWorld = false;
thread_local bool threadIsWorld = false;
}

bool ScriptCaller::isWorld() {
    return callerIsWorld || threadIsWorld;
}

void ScriptCaller::markThreadAsWorld() {
    threadIsWorld = true;
}

ScriptCaller::WorldScope::WorldScope(bool world) : _outer(callerIsWorld) {
    callerIsWorld = _outer || world;
}

ScriptCaller::WorldScope::~WorldScope() {
    callerIsWorld = _outer;
}
