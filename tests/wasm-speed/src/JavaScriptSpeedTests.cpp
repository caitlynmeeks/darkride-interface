//
//  JavaScriptSpeedTests.cpp
//  tests/wasm-speed/src
//
//  The speed of JavaScript on the script engine, side by side in one program: the same tests run in turn on the
//  JavaScript guest on WebAssembly as the client runs it (cooperative interruption: no deadline checks in its
//  loops), on the same guest with the runtime's epoch checks in every loop (as it ran before), and on QuickJS-NG
//  built natively when its source is at hand (the interpreter without the sandbox). Each round runs every test once on every engine, in an order that rotates from round to
//  round; the table gives each engine's fastest round, the one the rest of the machine disturbed least, in
//  microseconds per iteration and as a multiple of the first engine's (the guest as the client runs it). QtScript's
//  numbers, from before it was taken out, are in tests-manual/wasm-scripting/results.
//
//  Environment: DARKRIDE_SPEED_ROUNDS (default 5); DARKRIDE_SPEED_SCALE (a multiplier for the iterations, default 1);
//  DARKRIDE_SPEED_ONLY (a regular expression the test names must match); DARKRIDE_SPEED_RESULTS (a JSON file for
//  every round's numbers); DARKRIDE_SPEED_PROFILE (a folder: the guest's stack sampled every millisecond during
//  each test, one profile per test, for the Firefox Profiler or tests-manual/wasm-scripting/profile-summary.py);
//  DARKRIDE_JS_WASM (the guest module, otherwise tools/wasm-js/build/darkride-js.wasm).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "JavaScriptSpeedTests.h"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

#include <HostInterface.h>
#include <ObjectTable.h>
#include <SignalRelay.h>
#include <ValueCodec.h>
#include <WasmRuntime.h>

#include <Mat4.h>
#include <Quat.h>
#include <ScriptUUID.h>
#include <Vec3.h>

#ifdef HAVE_NATIVE_QUICKJS
#include <quickjs.h>
#endif

QTEST_GUILESS_MAIN(JavaScriptSpeedTests)

namespace {

// ES5 on purpose: QtScript ran them, and the numbers compare with its earlier runs. The js- tests are tests/manual baseline.js's engine tests and two more
// (property reads and writes, a method on a prototype); the host- tests reach the engine's objects. Every test returns
// a value its loop computed, so no engine can skip the work and the engines can be seen to agree.
const char* TESTS = R"JS(
var step = { x: 0.001, y: 0.002, z: 0.003 };
var tests = {
    "js-arithmetic": function (n) {
        var a = 1.0, i;
        for (i = 0; i < n; i++) { a = (a * 1.000001 + i) % 1000; }
        return Math.round(a);
    },
    "js-objects": function (n) {
        var sum = 0, i, v;
        for (i = 0; i < n; i++) { v = { x: i, y: i * 2, z: 0 }; sum += v.x + v.y; }
        return sum;
    },
    "js-strings": function (n) {
        var s = "", i, len = 0;
        for (i = 0; i < n; i++) { s = "entity-" + i + "-" + (i % 7); len += s.length; }
        return len;
    },
    "js-arrays": function (n) {
        var arr = [], i;
        for (i = 0; i < n; i++) { arr.push((i * 7919) % 1000); }
        arr.sort(function (a, b) { return a - b; });
        return arr[n - 1];
    },
    "js-function-calls": function (n) {
        function add(a, b) { return a + b; }
        var s = 0, i;
        for (i = 0; i < n; i++) { s = add(s, i & 7); }
        return s;
    },
    "js-json": function (n) {
        var obj = { position: { x: 1.5, y: 2.5, z: -3.5 }, rotation: { x: 0, y: 0, z: 0, w: 1 }, name: "box",
                    list: [1, 2, 3, 4, 5, 6, 7, 8], nested: { a: { b: { c: "deep" } } } };
        var len = 0, i;
        for (i = 0; i < n; i++) { len += JSON.stringify(JSON.parse(JSON.stringify(obj))).length; }
        return len;
    },
    "js-properties": function (n) {
        var o = { a: 1, b: 2, c: 3, d: 4 }, s = 0, i;
        for (i = 0; i < n; i++) { s += o.a + o.d; o.b = i; }
        return s + o.b;
    },
    "js-methods": function (n) {
        function Point(x, y) { this.x = x; this.y = y; }
        Point.prototype.dot = function (p) { return this.x * p.x + this.y * p.y; };
        var a = new Point(1, 2), b = new Point(3, 4), s = 0, i;
        for (i = 0; i < n; i++) { s += a.dot(b); }
        return s;
    },
    "host-noop": function (n) {
        var i;
        for (i = 0; i < n; i++) { Probe.noop(); }
        return n;
    },
    "host-add": function (n) {
        var s = 0, i;
        for (i = 0; i < n; i++) { s = Probe.add(s, 1); }
        return s;
    },
    "host-Vec3.sum": function (n) {
        var v = { x: 0, y: 0, z: 0 }, i;
        for (i = 0; i < n; i++) { v = Vec3.sum(v, step); }
        return Math.round(v.z * 1000);
    },
    "host-Vec3.length": function (n) {
        var v = { x: 3, y: 4, z: 12 }, s = 0, i;
        for (i = 0; i < n; i++) { s += Vec3.length(v); }
        return Math.round(s);
    },
    "host-Quat.multiply": function (n) {
        var q = { x: 0, y: 0, z: 0, w: 1 }, r = Quat.fromPitchYawRollDegrees(0, 1, 0), i;
        for (i = 0; i < n; i++) { q = Quat.multiply(q, r); }
        return Math.round(q.w * 1000);
    },
    "host-Uuid.generate": function (n) {
        var len = 0, i;
        for (i = 0; i < n; i++) { len += String(Uuid.generate()).length; }
        return len;
    },
    "host-property-get": function (n) {
        var s = 0, i, p;
        for (i = 0; i < n; i++) { p = Probe.position; s += p.y; }
        return Math.round(s);
    },
    "host-property-set": function (n) {
        var p = { x: 1, y: 2, z: 3 }, i;
        for (i = 0; i < n; i++) { Probe.position = p; }
        return 0;
    }
};
)JS";

// How the guest is told to run a test: through a signal, as the client's events reach a script.
const char* WIRING = R"JS(
Probe.run.connect(function (name, n) {
    Probe.report(name, tests[name](n));
});
)JS";

const char* NATIVE_WIRING = R"JS(
function runTest(name, n) { return tests[name](n); }
)JS";

struct SpeedTest {
    QString name;
    int iterations;
    bool reachesHost;
};

// Iterations chosen so that each test ran some tens of milliseconds on QtScript.
const std::vector<SpeedTest> SPEED_TESTS = {
    { "js-arithmetic", 2000000, false },
    { "js-objects", 1000000, false },
    { "js-strings", 500000, false },
    { "js-arrays", 400000, false },
    { "js-function-calls", 2000000, false },
    { "js-json", 10000, false },
    { "js-properties", 2000000, false },
    { "js-methods", 1000000, false },
    { "host-noop", 100000, true },
    { "host-add", 100000, true },
    { "host-Vec3.sum", 50000, true },
    { "host-Vec3.length", 50000, true },
    { "host-Quat.multiply", 50000, true },
    { "host-Uuid.generate", 50000, true },
    { "host-property-get", 50000, true },
    { "host-property-set", 50000, true },
};

QString guestModulePath() {
    QString fromEnvironment = qEnvironmentVariable("DARKRIDE_JS_WASM");
    if (!fromEnvironment.isEmpty()) {
        return fromEnvironment;
    }
    // this source file is tests/wasm-speed/src/...; the module is built beside the tools
    return QFileInfo(QString(__FILE__)).dir().absoluteFilePath("../../../tools/wasm-js/build/darkride-js.wasm");
}

class SpeedEngine {
public:
    virtual ~SpeedEngine() = default;
    virtual QString name() const = 0;
    virtual bool runs(const SpeedTest& test) const { Q_UNUSED(test); return true; }
    virtual bool start(QString& error) = 0;
    /// Runs one test to its end; false with the error when the engine reported one.
    virtual bool run(const SpeedTest& test, int iterations, QString& error) = 0;
    virtual QVariant check(const QString& test) const = 0;
};

class WasmSpeedEngine : public SpeedEngine {
public:
    WasmSpeedEngine(const QString& name, WasmRuntime::Interruption interruption, const WasmRuntime::Settings& settings = WasmRuntime::Settings()) :
        _name(name), _interruption(interruption), _runtime(std::make_shared<WasmRuntime>(settings)) {}

    ~WasmSpeedEngine() override {
        QString error;
        if (_host) {
            _host->callScriptStop(error);
        }
    }

    QString name() const override { return _name; }

    bool start(QString& error) override {
        QFile file(guestModulePath());
        if (!file.open(QIODevice::ReadOnly)) {
            error = "no guest module at " + guestModulePath();
            return false;
        }
        _module = _runtime->compile(file.readAll(), error, _interruption);
        if (!_module) {
            return false;
        }
        // as WasmScriptEngine runs JavaScript: no call time limit (QtScript has none), a gigabyte of memory
        WasmInstance::Limits limits;
        limits.memoryBytes = int64_t(1024) * 1024 * 1024;
        limits.callTimeout = std::chrono::milliseconds(0);
        _instance = std::make_unique<WasmInstance>(_module, limits);
        _host = std::make_unique<HostInterface>(*_instance, _objects, _codec, _relay);
        _host->setLogger([this](int level, const QString& text) {
            if (level == HostInterface::Error) {
                _errors << text;
            }
        });
        _host->setFailureHandler([this](const QString& entry, const QString& failure) { _errors << entry + ": " + failure; });
        _host->install();
        _instance->enableWasi();
        _objects.registerGlobal("Script", &_script);
        _objects.registerGlobal("Probe", &_probe);
        _objects.registerGlobal("Vec3", &_vec3);
        _objects.registerGlobal("Quat", &_quat);
        _objects.registerGlobal("Mat4", &_mat4);
        _objects.registerGlobal("Uuid", &_uuid);
        _script.source = QString(TESTS) + WIRING;
        if (!_instance->instantiate(error)) {
            return false;
        }
        std::vector<WasmValue> results;
        if (_instance->hasExport("_initialize") && !_instance->call("_initialize", {}, results, error)) {
            return false;
        }
        if (!_host->callScriptStart(error)) {
            return false;
        }
        if (!_errors.isEmpty()) {
            error = _errors.join(" | ");
            return false;
        }
        return true;
    }

    bool run(const SpeedTest& test, int iterations, QString& error) override {
        _errors.clear();
        emit _probe.run(test.name, iterations);
        if (!_errors.isEmpty()) {
            error = _errors.join(" | ");
            return false;
        }
        return true;
    }

    QVariant check(const QString& test) const override { return _probe.checks.value(test); }

    WasmInstance& instance() { return *_instance; }

private:
    QString _name;
    WasmRuntime::Interruption _interruption;
    std::shared_ptr<WasmRuntime> _runtime;
    std::shared_ptr<WasmModule> _module;
    std::unique_ptr<WasmInstance> _instance;
    ObjectTable _objects;
    ValueCodec _codec { &_objects };
    SignalRelay _relay;
    std::unique_ptr<HostInterface> _host;
    SpeedScript _script;
    SpeedProbe _probe;
    Vec3 _vec3;
    Quat _quat;
    Mat4 _mat4;
    ScriptUUID _uuid;
    QStringList _errors;
};

#ifdef HAVE_NATIVE_QUICKJS
class NativeQuickJSSpeedEngine : public SpeedEngine {
public:
    ~NativeQuickJSSpeedEngine() override {
        if (_context) {
            JS_FreeValue(_context, _runTest);
            JS_FreeContext(_context);
        }
        if (_runtime) {
            JS_FreeRuntime(_runtime);
        }
    }

    QString name() const override { return "QuickJS native"; }
    bool runs(const SpeedTest& test) const override { return !test.reachesHost; }

    bool start(QString& error) override {
        _runtime = JS_NewRuntime();
        _context = JS_NewContext(_runtime);
        QByteArray source = (QString(TESTS) + NATIVE_WIRING).toUtf8();
        JSValue result = JS_Eval(_context, source.constData(), size_t(source.size()), "bench.js", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) {
            error = takeException();
            return false;
        }
        JS_FreeValue(_context, result);
        JSValue global = JS_GetGlobalObject(_context);
        _runTest = JS_GetPropertyStr(_context, global, "runTest");
        JS_FreeValue(_context, global);
        if (!JS_IsFunction(_context, _runTest)) {
            error = "runTest is not a function";
            return false;
        }
        return true;
    }

    bool run(const SpeedTest& test, int iterations, QString& error) override {
        QByteArray name = test.name.toUtf8();
        JSValue args[2] = { JS_NewStringLen(_context, name.constData(), size_t(name.size())), JS_NewInt32(_context, iterations) };
        JSValue result = JS_Call(_context, _runTest, JS_UNDEFINED, 2, args);
        JS_FreeValue(_context, args[0]);
        if (JS_IsException(result)) {
            error = takeException();
            return false;
        }
        double value = 0.0;
        JS_ToFloat64(_context, &value, result);
        JS_FreeValue(_context, result);
        _checks[test.name] = value;
        return true;
    }

    QVariant check(const QString& test) const override { return _checks.value(test); }

private:
    QString takeException() {
        JSValue exception = JS_GetException(_context);
        const char* text = JS_ToCString(_context, exception);
        QString message = text ? QString::fromUtf8(text) : QString("an exception");
        JS_FreeCString(_context, text);
        JS_FreeValue(_context, exception);
        return message;
    }

    JSRuntime* _runtime { nullptr };
    JSContext* _context { nullptr };
    JSValue _runTest { JS_UNDEFINED };
    QVariantMap _checks;
};
#endif

double minimum(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::min_element(values.begin(), values.end());
}

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    size_t middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

} // namespace

void JavaScriptSpeedTests::testSpeed() {
    if (!QFileInfo::exists(guestModulePath())) {
        QSKIP("darkride-js.wasm is not built (tools/wasm-js/build.sh)");
    }
    const int rounds = qEnvironmentVariableIsSet("DARKRIDE_SPEED_ROUNDS") ? std::max(1, qEnvironmentVariableIntValue("DARKRIDE_SPEED_ROUNDS")) : 5;
    const double scale = qEnvironmentVariableIsSet("DARKRIDE_SPEED_SCALE") ? qEnvironmentVariable("DARKRIDE_SPEED_SCALE").toDouble() : 1.0;
    const QRegularExpression only(qEnvironmentVariable("DARKRIDE_SPEED_ONLY", "."));
    QVERIFY2(only.isValid(), "DARKRIDE_SPEED_ONLY is not a valid regular expression");
    QVERIFY2(scale > 0.0, "DARKRIDE_SPEED_SCALE must be a positive number");

    std::vector<SpeedTest> tests;
    for (const auto& test : SPEED_TESTS) {
        if (only.match(test.name).hasMatch()) {
            tests.push_back(test);
        }
    }
    QVERIFY2(!tests.empty(), "no test matches DARKRIDE_SPEED_ONLY");

    std::vector<std::unique_ptr<SpeedEngine>> engines;
    engines.push_back(std::make_unique<WasmSpeedEngine>("WebAssembly", WasmRuntime::Interruption::Cooperative));
    engines.push_back(std::make_unique<WasmSpeedEngine>("WebAssembly, epoch checks", WasmRuntime::Interruption::Epoch));
#ifdef HAVE_NATIVE_QUICKJS
    engines.push_back(std::make_unique<NativeQuickJSSpeedEngine>());
#endif
    for (auto& engine : engines) {
        QString error;
        QVERIFY2(engine->start(error), qPrintable(engine->name() + ": " + error));
    }

    auto iterationsOf = [scale](const SpeedTest& test, double fraction) {
        return std::max(1, int(test.iterations * scale * fraction));
    };

    // a warm-up at a tenth of the work: first calls build wrappers and caches that the rounds should not pay for
    for (const auto& test : tests) {
        for (auto& engine : engines) {
            if (engine->runs(test)) {
                QString error;
                QVERIFY2(engine->run(test, iterationsOf(test, 0.1), error), qPrintable(engine->name() + ", " + test.name + ": " + error));
            }
        }
    }

    // test -> engine -> microseconds per iteration, one value per round
    std::map<QString, std::map<QString, std::vector<double>>> samples;
    for (int round = 0; round < rounds; ++round) {
        for (const auto& test : tests) {
            const int iterations = iterationsOf(test, 1.0);
            for (size_t k = 0; k < engines.size(); ++k) {
                auto& engine = engines[(k + size_t(round)) % engines.size()];
                if (!engine->runs(test)) {
                    continue;
                }
                QString error;
                QElapsedTimer timer;
                timer.start();
                bool ok = engine->run(test, iterations, error);
                qint64 nanoseconds = timer.nsecsElapsed();
                QVERIFY2(ok, qPrintable(engine->name() + ", " + test.name + ": " + error));
                samples[test.name][engine->name()].push_back(double(nanoseconds) / 1000.0 / iterations);
            }
        }
    }

    // the table: each engine's fastest round, and its multiple of the first engine's
    QString header = QString("%1 %2").arg("test", -20).arg("iterations", 10);
    for (auto& engine : engines) {
        header += QString(" | %1").arg(engine->name(), 30);
    }
    qInfo().noquote() << "WASM-SPEED" << header;
    qInfo().noquote() << "WASM-SPEED" << QString("(microseconds per iteration, fastest of %1 rounds; x = a multiple of the first engine's)").arg(rounds);
    QStringList disagreements;
    QJsonObject results;
    for (const auto& test : tests) {
        const auto& byEngine = samples[test.name];
        double baseline = minimum(byEngine.at(engines.front()->name()));
        QString line = QString("%1 %2").arg(test.name, -20).arg(iterationsOf(test, 1.0), 10);
        QJsonObject testResults;
        for (auto& engine : engines) {
            auto found = byEngine.find(engine->name());
            if (found == byEngine.end()) {
                line += QString(" | %1").arg("-", 30);
                continue;
            }
            double fastest = minimum(found->second);
            line += QString(" | %1").arg(QString("%1 (x%2)").arg(fastest, 0, 'f', 4).arg(baseline > 0.0 ? fastest / baseline : 0.0, 0, 'f', 2), 30);
            QJsonArray rounds;
            for (double value : found->second) {
                rounds.append(value);
            }
            QJsonObject engineResults;
            engineResults["fastest"] = fastest;
            engineResults["median"] = median(found->second);
            engineResults["rounds"] = rounds;
            engineResults["check"] = QJsonValue::fromVariant(engine->check(test.name));
            testResults[engine->name()] = engineResults;
            QVariant expected = engines.front()->check(test.name);
            QVariant got = engine->check(test.name);
            if (got.toDouble() != expected.toDouble()) {
                disagreements << QString("%1 on %2: %3, %4 %5").arg(test.name, engine->name(), got.toString(),
                                                                      engines.front()->name(), expected.toString());
            }
        }
        testResults["iterations"] = iterationsOf(test, 1.0);
        results[test.name] = testResults;
        qInfo().noquote() << "WASM-SPEED" << line;
    }

    QString resultsPath = qEnvironmentVariable("DARKRIDE_SPEED_RESULTS");
    if (!resultsPath.isEmpty()) {
        QFile file(resultsPath);
        QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable("cannot write " + resultsPath));
        file.write(QJsonDocument(results).toJson());
    }

    // the guest's own profile, one per test, sampled every millisecond (a runtime of its own: the epoch ticks faster)
    QString profileFolder = qEnvironmentVariable("DARKRIDE_SPEED_PROFILE");
    if (!profileFolder.isEmpty()) {
        QVERIFY2(QDir().mkpath(profileFolder), qPrintable("cannot make " + profileFolder));
        WasmRuntime::Settings everyMillisecond;
        everyMillisecond.epochTick = std::chrono::milliseconds(1);
        // with epoch checks: a sample at every tick wherever the guest is, where a cooperative guest is seen only
        // where it asks whether to stop
        WasmSpeedEngine profiled("WebAssembly, profiled", WasmRuntime::Interruption::Epoch, everyMillisecond);
        QString error;
        QVERIFY2(profiled.start(error), qPrintable(error));
        for (const auto& test : tests) {
            QVERIFY2(profiled.run(test, iterationsOf(test, 0.1), error), qPrintable(error)); // warm-up, not sampled
            QVERIFY2(profiled.instance().startProfiling(test.name, error), qPrintable(error));
            QVERIFY2(profiled.run(test, iterationsOf(test, 1.0), error), qPrintable(error));
            QByteArray profile = profiled.instance().finishProfiling(error);
            QVERIFY2(!profile.isEmpty(), qPrintable(error));
            QFile file(QDir(profileFolder).filePath(test.name + ".json"));
            QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable("cannot write " + file.fileName()));
            file.write(profile);
        }
        qInfo().noquote() << "WASM-SPEED profiles in" << QDir(profileFolder).absolutePath();
    }

    QVERIFY2(disagreements.isEmpty(), qPrintable("the engines disagree: " + disagreements.join("; ")));
}
