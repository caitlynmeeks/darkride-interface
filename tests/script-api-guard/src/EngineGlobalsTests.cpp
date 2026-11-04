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
#include <WasmScriptEngine.h>

QTEST_GUILESS_MAIN(EngineGlobalsTests)

namespace {

// an engine with the globals it gives its context, registered as run() registers them before a script starts (no
// script runs here)
class GlobalsEngine : public WasmScriptEngine {
public:
    explicit GlobalsEngine(Context context) : WasmScriptEngine(context, NO_SCRIPT, "about:globals") {
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
