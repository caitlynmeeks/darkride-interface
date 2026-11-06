//
//  WasmRuntimeTests.cpp
//  tests/wasm-scripting/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "WasmRuntimeTests.h"

#include <atomic>
#include <thread>

#include <QtCore/QElapsedTimer>

#include <ScriptCaller.h>
#include <WasmRuntime.h>

QTEST_GUILESS_MAIN(WasmRuntimeTests)

namespace {

const QString ADD_MODULE = R"((module
  (func (export "add") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.add)))";

std::shared_ptr<WasmRuntime> makeRuntime() {
    WasmRuntime::Settings settings;
    settings.epochTick = std::chrono::milliseconds(5);
    return std::make_shared<WasmRuntime>(settings);
}

// An instantiated instance of a module given as text, or null with a failed assertion.
std::unique_ptr<WasmInstance> makeInstance(std::shared_ptr<WasmRuntime> runtime, const QString& text,
                                           const WasmInstance::Limits& limits) {
    QString error;
    auto module = runtime->compileText(text, error);
    if (!module) {
        qWarning() << "compile failed:" << error;
        return nullptr;
    }
    auto instance = std::make_unique<WasmInstance>(module, limits);
    if (!instance->instantiate(error)) {
        qWarning() << "instantiate failed:" << error;
        return nullptr;
    }
    return instance;
}

std::unique_ptr<WasmInstance> makeInstance(std::shared_ptr<WasmRuntime> runtime, const QString& text) {
    return makeInstance(std::move(runtime), text, WasmInstance::Limits());
}

} // namespace

void WasmRuntimeTests::testCompileAndCall() {
    auto runtime = makeRuntime();
    auto instance = makeInstance(runtime, ADD_MODULE);
    QVERIFY(instance);
    QVERIFY(instance->hasExport("add"));
    QVERIFY(!instance->hasExport("subtract"));
    std::vector<WasmValue> results;
    QString error;
    QVERIFY2(instance->call("add", { int32_t(2), int32_t(3) }, results, error), qPrintable(error));
    QCOMPARE(results.size(), size_t(1));
    QCOMPARE(std::get<int32_t>(results[0]), 5);
    // wrong arity is an error, not a crash
    QVERIFY(!instance->call("add", { int32_t(2) }, results, error));
    QVERIFY(error.contains("takes 2 arguments"));
    QVERIFY(!instance->call("subtract", {}, results, error));
    QVERIFY(error.contains("no exported function"));
}

void WasmRuntimeTests::testCompileErrorIsReported() {
    auto runtime = makeRuntime();
    QString error;
    auto module = runtime->compileText("(module (func (export \"broken\") i32.add))", error);
    QVERIFY(!module);
    QVERIFY(!error.isEmpty());
    error.clear();
    module = runtime->compile(QByteArray("not a module"), error);
    QVERIFY(!module);
    QVERIFY(!error.isEmpty());
}

void WasmRuntimeTests::testTrapIsAnError() {
    auto runtime = makeRuntime();
    auto instance = makeInstance(runtime, R"((module (func (export "boom") unreachable)))");
    QVERIFY(instance);
    std::vector<WasmValue> results;
    QString error;
    QVERIFY(!instance->call("boom", {}, results, error));
    QVERIFY2(error.contains("unreachable"), qPrintable(error));
    // the instance is still usable after a trap
    QVERIFY(instance->hasExport("boom"));
}

void WasmRuntimeTests::testHostFunction() {
    auto runtime = makeRuntime();
    QString error;
    auto module = runtime->compileText(R"((module
      (import "test" "twice" (func $twice (param i32) (result i32)))
      (import "test" "scale" (func $scale (param f64) (result f64)))
      (func (export "callTwice") (param i32) (result i32)
        local.get 0
        call $twice)
      (func (export "callScale") (param f64) (result f64)
        local.get 0
        call $scale)))", error);
    QVERIFY2(module, qPrintable(error));
    WasmInstance instance(module);
    int calls = 0;
    instance.defineHostFunction("test", "twice", { { WasmType::I32 }, { WasmType::I32 } },
        [&calls](WasmInstance&, const std::vector<WasmValue>& args, std::vector<WasmValue>& results) {
            ++calls;
            results.push_back(int32_t(std::get<int32_t>(args[0]) * 2));
            return QString();
        });
    instance.defineHostFunction("test", "scale", { { WasmType::F64 }, { WasmType::F64 } },
        [](WasmInstance&, const std::vector<WasmValue>& args, std::vector<WasmValue>& results) {
            results.push_back(std::get<double>(args[0]) * 1.5);
            return QString();
        });
    QVERIFY2(instance.instantiate(error), qPrintable(error));
    std::vector<WasmValue> results;
    QVERIFY2(instance.call("callTwice", { int32_t(21) }, results, error), qPrintable(error));
    QCOMPARE(std::get<int32_t>(results[0]), 42);
    QCOMPARE(calls, 1);
    QVERIFY2(instance.call("callScale", { 2.0 }, results, error), qPrintable(error));
    QCOMPARE(std::get<double>(results[0]), 3.0);
}

void WasmRuntimeTests::testHostFunctionError() {
    auto runtime = makeRuntime();
    QString error;
    auto module = runtime->compileText(R"((module
      (import "test" "fail" (func $fail))
      (func (export "callFail") call $fail)))", error);
    QVERIFY2(module, qPrintable(error));
    WasmInstance instance(module);
    instance.defineHostFunction("test", "fail", {},
        [](WasmInstance&, const std::vector<WasmValue>&, std::vector<WasmValue>&) {
            return QString("the host said no");
        });
    QVERIFY2(instance.instantiate(error), qPrintable(error));
    std::vector<WasmValue> results;
    QVERIFY(!instance.call("callFail", {}, results, error));
    QVERIFY2(error.contains("the host said no"), qPrintable(error));
}

void WasmRuntimeTests::testInfiniteLoopIsInterrupted() {
    auto runtime = makeRuntime();
    WasmInstance::Limits limits;
    limits.callTimeout = std::chrono::milliseconds(50);
    auto instance = makeInstance(runtime, R"((module (func (export "forever") (loop br 0))))", limits);
    QVERIFY(instance);
    std::vector<WasmValue> results;
    QString error;
    QElapsedTimer timer;
    timer.start();
    QVERIFY(!instance->call("forever", {}, results, error));
    qint64 elapsed = timer.elapsed();
    QVERIFY2(error.contains("longer than its limit"), qPrintable(error));
    QVERIFY2(elapsed < 500, qPrintable(QString("took %1 ms").arg(elapsed)));
    // a call within the limit still works afterwards
    auto quick = makeInstance(runtime, ADD_MODULE, limits);
    QVERIFY(quick);
    QVERIFY2(quick->call("add", { int32_t(1), int32_t(1) }, results, error), qPrintable(error));
}

void WasmRuntimeTests::testRequestStop() {
    auto runtime = makeRuntime();
    WasmInstance::Limits limits;
    limits.callTimeout = std::chrono::milliseconds(10000);
    auto instance = makeInstance(runtime, R"((module (func (export "forever") (loop br 0))))", limits);
    QVERIFY(instance);
    WasmInstance* raw = instance.get();
    std::thread stopper([raw] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        raw->requestStop();
    });
    std::vector<WasmValue> results;
    QString error;
    QElapsedTimer timer;
    timer.start();
    QVERIFY(!instance->call("forever", {}, results, error));
    qint64 elapsed = timer.elapsed();
    stopper.join();
    QVERIFY2(error.contains("stopped"), qPrintable(error));
    QVERIFY2(elapsed < 500, qPrintable(QString("took %1 ms").arg(elapsed)));
}

namespace {

// A module compiled for cooperative interruption: no deadline checks in its loop, it asks the host whether to stop
// (as the JavaScript guest does from QuickJS's interrupt handler) and returns how many times it asked.
const QString COOPERATIVE_MODULE = R"((module
  (import "darkride" "interrupted" (func $interrupted (result i32)))
  (func (export "until_told") (result i32)
    (local $asked i32)
    (block $done
      (loop $again
        (local.set $asked (i32.add (local.get $asked) (i32.const 1)))
        (br_if $done (call $interrupted))
        (br $again)))
    (local.get $asked))))";

std::unique_ptr<WasmInstance> makeCooperativeInstance(std::shared_ptr<WasmRuntime> runtime, const WasmInstance::Limits& limits) {
    QString error;
    auto module = runtime->compileText(COOPERATIVE_MODULE, error, WasmRuntime::Interruption::Cooperative);
    if (!module) {
        qWarning() << "compile failed:" << error;
        return nullptr;
    }
    auto instance = std::make_unique<WasmInstance>(module, limits);
    instance->defineHostFunction("darkride", "interrupted", { {}, { WasmType::I32 } },
                                 [](WasmInstance& self, const std::vector<WasmValue>&, std::vector<WasmValue>& results) {
                                     results.push_back(int32_t(self.interruptRequested() ? 1 : 0));
                                     return QString();
                                 });
    if (!instance->instantiate(error)) {
        qWarning() << "instantiate failed:" << error;
        return nullptr;
    }
    return instance;
}

} // namespace

// A stop requested from another thread: the module, which has no deadline checks, unwinds when it next asks, the
// call returns normally with the reason to take, and every later call is refused.
void WasmRuntimeTests::testCooperativeStop() {
    auto runtime = makeRuntime();
    WasmInstance::Limits limits;
    limits.callTimeout = std::chrono::milliseconds(0);
    auto instance = makeCooperativeInstance(runtime, limits);
    QVERIFY(instance);
    QCOMPARE(instance->module()->interruption(), WasmRuntime::Interruption::Cooperative);
    WasmInstance* raw = instance.get();
    std::thread stopper([raw] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        raw->requestStop();
    });
    std::vector<WasmValue> results;
    QString error;
    QElapsedTimer timer;
    timer.start();
    QVERIFY2(instance->call("until_told", {}, results, error), qPrintable(error));
    qint64 elapsed = timer.elapsed();
    stopper.join();
    QVERIFY2(elapsed >= 25 && elapsed < 500, qPrintable(QString("took %1 ms").arg(elapsed)));
    QVERIFY(std::get<int32_t>(results[0]) > 1);
    QString reason = instance->takeInterruptionReason();
    QVERIFY2(reason.contains("stopped"), qPrintable(reason));
    QVERIFY(instance->takeInterruptionReason().isEmpty());
    QVERIFY(!instance->call("until_told", {}, results, error));
    QVERIFY2(error.contains("stopped"), qPrintable(error));
}

// A stop taken back: a module that stops by itself can be called again, and one that a trap would stop stays stopped.
void WasmRuntimeTests::testResumeAfterStop() {
    auto runtime = makeRuntime();
    WasmInstance::Limits limits;
    limits.callTimeout = std::chrono::milliseconds(0);
    auto cooperative = makeCooperativeInstance(runtime, limits);
    QVERIFY(cooperative);
    cooperative->requestStop();
    cooperative->resumeAfterStop();
    QVERIFY(!cooperative->isStopRequested());

    auto trapped = makeInstance(runtime, R"((module (func (export "quick"))))", limits);
    QVERIFY(trapped);
    trapped->requestStop();
    trapped->resumeAfterStop();
    QVERIFY(trapped->isStopRequested());
    std::vector<WasmValue> results;
    QString error;
    QVERIFY(!trapped->call("quick", {}, results, error));
    QVERIFY2(error.contains("stopped"), qPrintable(error));
}

namespace {

// "run" asks the host, which calls back into the module's "alloc" for the answer's place, as the host does to return a
// value; "forever" runs until stopped.
const QString ASKING_MODULE = R"((module
  (import "host" "ask" (func $ask (result i32)))
  (memory (export "memory") 1)
  (func (export "alloc") (param $size i32) (result i32)
    i32.const 1024)
  (func (export "run") (result i32)
    call $ask)
  (func (export "add") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.add)
  (func (export "forever")
    (loop br 0))))";

std::chrono::steady_clock::time_point after(int milliseconds) {
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
}

} // namespace

// A stop that lets the running call finish: the call goes on (its call back into the module too, as a host function
// returning a value makes), returns what it returns, and only then is the next call refused.
void WasmRuntimeTests::testStopAfterLetsCallFinish() {
    auto runtime = makeRuntime();
    QString error;
    auto module = runtime->compileText(ASKING_MODULE, error);
    QVERIFY2(module, qPrintable(error));
    WasmInstance instance(module, WasmInstance::Limits());
    std::atomic<bool> inside { false };
    std::atomic<bool> stopAsked { false };
    QString innerError;
    instance.defineHostFunction("host", "ask", { {}, { WasmType::I32 } },
        [&](WasmInstance& self, const std::vector<WasmValue>&, std::vector<WasmValue>& results) -> QString {
            inside = true;
            while (!stopAsked) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));  // a few epoch ticks into the stop
            std::vector<WasmValue> out;
            if (!self.call("alloc", { int32_t(4) }, out, innerError)) {
                return "alloc failed: " + innerError;
            }
            results.push_back(out[0]);
            return QString();
        });
    QVERIFY2(instance.instantiate(error), qPrintable(error));
    WasmInstance* raw = &instance;
    std::thread stopper([&] {
        while (!inside) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        raw->requestStopAfter(after(3000), 0);
        stopAsked = true;
    });
    std::vector<WasmValue> results;
    const bool finished = instance.call("run", {}, results, error);
    stopper.join();
    QVERIFY2(finished, qPrintable(error + " / " + innerError));
    QCOMPARE(std::get<int32_t>(results[0]), int32_t(1024));
    QVERIFY(!instance.isStopRequested());  // the deadline is not here yet
    QVERIFY(!instance.call("add", { int32_t(1), int32_t(2) }, results, error));
    QVERIFY2(error.contains("stopped"), qPrintable(error));
}

// A runaway the stop found running is ended at the deadline, not at once, a module with deadline checks and one that
// asks alike.
void WasmRuntimeTests::testStopAfterEndsRunawayAtDeadline() {
    auto runtime = makeRuntime();
    WasmInstance::Limits limits;
    limits.callTimeout = std::chrono::milliseconds(0);
    auto epoch = makeInstance(runtime, R"((module (func (export "forever") (loop br 0))))", limits);
    QVERIFY(epoch);
    auto cooperative = makeCooperativeInstance(runtime, limits);
    QVERIFY(cooperative);
    for (WasmInstance* raw : { epoch.get(), cooperative.get() }) {
        const bool asks = raw == cooperative.get();
        std::thread stopper([raw] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            raw->requestStopAfter(after(150), 0);
        });
        std::vector<WasmValue> results;
        QString error;
        QElapsedTimer timer;
        timer.start();
        const bool returned = raw->call(asks ? "until_told" : "forever", {}, results, error);
        const qint64 elapsed = timer.elapsed();
        stopper.join();
        QVERIFY2(elapsed >= 170 && elapsed < 700, qPrintable(QString("took %1 ms").arg(elapsed)));
        if (asks) {
            QVERIFY2(returned, qPrintable(error));
            const QString reason = raw->takeInterruptionReason();
            QVERIFY2(reason.contains("stopped"), qPrintable(reason));
        } else {
            QVERIFY(!returned);
            QVERIFY2(error.contains("stopped"), qPrintable(error));
        }
        QVERIFY(raw->isStopRequested());
    }
}

// A world left while its call has until the deadline: the call ends at once.
void WasmRuntimeTests::testStopAfterEndsAtOnceWhenWorldLeft() {
    auto runtime = makeRuntime();
    WasmInstance::Limits limits;
    limits.callTimeout = std::chrono::milliseconds(0);
    auto epoch = makeInstance(runtime, R"((module (func (export "forever") (loop br 0))))", limits);
    QVERIFY(epoch);
    auto cooperative = makeCooperativeInstance(runtime, limits);
    QVERIFY(cooperative);
    for (WasmInstance* raw : { epoch.get(), cooperative.get() }) {
        const bool asks = raw == cooperative.get();
        const int world = ScriptCaller::newWorld();
        std::thread stopper([raw, world] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            raw->requestStopAfter(after(10000), world);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            ScriptCaller::retireWorld(world);
        });
        std::vector<WasmValue> results;
        QString error;
        QElapsedTimer timer;
        timer.start();
        raw->call(asks ? "until_told" : "forever", {}, results, error);
        const qint64 elapsed = timer.elapsed();
        stopper.join();
        QVERIFY2(elapsed >= 55 && elapsed < 500, qPrintable(QString("took %1 ms").arg(elapsed)));
        QVERIFY(raw->isStopRequested());
    }
}

// No new call begins once a stop waits for the running one, at once; once the stop is taken back (the script's ending
// handlers' turn) calls are let in again until the deadline.
void WasmRuntimeTests::testStopAfterRefusesNewCall() {
    auto runtime = makeRuntime();
    auto instance = makeInstance(runtime, ADD_MODULE);
    QVERIFY(instance);
    instance->requestStopAfter(after(10000), 0);
    std::vector<WasmValue> results;
    QString error;
    QElapsedTimer timer;
    timer.start();
    QVERIFY(!instance->call("add", { int32_t(1), int32_t(2) }, results, error));
    QVERIFY2(timer.elapsed() < 50, qPrintable(QString("took %1 ms").arg(timer.elapsed())));
    QVERIFY2(error.contains("stopped"), qPrintable(error));
    instance->resumeAfterStop();
    QVERIFY2(instance->call("add", { int32_t(1), int32_t(2) }, results, error), qPrintable(error));
    QCOMPARE(std::get<int32_t>(results[0]), int32_t(3));

    auto due = makeInstance(runtime, ADD_MODULE);
    QVERIFY(due);
    due->requestStopAfter(after(-1), 0);  // a deadline passed: nothing is let in, taken back or not
    due->resumeAfterStop();
    QVERIFY(due->isStopRequested());
    QVERIFY(!due->call("add", { int32_t(1), int32_t(2) }, results, error));
}

// A call that runs past its time limit is told to stop when it next asks; the instance stays usable.
void WasmRuntimeTests::testCooperativeTimeLimit() {
    auto runtime = makeRuntime();
    WasmInstance::Limits limits;
    limits.callTimeout = std::chrono::milliseconds(40);
    auto instance = makeCooperativeInstance(runtime, limits);
    QVERIFY(instance);
    for (int round = 0; round < 2; ++round) {
        std::vector<WasmValue> results;
        QString error;
        QElapsedTimer timer;
        timer.start();
        QVERIFY2(instance->call("until_told", {}, results, error), qPrintable(error));
        qint64 elapsed = timer.elapsed();
        QVERIFY2(elapsed >= 35 && elapsed < 500, qPrintable(QString("took %1 ms").arg(elapsed)));
        QString reason = instance->takeInterruptionReason();
        QVERIFY2(reason.contains("limit of 40 ms"), qPrintable(reason));
    }
}

// A call past its warning time is reported once and goes on: here until a stop is asked for, well after the warning.
void WasmRuntimeTests::testCallWarningGoesOn() {
    auto runtime = makeRuntime();
    WasmInstance::Limits limits;
    limits.callTimeout = std::chrono::milliseconds(0);
    limits.callWarning = std::chrono::milliseconds(30);
    auto instance = makeCooperativeInstance(runtime, limits);
    QVERIFY(instance);
    std::atomic<int> warnings { 0 };
    std::atomic<qint64> warnedAt { 0 };
    instance->setCallWarningHandler([&warnings, &warnedAt](std::chrono::milliseconds elapsed) {
        ++warnings;
        warnedAt = elapsed.count();
    });
    WasmInstance* raw = instance.get();
    std::thread stopper([raw] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        raw->requestStop();
    });
    std::vector<WasmValue> results;
    QString error;
    QElapsedTimer timer;
    timer.start();
    QVERIFY2(instance->call("until_told", {}, results, error), qPrintable(error));
    const qint64 elapsed = timer.elapsed();
    stopper.join();
    QCOMPARE(warnings.load(), 1);
    QVERIFY2(warnedAt.load() >= 30, qPrintable(QString("warned at %1 ms").arg(warnedAt.load())));
    QVERIFY2(elapsed >= 140, qPrintable(QString("the call ended after %1 ms, before the stop").arg(elapsed)));
    const QString reason = instance->takeInterruptionReason();
    QVERIFY2(reason.contains("stopped"), qPrintable(reason));
}

namespace {

// A cooperative module that first waits in a host function, then loops asking whether to stop.
const QString WAIT_THEN_SPIN_MODULE = R"((module
  (import "darkride" "interrupted" (func $interrupted (result i32)))
  (import "test" "wait" (func $wait))
  (func (export "wait_then_spin") (result i32)
    (local $asked i32)
    (call $wait)
    (block $done
      (loop $again
        (local.set $asked (i32.add (local.get $asked) (i32.const 1)))
        (br_if $done (call $interrupted))
        (br $again)))
    (local.get $asked))))";

} // namespace

// The time limit counts the module's own time: what it spends inside a host function (a download, a long host
// call) does not count. A host function that sleeps 80 ms under a 40 ms limit: the call ends about 120 ms in,
// not at 80, and the limit is the reason.
void WasmRuntimeTests::testTimeLimitCountsModuleTimeOnly() {
    auto runtime = makeRuntime();
    QString error;
    auto module = runtime->compileText(WAIT_THEN_SPIN_MODULE, error, WasmRuntime::Interruption::Cooperative);
    QVERIFY2(module, qPrintable(error));
    WasmInstance::Limits limits;
    limits.callTimeout = std::chrono::milliseconds(40);
    WasmInstance instance(module, limits);
    instance.defineHostFunction("darkride", "interrupted", { {}, { WasmType::I32 } },
                                [](WasmInstance& self, const std::vector<WasmValue>&, std::vector<WasmValue>& results) {
                                    results.push_back(int32_t(self.interruptRequested() ? 1 : 0));
                                    return QString();
                                });
    instance.defineHostFunction("test", "wait", { {}, {} }, [](WasmInstance&, const std::vector<WasmValue>&, std::vector<WasmValue>&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        return QString();
    });
    QVERIFY2(instance.instantiate(error), qPrintable(error));
    std::vector<WasmValue> results;
    QElapsedTimer timer;
    timer.start();
    QVERIFY2(instance.call("wait_then_spin", {}, results, error), qPrintable(error));
    qint64 elapsed = timer.elapsed();
    QVERIFY2(elapsed >= 115 && elapsed < 1000, qPrintable(QString("took %1 ms (80 ms waiting in the host, then 40 ms of the module's own)").arg(elapsed)));
    QString reason = instance.takeInterruptionReason();
    QVERIFY2(reason.contains("limit of 40 ms"), qPrintable(reason));
    QVERIFY(!instance.isStopRequested());
    // and the module runs on: a second call gets its own 40 ms
    timer.restart();
    QVERIFY2(instance.call("wait_then_spin", {}, results, error), qPrintable(error));
    elapsed = timer.elapsed();
    QVERIFY2(elapsed >= 115 && elapsed < 1000, qPrintable(QString("the second call took %1 ms").arg(elapsed)));
    QVERIFY(instance.takeInterruptionReason().contains("limit of 40 ms"));
}

void WasmRuntimeTests::testMemoryLimit() {
    auto runtime = makeRuntime();
    WasmInstance::Limits limits;
    limits.memoryBytes = 16 * 1024 * 1024;
    // grows one page at a time until growth is refused, and says how many pages it got
    auto instance = makeInstance(runtime, R"((module
      (memory (export "memory") 1)
      (func (export "grow") (result i32)
        (local $grown i32)
        (block $done
          (loop $again
            (br_if $done (i32.eq (memory.grow (i32.const 1)) (i32.const -1)))
            (local.set $grown (i32.add (local.get $grown) (i32.const 1)))
            (br $again)))
        (local.get $grown))))", limits);
    QVERIFY(instance);
    QCOMPARE(instance->memorySize(), size_t(64 * 1024));
    std::vector<WasmValue> results;
    QString error;
    QVERIFY2(instance->call("grow", {}, results, error), qPrintable(error));
    QCOMPARE(std::get<int32_t>(results[0]), 255); // 1 page to start, 256 pages is 16 MB
    QCOMPARE(instance->memorySize(), size_t(16 * 1024 * 1024));
}

void WasmRuntimeTests::testMemoryReadWrite() {
    auto runtime = makeRuntime();
    auto instance = makeInstance(runtime, R"((module
      (memory (export "memory") 1)
      (data (i32.const 8) "hello")
      (func (export "byteAt") (param i32) (result i32)
        local.get 0
        i32.load8_u)))");
    QVERIFY(instance);
    QCOMPARE(instance->readMemory(8, 5), QByteArray("hello"));
    QVERIFY(instance->writeMemory(8, QByteArray("HELLO")));
    QCOMPARE(instance->readMemory(8, 5), QByteArray("HELLO"));
    std::vector<WasmValue> results;
    QString error;
    QVERIFY2(instance->call("byteAt", { int32_t(8) }, results, error), qPrintable(error));
    QCOMPARE(std::get<int32_t>(results[0]), int32_t('H'));
    // out of bounds is refused, not read
    QVERIFY(instance->readMemory(64 * 1024 - 2, 5).isEmpty());
    QVERIFY(!instance->writeMemory(64 * 1024 - 2, QByteArray("12345")));
    QVERIFY(instance->readMemory(64 * 1024, 0).isEmpty());
}

void WasmRuntimeTests::testWasiImports() {
    auto runtime = makeRuntime();
    QString error;
    const QString text = R"((module
      (import "wasi_snapshot_preview1" "proc_exit" (func $exit (param i32)))
      (memory (export "memory") 1)
      (func (export "noop"))))";
    auto module = runtime->compileText(text, error);
    QVERIFY2(module, qPrintable(error));
    // without WASI the import is unknown
    WasmInstance bare(module);
    QVERIFY(!bare.instantiate(error));
    QVERIFY2(error.contains("wasi_snapshot_preview1"), qPrintable(error));
    // with it the module links and runs
    WasmInstance wasi(module);
    wasi.enableWasi();
    QVERIFY2(wasi.instantiate(error), qPrintable(error));
    std::vector<WasmValue> results;
    QVERIFY2(wasi.call("noop", {}, results, error), qPrintable(error));
}

void WasmRuntimeTests::testImportsAndExports() {
    auto runtime = makeRuntime();
    QString error;
    auto module = runtime->compileText(R"((module
      (import "darkride" "log" (func (param i32 i32)))
      (import "darkride" "now" (func (result f64)))
      (memory (export "memory") 1)
      (func (export "script_start"))
      (func (export "alloc") (param i32) (result i32) i32.const 0)))", error);
    QVERIFY2(module, qPrintable(error));
    QCOMPARE(module->imports(), QStringList({ "darkride.log", "darkride.now" }));
    QCOMPARE(module->exports(), QStringList({ "memory", "script_start", "alloc" }));
}

// A host function that calls an export of the module that called it (what the host interface does to have
// the guest allocate the memory for a result), with the module's memory read and written on the way.
void WasmRuntimeTests::testHostFunctionMayCallBackIn() {
    auto runtime = makeRuntime();
    QString error;
    auto module = runtime->compileText(R"((module
      (import "host" "ask" (func $ask (param i32) (result i32)))
      (memory (export "memory") 1)
      (global $next (mut i32) (i32.const 1024))
      (func (export "alloc") (param $size i32) (result i32)
        (local $ptr i32)
        global.get $next
        local.set $ptr
        global.get $next
        local.get $size
        i32.add
        global.set $next
        local.get $ptr)
      (func (export "run") (param i32) (result i32)
        local.get 0
        call $ask)
      (func (export "loop") (result i32)
        (loop $forever br $forever)
        i32.const 0)))", error);
    QVERIFY2(module, qPrintable(error));
    WasmInstance::Limits limits;
    limits.callTimeout = std::chrono::milliseconds(100);
    WasmInstance instance(module, limits);
    int depth = 0;
    instance.defineHostFunction("host", "ask", { { WasmType::I32 }, { WasmType::I32 } },
        [&](WasmInstance& self, const std::vector<WasmValue>& args, std::vector<WasmValue>& results) -> QString {
            ++depth;
            std::vector<WasmValue> out;
            QString callError;
            // the guest allocates, the host writes the answer there and returns the pointer
            if (!self.call("alloc", { int32_t(4) }, out, callError)) {
                return "alloc failed: " + callError;
            }
            int32_t pointer = std::get<int32_t>(out[0]);
            int32_t value = std::get<int32_t>(args[0]) * 2;
            self.writeMemory(uint64_t(pointer), QByteArray(reinterpret_cast<const char*>(&value), 4));
            results.push_back(pointer);
            return QString();
        });
    QVERIFY2(instance.instantiate(error), qPrintable(error));
    std::vector<WasmValue> results;
    QVERIFY2(instance.call("run", { int32_t(21) }, results, error), qPrintable(error));
    int32_t pointer = std::get<int32_t>(results[0]);
    QCOMPARE(pointer, 1024);
    QByteArray answer = instance.readMemory(uint64_t(pointer), 4);
    QCOMPARE(*reinterpret_cast<const int32_t*>(answer.constData()), 42);
    QCOMPARE(depth, 1);
    // a second call allocates after the first
    QVERIFY(instance.call("run", { int32_t(5) }, results, error));
    QCOMPARE(std::get<int32_t>(results[0]), 1028);
    // the time limit still holds after reentrant calls
    QElapsedTimer timer;
    timer.start();
    QVERIFY(!instance.call("loop", {}, results, error));
    QVERIFY(timer.elapsed() < 1000);
    QVERIFY(error.contains("limit"));
}
