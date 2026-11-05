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
    /// A call a guard marks as a world's script's on a thread with no world of its own (tests, a hop no one carried
    /// the world across): it counts as every world's.
    constexpr int ANY_WORLD = -1;

    /// A world id no engine had before (> 0), one series for every engine that runs a world's scripts: a client's
    /// entities engines (one per world served) and an avatar's scripts (a world of their own), so that an id names
    /// one of them and retireWorld reaches the right one.
    int newWorld();

    /// Which world's script makes the current call: 0 when it is no world's (the app's or the visitor's own), else the
    /// id of the world's entities engine (markThreadAsWorld), or ANY_WORLD.
    int world();
    bool isWorld();
    /// What a world's script made (tagged with world() then) belongs to the world `world` whose scripts stopped.
    bool belongsTo(int tag, int world);

    /// The world `world` was left (its entities engine retired, before that engine's scripts unload and stop): from now
    /// on its code gets nothing done, whatever it is still running (a scriptEnding or unload handler, a callback, a
    /// wrapper inside another script's unload). The script API guards refuse its calls, reads, writes and signals (but
    /// for a short list, ScriptApiPolicy::allowsAfterWorldLeft), and the parts that act for a world's script refuse it
    /// too, for calls the guards don't see: new entities, Entities writes and reads, controller mappings, pointers,
    /// picks, 2D overlays, web requests (security finding #32). ANY_WORLD is never retired (it is no world in
    /// particular). A live world's call costs one compare: the live world is newer than every world left.
    void retireWorld(int world);
    /// Whether the world `world` was left (retireWorld).
    bool isRetired(int world);
    /// Whether the current call is a script's of a world that was left: refuse it.
    bool isRetired();
    /// Logs that a call of a world that was left was refused (e.g. "Entities", "addEntity"), once per world and member.
    void logRetiredRefusal(const char* object, const char* member);

    /// Marks every call made on this thread from now on as the world `world`'s script's (> 0, or ANY_WORLD), for the
    /// rest of the thread's life. Set once, when the thread of an engine that runs only a world's scripts starts (the
    /// entities script engine, on both engines; its id is the engine's), so that a call the guards don't mark still
    /// counts as the world's. Nothing clears it, a WorldScope included.
    void markThreadAsWorld(int world);

    /// Marks the calls made while it lives as a world's script's: this thread's world, or ANY_WORLD on a thread with
    /// none (when `world` is true); or the world given (a world() read on the caller's thread before a hop). An outer
    /// scope's world stays.
    class WorldScope {
    public:
        explicit WorldScope(bool world);
        explicit WorldScope(int world);
        ~WorldScope();
        WorldScope(const WorldScope&) = delete;
        WorldScope& operator=(const WorldScope&) = delete;
    private:
        int _outer;
    };
}

#endif // hifi_ScriptCaller_h
