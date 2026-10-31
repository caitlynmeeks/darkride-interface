//
//  RefreshRateController.cpp
//  libraries/display-pluging/src/display-plugin
//
//  Created by Dante Ruiz on 2019-04-15.
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "RefreshRateController.h"

#include <QtCore/QThread>
#include <algorithm>
#include <thread>
#include <NumericalConstants.h>

#if defined(Q_OS_WIN)
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

#include "DisplayLink.h"
#include "Logging.h"

namespace {
    // Sleeps about this long. On Windows the plain sleep keeps to the system timer's resolution (a millisecond at
    // best, 15.6 ms unless something in the process raised it), so a high-resolution waitable timer is used where
    // Windows has one (10, 1803 and later); elsewhere the plain sleep, whose lateness the caller measures and covers.
    void sleepAbout(std::chrono::nanoseconds duration) {
#if defined(Q_OS_WIN)
        static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                                  TIMER_ALL_ACCESS);
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(duration.count() / 100);   // relative, in units of 100 ns
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
                return;
            }
        }
#endif
        std::this_thread::sleep_for(duration);
    }
}

long int hzToDurationNanoseconds(int refreshRate) {
    return (int64_t) (NSECS_PER_SECOND / (quint64) refreshRate);
}

int durationNanosecondsToHz(int64_t refreshRateLimitPeriod) {
    return (int) (NSECS_PER_SECOND / (quint64) refreshRateLimitPeriod);
}

void RefreshRateController::setRefreshRateLimitPeriod(int refreshRateLimit) {
    _refreshRateLimitPeriod = hzToDurationNanoseconds(refreshRateLimit);
}

int RefreshRateController::getRefreshRateLimitPeriod() const {
    return durationNanosecondsToHz(_refreshRateLimitPeriod);
}

float RefreshRateController::getPacedPeriodSeconds() const {
    if (_unpacedPresents > 0) {
        return 0.0f;   // the schedule restarted lately: the slots are not whole periods apart yet
    }
    // a period under a millisecond is none set yet (the default), one over 50 ms a window in the background
    const int64_t MIN_PERIOD_NS = 1000000;
    const int64_t MAX_PERIOD_NS = 50000000;
    int64_t period = _pacedPeriodNs > 0 ? _pacedPeriodNs.load() : _refreshRateLimitPeriod.load();
    return (period >= MIN_PERIOD_NS && period <= MAX_PERIOD_NS) ? (float)period * 1.0e-9f : 0.0f;
}

void RefreshRateController::countPresent(int64_t nowNs, Schedule schedule, int refreshes, int lateUs, int displayRate) {
    _presentRefreshes = refreshes;
    _presentLateUs = lateUs;
    CadenceCounters& counting = _cadenceCounting;
    counting.presents++;
    counting.displayRefreshRate = displayRate;
    if (schedule == Schedule::FREE) {
        counting.onFreeSchedule++;
    } else if (schedule == Schedule::VSYNC) {
        counting.withVsync++;
    } else if (schedule == Schedule::HMD) {
        counting.inHmd++;
    } else if (refreshes <= 1) {
        counting.onOneRefresh++;
    } else if (refreshes == 2) {
        counting.onTwoRefreshes++;
    } else if (refreshes == 3) {
        counting.onThreeRefreshes++;
    } else {
        counting.onMoreRefreshes++;
    }
    if (lateUs > 0) {
        counting.late++;
        counting.maxLateUs = std::max(counting.maxLateUs, lateUs);
    }
    if (_cadenceSecondStartNs == 0) {
        _cadenceSecondStartNs = nowNs;
    }
    const int64_t SECOND_NS = 1000000000LL;
    if (nowNs - _cadenceSecondStartNs >= SECOND_NS) {
        counting.rolledAtNs = nowNs;
        {
            std::lock_guard<std::mutex> lock(_cadenceMutex);
            _cadenceLastSecond = counting;
        }
        counting = CadenceCounters();
        _cadenceSecondStartNs = nowNs;
    }
}

void RefreshRateController::logSchedule(bool inPhase, int refreshesPerPresent, int targetRate, int displayRate, bool linkRunning) {
    _inPhase = inPhase;
    // the display's measured rate wanders by a hertz; a change of schedule, target or whole hertz is worth a line,
    // not the wander (rounded to the nearest 5 Hz for the comparison)
    int displayRateRounded = ((displayRate + 2) / 5) * 5;
    if ((int)inPhase == _loggedInPhase && refreshesPerPresent == _loggedRefreshesPerPresent &&
        targetRate == _loggedTargetRate && displayRateRounded == _loggedDisplayRate) {
        return;
    }
    _loggedInPhase = (int)inPhase;
    _loggedRefreshesPerPresent = refreshesPerPresent;
    _loggedTargetRate = targetRate;
    _loggedDisplayRate = displayRateRounded;
    if (inPhase) {
        qCDebug(displayPlugins).nospace() << "Present pacing: in phase with display " << DisplayLink::instance().displayId()
            << " at " << displayRate << " Hz, a present every " << refreshesPerPresent
            << (refreshesPerPresent == 1 ? " refresh" : " refreshes") << " (target " << targetRate << " Hz)";
    } else {
        const char* why = !linkRunning ? "no display link" :
            (displayRate > 0 && targetRate > 0 && targetRate < displayRate && displayRate % targetRate != 0) ?
                "the target is no whole fraction of the display's rate" : "the display link is quiet";
        qCDebug(displayPlugins).nospace() << "Present pacing: on the free schedule at " << targetRate << " Hz (display "
            << DisplayLink::instance().displayId() << " at " << displayRate << " Hz): " << why;
    }
}

void RefreshRateController::notePresentedFrame(bool newFrame) {
    if (!newFrame) {
        _cadenceCounting.repeated++;
    }
}

void RefreshRateController::noteNewFrame(int mainUs, int buildUs) {
    CadenceCounters& counting = _cadenceCounting;
    counting.newFrames++;
    counting.maxMainUs = std::max(counting.maxMainUs, mainUs);
    counting.maxBuildUs = std::max(counting.maxBuildUs, buildUs);
    // slow: over 70 % of one refresh of the display (the room a stage needs in a pipelined frame)
    const int displayRate = _displayRefreshRate;
    const int slowUs = displayRate > 0 ? 700000 / displayRate : 0;
    if (slowUs > 0 && mainUs > slowUs) {
        counting.slowMainFrames++;
    }
    if (slowUs > 0 && buildUs > slowUs) {
        counting.slowBuildFrames++;
    }
}

void RefreshRateController::notePresentCost(int executeUs, float gpuMs) {
    CadenceCounters& counting = _cadenceCounting;
    counting.maxExecuteUs = std::max(counting.maxExecuteUs, executeUs);
    const int displayRate = _displayRefreshRate;
    if (displayRate > 0 && executeUs > 700000 / displayRate) {
        counting.slowExecutes++;
    }
    counting.gpuMs = gpuMs;
}

RefreshRateController::CadenceCounters RefreshRateController::getCadenceCounters() const {
    std::lock_guard<std::mutex> lock(_cadenceMutex);
    return _cadenceLastSecond;
}

std::chrono::nanoseconds RefreshRateController::sleepThreadIfNeeded(QThread* thread, bool isHmd, bool vsync) {
    using namespace std::chrono;
    auto now = steady_clock::now();
    int64_t nowNs = duration_cast<nanoseconds>(now.time_since_epoch()).count();
    if (isHmd) {
        _scheduled = false;
        _slotIndex++;
        countPresent(nowNs, Schedule::HMD, 0, 0, _displayRefreshRate);
        _lastDeadlineNs = 0;
        return nanoseconds(0);
    }
    // With vsync the swap waits for the display's refresh: a sleep on top of it only beats against the refreshes.
    // The display-based profile, whose target is the display's own rate, sleeps not at all then; the throttled
    // profiles keep their pacing.
    if (vsync && _displayRefreshRate > 0 && getRefreshRateLimitPeriod() >= _displayRefreshRate) {
        _scheduled = false;
        _slotIndex++;
        countPresent(nowNs, Schedule::VSYNC, 0, 0, _displayRefreshRate);
        _lastDeadlineNs = 0;
        return nanoseconds(0);
    }
    auto period = nanoseconds(_refreshRateLimitPeriod);
    int64_t refreshNs = 0;
    int64_t refreshPeriodNs = 0;
    // In phase with the display when the target is the display's own rate, or a whole fraction of it (72 or 48 on a
    // 144 Hz panel): a present every refresh, or every second or third, so every frame shows for the same number of
    // refreshes. An uneven ratio (60 on 144) keeps the free schedule below, where frames show for 2 or 3 refreshes in
    // turn (2026-10-31, the monorail's judder).
    const int targetRate = getRefreshRateLimitPeriod();
    int refreshesPerPresent = 0;
    if (_displayRefreshRate > 0 && targetRate > 0) {
        if (targetRate >= _displayRefreshRate) {
            refreshesPerPresent = 1;
        } else if (_displayRefreshRate % targetRate == 0) {
            refreshesPerPresent = _displayRefreshRate / targetRate;
        }
    }
    if (refreshesPerPresent > 0 && DisplayLink::instance().nextRefresh(nowNs, refreshNs, refreshPeriodNs)) {
        // In phase with the display: one slot per refresh interval, a fixed phase after the interval's refresh, so
        // the frame is flushed at the same point of every interval and the compositor shows it at the next refresh.
        // The phase, 2 ms: measured on 2026-10-20 (a 144 Hz panel, 6.94 ms intervals, a 120 Hz second display), the
        // flush waits 1.4 to 2.4 ms for the compositor from a millisecond before each refresh to a millisecond after
        // it, 0.8 ms at 1.5 to 2 ms in and 0.3 to 0.5 ms from 2.5 to 5.5 ms in, with a tail to 4 ms wherever it
        // starts; a present of a millisecond's work from 2 ms in returns by 4 ms as a rule and has 3.9 ms for the
        // flush before the next refresh, which covers the tail (from 3 ms in, 2.9 ms did not: 1.7 refreshes a second
        // were missed). TIVOLI_PRESENT_PHASE_US in the environment sets another; TIVOLI_PRESENT_PHASE_SWEEP=1 walks it through the whole interval,
        // 50 us a present, for a scripted test's map of the flush's wait and the frames' arrival against the phase
        // (seat-jitter.js, PHYSICS.md "The driver's stutter"). This interval's slot is taken unless the last present
        // took it (one present an interval); a present that overran into this interval presents at once, in it: its
        // frame shows at this interval's refresh like an on-time one, and the interval count stays whole. Until the
        // 10-21 review a present more than 1.5 ms past its slot waited for the next interval's, which lost a refresh
        // every time and halved the rate of a scene whose presents take a little over a period (a 60 Hz panel at
        // 30 fps once presents took 18 ms, where presenting at once gives about 55).
        static const int64_t DEFAULT_PHASE_NS = 2000000;
        static const int64_t SWEEP_STEP_NS = 50000;
        static const int64_t phaseSetting = qgetenv("TIVOLI_PRESENT_PHASE_US").isEmpty() ? DEFAULT_PHASE_NS :
            (int64_t)qgetenv("TIVOLI_PRESENT_PHASE_US").toLongLong() * 1000;
        static const bool sweep = !qgetenv("TIVOLI_PRESENT_PHASE_SWEEP").isEmpty() && qgetenv("TIVOLI_PRESENT_PHASE_SWEEP") != "0";
        int64_t phase = sweep ? (int64_t)(_presentCount * SWEEP_STEP_NS) % refreshPeriodNs : phaseSetting % refreshPeriodNs;
        _presentCount++;
        int64_t slot = refreshNs - refreshPeriodNs + phase;   // this interval's (the refresh is the next one)
        // this interval's slot is taken when it was the last present's, or nearer the last than the presents' spacing
        // (one interval as a rule; two or three for a present every second or third refresh): the first interval's
        // slot at the spacing is next. The lateness policy stands: a present that overran presents at once.
        const int64_t minimumSpacing = refreshesPerPresent * refreshPeriodNs - refreshPeriodNs / 2;
        if (_lastSlotNs > 0 && slot <= _lastSlotNs + minimumSpacing) {
            slot += ((_lastSlotNs + minimumSpacing - slot) / refreshPeriodNs + 1) * refreshPeriodNs;
        }
        // the intervals this slot is on from the last (one as a rule; a lost refresh counts)
        int64_t advanced = _lastSlotNs > 0 ? (slot - _lastSlotNs + refreshPeriodNs / 2) / refreshPeriodNs : 1;
        _slotIndex += (uint64_t)std::max<int64_t>(1, advanced);
        // the counters: a present after its slot overran the one before (it presents at once, in this interval)
        const int measuredRate = refreshPeriodNs > 0 ? (int)((1000000000LL + refreshPeriodNs / 2) / refreshPeriodNs) : (int)_displayRefreshRate;
        countPresent(nowNs, Schedule::PHASE, (int)std::max<int64_t>(1, advanced), nowNs > slot ? (int)((nowNs - slot) / 1000) : 0, measuredRate);
        logSchedule(true, refreshesPerPresent, targetRate, measuredRate, true);
        _lastSlotNs = slot;
        _lastDeadlineNs = slot - phase + refreshPeriodNs;   // the refresh at the end of the slot's interval
        _nextPresent = steady_clock::time_point(nanoseconds(slot));   // in the past for an overrun: no wait
        _scheduled = false;   // the free schedule restarts from now if the link goes quiet
        _pacedPeriodNs = refreshPeriodNs;
        _unpacedPresents = 0;   // the display's own intervals: whole by construction
    } else {
        // The free schedule, at the target rate (no refresh signal: Windows, Linux, macOS off the display link, or a
        // throttled rate). Late means the next slot is already behind: a present overran its period. Such a present
        // goes at once and the schedule restarts from it; until the 10-21 review it waited a whole period more
        // (144 Hz presents of 8 ms ran at 67 fps where unpaced ones ran at 125). Its slot is then not a whole number
        // of periods after the last one, so the clock is reported unpaced (the physics on the wall clock) until
        // presents land in their slots again; likewise when the schedule starts or the rate changes.
        static const int RESUME_PACED_PRESENTS = 8;
        bool restart = !_scheduled || period != _scheduledPeriod || now >= _nextPresent + period;
        // the counters: off the display's phase; late when the schedule restarts because a present overran its period
        int64_t lateNs = (_scheduled && period == _scheduledPeriod && now >= _nextPresent + period) ?
            duration_cast<nanoseconds>(now - _nextPresent).count() : 0;
        countPresent(nowNs, Schedule::FREE, 0, (int)(lateNs / 1000), _displayRefreshRate);
        _lastDeadlineNs = 0;
        logSchedule(false, refreshesPerPresent, targetRate, _displayRefreshRate, DisplayLink::instance().isRunning());
        if (restart) {
            _scheduled = true;
            _scheduledPeriod = period;
            _nextPresent = now;
            _unpacedPresents = RESUME_PACED_PRESENTS;
        } else {
            _nextPresent += period;
            if (_unpacedPresents > 0) {
                _unpacedPresents--;
            }
        }
        _slotIndex++;
        _pacedPeriodNs = period.count();
    }
    auto remaining = duration_cast<nanoseconds>(_nextPresent - now);
    if (remaining.count() > 0) {
        // The kernel's sleep wakes late, by how much depends on the platform and the thread (measured 10-21 on this
        // Mac: about a millisecond for an ordinary thread, under 0.07 ms for a real-time one): sleep to within a
        // margin of the slot and spin the rest. The margin follows the lateness actually seen, a decaying maximum,
        // between 0.1 ms and the 0.6 ms it always was: never more spin than before, much less where sleeps are
        // precise (the spin cost 4 % of a core at 60 Hz and 9 % at 144 Hz at 0.6 ms, the review found).
        static const int64_t MIN_MARGIN_NS = 100000;
        static const int64_t MAX_MARGIN_NS = 600000;
        static const int64_t MARGIN_HEADROOM_NS = 50000;
        static const double LATE_DECAY = 0.995;   // per present: the maximum fades over a second or two at 144 Hz
        int64_t margin = std::min(std::max(_sleepLateNs + MARGIN_HEADROOM_NS, MIN_MARGIN_NS), MAX_MARGIN_NS);
        if (remaining.count() > margin) {
            auto wake = _nextPresent - nanoseconds(margin);
            sleepAbout(duration_cast<nanoseconds>(wake - now));
            int64_t late = duration_cast<nanoseconds>(steady_clock::now() - wake).count();
            _sleepLateNs = std::max((int64_t)(LATE_DECAY * (double)_sleepLateNs), std::max<int64_t>(late, 0));
        }
        while (steady_clock::now() < _nextPresent) {
            std::this_thread::yield();
        }
    }
    return remaining;
}
