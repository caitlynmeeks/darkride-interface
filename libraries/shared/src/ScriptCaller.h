//
//  ScriptCaller.h
//  libraries/shared/src
//
//  Created by scripting on 2026-11-01.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_ScriptCaller_h
#define hifi_ScriptCaller_h

/// Whether a world's script is the caller of what runs on this thread now: the script API guards mark each call a
/// world's script makes into the app's objects (both engines call on the script's own thread), so the parts that act
/// on what their caller owns can tell a world from the visitor's own scripts (a world's controller mappings, security
/// finding #29). Unmarked: the app itself or the visitor's own scripts.
/// The mark lives on the calling thread for the call's own length: a part that hands the work to another thread (a
/// queued or blocking invoke) loses it, so whatever reads it must run in the call itself, as the controller mappings'
/// checks (UserInputMapper::newMapping and enableMapping, RouteBuilderProxy::to) do.
namespace ScriptCaller {
    bool isWorld();

    /// Marks every call made on this thread from now on as a world's script's, for the rest of the thread's life. Set
    /// once, when the thread of an engine that runs only a world's scripts starts (the entities script engine, on
    /// both engines), so that a call the guards don't mark still counts as the world's. Nothing clears it, a
    /// WorldScope included.
    void markThreadAsWorld();

    /// Marks the calls made while it lives as a world's script's (when `world` is true).
    class WorldScope {
    public:
        explicit WorldScope(bool world);
        ~WorldScope();
        WorldScope(const WorldScope&) = delete;
        WorldScope& operator=(const WorldScope&) = delete;
    private:
        bool _outer;
    };
}

#endif // hifi_ScriptCaller_h
