//
//  ScriptStopTests.cpp
//  tests/script-api-guard/src
//
//  The stops here come at a fixed line of the script: its print calls reach the test on the script's thread, and the
//  test stops the script from a second thread there, as the app's own thread does at a quit or a reload. So what is a
//  matter of timing in the app happens the same way on every run.
//
//  Created by scripting on 2026-11-05.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "ScriptStopTests.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include <AccountManager.h>
#include <AddressManager.h>
#include <EntityScriptingInterface.h>
#include <NodeList.h>
#include <ScriptCaller.h>
#include <ScriptEngines.h>
#include <WasmScriptEngine.h>

QTEST_GUILESS_MAIN(ScriptStopTests)

namespace {

const int SCRIPT_WAIT_MS = 10000;

QString guestModulePath() {
    QString fromEnvironment = qEnvironmentVariable("DARKRIDE_JS_WASM");
    if (!fromEnvironment.isEmpty()) {
        return fromEnvironment;
    }
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../assignment-client/resources/wasm/darkride-js.wasm");
}

// a stop from another thread, the call ScriptEngines and waitTillDoneRunning make
void stopFromAnotherThread(ScriptEngine* engine) {
    std::thread([engine] { engine->stop(); }).join();
}

// a client script on a thread of its own, with what it prints and reports kept
class RunningScript {
public:
    explicit RunningScript(const QString& source) :
        engine(new WasmScriptEngine(ScriptEngine::CLIENT_SCRIPT, source, "about:stop-test"), &QObject::deleteLater) {
        auto scriptEngines = DependencyManager::get<ScriptEngines>();
        engine->setScriptEngines(scriptEngines);
        QObject::connect(engine.data(), &ScriptEngine::printedMessage, engine.data(),
                         [this](const QString& message, const QString&) {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _lines << message;
            }
            if (onLine) {
                onLine(message);
            }
        }, Qt::DirectConnection);
        QObject::connect(engine.data(), &ScriptEngine::errorMessage, engine.data(),
                         [this](const QString& message, const QString&) {
            std::lock_guard<std::mutex> lock(_mutex);
            _errors << message;
        }, Qt::DirectConnection);
        QObject::connect(engine.data(), &ScriptEngine::doneRunning, engine.data(), [this] { _done = true; },
                         Qt::DirectConnection);
    }

    QStringList lines() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _lines;
    }
    QStringList errors() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _errors;
    }
    bool hasLine(const QString& line) const { return lines().contains(line); }
    bool isDone() const { return _done; }
    QString report() const { return "printed: " + lines().join(" | ") + "; errors: " + errors().join(" | "); }

    ScriptEnginePointer engine;
    std::function<void(const QString&)> onLine;

private:
    mutable std::mutex _mutex;
    QStringList _lines;
    QStringList _errors;
    std::atomic<bool> _done { false };
};

} // namespace

void ScriptStopTests::initTestCase() {
    if (!QFileInfo::exists(guestModulePath())) {
        QSKIP("darkride-js.wasm is not built (tools/wasm-js/build.sh)");
    }
    qputenv("DARKRIDE_JS_WASM", guestModulePath().toUtf8());
    DependencyManager::set<AccountManager>();
    DependencyManager::set<AddressManager>();
    DependencyManager::set<NodeList>(NodeType::Agent, INVALID_PORT);
    DependencyManager::set<EntityScriptingInterface>(false);
    DependencyManager::set<ScriptEngines>(ScriptEngine::CLIENT_SCRIPT);
}

// At a quit the app stops every script, then waits on each, and the wait stops it again: by then its handlers may be
// running. The second stop leaves them be. Vec3 is first read here, as Overlays was in the tablet's handler: a read
// that needs the host's answer, like the call on it.
void ScriptStopTests::testAStopLeavesRunningHandlersAlone() {
    RunningScript script(R"(
        Script.scriptEnding.connect(function () {
            print("stop here");
            print("answer " + Vec3.length({ x: 3, y: 0, z: 4 }));
            print("last line");
        });
        Script.stop();
    )");
    script.onLine = [&script](const QString& line) {
        if (line == "stop here") {
            stopFromAnotherThread(script.engine.data());
        }
    };
    script.engine->runInThread();
    QTRY_VERIFY2_WITH_TIMEOUT(script.isDone(), qPrintable(script.report()), SCRIPT_WAIT_MS);
    script.engine->waitTillDoneRunning();
    QVERIFY2(script.hasLine("stop here"), qPrintable(script.report()));
    QVERIFY2(script.hasLine("answer 5"), qPrintable(script.report()));
    QVERIFY2(script.hasLine("last line"), qPrintable(script.report()));
    QVERIFY2(script.errors().isEmpty(), qPrintable(script.report()));
}

// A stop that finds the script inside a call lets it finish for 2 s and ends a runaway there. That was for the call:
// the handlers still run. Reloading scripts stops them this way, while they run as usual.
void ScriptStopTests::testHandlersRunAfterAStopEndedACall() {
    RunningScript script(R"(
        Script.scriptEnding.connect(function () {
            print("answer " + Vec3.length({ x: 3, y: 0, z: 4 }));
            print("last line");
        });
        Script.setTimeout(function () {
            print("stop here");
            while (true) {}
        }, 0);
    )");
    script.onLine = [&script](const QString& line) {
        if (line == "stop here") {
            stopFromAnotherThread(script.engine.data());
        }
    };
    script.engine->runInThread();
    QTRY_VERIFY2_WITH_TIMEOUT(script.isDone(), qPrintable(script.report()), SCRIPT_WAIT_MS);
    script.engine->waitTillDoneRunning();
    QVERIFY2(script.hasLine("answer 5"), qPrintable(script.report()));
    QVERIFY2(script.hasLine("last line"), qPrintable(script.report()));
    QVERIFY2(script.errors().isEmpty(), qPrintable(script.report()));
}

// A world that was left gets nothing more done: once a stop has ended its call, no handler of its runs. Its print lines
// still arrive after it was left ("still printing"), so a handler's line would be seen.
void ScriptStopTests::testALeftWorldRunsNoHandlers() {
    RunningScript script(R"(
        Script.scriptEnding.connect(function () {
            print("handler ran");
        });
        Script.setTimeout(function () {
            print("leave here");
            print("still printing");
            print("stop here");
            while (true) {}
        }, 0);
    )");
    const int world = ScriptCaller::newWorld();
    script.engine->setWorldId(world);
    script.onLine = [&script, world](const QString& line) {
        if (line == "leave here") {
            ScriptCaller::retireWorld(world);
        } else if (line == "stop here") {
            stopFromAnotherThread(script.engine.data());
        }
    };
    script.engine->runInThread();
    QTRY_VERIFY2_WITH_TIMEOUT(script.isDone(), qPrintable(script.report()), SCRIPT_WAIT_MS);
    script.engine->waitTillDoneRunning();
    QVERIFY2(script.hasLine("still printing"), qPrintable(script.report()));
    QVERIFY2(!script.hasLine("handler ran"), qPrintable(script.report()));
}

// A handler that never ends has until the deadline at a quit, and no longer: the script then ends by itself, without
// its thread being terminated.
void ScriptStopTests::testAnEndlessHandlerEndsAtTheDeadline() {
    RunningScript script(R"(
        Script.scriptEnding.connect(function () {
            print("spinning");
            while (true) {}
        });
        Script.stop();
    )");
    script.engine->runInThread();
    QTRY_VERIFY2_WITH_TIMEOUT(script.hasLine("spinning"), qPrintable(script.report()), SCRIPT_WAIT_MS);
    QElapsedTimer timer;
    timer.start();
    script.engine->waitTillDoneRunning();
    const qint64 elapsed = timer.elapsed();
    QVERIFY2(elapsed >= 2500 && elapsed < 3450, qPrintable(QString("took %1 ms").arg(elapsed)));
    QVERIFY2(script.isDone(), qPrintable(script.report()));
}

// Scripts stopped together share that deadline: the app waits on one engine after another, and every script of an avatar
// has an engine of its own, so the wait must not grow with how many handlers are stuck.
void ScriptStopTests::testStuckHandlersShareOneDeadline() {
    const QString source = R"(
        Script.scriptEnding.connect(function () {
            print("spinning");
            while (true) {}
        });
        Script.stop();
    )";
    RunningScript first(source);
    RunningScript second(source);
    first.engine->runInThread();
    second.engine->runInThread();
    QTRY_VERIFY2_WITH_TIMEOUT(first.hasLine("spinning"), qPrintable(first.report()), SCRIPT_WAIT_MS);
    QTRY_VERIFY2_WITH_TIMEOUT(second.hasLine("spinning"), qPrintable(second.report()), SCRIPT_WAIT_MS);
    QElapsedTimer timer;
    timer.start();
    first.engine->waitTillDoneRunning();
    second.engine->waitTillDoneRunning();
    const qint64 elapsed = timer.elapsed();
    QVERIFY2(elapsed < 4500, qPrintable(QString("took %1 ms").arg(elapsed)));
    QVERIFY2(first.isDone(), qPrintable(first.report()));
    QVERIFY2(second.isDone(), qPrintable(second.report()));
}

// The app's safety net for a world that was left: a handler that never ends is interrupted about a second after the
// stop, not at a deadline, so the world's leftovers go and the visitor's own scripts start again. "spinning" is the
// control: the handler did begin, and without the interrupt it would still be running.
void ScriptStopTests::testAnInterruptEndsALeftWorldsHandler() {
    RunningScript script(R"(
        Script.scriptEnding.connect(function () {
            print("spinning");
            while (true) {}
        });
        Script.stop();
    )");
    const int world = ScriptCaller::newWorld();
    script.engine->setWorldId(world);
    script.onLine = [world](const QString& line) {
        if (line == "spinning") {
            ScriptCaller::retireWorld(world);
        }
    };
    script.engine->runInThread();
    QTRY_VERIFY2_WITH_TIMEOUT(script.hasLine("spinning"), qPrintable(script.report()), SCRIPT_WAIT_MS);
    QVERIFY2(!script.isDone(), qPrintable(script.report()));
    script.engine->interruptRunningCall();
    QTest::qWaitFor([&script] { return script.isDone(); }, 1500);
    const bool ended = script.isDone();
    script.engine->waitTillDoneRunning();  // whatever happened: no thread is left spinning
    QVERIFY2(ended, qPrintable(script.report()));
}

// A call that came through a connected signal, which the engine's own calls don't count: a stop finds it all the same,
// and the call finishes what it is doing; then the handlers run.
void ScriptStopTests::testASignalHandlersCallFinishes() {
    StopTestPoker poker;
    RunningScript script(R"(
        Script.scriptEnding.connect(function () {
            print("handler ran");
        });
        Poker.poked.connect(function () {
            print("stop here");
            var started = Date.now();
            while (Date.now() - started < 500) {}
            print("finished");
        });
        print("ready");
    )");
    script.engine->registerGlobalObject("Poker", &poker);
    script.onLine = [&script](const QString& line) {
        if (line == "stop here") {
            stopFromAnotherThread(script.engine.data());
        }
    };
    script.engine->runInThread();
    QTRY_VERIFY2_WITH_TIMEOUT(script.hasLine("ready"), qPrintable(script.report()), SCRIPT_WAIT_MS);
    emit poker.poked();
    QTRY_VERIFY2_WITH_TIMEOUT(script.isDone(), qPrintable(script.report()), SCRIPT_WAIT_MS);
    script.engine->waitTillDoneRunning();
    QVERIFY2(script.hasLine("finished"), qPrintable(script.report()));
    QVERIFY2(script.hasLine("handler ran"), qPrintable(script.report()));
    QVERIFY2(script.errors().isEmpty(), qPrintable(script.report()));
}

// A runaway in a connected signal's handler: the stop ends it 2 s on (not never, as when the stop could not see it),
// and the handlers run.
void ScriptStopTests::testARunawaySignalHandlerEndsAfterTwoSeconds() {
    StopTestPoker poker;
    RunningScript script(R"(
        Script.scriptEnding.connect(function () {
            print("handler ran");
        });
        Poker.poked.connect(function () {
            print("stop here");
            while (true) {}
        });
        print("ready");
    )");
    script.engine->registerGlobalObject("Poker", &poker);
    QElapsedTimer sinceStop;
    std::atomic<bool> stopped { false };
    script.onLine = [&script, &sinceStop, &stopped](const QString& line) {
        if (line == "stop here") {
            sinceStop.start();
            stopped = true;
            stopFromAnotherThread(script.engine.data());
        }
    };
    script.engine->runInThread();
    QTRY_VERIFY2_WITH_TIMEOUT(script.hasLine("ready"), qPrintable(script.report()), SCRIPT_WAIT_MS);
    emit poker.poked();
    QTRY_VERIFY2_WITH_TIMEOUT(stopped.load(), qPrintable(script.report()), SCRIPT_WAIT_MS);
    QTRY_VERIFY2_WITH_TIMEOUT(script.isDone(), qPrintable(script.report()), 6000);
    const qint64 elapsed = sinceStop.elapsed();
    script.engine->waitTillDoneRunning();
    QVERIFY2(elapsed >= 1800 && elapsed < 2900, qPrintable(QString("ended %1 ms after the stop").arg(elapsed)));
    QVERIFY2(script.hasLine("handler ran"), qPrintable(script.report()));
}

namespace {

// a script whose top level never ends, stopped from another thread there (and its world left first, when it has one);
// how long after the stop it was done, or -1
qint64 stopATopLevelSpin(int world) {
    RunningScript script(R"(
        print("stop here");
        while (true) {}
    )");
    if (world != 0) {
        script.engine->setWorldId(world);
    }
    QElapsedTimer sinceStop;
    std::atomic<bool> stopped { false };
    script.onLine = [&script, &sinceStop, &stopped, world](const QString& line) {
        if (line == "stop here") {
            if (world != 0) {
                ScriptCaller::retireWorld(world);
            }
            sinceStop.start();
            stopped = true;
            stopFromAnotherThread(script.engine.data());
        }
    };
    script.engine->runInThread();
    if (!QTest::qWaitFor([&stopped] { return stopped.load(); }, SCRIPT_WAIT_MS)) {
        return -1;
    }
    const bool done = QTest::qWaitFor([&script] { return script.isDone(); }, 6000);
    const qint64 elapsed = sinceStop.elapsed();
    script.engine->waitTillDoneRunning();
    return done ? elapsed : -1;
}

} // namespace

// The script's top level runs before the script is its engine's main module: a stop reaches it there too, and a
// runaway there ends 2 s on, as anywhere else.
void ScriptStopTests::testATopLevelSpinEndsAfterTwoSeconds() {
    const qint64 elapsed = stopATopLevelSpin(0);
    QVERIFY2(elapsed >= 1800 && elapsed < 2900, qPrintable(QString("ended %1 ms after the stop").arg(elapsed)));
}

// The same for a world that was left: at once.
void ScriptStopTests::testATopLevelSpinOfALeftWorldEndsAtOnce() {
    const qint64 elapsed = stopATopLevelSpin(ScriptCaller::newWorld());
    QVERIFY2(elapsed >= 0 && elapsed < 1000, qPrintable(QString("ended %1 ms after the stop").arg(elapsed)));
}
