//
//  EngineGlobalsTests.cpp
//  tests/script-api-guard
//
//  Created by scripting on 2026-11-04.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "EngineGlobalsTests.h"

#include <AddressManager.h>
#include <DependencyManager.h>
#include <NodeList.h>
#include <ResourceManager.h>
#include <ScriptCaller.h>
#include <WasmScriptEngine.h>

QTEST_GUILESS_MAIN(EngineGlobalsTests)

namespace {

// an engine with the globals it gives its context, registered as run() registers them before a script starts (no
// script runs here); given a world id first when its maker would (an entities engine, an avatar's scripts)
class GlobalsEngine : public WasmScriptEngine {
public:
    explicit GlobalsEngine(Context context, int world = 0) : WasmScriptEngine(context, NO_SCRIPT, "about:globals") {
        setWorldId(world);
        registerEngineGlobals();
    }
};

QStringList globalsOf(ScriptEngine::Context context) {
    GlobalsEngine engine(context);
    return engine.registeredGlobalNames();
}

} // namespace

void EngineGlobalsTests::initTestCase() {
    DependencyManager::set<ResourceManager>(false);
    DependencyManager::set<AddressManager>();
    DependencyManager::set<NodeList>(NodeType::Agent, INVALID_PORT);
}

// A listening socket on the machine is an agent's alone (the operator's own script). A world's entity scripts, on
// a visitor's client or on the entity script server, and a client script get no WebSocketServers: inbound surface
// no sign-in or ban list covers (security's ruling, 2026-11-04).
void EngineGlobalsTests::testListeningSocketsAreAnAgentsAlone() {
    QVERIFY(globalsOf(ScriptEngine::AGENT_SCRIPT).contains("WebSocketServers"));
    QVERIFY(!globalsOf(ScriptEngine::ENTITY_SERVER_SCRIPT).contains("WebSocketServers"));
    QVERIFY(!globalsOf(ScriptEngine::ENTITY_CLIENT_SCRIPT).contains("WebSocketServers"));
    QVERIFY(!globalsOf(ScriptEngine::CLIENT_SCRIPT).contains("WebSocketServers"));
}

// What every context gets: the objects the guest's XMLHttpRequest and WebSocket are built on (each request judged
// by the network rule as it is made), the audio effect options and the resource states.
void EngineGlobalsTests::testNetworkObjectsInEveryContext() {
    for (auto context : { ScriptEngine::CLIENT_SCRIPT, ScriptEngine::ENTITY_CLIENT_SCRIPT,
                          ScriptEngine::ENTITY_SERVER_SCRIPT, ScriptEngine::AGENT_SCRIPT }) {
        const QStringList names = globalsOf(context);
        for (const char* expected : { "Http", "Sockets", "AudioEffectOptions", "Resource" }) {
            QVERIFY2(names.contains(expected), qPrintable(QString("%1 missing from context %2: %3")
                                                           .arg(expected).arg(int(context)).arg(names.join(", "))));
        }
    }
}

// An avatar's scripts (a client script engine given a world id before it runs) run under a world's rules, as a
// world's entity scripts do: a global the script API policy removes becomes a stand-in, Messages is the world's.
// The visitor's own scripts keep everything, and a world id on a server's or an agent's engine changes nothing
// (security finding #66).
void EngineGlobalsTests::testAnAvatarsScriptsGetAWorldsGlobals() {
    QObject menu;   // stands for a host's registration the policy refuses a world's scripts
    GlobalsEngine visitor(ScriptEngine::CLIENT_SCRIPT);
    GlobalsEngine avatar(ScriptEngine::CLIENT_SCRIPT, ScriptCaller::newWorld());
    GlobalsEngine entities(ScriptEngine::ENTITY_CLIENT_SCRIPT, ScriptCaller::newWorld());
    GlobalsEngine agent(ScriptEngine::AGENT_SCRIPT, ScriptCaller::newWorld());
    QVERIFY(!visitor.holdsWorldRules());
    QVERIFY(avatar.holdsWorldRules());
    QVERIFY(entities.holdsWorldRules());
    QVERIFY(!agent.holdsWorldRules());
    // (the world's Messages object, registered with the standard globals, is checked in the walled kit,
    // tests-manual/wasm-scripting/avatar-scripts.sh: an app channel's message never arrives, a plain one's does)

    for (GlobalsEngine* engine : { &visitor, &avatar, &entities, &agent }) {
        engine->registerGlobalObject("Menu", &menu);
    }
    QCOMPARE(visitor.registeredGlobalClass("Menu"), QString("QObject"));
    QCOMPARE(avatar.registeredGlobalClass("Menu"), QString("RefusedGlobal"));
    QCOMPARE(entities.registeredGlobalClass("Menu"), QString("RefusedGlobal"));
    QCOMPARE(agent.registeredGlobalClass("Menu"), QString("QObject"));
    // what the policy allows every script stays itself
    QCOMPARE(avatar.registeredGlobalClass("Vec3"), visitor.registeredGlobalClass("Vec3"));
    QVERIFY(avatar.registeredGlobalClass("Vec3") != QString("RefusedGlobal"));

    // world ids come in one series, each new
    const int first = ScriptCaller::newWorld();
    QVERIFY(first > 0);
    QVERIFY(ScriptCaller::newWorld() > first);
}
