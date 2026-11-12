//
//  DeadlockWatchdog.h
//  interface/src
//
//  The deadlock watchdog: a thread that crashes the client on purpose when the main thread has not reported a
//  heartbeat for minutes, so a hang becomes a crash report instead of a frozen window. The main thread beats every
//  100 ms from a timer and at the slow steps of start-up (Application::updateHeartbeat); long known waits pause it
//  (withPause), as do the machine's sleep and wake. Out of Application.cpp as it was, unchanged
//  (ARCHITECTURE.md §5.4, step 4).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_DeadlockWatchdog_h
#define hifi_DeadlockWatchdog_h

#include <atomic>
#include <chrono>
#include <functional>

#include <QtCore/QThread>

#include <SharedUtil.h>
#include <SimpleMovingAverage.h>

class QCoreApplication;

class DeadlockWatchdogThread : public QThread {
public:
    static const unsigned long HEARTBEAT_UPDATE_INTERVAL_SECS = 1;
    // TODO: go back to 2 min across the board, after figuring out the issues with mac
#if defined(Q_OS_MACOS)
    static const unsigned long MAX_HEARTBEAT_AGE_USECS = 600 * USECS_PER_SECOND; // 10 mins with no checkin probably a deadlock, right now, on MAC
#else
    static const unsigned long MAX_HEARTBEAT_AGE_USECS = 120 * USECS_PER_SECOND; // 2 mins with no checkin probably a deadlock
#endif
    static const int WARNING_ELAPSED_HEARTBEAT = 500 * USECS_PER_MSEC; // warn if elapsed heartbeat average is large
    static const int HEARTBEAT_SAMPLES = 100000; // ~5 seconds worth of samples
    static const int HEARTBEAT_TIMER_MSECS = 100;   // the main thread's own beat
    // The watchdog's own rounds, a second apart, this far apart instead: the whole process stood still (the machine
    // slept, the process was suspended, or the clock jumped), not the main thread alone. Well under the shortest
    // limit above, so any stop long enough to trip it is caught
    static const unsigned long PROCESS_STOOD_STILL_USECS = 30 * USECS_PER_SECOND;

    // Whether this build or this machine does without the watchdog: always on Android; under nsight, or with
    // HIFI_DISABLE_WATCHDOG in the environment, elsewhere. (--disableWatchdog is the command line's say.)
    static bool disabledByEnvironment();

    // Starts the watchdog for the application's main thread: the thread itself, the 100 ms heartbeat timer (owned
    // by the application, stopped as it quits) and the pause over the machine's sleep.
    static void startWatching(QCoreApplication* app);

    // Set the heartbeat on launch
    DeadlockWatchdogThread();

    void setMainThreadID(Qt::HANDLE threadID) {
        _mainThreadID = threadID;
    }

    static void updateHeartbeat() {
        auto now = usecTimestampNow();
        auto elapsed = now - _heartbeat;
        _movingAverage.addSample(elapsed);
        _heartbeat = now;
    }

    void deadlockDetectionCrash();

    static void withPause(const std::function<void()>& lambda) {
        pause();
        lambda();
        resume();
    }
    static void pause() {
        _paused = true;
    }

    static void resume() {
        // Update the heartbeat BEFORE resuming the checks
        updateHeartbeat();
        _paused = false;
    }

    void run() override;

    // When presents stop while the present loop has a display plugin: one line saying where the loop is, once they
    // have been gone a second and at most every 5 s. Release-1.1's Windows run of 10-21 lost presents and new frames
    // for 45 s with nothing logged; this names the branch next time. Silent while presents flow, before the first, and
    // while the loop has no plugin on purpose (minimized, quitting). Its own thread, so it reports whichever of the
    // main and present threads is stuck.
    void reportPresentStall();

    static std::atomic<bool> _paused;
    static std::atomic<uint64_t> _heartbeat;
    static std::atomic<uint64_t> _maxElapsed;
    static std::atomic<int> _maxElapsedAverage;
    static ThreadSafeMovingAverage<int, HEARTBEAT_SAMPLES> _movingAverage;

    bool _quit { false };

    // run()'s own: when its last round began, and the moment the main thread's heartbeat is counted from at the
    // earliest (after the process stood still, the time it stood still isn't the main thread's)
    uint64_t _lastRound { 0 };
    uint64_t _heartbeatCountsFrom { 0 };

    Qt::HANDLE _mainThreadID = nullptr;

    // reportPresentStall's: the present count last seen, when it last moved, the last report
    uint64_t _presentsSeen { 0 };
    std::chrono::steady_clock::time_point _presentsMoved;
    std::chrono::steady_clock::time_point _stallReported;
};

#endif // hifi_DeadlockWatchdog_h
