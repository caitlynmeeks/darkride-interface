//
//  JavaScriptGuestTests.cpp
//  tests/wasm-scripting/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "JavaScriptGuestTests.h"

#include <thread>

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

#include <HostInterface.h>
#include <ObjectTable.h>
#include <SignalRelay.h>
#include <ValueCodec.h>
#include <WasmRuntime.h>

#include "TestObjects.h"

QTEST_GUILESS_MAIN(JavaScriptGuestTests)

namespace {

QString guestModulePath() {
    QString fromEnvironment = qEnvironmentVariable("DARKRIDE_JS_WASM");
    if (!fromEnvironment.isEmpty()) {
        return fromEnvironment;
    }
    // this source file is tests/wasm-scripting/src/...; the module is built beside the tools
    return QFileInfo(QString(__FILE__)).dir().absoluteFilePath("../../../tools/wasm-js/build/darkride-js.wasm");
}

struct Guest {
    std::shared_ptr<WasmRuntime> runtime { std::make_shared<WasmRuntime>() };
    std::shared_ptr<WasmModule> module;
    std::unique_ptr<WasmInstance> instance;
    ObjectTable objects;
    ValueCodec codec { &objects };
    SignalRelay relay;
    std::unique_ptr<HostInterface> host;
    FakeScript script;
    Probe probe;
    AssetScriptingInterface assets;
    QStringList logged;        // "<level> <text>"
    QStringList errors;        // level 3 only
    QStringList failures;
    QStringList timeLimits;    // "<entry> <entity> <reason>" for each call ended at the time limit
    QVariantList carried;      // what the script handed to Probe.carry
    QHash<QString, QObject*> extraGlobals;  // more host globals, by name, registered before the script starts
    std::chrono::milliseconds callTimeout { 10000 };
    bool nameTimeLimits { false }; // with a time-limit handler, a limit is not a failure

    bool start(const QString& source, QString& error) {
        QFile file(guestModulePath());
        if (!file.open(QIODevice::ReadOnly)) {
            error = "no guest module at " + guestModulePath();
            return false;
        }
        // as the engine runs it: the guest asks whether to stop, the runtime checks no deadlines in its loops
        module = runtime->compile(file.readAll(), error, WasmRuntime::Interruption::Cooperative);
        if (!module) {
            return false;
        }
        WasmInstance::Limits limits;
        limits.callTimeout = callTimeout;
        instance = std::make_unique<WasmInstance>(module, limits);
        host = std::make_unique<HostInterface>(*instance, objects, codec, relay);
        host->setLogger([this](int level, const QString& text) {
            logged << QString::number(level) + " " + text;
            if (level == HostInterface::Error) {
                errors << text;
            }
        });
        host->setFailureHandler([this](const QString& entry, const QString& failure) { failures << entry + ": " + failure; });
        if (nameTimeLimits) {
            host->setTimeLimitHandler([this](const QString& entry, const QString& entity, const QString& reason) {
                timeLimits << entry + " " + entity + " " + reason;
            });
        }
        host->install();
        instance->enableWasi();
        objects.registerGlobal("Script", &script);
        objects.registerGlobal("Probe", &probe);
        objects.registerGlobal("Assets", &assets);
        for (auto global = extraGlobals.begin(); global != extraGlobals.end(); ++global) {
            objects.registerGlobal(global.key(), global.value());
        }
        QObject::connect(&probe, &Probe::carried, [this](const QVariant& value) { carried << value; });
        script.source = source;
        if (!instance->instantiate(error)) {
            return false;
        }
        std::vector<WasmValue> results;
        if (instance->hasExport("_initialize") && !instance->call("_initialize", {}, results, error)) {
            return false;
        }
        return host->callScriptStart(error);
    }

    void stop() {
        QString error;
        if (host) {
            host->callScriptStop(error);
        }
    }
};

#define START_OR_SKIP(guest, source)                                                        \
    {                                                                                      \
        if (!QFileInfo::exists(guestModulePath())) {                                       \
            QSKIP("darkride-js.wasm is not built (tools/wasm-js/build.sh)");               \
        }                                                                                  \
        QString error;                                                                     \
        QVERIFY2(guest.start(source, error), qPrintable(error + " " + guest.errors.join(" | "))); \
    }

} // namespace

void JavaScriptGuestTests::testCallsPropertiesAndValues() {
    Guest guest;
    const QString source = R"(
        var out = [];
        out.push(Probe.add(2, 3));
        out.push(Probe.add("a", "b"));
        Probe.count = 7;
        out.push(Probe.count);
        out.push(Probe.scale({ x: 1, y: 2, z: 3 }, 2).y);
        out.push(Probe.self() === Probe);
        out.push(Probe.label);
        Probe.position = { x: 1.5, y: 2.5, z: -3 };
        out.push(Probe.position.z);
        out.push(Probe.join(["a", "b"], "c").join("+"));
        out.push(String(Probe.uuidOf("{6ba7b810-9dad-11d1-80b4-00c04fd430c8}")));
        out.push(Probe.countOf(Probe));
        out.push(PickType.Ray);
        out.push(typeof Probe.hidden);
        out.push(Object.prototype.toString.call(Probe.echo(new Uint8Array([1, 2, 3]).buffer)));
        out.push("héllo ✓");
        print("printed", 1, { a: 1 });
        Probe.carry(Probe.echo({ nested: [1, "two", null, { three: true }], n: 2.5 }));
        Probe.carry(out);
    )";
    START_OR_SKIP(guest, source);
    QVERIFY2(guest.errors.isEmpty(), qPrintable(guest.errors.join(" | ")));
    QCOMPARE(guest.carried.size(), 2);
    QVariantMap echoed = guest.carried[0].toMap();
    QCOMPARE(echoed["n"].toDouble(), 2.5);
    QVariantList nested = echoed["nested"].toList();
    QCOMPARE(nested.size(), 4);
    QCOMPARE(nested[1].toString(), QString("two"));
    QCOMPARE(nested[3].toMap()["three"].toBool(), true);
    QVariantList out = guest.carried[1].toList();
    QCOMPARE(out.size(), 14);
    QCOMPARE(out[0].toInt(), 5);
    QCOMPARE(out[1].toString(), QString("ab"));
    QCOMPARE(out[2].toInt(), 7);
    QCOMPARE(out[3].toDouble(), 4.0);
    QCOMPARE(out[4].toBool(), true);
    QCOMPARE(out[5].toString(), guest.probe.label());
    QCOMPARE(out[6].toDouble(), -3.0);
    QCOMPARE(out[7].toString(), QString("a+b+c"));
    QCOMPARE(out[8].toString(), QString("{6ba7b810-9dad-11d1-80b4-00c04fd430c8}"));
    QCOMPARE(out[9].toInt(), 7);
    QCOMPARE(out[10].toInt(), 1);
    QCOMPARE(out[11].toString(), QString("undefined"));
    QCOMPARE(out[12].toString(), QString("[object ArrayBuffer]"));
    QCOMPARE(out[13].toString(), QString::fromUtf8("héllo ✓"));
    QVERIFY(guest.logged.contains("0 printed 1 [object Object]"));
    guest.stop();
}

void JavaScriptGuestTests::testSignalsCallbacksAndTimers() {
    Guest guest;
    const QString source = R"(
        var seen = [];
        function onCount(c) { seen.push("count " + c); }
        Probe.countChanged.connect(onCount);
        Probe.named.connect(function (name, flag) { seen.push(name + ":" + flag); });
        Probe.count = 3;                       // emitted inside the set: delivered re-entrantly
        Probe.send("x", true);
        Probe.countChanged.disconnect(onCount);
        Probe.count = 4;                       // not seen
        var t = Script.setTimeout(function () { seen.push("timer"); Probe.carry(seen); }, 10);
        var cleared = Script.setTimeout(function () { seen.push("never"); }, 10);
        Script.clearTimeout(cleared);
        Script.update.connect(function (dt) { seen.push("update " + dt); });
        var holder = { value: 0, method: function (c) { this.value = c; seen.push("this " + this.value); } };
        Probe.countChanged.connect(holder, holder.method);
        Probe.count = 5;
        Script.scriptEnding.connect(function () { Probe.carry("ending"); });
        Probe.carry(typeof t);
    )";
    START_OR_SKIP(guest, source);
    QVERIFY2(guest.errors.isEmpty(), qPrintable(guest.errors.join(" | ")));
    QCOMPARE(guest.carried.size(), 1);
    QCOMPARE(guest.carried[0].toString(), QString("number"));
    // the timer the script set: fire it as the engine would
    QCOMPARE(guest.script.timers.size(), 1);
    QString error;
    emit guest.script.update(0.5f);
    QVERIFY(guest.host->deliverCallback(guest.script.timers.begin().value().id, QVariantList(), error));
    QCOMPARE(guest.carried.size(), 2);
    QStringList seen = guest.carried[1].toStringList();
    QCOMPARE(seen, QStringList({ "count 3", "x:true", "this 5", "update 0.5", "timer" }));
    emit guest.script.scriptEnding();
    QCOMPARE(guest.carried.size(), 3);
    QCOMPARE(guest.carried[2].toString(), QString("ending"));
    QVERIFY2(guest.failures.isEmpty(), qPrintable(guest.failures.join(" | ")));
    guest.stop();
}

void JavaScriptGuestTests::testIncludeAndRequire() {
    Guest guest;
    guest.script.files["file:///test/lib/helper.js"] = "var helperLoaded = (typeof helperLoaded === 'number' ? helperLoaded : 0) + 1; var helperPath = Script.resolvePath('data.json');";
    guest.script.files["file:///test/lib/mod.js"] = "module.exports = { answer: 42, dir: __dirname, again: require('./other.js').twice };";
    guest.script.files["file:///test/lib/other.js"] = "exports.twice = require('./mod.js') ? 2 : 0; exports.self = module.filename;";
    guest.script.files["file:///test/lib/settings.json"] = "{ \"volume\": 0.5 }";
    const QString source = R"(
        Script.include("lib/helper.js");
        Script.include(["lib/helper.js"]);     // a second include of the same file is skipped
        var m = Script.require("./lib/mod.js");
        var j = Script.require("./lib/settings.json");
        var missing = null;
        try { Script.require("./lib/missing.js"); } catch (e) { missing = e.message; }
        Probe.carry([helperLoaded, helperPath, m.answer, m.dir, m.again, j.volume, missing, Script.resolvePath("x.js"),
                     Object.keys(Script.require.cache).length]);
    )";
    START_OR_SKIP(guest, source);
    QVERIFY2(guest.errors.isEmpty(), qPrintable(guest.errors.join(" | ")));
    QCOMPARE(guest.carried.size(), 1);
    QVariantList out = guest.carried[0].toList();
    QCOMPARE(out[0].toInt(), 1);
    QCOMPARE(out[1].toString(), QString("file:///test/lib/data.json"));  // resolved against the included file
    QCOMPARE(out[2].toInt(), 42);
    QCOMPARE(out[3].toString(), QString("file:///test/lib"));
    QCOMPARE(out[4].toInt(), 2);                                          // the cycle: other saw mod's partial exports
    QCOMPARE(out[5].toDouble(), 0.5);
    QVERIFY(out[6].toString().contains("missing.js"));
    QCOMPARE(out[7].toString(), QString("file:///test/x.js"));
    QCOMPARE(out[8].toInt(), 3);
    guest.stop();
}

void JavaScriptGuestTests::testErrorsAreReportedNotFatal() {
    Guest guest;
    const QString source = R"(
        var messages = [];
        try { Probe.add(); } catch (e) { messages.push(e.name + "|" + e.message); }
        try { Probe.label = "x"; messages.push("silent"); } catch (e) { messages.push("threw"); }
        try { Probe.nothing(); } catch (e) { messages.push(e instanceof TypeError); }
        Probe.bare.connect(function () { throw new Error("in a handler"); });
        Probe.ping();
        messages.push("after the handler");
        Probe.carry(messages);
        throw new Error("at the top level");
    )";
    START_OR_SKIP(guest, source);
    QCOMPARE(guest.carried.size(), 1);
    QVariantList messages = guest.carried[0].toList();
    QCOMPARE(messages.size(), 4);
    QVERIFY2(messages[0].toString().contains("add"), qPrintable(messages[0].toString()));
    QCOMPARE(messages[1].toString(), QString("silent"));
    QCOMPARE(messages[2].toBool(), true);
    QCOMPARE(messages[3].toString(), QString("after the handler"));
    QCOMPARE(guest.errors.size(), 2);
    QVERIFY2(guest.errors[0].contains("in a handler"), qPrintable(guest.errors[0]));
    QVERIFY2(guest.errors[1].contains("at the top level") && guest.errors[1].contains("main.js"), qPrintable(guest.errors[1]));
    QVERIFY(guest.failures.isEmpty());
    guest.stop();
}

void JavaScriptGuestTests::testApiDump() {
    Guest guest;
    const QString source = R"(Script.dumpApi("/nowhere/api.json");)";
    START_OR_SKIP(guest, source);
    QVERIFY2(guest.errors.isEmpty(), qPrintable(guest.errors.join(" | ")));
    QCOMPARE(guest.script.dumpPath, QString("/nowhere/api.json"));
    QVariantMap probe = guest.script.dumped["Probe"].toMap();
    QCOMPARE(probe["kind"].toString(), QString("qobject"));
    QCOMPARE(probe["meta"].toMap()["class"].toString(), QString("Probe"));
    QCOMPARE(guest.script.dumped["XMLHttpRequest"].toMap()["kind"].toString(), QString("function"));
    QCOMPARE(guest.script.dumped["Math"].toMap()["kind"].toString(), QString("object"));
    QCOMPARE(guest.script.dumped["PickType"].toMap()["members"].toMap()["Ray"].toString(), QString("number"));
    guest.stop();
}

// Tens of thousands of calls with maps crossing both ways, as a script's hot loop makes them: the guest's heap
// must stay sound and its memory bounded.
void JavaScriptGuestTests::testManyCalls() {
    Guest guest;
    const QString source = R"(
        var s = 0, i, m;
        for (i = 0; i < 30000; i++) {
            m = Probe.echo({ position: { x: i, y: 2.5, z: -3 }, name: "box-" + i, list: [1, 2, 3], nested: { deep: { er: true } } });
            s += m.position.y + m.list.length;
            Probe.count = i;
            s += Probe.count;
            Probe.position = m.position;
            s += Probe.describeSelf().count;
        }
        Probe.carry([s, Probe.position.x]);
    )";
    START_OR_SKIP(guest, source);
    QVERIFY2(guest.errors.isEmpty(), qPrintable(guest.errors.join(" | ")));
    QVERIFY2(guest.failures.isEmpty(), qPrintable(guest.failures.join(" | ")));
    QCOMPARE(guest.carried.size(), 1);
    QVariantList out = guest.carried[0].toList();
    QCOMPARE(out[1].toDouble(), 29999.0);
    guest.stop();
}

// The entity-script contract: a script loaded for two entities (one object each, the code shared), its
// methods called with the entity's id first, a remote call filtered by remotelyCallable, timers cleared at
// unload, and a script that is not a function refused with an error record.
void JavaScriptGuestTests::testEntityScripts() {
    Guest guest;
    START_OR_SKIP(guest, "");
    QString error;
    const QString script = R"((function () {
        var self = this;
        this.remotelyCallable = ["ping"];
        this.preload = function (id) { self.id = id; Probe.carry("preload " + id); Script.setTimeout(function () { Probe.carry("timer of " + id); }, 10); };
        this.ping = function (id, params) { Probe.carry("ping " + id + " " + params.join(",") + " from " + Script.remoteCallerID); return "pong"; };
        this.secret = function (id) { Probe.carry("secret " + id); };
        this.unload = function (id) { Probe.carry("unload " + id); };
    }))";
    auto payload = [](const QString& entity, const QString& method, const QVariantList& args, const QString& caller = QString()) {
        QVariantMap map;
        map["entity"] = entity;
        map["method"] = method;
        map["args"] = args;
        if (!caller.isEmpty()) {
            map["remoteCallerID"] = caller;
        }
        return map;
    };
    QVariant result;
    for (const QString& entity : { "{aaaaaaaa-0000-0000-0000-000000000001}", "{aaaaaaaa-0000-0000-0000-000000000002}" }) {
        QVariantMap load;
        load["entity"] = entity;
        load["url"] = "file:///test/entity.js";
        load["source"] = script;
        QVERIFY2(guest.host->callWithPayload("entity_load", load, result, error), qPrintable(error));
        QVERIFY2(!HostInterface::isErrorRecord(result), qPrintable(result.toString()));
        QVERIFY(guest.host->callWithPayload("entity_call", payload(entity, "preload", QVariantList()), result, error));
    }
    QCOMPARE(guest.carried.size(), 2);
    QCOMPARE(guest.carried[0].toString(), QString("preload {aaaaaaaa-0000-0000-0000-000000000001}"));
    QCOMPARE(guest.script.timers.size(), 2);
    // has: a method that exists, one that does not
    QVERIFY(guest.host->callWithPayload("entity_has", payload("{aaaaaaaa-0000-0000-0000-000000000001}", "ping", QVariantList()), result, error));
    QCOMPARE(result.toInt(), 1);
    QVERIFY(guest.host->callWithPayload("entity_has", payload("{aaaaaaaa-0000-0000-0000-000000000001}", "nothing", QVariantList()), result, error));
    QCOMPARE(result.toInt(), 0);
    // a remote call of a remotely callable method, with the string list the server hands over, and of one that is not
    QVERIFY(guest.host->callWithPayload("entity_call", payload("{aaaaaaaa-0000-0000-0000-000000000002}", "ping", QVariantList { QStringList { "a", "b" } },
                                                              "{cccccccc-0000-0000-0000-000000000003}"), result, error));
    QCOMPARE(result.toString(), QString("pong"));
    QCOMPARE(guest.carried.last().toString(), QString("ping {aaaaaaaa-0000-0000-0000-000000000002} a,b from {cccccccc-0000-0000-0000-000000000003}"));
    QVERIFY(guest.host->callWithPayload("entity_call", payload("{aaaaaaaa-0000-0000-0000-000000000002}", "secret", QVariantList(), "{cccccccc-0000-0000-0000-000000000003}"), result, error));
    QCOMPARE(guest.carried.size(), 3);  // not called
    QVERIFY(guest.logged.last().contains("not remotely callable"));
    // a local call of it is fine
    QVERIFY(guest.host->callWithPayload("entity_call", payload("{aaaaaaaa-0000-0000-0000-000000000002}", "secret", QVariantList()), result, error));
    QCOMPARE(guest.carried.last().toString(), QString("secret {aaaaaaaa-0000-0000-0000-000000000002}"));
    // unload: the method, then the record and the timers of that entity go
    QVERIFY(guest.host->callWithPayload("entity_call", payload("{aaaaaaaa-0000-0000-0000-000000000001}", "unload", QVariantList()), result, error));
    QVariantMap unload;
    unload["entity"] = "{aaaaaaaa-0000-0000-0000-000000000001}";
    QVERIFY(guest.host->callWithPayload("entity_unload", unload, result, error));
    QCOMPARE(guest.carried.last().toString(), QString("unload {aaaaaaaa-0000-0000-0000-000000000001}"));
    QCOMPARE(guest.script.timers.size(), 1);
    QVERIFY(guest.host->callWithPayload("entity_has", payload("{aaaaaaaa-0000-0000-0000-000000000001}", "ping", QVariantList()), result, error));
    QCOMPARE(result.toInt(), 0);
    // a script that is not a function
    QVariantMap bad;
    bad["entity"] = "{aaaaaaaa-0000-0000-0000-000000000009}";
    bad["url"] = "file:///test/bad.js";
    bad["source"] = "var x = 1;";
    QVERIFY(guest.host->callWithPayload("entity_load", bad, result, error));
    QVERIFY(HostInterface::isErrorRecord(result));
    QVERIFY2(result.toMap()["@error"].toMap()["message"].toString().contains("expected a function"), qPrintable(result.toMap()["@error"].toMap()["message"].toString()));
    guest.stop();
}

#include "moc_TestObjects.cpp"

void JavaScriptGuestTests::testObjectsAreReleased() {
    // a type whose objects the script owns, as the engine registers the QtScript engine's ScriptOwnership types
    ValueCodec::registerType(qRegisterMetaType<Other*>(),
        [](const ValueCodec& codec, const QVariant& value) { return codec.toCborOwned(value.value<Other*>()); },
        [](const ValueCodec& codec, const QCborValue& value, QVariant& out) {
            out = QVariant::fromValue(qobject_cast<Other*>(codec.fromCbor(value).value<QObject*>()));
            return true;
        });
    const int aliveBefore = Other::alive;
    Guest guest;
    const QString source = R"(
        var kept = Probe.makeOther();
        for (var i = 0; i < 200; i++) {
            Probe.makeOther();
        }
        gc();
        Probe.carry([typeof kept, typeof Probe.add, Probe.add(1, 2)]);
    )";
    START_OR_SKIP(guest, source);
    QVERIFY2(guest.errors.isEmpty(), qPrintable(guest.errors.join(" | ")));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    // the 200 dropped objects were released and deleted; the kept one and the globals stay
    QCOMPARE(Other::alive - aliveBefore, 1);
    QCOMPARE(guest.objects.size(), 4);
    QCOMPARE(guest.carried.size(), 1);
    QCOMPARE(guest.carried[0].toList()[0].toString(), QString("object"));
    QCOMPARE(guest.carried[0].toList()[2].toInt(), 3);
    // what the script still owns goes when it stops, as the engine releases it
    guest.stop();
    guest.objects.releaseOwned();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(Other::alive, aliveBefore);
}

int FakeRequest::alive = 0;

// The guest's XMLHttpRequest lets a request go once it is done: two hundred fire-and-forget posts leave nothing behind
// in the guest or the host. A request's signal connections kept it reachable, so the host never deleted it; a body
// that posts what it hears ran out of memory on both sides in two hours (2026-10-25).
void JavaScriptGuestTests::testRequestsAreReleased() {
    ValueCodec::registerType(qRegisterMetaType<FakeRequest*>(),
        [](const ValueCodec& codec, const QVariant& value) { return codec.toCborOwned(value.value<FakeRequest*>()); },
        [](const ValueCodec& codec, const QCborValue& value, QVariant& out) {
            out = QVariant::fromValue(qobject_cast<FakeRequest*>(codec.fromCbor(value).value<QObject*>()));
            return true;
        });
    const int aliveBefore = FakeRequest::alive;
    Guest guest;
    FakeHttp http;
    guest.extraGlobals.insert("Http", &http);
    const QString source = R"(
        (function () {
            for (var i = 0; i < 200; i++) {
                var request = new XMLHttpRequest();
                request.open("POST", "http://127.0.0.1:9/hear", true);
                request.send(new ArrayBuffer(16000));
            }
        })();
        var kept = new XMLHttpRequest();
        kept.onreadystatechange = function () {
            if (kept.readyState === kept.DONE) { Probe.carry(["kept", kept.responseText]); }
        };
        kept.open("GET", "http://127.0.0.1:9/kept", true);
        kept.send();
        Script.setTimeout(function () { gc(); }, 0);
    )";
    START_OR_SKIP(guest, source);
    QVERIFY2(guest.errors.isEmpty(), qPrintable(guest.errors.join(" | ")));
    QCOMPARE(http.made.size(), 201);
    // the replies come
    for (auto& request : http.made) {
        QVERIFY(request && request->sent);
        request->finish();
    }
    QCOMPARE(guest.carried.size(), 1);
    QCOMPARE(guest.carried[0].toList()[1].toString(), QString("ok"));
    // the script's timer, fired as the engine would: it collects the garbage, and the guest lets the requests go
    QString error;
    QCOMPARE(guest.script.timers.size(), 1);
    QVERIFY2(guest.host->deliverCallback(guest.script.timers.begin().value().id, QVariantList(), error), qPrintable(error));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    // the 200 dropped requests were released and deleted; the kept one stays
    QCOMPARE(FakeRequest::alive - aliveBefore, 1);
    guest.stop();
    guest.objects.releaseOwned();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(FakeRequest::alive, aliveBefore);
}

void JavaScriptGuestTests::testScopedCallbacks() {
    Guest guest;
    const QString source = R"(
        var scope = {
            name: "scope",
            done: function (error, result) { Probe.carry(["method", this.name, error, result]); }
        };
        Assets.getAsset({ url: "atp:/a.txt" }, function (error, result) { Probe.carry(["plain", error, result]); });
        Assets.getAsset("atp:/b.txt", scope, "done");
        Assets.getAsset("atp:/c.txt", scope, function (error, result) { Probe.carry(["function", this.name, error, result]); });
        Assets.getAsset("atp:/d.txt", { scope: scope, callback: scope.done });
        Assets.getCacheStatus(scope, "done");
        Assets.saveToCache("cache:/e", "data", {}, scope, "done");
        Assets.saveToCache({ url: "cache:/f", data: "data" }, function (error, result) { Probe.carry(["options", error, result]); });
        Assets.uploadData("text", function (url, hash) { Probe.carry(["upload", this === Assets, url, hash]); });
    )";
    START_OR_SKIP(guest, source);
    QVERIFY2(guest.errors.isEmpty(), qPrintable(guest.errors.join(" | ")));
    const auto& requests = guest.assets.requests;
    QCOMPARE(requests.size(), 8);
    QCOMPARE(requests[0].values[0].toMap()["url"].toString(), QString("atp:/a.txt"));
    QCOMPARE(requests[1].values[0].toString(), QString("atp:/b.txt"));
    QCOMPARE(requests[5].values.size(), 3);  // the long form of saveToCache
    QCOMPARE(requests[5].values[0].toUrl(), QUrl("cache:/e"));
    QCOMPARE(requests[6].values.size(), 1);  // the options form
    for (const auto& request : requests) {
        QVERIFY2(request.callback.isValid(), qPrintable(request.method));
    }
    // the outcomes, as the engine posts them: (error or null, result)
    QString error;
    QVariantMap result { { "ok", 1 } };
    for (int i = 0; i < 7; ++i) {
        QVERIFY(guest.host->deliverCallback(requests[i].callback.id, { QVariant::fromValue(nullptr), result }, error));
    }
    QVERIFY(guest.host->deliverCallback(requests[7].callback.id, { "atp:abc", "abc" }, error));
    QCOMPARE(guest.carried.size(), 8);
    QCOMPARE(guest.carried[0].toList()[0].toString(), QString("plain"));
    QVERIFY(guest.carried[0].toList()[1].isNull());
    QCOMPARE(guest.carried[0].toList()[2].toMap()["ok"].toInt(), 1);
    for (int i : { 1, 3, 4, 5 }) {
        QCOMPARE(guest.carried[i].toList()[0].toString(), QString("method"));
        QCOMPARE(guest.carried[i].toList()[1].toString(), QString("scope"));
    }
    QCOMPARE(guest.carried[2].toList()[0].toString(), QString("function"));
    QCOMPARE(guest.carried[2].toList()[1].toString(), QString("scope"));
    QCOMPARE(guest.carried[6].toList()[0].toString(), QString("options"));
    QCOMPARE(guest.carried[7].toList(), QVariantList({ "upload", true, "atp:abc", "abc" }));
    // each callback runs once: a second outcome finds it forgotten
    QVERIFY(guest.host->deliverCallback(requests[0].callback.id, { QVariant::fromValue(nullptr), result }, error));
    QCOMPARE(guest.carried.size(), 8);
    guest.stop();
}

// A handler that never returns. The guest runs without deadline checks in its loops, so QuickJS's interrupt
// handler asks the host whether to stop: the call ends soon after its time limit or a stop, the reason is the
// host's to report (the guest logs the interruption at the debug level only), and after a time limit the script
// runs on.
void JavaScriptGuestTests::testRunawayLoopIsInterrupted() {
    {
        Guest guest;
        guest.callTimeout = std::chrono::milliseconds(100);
        START_OR_SKIP(guest, R"(
            Probe.bare.connect(function () { var n = 0; while (true) { n++; } });
            Probe.named.connect(function (name) { Probe.carry(name); });
        )");
        QElapsedTimer timer;
        timer.start();
        guest.probe.ping();
        qint64 elapsed = timer.elapsed();
        QVERIFY2(elapsed >= 90 && elapsed < 2000, qPrintable(QString("took %1 ms").arg(elapsed)));
        QVERIFY2(guest.failures.join(" ").contains("limit of 100 ms"), qPrintable(guest.failures.join(" | ")));
        QVERIFY2(guest.errors.isEmpty(), qPrintable(guest.errors.join(" | ")));
        guest.probe.send("after", true);
        QCOMPARE(guest.carried.size(), 1);
        QCOMPARE(guest.carried[0].toString(), QString("after"));
        guest.stop();
    }
    {
        Guest guest;
        guest.callTimeout = std::chrono::milliseconds(0); // no limit, as JavaScript runs in the client
        START_OR_SKIP(guest, R"( Probe.bare.connect(function () { while (true) {} }); )");
        WasmInstance* instance = guest.instance.get();
        std::thread stopper([instance] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            instance->requestStop();
        });
        QElapsedTimer timer;
        timer.start();
        guest.probe.ping();
        qint64 elapsed = timer.elapsed();
        stopper.join();
        QVERIFY2(elapsed >= 40 && elapsed < 2000, qPrintable(QString("took %1 ms").arg(elapsed)));
        QVERIFY2(guest.failures.join(" ").contains("stopped"), qPrintable(guest.failures.join(" | ")));
    }
}

// The time limit per entity script: the guest hosts every entity script of a world, so when one runs away in a
// method, a timer or a signal handler, the call ends at the limit naming that entity (the shim knows which
// script is running, for its callbacks and handlers too), the guest is whole, the other scripts' calls work,
// and the state the unwinding skipped (Script.remoteCallerID, the running entity) is reset. After its unload
// the runaway's handlers fire no more.
void JavaScriptGuestTests::testRunawayEntityScriptIsNamed() {
    Guest guest;
    // a limit long enough that a busy machine's preemption does not reach it during the short calls (the clock
    // is the module's own time, wall time of its run)
    guest.callTimeout = std::chrono::milliseconds(250);
    guest.nameTimeLimits = true;
    START_OR_SKIP(guest, "");
    const QString script = R"((function () {
        var self = this;
        this.remotelyCallable = ["spin", "ping"];
        this.preload = function (id) {
            self.id = id;
            Probe.bare.connect(function () { if (self.armed) { while (true) {} } Probe.carry("signal to " + id); });
        };
        this.spin = function (id) { while (true) {} };
        this.later = function (id) { Script.setTimeout(function () { while (true) {} }, 10); };
        this.arm = function (id) { self.armed = true; };
        this.ping = function (id) { Probe.carry("ping " + id + " caller " + Script.remoteCallerID); return "pong"; };
        this.unload = function (id) { Probe.carry("unload " + id); };
    }))";
    const QString A = "{aaaaaaaa-0000-0000-0000-00000000000a}";
    const QString B = "{bbbbbbbb-0000-0000-0000-00000000000b}";
    auto payload = [](const QString& entity, const QString& method, const QString& caller = QString()) {
        QVariantMap map;
        map["entity"] = entity;
        map["method"] = method;
        map["args"] = QVariantList();
        if (!caller.isEmpty()) {
            map["remoteCallerID"] = caller;
        }
        return map;
    };
    QVariant result;
    QString error;
    for (const QString& entity : { A, B }) {
        QVariantMap load;
        load["entity"] = entity;
        load["url"] = "file:///test/runaway.js";
        load["source"] = script;
        QVERIFY2(guest.host->callWithPayload("entity_load", load, result, error), qPrintable(error));
        QVERIFY2(!HostInterface::isErrorRecord(result), qPrintable(result.toString()));
        QVERIFY(guest.host->callWithPayload("entity_call", payload(entity, "preload"), result, error));
    }

    // a method that never returns, called from another client: the call ends at the limit, naming A
    QElapsedTimer timer;
    timer.start();
    QVERIFY(!guest.host->callWithPayload("entity_call", payload(A, "spin", "{cccccccc-0000-0000-0000-00000000000c}"), result, error));
    qint64 elapsed = timer.elapsed();
    QVERIFY2(elapsed >= 230 && elapsed < 3000, qPrintable(QString("took %1 ms").arg(elapsed)));
    QVERIFY2(error.contains("limit of 250 ms"), qPrintable(error));
    QCOMPARE(guest.timeLimits.size(), 1);
    QVERIFY2(guest.timeLimits[0].startsWith("entity_call " + A + " "), qPrintable(guest.timeLimits[0]));
    QVERIFY2(guest.failures.isEmpty(), qPrintable(guest.failures.join(" | ")));
    QVERIFY2(guest.errors.isEmpty(), qPrintable(guest.errors.join(" | ")));

    // the guest is whole, the other script answers, and the remote caller of the interrupted call is forgotten
    error.clear();
    QVERIFY2(guest.host->callWithPayload("entity_call", payload(B, "ping"), result, error),
             qPrintable(error + " | logged: " + guest.logged.join(" | ")));
    QCOMPARE(result.toString(), QString("pong"));
    QCOMPARE(guest.carried.size(), 1);
    QCOMPARE(guest.carried[0].toString(), "ping " + B + " caller undefined");

    // a timer A set runs away: the callback is A's
    QVERIFY(guest.host->callWithPayload("entity_call", payload(A, "later"), result, error));
    QCOMPARE(guest.script.timers.size(), 1);
    const int timerCallback = guest.script.timers.begin().value().id;
    QVERIFY(!guest.host->callCallback(timerCallback, QVariantList(), result, error));
    QVERIFY2(error.contains("limit of 250 ms"), qPrintable(error));
    QCOMPARE(guest.timeLimits.size(), 2);
    QVERIFY2(guest.timeLimits[1].startsWith("call_callback " + A + " "), qPrintable(guest.timeLimits[1]));

    // a signal handler A connected runs away: the handler is A's; B's handler on the same signal still ran or
    // runs on the next emission
    QVERIFY(guest.host->callWithPayload("entity_call", payload(A, "arm"), result, error));
    guest.carried.clear();
    guest.probe.ping();
    QCOMPARE(guest.timeLimits.size(), 3);
    QVERIFY2(guest.timeLimits[2].startsWith("on_signal " + A + " "), qPrintable(guest.timeLimits[2]));
    QVERIFY2(guest.failures.isEmpty(), qPrintable(guest.failures.join(" | ")));

    // A unloaded by the host: its handlers and callbacks go; B's handler runs alone
    QVariantMap unload;
    unload["entity"] = A;
    QVERIFY(guest.host->callWithPayload("entity_unload", unload, result, error));
    guest.carried.clear();
    guest.probe.ping();
    QCOMPARE(guest.timeLimits.size(), 3);
    QCOMPARE(guest.carried.size(), 1);
    QCOMPARE(guest.carried[0].toString(), "signal to " + B);
    QVariantMap has;
    has["entity"] = A;
    has["method"] = "spin";
    QVERIFY(guest.host->callWithPayload("entity_has", has, result, error));
    QCOMPARE(result.toInt(), 0);
    QVERIFY2(guest.errors.isEmpty(), qPrintable(guest.errors.join(" | ")));
    guest.stop();
}

// What a script sees as QtScript showed it: an API object prints as "Class(name = "objectName")", a call with too few
// arguments throws a real SyntaxError and a call whose arguments will not convert a real TypeError (instanceof works).
void JavaScriptGuestTests::testErrorsAndPrintedNamesAsOnQtScript() {
    Guest guest;
    guest.probe.setObjectName("the probe");
    START_OR_SKIP(guest, R"(
        var out = [String(Probe), "" + Assets];
        try { Probe.add(1); } catch (e) { out.push(e.name, e instanceof SyntaxError, e.message.indexOf("arguments") >= 0); }
        try { Probe.scale(true, {}); } catch (e) { out.push(e.name, e instanceof TypeError); }
        Probe.carry(out);
    )");
    QVERIFY2(guest.errors.isEmpty(), qPrintable(guest.errors.join(" | ")));
    QCOMPARE(guest.carried.size(), 1);
    const QVariantList out = guest.carried[0].toList();
    QCOMPARE(out.size(), 7);
    QCOMPARE(out[0].toString(), QString("Probe(name = \"the probe\")"));
    QCOMPARE(out[1].toString(), QString("AssetScriptingInterface(name = \"\")"));
    QCOMPARE(out[2].toString(), QString("SyntaxError"));
    QCOMPARE(out[3].toBool(), true);
    QCOMPARE(out[4].toBool(), true);
    QCOMPARE(out[5].toString(), QString("TypeError"));
    QCOMPARE(out[6].toBool(), true);
    guest.stop();
}

void JavaScriptGuestTests::testConsoleLines() {
    // the scripting console's lines (console_eval, revision 6): in the script's global scope, what a line declares
    // stays for the next, a throw is shown as an error and the script goes on
    Guest guest;
    START_OR_SKIP(guest, "var fromTheScript = 40;");
    if (!guest.instance->hasExport("console_eval")) {
        QSKIP("this guest is from before revision 6 (no console_eval)");
    }
    auto line = [&guest](const QString& code) {
        QVariant result;
        QString error;
        if (!guest.host->callWithPayload("console_eval", QVariant(code), result, error)) {
            return QVariantMap { { "text", "the call failed: " + error }, { "error", true } };
        }
        return result.toMap();
    };
    QVariantMap shown = line("fromTheScript + 2");
    QCOMPARE(shown.value("text").toString(), QString("42"));
    QCOMPARE(shown.value("error").toBool(), false);
    shown = line("var typed = 'kept'; function twice(x) { return 2 * x; }");
    QCOMPARE(shown.value("text").toString(), QString("undefined"));
    QCOMPARE(line("typed + ' ' + twice(21)").value("text").toString(), QString("kept 42"));
    shown = line("throw new Error('typed wrong')");
    QCOMPARE(shown.value("error").toBool(), true);
    QCOMPARE(shown.value("text").toString(), QString("Error: typed wrong"));
    shown = line("}{");
    QVERIFY(shown.value("error").toBool());
    QVERIFY2(shown.value("text").toString().startsWith("SyntaxError"), qPrintable(shown.value("text").toString()));
    // the host's objects, as the script reaches them
    shown = line("typeof Probe.ping");
    QCOMPARE(shown.value("text").toString(), QString("function"));
    QCOMPARE(line("fromTheScript").value("text").toString(), QString("40"));
    QVERIFY2(guest.failures.isEmpty(), qPrintable(guest.failures.join(" | ")));
    guest.stop();
}
