//
//  HostInterfaceTests.cpp
//  tests/wasm-scripting/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "HostInterfaceTests.h"


#include <QtCore/QCborValue>

#include <HostInterface.h>
#include <ScriptCaller.h>
#include <ObjectTable.h>
#include <SignalRelay.h>
#include <ValueCodec.h>
#include <WasmRuntime.h>

#include "TestObjects.h"

QTEST_GUILESS_MAIN(HostInterfaceTests)

namespace {

// A module that forwards each import to an export of the same name, with a bump allocator, and records what
// the host delivers through on_signal and on_callback (the last connection or callback id and where its
// arguments were placed), so the test can read them back. set_fail(1) makes on_signal trap.
const QString MODULE = R"((module
  (import "darkride" "version" (func $version (result i32)))
  (import "darkride" "globals" (func $globals (result i64)))
  (import "darkride" "describe" (func $describe (param i32 i32) (result i64)))
  (import "darkride" "call" (func $call (param i32 i32 i32 i32 i32) (result i64)))
  (import "darkride" "get" (func $get (param i32 i32 i32) (result i64)))
  (import "darkride" "set" (func $set (param i32 i32 i32 i32 i32) (result i64)))
  (import "darkride" "connect" (func $connect (param i32 i32 i32) (result i32)))
  (import "darkride" "disconnect" (func $disconnect (param i32)))
  (import "darkride" "release" (func $release (param i32)))
  (import "darkride" "log" (func $log (param i32 i32 i32)))
  (memory (export "memory") 2)
  (global $next (mut i32) (i32.const 65536))
  (global $events (mut i32) (i32.const 0))
  (global $last_id (mut i32) (i32.const 0))
  (global $last_ptr (mut i32) (i32.const 0))
  (global $last_len (mut i32) (i32.const 0))
  (global $fail (mut i32) (i32.const 0))
  (global $started (mut i32) (i32.const 0))
  (func (export "alloc") (param $size i32) (result i32)
    (local $ptr i32)
    global.get $next
    local.set $ptr
    global.get $next
    local.get $size
    i32.add
    i32.const 7
    i32.add
    i32.const -8
    i32.and
    global.set $next
    local.get $ptr)
  (func (export "free") (param i32 i32))
  (func (export "script_start") i32.const 1 global.set $started)
  (func (export "script_stop") i32.const 2 global.set $started)
  (func (export "version") (result i32) call $version)
  (func (export "globals") (result i64) call $globals)
  (func (export "describe") (param i32 i32) (result i64) local.get 0 local.get 1 call $describe)
  (func (export "call") (param i32 i32 i32 i32 i32) (result i64)
    local.get 0 local.get 1 local.get 2 local.get 3 local.get 4 call $call)
  (func (export "get") (param i32 i32 i32) (result i64) local.get 0 local.get 1 local.get 2 call $get)
  (func (export "set") (param i32 i32 i32 i32 i32) (result i64)
    local.get 0 local.get 1 local.get 2 local.get 3 local.get 4 call $set)
  (func (export "connect") (param i32 i32 i32) (result i32) local.get 0 local.get 1 local.get 2 call $connect)
  (func (export "disconnect") (param i32) local.get 0 call $disconnect)
  (func (export "release") (param i32) local.get 0 call $release)
  (func (export "log") (param i32 i32 i32) local.get 0 local.get 1 local.get 2 call $log)
  (func $record (param i32 i32 i32)
    global.get $events i32.const 1 i32.add global.set $events
    local.get 0 global.set $last_id
    local.get 1 global.set $last_ptr
    local.get 2 global.set $last_len)
  (func (export "on_signal") (param i32 i32 i32)
    (if (global.get $fail) (then unreachable))
    local.get 0 local.get 1 local.get 2 call $record)
  (func (export "on_callback") (param i32 i32 i32) local.get 0 local.get 1 local.get 2 call $record)
  (func (export "set_fail") (param i32) local.get 0 global.set $fail)
  (func (export "events") (result i32) global.get $events)
  (func (export "last_id") (result i32) global.get $last_id)
  (func (export "last_ptr") (result i32) global.get $last_ptr)
  (func (export "last_len") (result i32) global.get $last_len)
  (func (export "started") (result i32) global.get $started)))";

const int32_t SCRATCH = 1024;  // where the test places its own bytes in the module's memory

// Everything a test needs: the module running with the host interface over a Probe registered as "Probe".
struct Fixture {
    std::shared_ptr<WasmRuntime> runtime { std::make_shared<WasmRuntime>() };
    std::shared_ptr<WasmModule> module;
    std::unique_ptr<WasmInstance> instance;
    ObjectTable objects;
    ValueCodec codec { &objects };
    SignalRelay relay;
    std::unique_ptr<HostInterface> host;
    Probe probe;
    QStringList logged;
    QStringList failures;
    int32_t scratch { SCRATCH };

    bool start(QString& error) {
        module = runtime->compileText(MODULE, error);
        if (!module) {
            return false;
        }
        instance = std::make_unique<WasmInstance>(module);
        host = std::make_unique<HostInterface>(*instance, objects, codec, relay);
        host->setLogger([this](int level, const QString& text) { logged.append(QString("%1:%2").arg(level).arg(text)); });
        host->setFailureHandler([this](const QString& entry, const QString& failure) { failures.append(entry + ": " + failure); });
        host->install();
        if (!instance->instantiate(error)) {
            return false;
        }
        objects.registerGlobal("Probe", &probe);
        return true;
    }

    // Places a value in the module's memory (as the guest would have), returning (ptr, len).
    QPair<int32_t, int32_t> place(const QVariant& value) {
        QByteArray bytes = codec.encode(value);
        int32_t pointer = scratch;
        scratch += int32_t(bytes.size()) + 8;
        instance->writeMemory(uint64_t(pointer), bytes);
        return qMakePair(pointer, int32_t(bytes.size()));
    }
    QPair<int32_t, int32_t> placeText(const QString& text) {
        QByteArray bytes = text.toUtf8();
        int32_t pointer = scratch;
        scratch += int32_t(bytes.size()) + 8;
        instance->writeMemory(uint64_t(pointer), bytes);
        return qMakePair(pointer, int32_t(bytes.size()));
    }
    QVariant unpack(int64_t packed) {
        if (packed == 0) {
            return QVariant();
        }
        int32_t pointer = int32_t(uint32_t(uint64_t(packed) >> 32));
        int32_t length = int32_t(uint32_t(uint64_t(packed) & 0xffffffffu));
        QVariant out;
        QString error;
        if (!codec.decode(instance->readMemory(uint64_t(pointer), size_t(length)), out, error)) {
            qWarning() << "result is not CBOR:" << error;
        }
        return out;
    }
    int32_t i32(const QString& name) {
        std::vector<WasmValue> results;
        QString error;
        if (!instance->call(name, {}, results, error)) {
            qWarning() << name << "failed:" << error;
            return -1;
        }
        return std::get<int32_t>(results[0]);
    }
    int64_t i64(const QString& name, const std::vector<WasmValue>& args) {
        std::vector<WasmValue> results;
        QString error;
        if (!instance->call(name, args, results, error)) {
            qWarning() << name << "failed:" << error;
            return 0;
        }
        return std::get<int64_t>(results[0]);
    }
    QVariant call(int32_t handle, const QString& method, const QVariantList& arguments) {
        auto name = placeText(method);
        auto args = place(arguments);
        return unpack(i64("call", { handle, name.first, name.second, args.first, args.second }));
    }
    QVariant get(int32_t handle, const QString& property) {
        auto name = placeText(property);
        return unpack(i64("get", { handle, name.first, name.second }));
    }
    QVariant set(int32_t handle, const QString& property, const QVariant& value) {
        auto name = placeText(property);
        auto bytes = place(value);
        return unpack(i64("set", { handle, name.first, name.second, bytes.first, bytes.second }));
    }
    int32_t connect(int32_t handle, const QString& signal) {
        auto name = placeText(signal);
        std::vector<WasmValue> results;
        QString error;
        if (!instance->call("connect", { handle, name.first, name.second }, results, error)) {
            qWarning() << "connect failed:" << error;
            return 0;
        }
        return std::get<int32_t>(results[0]);
    }
    QVariant lastArguments() {
        int32_t pointer = i32("last_ptr");
        int32_t length = i32("last_len");
        QVariant out;
        QString error;
        codec.decode(instance->readMemory(uint64_t(pointer), size_t(length)), out, error);
        return out;
    }
    int32_t probeHandle() { return objects.global("Probe"); }
};

QString errorMessage(const QVariant& result) {
    return result.toMap().value(HostInterface::ERROR_KEY).toMap().value("message").toString();
}

} // namespace

void HostInterfaceTests::testVersionAndGlobals() {
    Fixture f;
    QString error;
    QVERIFY2(f.start(error), qPrintable(error));
    QCOMPARE(f.i32("version"), HostInterface::ABI_VERSION);
    QVariantMap globals = f.unpack(f.i64("globals", {})).toMap();
    QCOMPARE(globals.size(), 1);
    QCOMPARE(globals.value("Probe").toInt(), f.probeHandle());
    QCOMPARE(f.i32("started"), 0);
    QVERIFY(f.host->callScriptStart(error));
    QCOMPARE(f.i32("started"), 1);
    QVERIFY(f.host->callScriptStop(error));
    QCOMPARE(f.i32("started"), 2);
}

void HostInterfaceTests::testDescribe() {
    Fixture f;
    QString error;
    QVERIFY2(f.start(error), qPrintable(error));
    auto name = f.place(QVariant("Probe"));
    QVariant description = f.unpack(f.i64("describe", { name.first, name.second }));
    QCOMPARE(description.toMap().value("class").toString(), QString("Probe"));
    QVERIFY(!description.toMap().value("methods").toList().isEmpty());
    // by handle record, as a method result would give it
    QVariantMap record;
    record[ValueCodec::OBJECT_KEY] = f.probeHandle();
    auto handle = f.place(record);
    QCOMPARE(f.unpack(f.i64("describe", { handle.first, handle.second })).toMap().value("class").toString(), QString("Probe"));
    // unknown: null
    auto unknown = f.place(QVariant("Nothing"));
    QCOMPARE(f.unpack(f.i64("describe", { unknown.first, unknown.second })).userType(), int(QMetaType::Nullptr));
    QVERIFY(f.host->statistics().bytesToGuest > 0);
}

void HostInterfaceTests::testCall() {
    Fixture f;
    QString error;
    QVERIFY2(f.start(error), qPrintable(error));
    const int32_t probe = f.probeHandle();
    // overloads by argument type
    QCOMPARE(f.call(probe, "add", { 2, 3 }), QVariant(5));
    QCOMPARE(f.call(probe, "add", { "a", "b" }), QVariant("ab"));
    // by signature and by index
    QCOMPARE(f.call(probe, "add(QString,QString)", { 1, 2 }), QVariant("12"));
    int index = Probe::staticMetaObject.indexOfMethod("add(int,int)");
    QCOMPARE(f.call(probe, QString("#%1").arg(index), { 2.9, 3 }), QVariant(5));
    // a struct in and out, a slot, a property-backed value
    QVariantMap v;
    v["x"] = 1;
    v["y"] = 2;
    v["z"] = 3;
    QVariant scaled = f.call(probe, "scale", { v, 2 });
    QCOMPARE(scaled.toMap().value("x").toDouble(), 2.0);
    QCOMPARE(scaled.toMap().value("z").toDouble(), 6.0);
    QVERIFY(f.call(probe, "setCount", { 9 }).userType() == QMetaType::UnknownType);  // void: undefined
    QCOMPARE(f.probe.count(), 9);
    // a QVariant parameter and result, a QStringList, a QUuid
    QCOMPARE(f.call(probe, "echo", { "text" }), QVariant("text"));
    QCOMPARE(f.call(probe, "echo", { 2.5 }), QVariant(2.5));
    QCOMPARE(f.call(probe, "join", { QVariantList { "a", "b" }, "c" }).toList(), (QVariantList { "a", "b", "c" }));
    QUuid uuid = QUuid::createUuid();
    QCOMPARE(f.call(probe, "uuidOf", { uuid.toString() }), QVariant(uuid.toString()));
    // a map result
    QVariant described = f.call(probe, "describeSelf", {});
    QCOMPARE(described.toMap().value("count"), QVariant(9));
    // an object result is a handle, usable in a later call and as an argument
    QVariant self = f.call(probe, "self", {});
    QVERIFY(self.userType() == QMetaType::QObjectStar);
    QCOMPARE(self.value<QObject*>(), &f.probe);
    QVariantMap handleRecord;
    handleRecord[ValueCodec::OBJECT_KEY] = f.objects.handleFor(&f.probe);
    QCOMPARE(f.call(probe, "countOf", { handleRecord }), QVariant(9));
    QCOMPARE(f.call(probe, "countOf", { QVariant::fromValue(nullptr) }), QVariant(-1));
    // a pointer to a const object, its class registered without the const (as Audio.setReverbOptions takes one)
    qRegisterMetaType<Probe*>();
    QCOMPARE(f.call(probe, "countOfProbe", { handleRecord }), QVariant(9));
    // extra arguments are ignored, as in JavaScript
    QCOMPARE(f.call(probe, "add", { 1, 2, 3 }), QVariant(3));
    QCOMPARE(f.host->statistics().calls, uint64_t(16));
}

void HostInterfaceTests::testCallErrors() {
    Fixture f;
    QString error;
    QVERIFY2(f.start(error), qPrintable(error));
    const int32_t probe = f.probeHandle();
    QVariant result = f.call(probe, "noSuchMethod", {});
    QVERIFY(HostInterface::isErrorRecord(result));
    QVERIFY(errorMessage(result).contains("no method noSuchMethod"));
    // the error types QtScript gave the same failures: a missing method a TypeError, too few arguments for every
    // candidate a SyntaxError, an argument that will not convert a TypeError
    auto typeOf = [](const QVariant& record) { return record.toMap().value(HostInterface::ERROR_KEY).toMap().value("type").toString(); };
    QCOMPARE(typeOf(result), QString("TypeError"));
    // too few arguments
    result = f.call(probe, "add", { 1 });
    QVERIFY(HostInterface::isErrorRecord(result));
    QVERIFY(errorMessage(result).contains("arguments"));
    QCOMPARE(typeOf(result), QString("SyntaxError"));
    // an argument that will not convert (a string parameter takes anything, as toString does in JavaScript,
    // so add's overloads always match two arguments; scale's vec3 and float do not take a bool and a map)
    QCOMPARE(f.call(probe, "add", { QVariantMap(), QVariantList() }), QVariant("{}[]"));
    result = f.call(probe, "scale", { true, QVariantMap() });
    QVERIFY(HostInterface::isErrorRecord(result));
    QVERIFY(errorMessage(result).contains("converted"));
    QCOMPARE(typeOf(result), QString("TypeError"));
    // a wrong signature, a bad index, an unknown handle, a signal is not callable, a private invokable is hidden;
    // a protected one is callable, as on QtScript
    QVERIFY(HostInterface::isErrorRecord(f.call(probe, "add(float)", { 1 })));
    QVERIFY(HostInterface::isErrorRecord(f.call(probe, "#9999", {})));
    QVERIFY(errorMessage(f.call(12345, "add", { 1, 2 })).contains("unknown object"));
    QVERIFY(HostInterface::isErrorRecord(f.call(probe, "countChanged", { 1 })));
    QVERIFY(HostInterface::isErrorRecord(f.call(probe, "hidden", {})));
    // the rules describe() gives the guest hold for every form of a call, as a module that bypasses the shim uses
    // them: deleteLater (a public slot of QObject) by name, by index and by signature; a private invokable and a
    // signal by index or signature; a protected invokable stays callable
    const int deleteLaterIndex = f.probe.metaObject()->indexOfMethod("deleteLater()");
    QVERIFY(deleteLaterIndex >= 0);
    QVERIFY(HostInterface::isErrorRecord(f.call(probe, "deleteLater", {})));
    QVERIFY(HostInterface::isErrorRecord(f.call(probe, QString("#%1").arg(deleteLaterIndex), {})));
    QVERIFY(HostInterface::isErrorRecord(f.call(probe, "deleteLater()", {})));
    const int hiddenIndex = f.probe.metaObject()->indexOfMethod("hidden()");
    QVERIFY(hiddenIndex >= 0);
    QVERIFY(HostInterface::isErrorRecord(f.call(probe, QString("#%1").arg(hiddenIndex), {})));
    QVERIFY(HostInterface::isErrorRecord(f.call(probe, "hidden()", {})));
    const int bareIndex = f.probe.metaObject()->indexOfMethod("bare()");
    QVERIFY(bareIndex >= 0);
    QVERIFY(HostInterface::isErrorRecord(f.call(probe, QString("#%1").arg(bareIndex), {})));
    QVERIFY(HostInterface::isErrorRecord(f.call(probe, "bare()", {})));
    QCOMPARE(f.call(probe, "guarded", {}), QVariant(9));
    QCOMPARE(f.call(probe, "guarded()", {}), QVariant(9));
    // a registered global is not released by a module: the handle stays and keeps working
    f.host->release(probe);
    QVERIFY(f.objects.contains(probe));
    QCOMPARE(f.call(probe, "add", { 1, 2 }), QVariant(3));
    QCOMPARE(f.call(probe, "guarded", {}).toInt(), 9);
    // a wrong-class handle for an object parameter
    Other other;
    QVariantMap record;
    record[ValueCodec::OBJECT_KEY] = f.objects.handleFor(&other);
    QCOMPARE(f.call(probe, "countOf", { record }), QVariant(-1));  // countOf takes any QObject*, casts inside
    qRegisterMetaType<Probe*>();
    QVERIFY(errorMessage(f.call(probe, "countOfProbe", { record })).contains("converted"));  // a const Probe* only
    // malformed bytes for the arguments trap the call itself
    auto name = f.placeText("add");
    std::vector<WasmValue> results;
    f.instance->writeMemory(uint64_t(f.scratch), QByteArray("\xff\xff", 2));
    QVERIFY(!f.instance->call("call", { probe, name.first, name.second, f.scratch, int32_t(2) }, results, error));
    QVERIFY(error.contains("CBOR"));
    // a pointer outside memory traps too
    QVERIFY(!f.instance->call("call", { probe, int32_t(1 << 30), int32_t(10), int32_t(0), int32_t(0) }, results, error));
    QVERIFY(error.contains("bad pointer"));
}

void HostInterfaceTests::testEnumResults() {
    Fixture f;
    QString error;
    QVERIFY2(f.start(error), qPrintable(error));
    const int32_t probe = f.probeHandle();
    f.probe.setMode(Probe::Stopped);
    QCOMPARE(f.call(probe, "currentMode", {}).toInt(), 10);
    f.call(probe, "useMode", { 1 });
    QCOMPARE(f.probe.mode(), Probe::Running);
    // as QtScript did: it threw for it too
    QVERIFY(HostInterface::isErrorRecord(f.call(probe, "plainLevel", {})));
}

void HostInterfaceTests::testProperties() {
    Fixture f;
    QString error;
    QVERIFY2(f.start(error), qPrintable(error));
    const int32_t probe = f.probeHandle();
    QCOMPARE(f.get(probe, "count"), QVariant(0));
    QCOMPARE(f.get(probe, "label"), QVariant("probe"));
    // a write that worked returns nothing: no bytes cross back
    QCOMPARE(f.set(probe, "count", 7).userType(), int(QMetaType::UnknownType));
    QCOMPARE(f.probe.count(), 7);
    QCOMPARE(f.get(probe, "count"), QVariant(7));
    // the value is converted to the property's type
    QCOMPARE(f.set(probe, "count", "12").userType(), int(QMetaType::UnknownType));
    QCOMPARE(f.probe.count(), 12);
    QVariantMap position;
    position["x"] = 1.5;
    position["y"] = 2.5;
    position["z"] = 3.5;
    QCOMPARE(f.set(probe, "position", position).userType(), int(QMetaType::UnknownType));
    QCOMPARE(f.probe.position(), glm::vec3(1.5f, 2.5f, 3.5f));
    QCOMPARE(f.get(probe, "position").toMap().value("y").toDouble(), 2.5);
    QCOMPARE(f.set(probe, "mode", "Stopped").userType(), int(QMetaType::UnknownType));
    QCOMPARE(f.probe.mode(), Probe::Stopped);
    QCOMPARE(f.get(probe, "mode"), QVariant(10));
    // errors
    QVERIFY(errorMessage(f.set(probe, "label", "x")).contains("read-only"));
    QVERIFY(errorMessage(f.get(probe, "nothing")).contains("no property"));
    QVERIFY(errorMessage(f.set(probe, "nothing", 1)).contains("no property"));
    QVERIFY(errorMessage(f.get(999, "count")).contains("handle"));
    QVERIFY(errorMessage(f.set(probe, "count", QVariantMap())).contains("converted"));
    QCOMPARE(f.host->statistics().gets, uint64_t(7));
}

void HostInterfaceTests::testSignals() {
    Fixture f;
    QString error;
    QVERIFY2(f.start(error), qPrintable(error));
    const int32_t probe = f.probeHandle();
    int32_t connection = f.connect(probe, "countChanged");
    QVERIFY(connection > 0);
    QCOMPARE(f.relay.connectionCount(), 1);
    // a call that makes the signal fire reaches the module's on_signal while the call is in progress
    f.call(probe, "setCount", { 4 });
    QCOMPARE(f.i32("events"), 1);
    QCOMPARE(f.i32("last_id"), connection);
    QCOMPARE(f.lastArguments().toList(), QVariantList { 4 });
    // and from C++ directly
    f.probe.setPosition(glm::vec3(1.0f));
    QCOMPARE(f.i32("events"), 1);  // not subscribed
    int32_t moved = f.connect(probe, "positionChanged(glm::vec3)");
    QVERIFY(moved > 0);
    f.probe.setPosition(glm::vec3(2.0f));
    QCOMPARE(f.i32("events"), 2);
    QCOMPARE(f.i32("last_id"), moved);
    QCOMPARE(f.lastArguments().toList()[0].toMap().value("x").toDouble(), 2.0);
    // disconnect
    std::vector<WasmValue> results;
    QVERIFY(f.instance->call("disconnect", { connection }, results, error));
    f.probe.setCount(5);
    QCOMPARE(f.i32("events"), 2);
    QCOMPARE(f.relay.connectionCount(), 1);
    // an unknown signal gives 0 and a warning
    QCOMPARE(f.connect(probe, "nothing"), 0);
    QVERIFY(!f.logged.isEmpty());
    QVERIFY(f.logged.last().contains("nothing"));
    QCOMPARE(f.connect(999, "countChanged"), 0);
    QCOMPARE(f.host->statistics().signalsDelivered, uint64_t(2));
}

void HostInterfaceTests::testCallbacksAndFailures() {
    Fixture f;
    QString error;
    QVERIFY2(f.start(error), qPrintable(error));
    QVERIFY(f.host->deliverCallback(42, { "a", 1 }, error));
    QCOMPARE(f.i32("events"), 1);
    QCOMPARE(f.i32("last_id"), 42);
    QCOMPARE(f.lastArguments().toList(), (QVariantList { "a", 1 }));
    // a trap inside on_signal is reported, not fatal to the host
    const int32_t probe = f.probeHandle();
    int32_t connection = f.connect(probe, "countChanged");
    QVERIFY(connection > 0);
    std::vector<WasmValue> results;
    QVERIFY(f.instance->call("set_fail", { int32_t(1) }, results, error));
    f.probe.setCount(1);
    QCOMPARE(f.failures.size(), 1);
    QVERIFY(f.failures[0].startsWith("on_signal:"));
    QVERIFY(f.failures[0].contains("unreachable"));
    QVERIFY(f.instance->call("set_fail", { int32_t(0) }, results, error));
    // a missing export is a failure too
    QVERIFY(!f.host->callGuest("no_such_export", 1, QVariantList(), error));
    QCOMPARE(f.failures.size(), 2);
    QVERIFY(f.host->hasGuestExport("on_signal"));
    QVERIFY(!f.host->hasGuestExport("no_such_export"));
}

void HostInterfaceTests::testHandlesAndRelease() {
    Fixture f;
    QString error;
    QVERIFY2(f.start(error), qPrintable(error));
    const int32_t probe = f.probeHandle();
    QVariant self = f.call(probe, "self", {});
    int32_t handle = f.objects.handleFor(&f.probe);
    QCOMPARE(handle, probe);  // the global and the returned object are one handle
    auto other = std::make_unique<Other>();
    QVariant otherRecord = f.codec.toCbor(QVariant::fromValue<QObject*>(other.get())).toVariant();
    int32_t otherHandle = int32_t(f.codec.toCbor(QVariant::fromValue<QObject*>(other.get())).toMap().value(ValueCodec::OBJECT_KEY).toInteger());
    QVERIFY(otherHandle > 0);
    QCOMPARE(f.objects.size(), 2);
    std::vector<WasmValue> results;
    QVERIFY(f.instance->call("release", { otherHandle }, results, error));
    QCOMPARE(f.objects.size(), 1);
    QVERIFY(errorMessage(f.call(otherHandle, "objectName", {})).contains("unknown object"));
    // a dead object: the handle stays until released, calls say what died
    int32_t again = f.objects.handleFor(other.get());
    other.reset();
    QVERIFY(errorMessage(f.call(again, "objectName", {})).contains("no longer exists"));
    QVERIFY(errorMessage(f.call(again, "objectName", {})).contains("Other"));
}

void HostInterfaceTests::testLog() {
    Fixture f;
    QString error;
    QVERIFY2(f.start(error), qPrintable(error));
    auto text = f.placeText("hello from the module");
    std::vector<WasmValue> results;
    QVERIFY(f.instance->call("log", { int32_t(HostInterface::Warning), text.first, text.second }, results, error));
    QCOMPARE(f.logged, QStringList { "2:hello from the module" });
    // a bad pointer in log traps the call
    QVERIFY(!f.instance->call("log", { int32_t(0), int32_t(1 << 30), int32_t(5) }, results, error));
    QVERIFY(error.contains("bad pointer"));
}

// TestObjects.h has no source file of its own, so its moc output is pulled in here.
#include "moc_TestObjects.cpp"

// The member gate (the script API policy for a world's scripts) holds at the host boundary itself: the module's
// exports, reached exactly as a guest reaches them, with no shim in between. Nothing a guest does on its side can
// reach a denied member.
void HostInterfaceTests::testCheckedDeliveries() {
    // a signal the gate checks at each delivery: the deliveries it holds back never reach the module, the others do
    Fixture f;
    QString error;
    QVERIFY2(f.start(error), qPrintable(error));
    HostInterface::MemberGate gate;
    gate.allowsClass = [](const QMetaObject* meta) { return QByteArray(meta->className()) == "Probe"; };
    gate.checksDelivery = [](const QMetaObject*, const QByteArray& name) { return name == "countChanged"; };
    gate.holdsBackDelivery = [](const QMetaObject*, const QByteArray& name, const QVariantList& arguments) {
        return name == "countChanged" && arguments.value(0).toInt() == 13;
    };
    f.host->setMemberGate(gate);
    const int32_t probe = f.probeHandle();
    const int32_t byName = f.connect(probe, "countChanged");
    QVERIFY(byName > 0);
    f.probe.setCount(13);
    QCOMPARE(f.i32("events"), 0);
    f.probe.setCount(4);
    QCOMPARE(f.i32("events"), 1);
    QCOMPARE(f.lastArguments().toList(), QVariantList { 4 });
    // by its signature too
    std::vector<WasmValue> results;
    QVERIFY(f.instance->call("disconnect", { byName }, results, error));
    const int32_t bySignature = f.connect(probe, "countChanged(int)");
    QVERIFY(bySignature > 0);
    f.probe.setCount(13);
    QCOMPARE(f.i32("events"), 1);
    f.probe.setCount(5);
    QCOMPARE(f.i32("events"), 2);
    // a signal the gate doesn't check: every delivery
    QVERIFY(f.connect(probe, "positionChanged(glm::vec3)") > 0);
    f.probe.setPosition(glm::vec3(13.0f));
    QCOMPARE(f.i32("events"), 3);
}

void HostInterfaceTests::testMemberGate() {
    Fixture f;
    QString error;
    QVERIFY2(f.start(error), qPrintable(error));
    Other other;
    f.objects.registerGlobal("Other", &other);

    HostInterface::MemberGate gate;
    gate.allowsClass = [](const QMetaObject* meta) { return QByteArray(meta->className()) == "Probe"; };   // default-deny: Other is not listed
    gate.deniesMethod = [](const QMetaObject* meta, const QByteArray& name) { return QByteArray(meta->className()) == "Probe" && name == "scale"; };
    gate.deniesPropertyWrite = [](const QMetaObject* meta, const QByteArray& name) { return QByteArray(meta->className()) == "Probe" && name == "count"; };
    gate.deniesPropertyRead = [](const QMetaObject* meta, const QByteArray& name) { return QByteArray(meta->className()) == "Probe" && name == "label"; };
    gate.deniesSignal = [](const QMetaObject* meta, const QByteArray& name) { return QByteArray(meta->className()) == "Probe" && name == "named"; };
    gate.refusalLine = [](const QString& object, const QString& member) { return object + "." + member + " is for the visitor's own scripts"; };
    f.host->setMemberGate(gate);

    const int32_t probe = f.probeHandle();
    const int32_t otherHandle = f.objects.global("Other");

    // describe: the denied method and the unreadable property are not shown; the unwritable one says so
    auto name = f.place(QVariant("Probe"));
    QVariantMap description = f.unpack(f.i64("describe", { name.first, name.second })).toMap();
    QStringList methodNames;
    for (const QVariant& entry : description.value("methods").toList()) {
        methodNames << entry.toMap().value("name").toString();
    }
    QVERIFY(methodNames.contains("add"));
    QVERIFY(!methodNames.contains("scale"));
    bool sawCount = false, sawLabel = false;
    for (const QVariant& entry : description.value("properties").toList()) {
        QVariantMap property = entry.toMap();
        if (property.value("name") == "count") { sawCount = true; QCOMPARE(property.value("writable").toBool(), false); }
        if (property.value("name") == "label") { sawLabel = true; }
    }
    QVERIFY(sawCount);
    QVERIFY(!sawLabel);
    QStringList signalNames;
    for (const QVariant& entry : description.value("signals").toList()) {
        signalNames << entry.toMap().value("name").toString();
    }
    QVERIFY(signalNames.contains("countChanged"));
    QVERIFY(!signalNames.contains("named"));

    // a denied signal: no connection by its name, its signature or its index; an allowed one connects
    QCOMPARE(f.connect(probe, "named"), 0);
    QCOMPARE(f.connect(probe, "named(QString,bool)"), 0);
    QCOMPARE(f.connect(probe, QString("#%1").arg(Probe::staticMetaObject.indexOfSignal("named(QString,bool)"))), 0);
    QVERIFY(f.connect(probe, "countChanged") != 0);
    auto otherName = f.place(QVariant("Other"));
    QVariantMap otherDescription = f.unpack(f.i64("describe", { otherName.first, otherName.second })).toMap();
    QCOMPARE(otherDescription.value("class").toString(), QString("Other"));
    QVERIFY(otherDescription.value("methods").toList().isEmpty());
    QVERIFY(otherDescription.value("properties").toList().isEmpty());

    // calls, in every form the boundary takes: by name, by index, by signature
    QCOMPARE(f.call(probe, "add", { 2, 3 }), QVariant(5));
    QVariantMap v { { "x", 1.0 }, { "y", 2.0 }, { "z", 3.0 } };
    QVERIFY(errorMessage(f.call(probe, "scale", { v, 2 })).contains("Probe.scale is for the visitor's own scripts"));
    const int scaleIndex = Probe::staticMetaObject.indexOfMethod("scale(glm::vec3,float)");
    QVERIFY(scaleIndex >= 0);
    QVERIFY(errorMessage(f.call(probe, QString("#%1").arg(scaleIndex), { v, 2 })).contains("is for the visitor's own scripts"));
    QVERIFY(errorMessage(f.call(probe, "scale(glm::vec3,float)", { v, 2 })).contains("is for the visitor's own scripts"));

    // properties: the unreadable one and the unwritable one
    QVERIFY(errorMessage(f.get(probe, "label")).contains("Probe.label is for the visitor's own scripts"));
    f.probe.setCount(7);
    QCOMPARE(f.get(probe, "count"), QVariant(7));
    QVERIFY(errorMessage(f.set(probe, "count", 9)).contains("Probe.count is for the visitor's own scripts"));
    QCOMPARE(f.probe.count(), 7);

    // a denied class: nothing to call, read or connect, whatever the name
    QVERIFY(errorMessage(f.call(otherHandle, "objectName", {})).contains("is for the visitor's own scripts"));
    QVERIFY(errorMessage(f.get(otherHandle, "objectName")).contains("is for the visitor's own scripts"));
    QCOMPARE(f.connect(otherHandle, "destroyed"), 0);

    // each refusal was logged as a warning
    int warnings = 0;
    for (const QString& line : f.logged) {
        if (line.startsWith("2:") && line.contains("is for the visitor's own scripts")) {
            warnings++;
        }
    }
    QVERIFY(warnings >= 10);
}

void HostInterfaceTests::testWorldLeft() {
    // security #32: a script of a world that was left gets nothing done through a gated host (calls, reads, writes,
    // connects, on a handed-back object too), but for what the gate still allows it; undefined, no error
    const int LEFT = 9201;
    Fixture f;
    QString error;
    QVERIFY2(f.start(error), qPrintable(error));
    HostInterface::MemberGate gate;
    gate.allowsClass = [](const QMetaObject* meta) { return QByteArray(meta->className()) == "Probe"; };
    gate.deniesMethod = [](const QMetaObject*, const QByteArray&) { return false; };
    gate.deniesPropertyWrite = [](const QMetaObject*, const QByteArray&) { return false; };
    gate.deniesPropertyRead = [](const QMetaObject*, const QByteArray&) { return false; };
    gate.allowsAfterWorldLeft = [](const QMetaObject*, const QByteArray& name) { return name == "add"; };
    f.host->setMemberGate(gate);
    const int32_t probe = f.probeHandle();
    f.probe.setCount(3);
    const QVariant self = f.call(probe, "self", {});
    {
        ScriptCaller::WorldScope world(LEFT);
        ScriptCaller::retireWorld(LEFT);
        QVERIFY(!f.call(probe, "describeSelf", {}).isValid() || f.call(probe, "describeSelf", {}).isNull());
        QVERIFY(!f.get(probe, "count").isValid() || f.get(probe, "count").isNull());
        f.set(probe, "count", 9);
        QCOMPARE(f.probe.count(), 3);
        QCOMPARE(f.connect(probe, "countChanged"), 0);
        QCOMPARE(f.call(probe, "add", { 2, 3 }), QVariant(5));
    }
    {
        ScriptCaller::WorldScope world(LEFT + 1);
        QCOMPARE(f.get(probe, "count"), QVariant(3));
        QVERIFY(f.connect(probe, "countChanged") != 0);
    }
    Q_UNUSED(self);
}
