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

#include <DependencyManager.h>
#include <ResourceManager.h>

#include <HttpRequestObject.h>
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

    // an entity script this engine runs, or is loading, is believed
    QCOMPARE(engine.verifiedSandbox(NETWORK_SCRIPT.toString()), NETWORK_SCRIPT);
    QCOMPARE(engine.verifiedSandbox(localScript.toString()), localScript);
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
    // the control: the world's real local entity script includes its neighbour
    const QVariantMap result = engine.resolveInclude("lib.js", localScript.toString(), localScript.toString());
    QCOMPARE(result.value("url").toString(), QUrl::fromLocalFile(_root + "/world/lib.js").toString());
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
    // the control: the world's real local entity script requires its neighbour
    const QVariantMap result = engine.resolveModule("./lib.js", QString(), localScript.toString(), localScript.toString());
    QCOMPARE(result.value("url").toString(), QUrl::fromLocalFile(_root + "/world/lib.js").toString());
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
