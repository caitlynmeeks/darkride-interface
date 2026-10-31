//
//  RefreshRateController.h
//  libraries/display-pluging/src/display-plugin
//
//  Created by Dante Ruiz on 2019-04-15.
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_RefreshRateController_h
#define hifi_RefreshRateController_h

#include <DependencyManager.h>

#include <chrono>
#include <atomic>
#include <mutex>

class QThread;

class RefreshRateController {
public:
    RefreshRateController() = default;
    ~RefreshRateController() = default;

    void setRefreshRateLimitPeriod(int refreshRateLimit);
    int getRefreshRateLimitPeriod() const;

    // the display's own rate (Hz), what the display-based profile targets; 0 when unknown
    void setDisplayRefreshRate(int refreshRate) { _displayRefreshRate = refreshRate; }
    // the period the presents are paced at, in seconds (sleepThreadIfNeeded); 0 when they are not paced to a
    // sensible one (a minimized window's, or none set yet)
    float getPacedPeriodSeconds() const;
    // the slot of the next present: counts the paced intervals, one per present as a rule and more when a present
    // lost one (DisplayPlugin::presented carries it to the game loop)
    uint64_t getSlotIndex() const { return _slotIndex; }

    // The present cadence's counters (team/performance/INSTRUMENTS-PLAN.md, item 3): a few integers the present
    // thread keeps per present and rolls over once a second, so any thread can read what the last whole second's
    // presents looked like (one short lock per read). Paid every present: a dozen integer operations.
    // how a present was paced: in phase with the display's refreshes (the slots), on the free schedule (no refresh
    // signal, or a target that is no whole fraction of the display's rate), by vsync at the display's own rate (the
    // swap waits for the refresh: one present per refresh, which the controller cannot count), or by an HMD
    enum class Schedule { PHASE, FREE, VSYNC, HMD };
    struct CadenceCounters {
        int displayRefreshRate { 0 };   // Hz: the display link's measured rate while it runs, else the screen's
        int presents { 0 };             // presents in the second
        int onOneRefresh { 0 };         // in phase, one refresh after the last present
        int onTwoRefreshes { 0 };       // two refreshes after the last: a frame shown twice, or a present every second refresh
        int onThreeRefreshes { 0 };
        int onMoreRefreshes { 0 };
        int onFreeSchedule { 0 };       // off the display's phase: no refresh signal, or a target that is no whole fraction of the display's rate
        int withVsync { 0 };            // paced by vsync at the display's own rate (in phase by the hardware; refreshes per present unknown here)
        int inHmd { 0 };                // paced by the HMD
        int late { 0 };                 // arrived after their slot: the present before overran it
        int maxLateUs { 0 };            // the worst of them
        int repeated { 0 };             // showed the frame before again (no new frame was ready)
        // the frames' fit, what the cadence governor's return reads (RefreshRateManager): each stage's own work
        // against 70 % of one refresh of the display, never its waits (with vsync the swap waits for the refresh, and
        // a gate that counted the wait parked a session at half rate for good: performance's ride, 2026-10-31)
        int newFrames { 0 };
        int slowMainFrames { 0 };       // whose main-thread frame took over 70 % of a refresh
        int slowBuildFrames { 0 };      // whose render-thread build took over 70 % of a refresh
        int maxMainUs { 0 };
        int maxBuildUs { 0 };
        int slowExecutes { 0 };         // presents whose own work (the present less its swap) took over half a refresh (its real budget at full rate)
        int maxExecuteUs { 0 };
        float gpuMs { 0.0f };           // the GPU's frame time as last averaged
        int64_t rolledAtNs { 0 };       // when the second ended, on std::chrono::steady_clock; 0 before the first second
    };
    CadenceCounters getCadenceCounters() const;
    // the present under way, for the frame trace (present thread): refreshes since the last present (0 off the
    // display's phase) and how late it came past its slot, in microseconds (0 when on time)
    int getPresentRefreshes() const { return _presentRefreshes; }
    int getPresentLateUs() const { return _presentLateUs; }
    // the present thread says whether the present under way shows a new frame; a held one counts as repeated
    void notePresentedFrame(bool newFrame);
    // a new frame's own work (the main thread's frame and the render thread's build, microseconds, stamped on the
    // gpu::Frame), and the present's own work (the present less its swap) with the GPU's frame time; present thread
    void noteNewFrame(int mainUs, int buildUs);
    void notePresentCost(int executeUs, float gpuMs);
    // whether the presents are in phase with the display's refreshes (the slots) or on the free schedule
    bool isInPhase() const { return _inPhase; }
    // the refresh the present under way is due for, on std::chrono::steady_clock (nanoseconds); 0 off the phase
    int64_t getPresentDeadlineNs() const { return _lastDeadlineNs; }

    void clockStartTime() { _startTime = std::chrono::high_resolution_clock::now(); }
    void clockEndTime() { _endTime = std::chrono::high_resolution_clock::now(); }
    // Paces the presents, waiting precisely for the next slot (a coarse sleep, then a spin for the last fraction of
    // a millisecond). With the display's refresh signal (DisplayLink) and a target at the display's rate, each slot
    // is just after a refresh; otherwise the slots are an absolute schedule, one every period on the steady clock.
    // The frames then land on the display's refreshes evenly (PHYSICS.md "The driver's stutter": the old sleep, whole
    // milliseconds measured from the present's own duration, wandered by a millisecond either way and the display
    // repeated or skipped a frame wherever that drifted across a refresh). On the free schedule a present that ran
    // late is followed by a shorter wait, back onto it; more than a period late, the schedule restarts from now.
    // With vsync the swap itself waits for the display's refresh, so a target at the display's rate sleeps not at
    // all; an HMD never sleeps.
    std::chrono::nanoseconds sleepThreadIfNeeded(QThread* thread, bool isHmd, bool vsync = false);
private:
    void countPresent(int64_t nowNs, Schedule schedule, int refreshes, int lateUs, int displayRate);
    // a log line when the schedule, the target or the display's rate changes (present thread)
    void logSchedule(bool inPhase, int refreshesPerPresent, int targetRate, int displayRate, bool linkRunning);
    std::chrono::time_point<std::chrono::high_resolution_clock> _startTime { std::chrono::high_resolution_clock::now() };
    std::chrono::time_point<std::chrono::high_resolution_clock> _endTime { std::chrono::high_resolution_clock::now() };
    std::atomic<int64_t> _refreshRateLimitPeriod { 50 };
    std::atomic<int> _displayRefreshRate { 0 };
    std::atomic<int64_t> _pacedPeriodNs { 0 };   // the period of the slots as last paced (the display's when locked)
    std::atomic<uint64_t> _slotIndex { 0 };
    // after the free schedule restarts (a present that overran its period, a change of rate) the slots are not whole
    // periods after the last ones: the clock is reported unpaced (getPacedPeriodSeconds 0) for this many presents more
    std::atomic<int> _unpacedPresents { 0 };
    // how late the kernel's sleep wakes the present thread, a decaying maximum: the spin before a slot covers it
    int64_t _sleepLateNs { 600000 };
    // the schedule (present thread only)
    uint64_t _presentCount { 0 };
    int64_t _lastSlotNs { 0 };   // the last locked slot: the next is in a later interval
    int64_t _lastDeadlineNs { 0 };   // the refresh the present in the last slot is due for
    bool _scheduled { false };
    std::chrono::nanoseconds _scheduledPeriod { 0 };
    std::chrono::steady_clock::time_point _nextPresent;
    // the cadence counters: the second being counted (present thread only) and the last whole second (under the lock)
    CadenceCounters _cadenceCounting;
    CadenceCounters _cadenceLastSecond;
    int64_t _cadenceSecondStartNs { 0 };
    mutable std::mutex _cadenceMutex;
    int _presentRefreshes { 0 };
    int _presentLateUs { 0 };
    std::atomic<bool> _inPhase { false };
    // what the last log line said (present thread only)
    int _loggedInPhase { -1 };
    int _loggedRefreshesPerPresent { 0 };
    int _loggedTargetRate { 0 };
    int _loggedDisplayRate { 0 };
};

#endif
