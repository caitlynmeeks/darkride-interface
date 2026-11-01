//
//  VerifiedSandboxTests.cpp
//  tests/script-api-guard
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  Which entity script is running, as the WebAssembly engine's host records it (WasmScriptEngine::verifiedSandbox).
//  The JavaScript guest passes the running entity script's address with every include, require, script fetch and web
//  request; the host goes by the entity scripts it loaded itself and still runs, and any other address counts as the
//  entities engine itself, about:Entities, which is never local. Checked for each caller: Script.include's resolver,
//  Script.require's resolver, the script fetch both use, Http.createRequest and Sockets.createWebSocket. The guest's
//  __darkride helpers (includeFrom, resolvePathFrom, requireFrom, requireResolveFrom) pass a parent URL, never the
//  entity script's address, and come to these same calls.
//

#include "VerifiedSandboxTests.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <AddressManager.h>
#include <AudioScriptingInterface.h>
#include <DependencyManager.h>
#include <NodeList.h>
#include <PathUtils.h>
#include <ResourceManager.h>
#include <ResourceScriptingInterface.h>

#include <HttpRequestObject.h>
#include <ScriptApiPolicy.h>
#include <controllers/ScriptingInterface.h>
#include <ScriptNetworkPolicy.h>
#include <WasmScriptEngine.h>
#include <WebSocketObject.h>

QTEST_GUILESS_MAIN(VerifiedSandboxTests)

namespace {

const QUrl ENTITIES("about:Entities");
// a world on the internet: its entity script
const QUrl NETWORK_SCRIPT("https://example.invalid/world/entity.js");

// an entities engine with entity scripts recorded as the engine records them when it loads one (no guest is run)
class EntitiesEngine : public WasmScriptEngine {
public:
    EntitiesEngine() : WasmScriptEngine(ENTITY_CLIENT_SCRIPT, NO_SCRIPT, "about:Entities 1") {}
    void addEntityScript(const QUrl& sandbox, EntityScriptStatus status) {
        EntityScriptDetails details;
        details.status = status;
        details.scriptText = sandbox.toString();
        details.definingSandboxURL = sandbox;
        setEntityScriptDetails(EntityItemID(QUuid::createUuid()), details);
    }
};

// the engine's warning lines, as the script log shows them
class Warnings {
public:
    explicit Warnings(ScriptEngine& engine) {
        QObject::connect(&engine, &ScriptEngine::warningMessage, &_context,
                         [this](const QString& message, const QString&) { lines.append(message); });
    }
    QStringList lines;
private:
    QObject _context;
};

// the world the client is in, by its server's address: on the internet, on this machine, on the home network
const char* PUBLIC_WORLD = "203.0.113.5";
const char* THIS_MACHINE_WORLD = "127.0.0.1";
const char* HOME_NETWORK_WORLD = "192.168.1.20";

void setWorld(const char* address) {
    DependencyManager::get<NodeList>()->getDomainHandler().setSockAddr(HifiSockAddr(QHostAddress(address), 40102), address);
}

bool writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QString refusedFrom(const QUrl& origin, const QString& target) {
    return origin.toDisplayString() + " may not reach " + target + ":";
}

} // namespace

void VerifiedSandboxTests::initTestCase() {
    DependencyManager::set<ResourceManager>(false);
    QVERIFY(_dir.isValid());
    _root = QFileInfo(_dir.path()).canonicalFilePath();
    // a world served from this machine (its entity script a file here), and a folder beside it
    QVERIFY(QDir(_root).mkpath("world"));
    QVERIFY(QDir(_root).mkpath("outside"));
    QVERIFY(writeFile(_root + "/world/entity.js", "(function () {});"));
    QVERIFY(writeFile(_root + "/world/lib.js", "module.exports = 1;"));
    QVERIFY(writeFile(_root + "/outside/other.js", "module.exports = 2;"));
    // the app's own scripts, in a folder of the test's own
    QVERIFY(QDir(_root).mkpath("app/scripts/entities"));
    QVERIFY(writeFile(_root + "/app/scripts/entities/example.js", "(function () {});"));
    QVERIFY(writeFile(_root + "/app/scripts/entities/helper.js", "var helper = 1;"));
    PathUtils::defaultScriptsLocation(_root + "/app/scripts");
    // a world on the internet, unless a test says otherwise
    DependencyManager::set<AddressManager>();
    DependencyManager::set<NodeList>(NodeType::Agent, INVALID_PORT);
    setWorld(PUBLIC_WORLD);
}

void VerifiedSandboxTests::testClaims() {
    EntitiesEngine engine;
    const QUrl localScript = QUrl::fromLocalFile(_root + "/world/entity.js");
    const QUrl loading("https://example.invalid/world/loading.js");
    const QUrl failed = QUrl::fromLocalFile(_root + "/outside/failed.js");
    const QUrl unloaded("http://127.0.0.1:8000/unloaded.js");
    engine.addEntityScript(NETWORK_SCRIPT, EntityScriptStatus::RUNNING);
    engine.addEntityScript(localScript, EntityScriptStatus::RUNNING);
    engine.addEntityScript(loading, EntityScriptStatus::LOADING);
    engine.addEntityScript(failed, EntityScriptStatus::ERROR_RUNNING_SCRIPT);
    engine.addEntityScript(unloaded, EntityScriptStatus::UNLOADED);

    // an entity script this engine runs, or is loading, is believed (a local one only in a world on this machine: the
    // world rule, testWorldRule)
    QCOMPARE(engine.verifiedSandbox(NETWORK_SCRIPT.toString()), NETWORK_SCRIPT);
    setWorld(THIS_MACHINE_WORLD);
    {
        EntitiesEngine local;
        local.addEntityScript(localScript, EntityScriptStatus::RUNNING);
        QCOMPARE(local.verifiedSandbox(localScript.toString()), localScript);
    }
    setWorld(PUBLIC_WORLD);
    QCOMPARE(engine.verifiedSandbox(loading.toString()), loading);
    QCOMPARE(engine.verifiedScriptOrigin(NETWORK_SCRIPT.toString()), NETWORK_SCRIPT);
    // no address: the engine itself; an entity script given as code keeps its about: name (never local)
    QCOMPARE(engine.verifiedSandbox(QString()), ENTITIES);
    QCOMPARE(engine.verifiedSandbox("about:EmbeddedEntityScript"), QUrl("about:EmbeddedEntityScript"));
    QVERIFY(!engine.verifiedSandbox("about:blank").isLocalFile());

    // addresses of no running entity script: files and folders here, this machine's and its network's, stopped scripts
    const QStringList unrecorded {
        "file:///",
        "file:///a/b.js",
        "file:///x.js",
        QUrl::fromLocalFile(_root + "/outside/").toString(),
        QUrl::fromLocalFile(_root + "/outside/other.js").toString(),
        QUrl::fromLocalFile(_root + "/world/").toString(),                    // the folder of a real one
        QUrl::fromLocalFile(_root + "/world/./entity.js").toString(),         // a real one, spelled otherwise
        localScript.toString() + "?x",
        failed.toString(),
        unloaded.toString(),
        "http://127.0.0.1:8000/entity.js",
        "http://localhost/entity.js",
        "http://192.168.1.10/entity.js",
        "https://example.invalid/world/",
        "qrc:///qml/Browser.qml",
        "not a url at all",
    };
    for (const QString& claim : unrecorded) {
        QCOMPARE(engine.verifiedSandbox(claim), ENTITIES);
        QCOMPARE(engine.verifiedScriptOrigin(claim), ENTITIES);
    }
}

void VerifiedSandboxTests::testClientEngineIgnoresClaims() {
    // a client script or an agent has no entity scripts, and its code is the engine's own: the address passed changes nothing
    WasmScriptEngine engine(ScriptEngine::CLIENT_SCRIPT, NO_SCRIPT, "https://example.invalid/client.js");
    QVERIFY(engine.verifiedSandbox("file:///a/b.js").isEmpty());
    QCOMPARE(engine.verifiedScriptOrigin("file:///a/b.js"), QUrl("https://example.invalid/client.js"));
    QCOMPARE(engine.verifiedScriptOrigin("http://127.0.0.1/x.js"), QUrl("https://example.invalid/client.js"));
}

void VerifiedSandboxTests::testInclude() {
    EntitiesEngine engine;
    const QUrl localScript = QUrl::fromLocalFile(_root + "/world/entity.js");
    engine.addEntityScript(NETWORK_SCRIPT, EntityScriptStatus::RUNNING);
    engine.addEntityScript(localScript, EntityScriptStatus::RUNNING);
    const QString outside = QUrl::fromLocalFile(_root + "/outside/other.js").toString();
    const QString outsideFolder = QUrl::fromLocalFile(_root + "/outside/").toString();

    // with an unrecorded local address, a file outside the entity script's folder isn't included, by full URL or relative to a parent
    for (const QString& claim : { outside, outsideFolder, QString("file:///") }) {
        QVariantMap result = engine.resolveInclude(outside, NETWORK_SCRIPT.toString(), claim);
        QVERIFY2(!result.contains("url"), qPrintable(claim));
        QCOMPARE(result.value("reason").toString(), QString("outside of original entity script about:Entities"));
        result = engine.resolveInclude("other.js", outsideFolder, claim);
        QVERIFY2(!result.contains("url"), qPrintable(claim));
        QCOMPARE(result.value("reason").toString(), QString("outside of original entity script about:Entities"));
    }
    // the network script's own address: the same answer, under its own name
    QCOMPARE(engine.resolveInclude(outside, NETWORK_SCRIPT.toString(), NETWORK_SCRIPT.toString()).value("reason").toString(),
             "outside of original entity script " + NETWORK_SCRIPT.toString());
    // the control: in a world on this machine, the world's real local entity script includes its neighbour
    setWorld(THIS_MACHINE_WORLD);
    EntitiesEngine local;
    local.addEntityScript(localScript, EntityScriptStatus::RUNNING);
    const QVariantMap result = local.resolveInclude("lib.js", localScript.toString(), localScript.toString());
    QCOMPARE(result.value("url").toString(), QUrl::fromLocalFile(_root + "/world/lib.js").toString());
    setWorld(PUBLIC_WORLD);
}

void VerifiedSandboxTests::testRequire() {
    EntitiesEngine engine;
    const QUrl localScript = QUrl::fromLocalFile(_root + "/world/entity.js");
    engine.addEntityScript(NETWORK_SCRIPT, EntityScriptStatus::RUNNING);
    engine.addEntityScript(localScript, EntityScriptStatus::RUNNING);
    const QString outside = QUrl::fromLocalFile(_root + "/outside/other.js").toString();
    const QString outsideFolder = QUrl::fromLocalFile(_root + "/outside/").toString();

    for (const QString& claim : { outside, outsideFolder, QString("file:///") }) {
        // by full URL, relative to a parent, and relative to relativeTo
        for (const QVariantMap& result : { engine.resolveModule(outside, QString(), NETWORK_SCRIPT.toString(), claim),
                                           engine.resolveModule("./other.js", QString(), outside, claim),
                                           engine.resolveModule("./other.js", outsideFolder, NETWORK_SCRIPT.toString(), claim) }) {
            QVERIFY2(!result.contains("url"), qPrintable(claim));
            QVERIFY2(result.value("error").toString().contains("outside of origin script 'about:"),
                     qPrintable(result.value("error").toString()));
        }
    }
    // the control: in a world on this machine, the world's real local entity script requires its neighbour
    setWorld(THIS_MACHINE_WORLD);
    EntitiesEngine local;
    local.addEntityScript(localScript, EntityScriptStatus::RUNNING);
    const QVariantMap result = local.resolveModule("./lib.js", QString(), localScript.toString(), localScript.toString());
    QCOMPARE(result.value("url").toString(), QUrl::fromLocalFile(_root + "/world/lib.js").toString());
    setWorld(PUBLIC_WORLD);
}

void VerifiedSandboxTests::testFetch() {
    EntitiesEngine engine;
    engine.addEntityScript(NETWORK_SCRIPT, EntityScriptStatus::RUNNING);
    Warnings warnings(engine);
    const QString outside = QUrl::fromLocalFile(_root + "/outside/other.js").toString();
    const QString outsideFolder = QUrl::fromLocalFile(_root + "/outside/").toString();

    // the fetch decides again, as it is reachable by name: refused, and the log names the engine as the asker
    for (const QString& claim : { outside, outsideFolder, QString("file:///") }) {
        warnings.lines.clear();
        const QVariantMap result = engine.fetchScript(outside, false, claim);
        QCOMPARE(result.value("status").toString(), QString("Refused"));
        QVERIFY(!result.value("success").toBool());
        QVERIFY(!result.contains("contents"));
        QCOMPARE(warnings.lines.size(), 1);
        QVERIFY2(warnings.lines[0].startsWith(refusedFrom(ENTITIES, QUrl(outside).toDisplayString())), qPrintable(warnings.lines[0]));
    }
    // the control: a recorded address is believed (the line names the entity script), and refused all the same
    warnings.lines.clear();
    QCOMPARE(engine.fetchScript(outside, false, NETWORK_SCRIPT.toString()).value("status").toString(), QString("Refused"));
    QCOMPARE(warnings.lines.size(), 1);
    QVERIFY2(warnings.lines[0].startsWith(refusedFrom(NETWORK_SCRIPT, QUrl(outside).toDisplayString())), qPrintable(warnings.lines[0]));
}

void VerifiedSandboxTests::testHttpRequest() {
    EntitiesEngine engine;
    engine.addEntityScript(NETWORK_SCRIPT, EntityScriptStatus::RUNNING);
    Warnings warnings(engine);
    HttpRequestFactory factory(&engine);    // the engine's Http, as the guest's XMLHttpRequest calls it
    const QString outside = QUrl::fromLocalFile(_root + "/outside/other.js").toString();

    struct Case {
        QString claim;
        QUrl origin;    // what the request is judged as
        QString target;
    };
    const QList<Case> cases {
        { "file:///", ENTITIES, outside },
        { "file:///x.js", ENTITIES, "file:///a/b.js" },
        { outside, ENTITIES, "http://127.0.0.1:9/" },
        { "http://127.0.0.1:8000/entity.js", ENTITIES, "http://localhost:9/" },
        { "http://192.168.1.10/entity.js", ENTITIES, "http://192.168.1.1/" },
        { NETWORK_SCRIPT.toString(), NETWORK_SCRIPT, "http://127.0.0.1:9/" },     // the control
    };
    for (const Case& c : cases) {
        warnings.lines.clear();
        HttpRequestObject* request = factory.createRequest(c.claim);
        bool completed = false;
        QObject::connect(request, &HttpRequestObject::requestComplete, [&completed] { completed = true; });
        request->open("GET", c.target);
        request->send();
        QTRY_VERIFY(completed);
        QCOMPARE(request->errorCode(), int(QNetworkReply::ContentAccessDenied));
        QCOMPARE(warnings.lines.size(), 1);
        QVERIFY2(warnings.lines[0].startsWith(refusedFrom(c.origin, QUrl(c.target).toDisplayString())), qPrintable(warnings.lines[0]));
        factory.dispose(request);
    }
}

void VerifiedSandboxTests::testWebSocket() {
    EntitiesEngine engine;
    engine.addEntityScript(NETWORK_SCRIPT, EntityScriptStatus::RUNNING);
    Warnings warnings(engine);
    WebSocketFactory factory(&engine);      // the engine's Sockets, as the guest's WebSocket calls it

    struct Case {
        QString claim;
        QUrl origin;
        QString target;
    };
    const QList<Case> cases {
        { "file:///", ENTITIES, "ws://127.0.0.1:9/" },
        { QUrl::fromLocalFile(_root + "/outside/other.js").toString(), ENTITIES, "ws://192.168.1.1:9/" },
        { "http://127.0.0.1:8000/entity.js", ENTITIES, "ws://localhost:9/" },
        { NETWORK_SCRIPT.toString(), NETWORK_SCRIPT, "ws://127.0.0.1:9/" },       // the control
    };
    for (const Case& c : cases) {
        warnings.lines.clear();
        WebSocketObject* socket = factory.createWebSocket(c.target, c.claim);
        bool errored = false;
        QObject::connect(socket, &WebSocketObject::errored, [&errored] { errored = true; });
        QTRY_VERIFY(errored);
        QCOMPARE(warnings.lines.size(), 1);
        QVERIFY2(warnings.lines[0].startsWith(refusedFrom(c.origin, QUrl(c.target).toDisplayString())), qPrintable(warnings.lines[0]));
        factory.dispose(socket);
    }
}


void VerifiedSandboxTests::testWorldRule() {
    const QUrl localScript = QUrl::fromLocalFile(_root + "/world/entity.js");
    const QUrl appScript = QUrl::fromLocalFile(_root + "/app/scripts/entities/example.js");
    const QUrl machineScript("http://127.0.0.1:8000/entity.js");
    const QString lib = QUrl::fromLocalFile(_root + "/world/lib.js").toString();

    // a world on the internet: no entity script is local, whatever its own address
    {
        EntitiesEngine engine;
        for (const QUrl& url : { localScript, appScript, machineScript, NETWORK_SCRIPT }) {
            engine.addEntityScript(url, EntityScriptStatus::RUNNING);
        }
        QVERIFY(!engine.entityScriptsMayBeLocal());
        QVERIFY(!engine.worldIsLocal());
        QCOMPARE(engine.verifiedSandbox(localScript.toString()), ENTITIES);
        QCOMPARE(engine.verifiedSandbox(appScript.toString()), ENTITIES);
        QCOMPARE(engine.verifiedSandbox(machineScript.toString()), ENTITIES);
        QCOMPARE(engine.verifiedSandbox(NETWORK_SCRIPT.toString()), NETWORK_SCRIPT);
        QCOMPARE(engine.verifiedScriptOrigin(localScript.toString()), ENTITIES);
        QCOMPARE(engine.effectiveSandbox(QUrl("qrc:///scripts/x.js")), ENTITIES);
        QCOMPARE(engine.effectiveSandbox(QUrl("about:EmbeddedEntityScript")), QUrl("about:EmbeddedEntityScript"));
        // the local entity script reaches no file beside it
        QCOMPARE(engine.resolveInclude("lib.js", localScript.toString(), localScript.toString()).value("reason").toString(),
                 QString("outside of original entity script about:Entities"));
        QVERIFY(engine.resolveModule("./lib.js", QString(), localScript.toString(), localScript.toString()).contains("error"));
        QCOMPARE(engine.fetchScript(lib, false, localScript.toString()).value("status").toString(), QString("Refused"));
        // an app script run as an entity script has what any script of the world has: no file here (the network rule
        // refuses file addresses, the app's own scripts too, as for every script from the internet since 1.1.1)
        QVERIFY(!engine.resolveInclude("helper.js", appScript.toString(), appScript.toString()).contains("url"));
        QVERIFY(!engine.resolveInclude(lib, appScript.toString(), appScript.toString()).contains("url"));
        QCOMPARE(engine.fetchScript(lib, false, appScript.toString()).value("status").toString(), QString("Refused"));
    }

    // a world on this machine: as before, a local entity script keeps every file
    setWorld(THIS_MACHINE_WORLD);
    {
        EntitiesEngine engine;
        engine.addEntityScript(localScript, EntityScriptStatus::RUNNING);
        QVERIFY(engine.entityScriptsMayBeLocal());
        QVERIFY(engine.worldIsLocal());
        QCOMPARE(engine.verifiedSandbox(localScript.toString()), localScript);
        QCOMPARE(engine.resolveInclude("../outside/other.js", localScript.toString(), localScript.toString()).value("url").toString(),
                 QUrl::fromLocalFile(_root + "/outside/other.js").toString());
        // held one way: once the engine has served a world on the internet, a local world doesn't change that
        setWorld(PUBLIC_WORLD);
        QCOMPARE(engine.verifiedSandbox(localScript.toString()), ENTITIES);
        setWorld(THIS_MACHINE_WORLD);
        QCOMPARE(engine.verifiedSandbox(localScript.toString()), ENTITIES);
        QVERIFY(!engine.entityScriptsMayBeLocal());
        QVERIFY(!engine.worldIsLocal());
    }
    // a world on the home network counts as local (the rule as it stands)
    setWorld(HOME_NETWORK_WORLD);
    {
        EntitiesEngine engine;
        QVERIFY(engine.entityScriptsMayBeLocal());
    }
    setWorld(PUBLIC_WORLD);

    // a client script is the visitor's own: its file keeps its rights in any world
    {
        WasmScriptEngine engine(ScriptEngine::CLIENT_SCRIPT, NO_SCRIPT, localScript.toString());
        QCOMPARE(engine.verifiedScriptOrigin(QString()), localScript);
        QCOMPARE(engine.effectiveSandbox(localScript), localScript);
    }
}

void VerifiedSandboxTests::testWorldRuleOnRequests() {
    const QUrl localScript = QUrl::fromLocalFile(_root + "/world/entity.js");
    const QUrl appScript = QUrl::fromLocalFile(_root + "/app/scripts/entities/example.js");
    const QStringList targets { "http://127.0.0.1:9/", "http://192.168.1.1/", "file:///a/b.js" };

    // a world on the internet: a local or app entity script's requests are held to the rule, as the entities engine's
    EntitiesEngine engine;
    engine.addEntityScript(localScript, EntityScriptStatus::RUNNING);
    engine.addEntityScript(appScript, EntityScriptStatus::RUNNING);
    Warnings warnings(engine);
    HttpRequestFactory requests(&engine);
    WebSocketFactory sockets(&engine);
    for (const QUrl& script : { localScript, appScript }) {
        for (const QString& target : targets) {
            warnings.lines.clear();
            HttpRequestObject* request = requests.createRequest(script.toString());
            bool completed = false;
            QObject::connect(request, &HttpRequestObject::requestComplete, [&completed] { completed = true; });
            request->open("GET", target);
            request->send();
            QTRY_VERIFY(completed);
            QCOMPARE(request->errorCode(), int(QNetworkReply::ContentAccessDenied));
            QCOMPARE(warnings.lines.size(), 1);
            QVERIFY2(warnings.lines[0].startsWith(refusedFrom(ENTITIES, QUrl(target).toDisplayString())), qPrintable(warnings.lines[0]));
            requests.dispose(request);
        }
        warnings.lines.clear();
        WebSocketObject* socket = sockets.createWebSocket("ws://127.0.0.1:9/", script.toString());
        bool errored = false;
        QObject::connect(socket, &WebSocketObject::errored, [&errored] { errored = true; });
        QTRY_VERIFY(errored);
        QVERIFY2(warnings.lines.value(0).startsWith(refusedFrom(ENTITIES, "ws://127.0.0.1:9/")), qPrintable(warnings.lines.value(0)));
        sockets.dispose(socket);
    }

    // leaving that world for one on this machine: a request the world's script makes now is still held to the rule,
    // even from a script on the network
    setWorld(THIS_MACHINE_WORLD);
    engine.addEntityScript(NETWORK_SCRIPT, EntityScriptStatus::RUNNING);
    for (const QUrl& script : { NETWORK_SCRIPT, localScript }) {
        warnings.lines.clear();
        HttpRequestObject* request = requests.createRequest(script.toString());
        bool completed = false;
        QObject::connect(request, &HttpRequestObject::requestComplete, [&completed] { completed = true; });
        request->open("GET", "http://127.0.0.1:9/");
        request->send();
        QTRY_VERIFY(completed);
        QCOMPARE(request->errorCode(), int(QNetworkReply::ContentAccessDenied));
        QCOMPARE(warnings.lines.size(), 1);
        requests.dispose(request);
    }
    QString reason;
    QVERIFY(!ScriptNetworkPolicy::decideBlocking(QUrl("http://127.0.0.1:9/x.js"), NETWORK_SCRIPT, reason, engine.worldIsLocal()));
    setWorld(PUBLIC_WORLD);
}

void VerifiedSandboxTests::testRefusalAtLoad() {
    const QString app = _root + "/app/scripts/entities/example.js";
    const QString outside = _root + "/outside/other.js";
    QStringList refused {
        "file:///a/b.js",
        QUrl::fromLocalFile(outside).toString(),
        outside,                                                    // a path: the script cache reads it as a file
        _root.mid(1) + "/outside/other.js",                         // a path without its first slash
        "file://" + _root + "/app/scripts/../../outside/other.js",
        "file://" + _root + "/app/scripts/%2e%2e/%2e%2e/outside/other.js",
        "file://" + _root + "/app/scripts/..%5c..%5coutside%5cother.js",
        "unknown:/a/b.js",                                          // an unknown scheme: read as a file too
    };
    const QStringList allowed {
        QUrl::fromLocalFile(app).toString(),
        app,
        "qrc:///scripts/x.js",
        "https://example.invalid/world/entity.js",
        "http://127.0.0.1:8000/entity.js",                          // held to the network rule instead
        "atp:/entity.js",
        "(function () { return {}; })",
        "javascript:(function () { return {}; })",
    };

    // a world on the internet
    {
        EntitiesEngine engine;
        for (const QString& script : refused) {
            QVERIFY2(!engine.entityScriptRefusal(script).isEmpty(), qPrintable(script));
        }
        for (const QString& script : allowed) {
            QVERIFY2(engine.entityScriptRefusal(script).isEmpty(), qPrintable(script));
        }
    }
    // a world on this machine: a local file runs, as before
    setWorld(THIS_MACHINE_WORLD);
    {
        EntitiesEngine engine;
        for (const QString& script : refused) {
            QVERIFY2(engine.entityScriptRefusal(script).isEmpty(), qPrintable(script));
        }
    }
    setWorld(PUBLIC_WORLD);
    // the entity script server is the world's own machine: not this rule
    {
        WasmScriptEngine engine(ScriptEngine::ENTITY_SERVER_SCRIPT, NO_SCRIPT, "about:Entities 1");
        QVERIFY(engine.entityScriptRefusal("file:///a/b.js").isEmpty());
        QCOMPARE(engine.effectiveSandbox(QUrl("file:///a/b.js")), QUrl("file:///a/b.js"));
    }
}

void VerifiedSandboxTests::testAppScriptsFolder() {
    // the app's scripts folder is open to every script; a path that only starts there is not. In a world on this
    // machine, so the network rule doesn't stand in front of the folder test (a script from the internet in it)
    setWorld(THIS_MACHINE_WORLD);
    EntitiesEngine engine;
    engine.addEntityScript(NETWORK_SCRIPT, EntityScriptStatus::RUNNING);
    Warnings warnings(engine);
    const QString network = NETWORK_SCRIPT.toString();
    const QString up = "../../";
    QCOMPARE(engine.resolveInclude("/~/entities/helper.js", network, network).value("url").toString(),
             QUrl::fromLocalFile(_root + "/app/scripts/entities/helper.js").toString());
    QCOMPARE(engine.resolveInclude("/~/" + up + "outside/other.js", network, network).value("reason").toString(),
             QString("outside of standard libraries"));
    // percent signs in a /~/ path are part of a name, not dots: the address stays in the folder, and names nothing
    const QUrl encoded(engine.resolveInclude("/~/%2e%2e/%2e%2e/outside/other.js", network, network).value("url").toString());
    QVERIFY(ScriptNetworkPolicy::isAppScript(encoded));
    QVERIFY(!QFileInfo(encoded.toLocalFile()).exists());
    for (const QString& target : { "file://" + _root + "/app/scripts/" + up + "outside/other.js",
                                   "file://" + _root + "/app/scripts/%2e%2e/%2e%2e/outside/other.js",
                                   "file://" + _root + "/app/scripts/entities/" + up + up + "outside/other.js" }) {
        QVERIFY2(!engine.resolveInclude(target, network, network).contains("url"), qPrintable(target));
        QVERIFY2(!engine.resolveModule(target, QString(), network, network).contains("url"), qPrintable(target));
        QCOMPARE(engine.fetchScript(target, false, network).value("status").toString(), QString("Refused"));
    }
    setWorld(PUBLIC_WORLD);
}

void VerifiedSandboxTests::testResourcesNotForWorlds() {
    // for the visitor's own scripts
    QVERIFY(!ScriptApiPolicy::allowsGlobal("Resources"));
    QVERIFY(!ScriptApiPolicy::allowsClass(&ResourceScriptingInterface::staticMetaObject));
    QVERIFY(ScriptApiPolicy::allowsGlobal("Resource"));
}

void VerifiedSandboxTests::testInputAndPrivacyNotForWorlds() {
    // the app's folders (the visitor's account name in them) and every address the app requests
    QVERIFY(!ScriptApiPolicy::allowsGlobal("Paths"));
    QVERIFY(!ScriptApiPolicy::allowsGlobal("ResourceRequestObserver"));
    // taking the visitor's input for itself; hearing and mapping it stay (games)
    const QMetaObject* controller = &controller::ScriptingInterface::staticMetaObject;
    for (const char* member : { "captureMouseEvents", "releaseMouseEvents", "captureTouchEvents", "releaseTouchEvents",
                                "captureWheelEvents", "releaseWheelEvents", "captureActionEvents", "releaseActionEvents" }) {
        QVERIFY2(ScriptApiPolicy::deniesMethod(controller, member), member);
    }
    for (const char* member : { "newMapping", "enableMapping", "getValue", "getActionNames", "getPoseValue" }) {
        QVERIFY2(!ScriptApiPolicy::deniesMethod(controller, member), member);
    }
}

void VerifiedSandboxTests::testSignalsNotForWorlds() {
    // the microphone's samples, and when the visitor speaks or clips (AudioScriptingInterface's, and Audio's on it)
    const QMetaObject* audio = &AudioScriptingInterface::staticMetaObject;
    for (const char* member : { "inputReceived", "noiseGateOpened", "noiseGateClosed" }) {
        QVERIFY2(ScriptApiPolicy::deniesSignal(audio, member), member);
    }
    QVERIFY(!ScriptApiPolicy::deniesSignal(audio, "mutedByMixer"));
    QVERIFY(!ScriptApiPolicy::deniesSignal(audio, "isStereoInputChanged"));
    // the app's own classes are listed by name (the test does not link the app)
    QVERIFY(ScriptApiPolicy::allowsClass(&WindowScriptingInterface::staticMetaObject));
    for (const char* member : { "browseChanged", "browseDirChanged", "saveFileChanged", "assetsDirChanged", "promptTextChanged",
                                "stillSnapshotTaken", "snapshot360Taken", "snapshotShared", "processingGifStarted",
                                "processingGifCompleted" }) {
        QVERIFY2(ScriptApiPolicy::deniesSignal(&WindowScriptingInterface::staticMetaObject, member), member);
    }
    QVERIFY(!ScriptApiPolicy::deniesSignal(&WindowScriptingInterface::staticMetaObject, "domainChanged"));
    QVERIFY(!ScriptApiPolicy::deniesSignal(&WindowScriptingInterface::staticMetaObject, "geometryChanged"));
    // a class a world doesn't get: none of its signals
    QVERIFY(ScriptApiPolicy::deniesSignal(&QObject::staticMetaObject, "destroyed"));
}
