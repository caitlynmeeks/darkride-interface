//
//  DeadlockWatchdog.cpp
//  interface/src
//
//  See DeadlockWatchdog.h. The body of run() is the one that stood in Application.cpp since High Fidelity's days;
//  only its home changed.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "DeadlockWatchdog.h"

#include <algorithm>
#include <chrono>
#include <string>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTimer>

#include <display-plugins/OpenGLDisplayPlugin.h>
#include <gl/GLHelpers.h>
#include <shared/NsightHelpers.h>
#include <shared/PlatformHelper.h>

#include "InterfaceLogging.h"

std::atomic<bool> DeadlockWatchdogThread::_paused;
std::atomic<uint64_t> DeadlockWatchdogThread::_heartbeat;
std::atomic<uint64_t> DeadlockWatchdogThread::_maxElapsed;
std::atomic<int> DeadlockWatchdogThread::_maxElapsedAverage;
ThreadSafeMovingAverage<int, DeadlockWatchdogThread::HEARTBEAT_SAMPLES> DeadlockWatchdogThread::_movingAverage;

bool DeadlockWatchdogThread::disabledByEnvironment() {
#if defined(Q_OS_ANDROID)
    return true;
#else
    static const QString DISABLE_WATCHDOG_FLAG{ "HIFI_DISABLE_WATCHDOG" };
    static const bool disabled = nsightActive() || QProcessEnvironment::systemEnvironment().contains(DISABLE_WATCHDOG_FLAG);
    return disabled;
#endif
}

void DeadlockWatchdogThread::startWatching(QCoreApplication* app) {
    // Set up a watchdog thread to intentionally crash the application on deadlocks
    auto deadlockWatchdogThread = new DeadlockWatchdogThread();
    deadlockWatchdogThread->setMainThreadID(QThread::currentThreadId());
    deadlockWatchdogThread->start();

    // Pause the deadlock watchdog when we sleep, or it might
    // trigger a false positive when we wake back up
    auto platformHelper = PlatformHelper::instance();

    QObject::connect(platformHelper, &PlatformHelper::systemWillSleep, [] {
        DeadlockWatchdogThread::pause();
    });

    QObject::connect(platformHelper, &PlatformHelper::systemWillWake, [] {
        DeadlockWatchdogThread::resume();
    });

    // Main thread timer to keep the watchdog updated
    QTimer* watchdogUpdateTimer = new QTimer(app);
    QObject::connect(watchdogUpdateTimer, &QTimer::timeout, [] { updateHeartbeat(); });
    QObject::connect(app, &QCoreApplication::aboutToQuit, [watchdogUpdateTimer] {
        watchdogUpdateTimer->stop();
        watchdogUpdateTimer->deleteLater();
    });
    watchdogUpdateTimer->setSingleShot(false);
    watchdogUpdateTimer->setInterval(HEARTBEAT_TIMER_MSECS); // 100ms, Qt::CoarseTimer acceptable
    watchdogUpdateTimer->start();
}

DeadlockWatchdogThread::DeadlockWatchdogThread() {
    setObjectName("Deadlock Watchdog");
    // Give the heartbeat an initial value
    _heartbeat = usecTimestampNow();
    _paused = false;
    connect(qApp, &QCoreApplication::aboutToQuit, [this] {
        _quit = true;
    });
}

void DeadlockWatchdogThread::deadlockDetectionCrash() {
    uint32_t* crashTrigger = nullptr;
    *crashTrigger = 0xDEAD10CC;
}

void DeadlockWatchdogThread::reportPresentStall() {
    using namespace std::chrono;
    const auto STALL = milliseconds(1000);
    const auto REPORT_EVERY = seconds(5);
    auto now = steady_clock::now();
    uint64_t presents = OpenGLDisplayPlugin::getPresentsDone();
    if (presents != _presentsSeen || presents == 0) {
        _presentsSeen = presents;
        _presentsMoved = now;
        return;
    }
    if (OpenGLDisplayPlugin::isPresentLoopIdle() || now - _presentsMoved < STALL
        || (_stallReported != steady_clock::time_point() && now - _stallReported < REPORT_EVERY)) {
        return;
    }
    _stallReported = now;
    auto view = gl::viewChangeState();
    const char* lastChange = "the editor";
    switch (view.lastEventType) {
        case QEvent::LayoutRequest: lastChange = "a layout pass"; break;
        case QEvent::Resize: lastChange = "a resize"; break;
        case QEvent::Move: lastChange = "a move"; break;
        case QEvent::MouseButtonRelease: lastChange = "a mouse button release"; break;
        case QEvent::MouseMove: lastChange = "a drag"; break;
        default: break;
    }
    qCWarning(interfaceapp).nospace() << "Presents stopped " << duration_cast<milliseconds>(now - _presentsMoved).count()
        << " ms ago: the present loop is " << OpenGLDisplayPlugin::getPresentLoopStage()
        << "; presentation paused " << gl::isPresentationPaused() << " (" << view.pauseLeftMs << " ms left); "
        << view.guards << " view changes so far, the last from " << lastChange;
}

void DeadlockWatchdogThread::run() {
    _lastRound = usecTimestampNow();
    while (!_quit) {
        QThread::sleep(HEARTBEAT_UPDATE_INTERVAL_SECS);
        reportPresentStall();
        // This thread wakes every second. When its rounds are far further apart than that, the whole process stood
        // still (the machine slept, the process was suspended, or the clock jumped), and the main thread could not beat
        // either: that time isn't a hang, so the main thread's limit counts from now. The pause over the machine's sleep
        // (startWatching) relies on the system's sleep message, which didn't pause it in time when a client left open
        // overnight on a Windows laptop was crashed on waking (11-11). A main thread that is really stuck still runs
        // out of its limit, as this thread keeps waking every second.
        const uint64_t roundStart = usecTimestampNow();
        if (roundStart < _lastRound) {
            // the clock went back: a count start set before would lie in its future and blind the check until then
            _heartbeatCountsFrom = 0;
        }
        const uint64_t sinceLastRound = (roundStart > _lastRound) ? roundStart - _lastRound : 0;
        _lastRound = roundStart;
        if (sinceLastRound > PROCESS_STOOD_STILL_USECS) {
            _heartbeatCountsFrom = roundStart;
            qCInfo(interfaceapp_deadlock) << "The deadlock watchdog's rounds were" << sinceLastRound / USECS_PER_SECOND
                << "s apart instead of 1 (the machine slept, the process was suspended, or the clock jumped): the main"
                << "thread's heartbeat limit counts from now";
        }
        // Don't do heartbeat detection under nsight
        if (_paused) {
            continue;
        }
        // sample atomic _heartbeat, because we could context switch away and have it updated on us; no earlier than
        // the process last stood still
        uint64_t lastHeartbeat = std::max<uint64_t>(_heartbeat, _heartbeatCountsFrom);
        uint64_t now = usecTimestampNow();
        auto lastHeartbeatAge = (now > lastHeartbeat) ? now - lastHeartbeat : 0;
        auto elapsedMovingAverage = _movingAverage.getAverage();

        if (elapsedMovingAverage > _maxElapsedAverage) {
#if !defined(NDEBUG)
            qCDebug(interfaceapp_deadlock) << "DEADLOCK WATCHDOG WARNING:"
                << "lastHeartbeatAge:" << lastHeartbeatAge
                << "elapsedMovingAverage:" << elapsedMovingAverage
                << "maxElapsed:" << _maxElapsed
                << "PREVIOUS maxElapsedAverage:" << _maxElapsedAverage
                << "NEW maxElapsedAverage:" << elapsedMovingAverage << "** NEW MAX ELAPSED AVERAGE **"
                << "samples:" << _movingAverage.getSamples();
#endif
            _maxElapsedAverage = elapsedMovingAverage;
        }
        if (lastHeartbeatAge > _maxElapsed) {
#if !defined(NDEBUG)
            qCDebug(interfaceapp_deadlock) << "DEADLOCK WATCHDOG WARNING:"
                << "lastHeartbeatAge:" << lastHeartbeatAge
                << "elapsedMovingAverage:" << elapsedMovingAverage
                << "PREVIOUS maxElapsed:" << _maxElapsed
                << "NEW maxElapsed:" << lastHeartbeatAge << "** NEW MAX ELAPSED **"
                << "maxElapsedAverage:" << _maxElapsedAverage
                << "samples:" << _movingAverage.getSamples();
#endif
            _maxElapsed = lastHeartbeatAge;
        }

#if !defined(NDEBUG)
        if (elapsedMovingAverage > WARNING_ELAPSED_HEARTBEAT) {
            qCDebug(interfaceapp_deadlock) << "DEADLOCK WATCHDOG WARNING:"
                << "lastHeartbeatAge:" << lastHeartbeatAge
                << "elapsedMovingAverage:" << elapsedMovingAverage << "** OVER EXPECTED VALUE **"
                << "maxElapsed:" << _maxElapsed
                << "maxElapsedAverage:" << _maxElapsedAverage
                << "samples:" << _movingAverage.getSamples();
        }
#endif

        if (lastHeartbeatAge > MAX_HEARTBEAT_AGE_USECS) {
            qCDebug(interfaceapp_deadlock) << "DEADLOCK DETECTED -- "
                     << "lastHeartbeatAge:" << lastHeartbeatAge
                     << "[ lastHeartbeat :" << lastHeartbeat
                     << "now:" << now << " ]"
                     << "elapsedMovingAverage:" << elapsedMovingAverage
                     << "maxElapsed:" << _maxElapsed
                     << "maxElapsedAverage:" << _maxElapsedAverage
                     << "samples:" << _movingAverage.getSamples();

            // Don't actually crash in debug builds, in case this apparent deadlock is simply from
            // the developer actively debugging code
            #ifdef NDEBUG
            deadlockDetectionCrash();
            #endif
        }
    }
}
