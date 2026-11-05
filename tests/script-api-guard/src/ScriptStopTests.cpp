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

// A stop that finds the script inside a call ends that call, a runaway's too. That was for the call: the handlers
// still run. Reloading scripts stops them this way, while they run as usual.
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
