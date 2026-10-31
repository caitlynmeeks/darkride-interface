//
//  DisplayLink.h
//  libraries/display-plugins/src/display-plugins
//
//  The display's refresh signal (on macOS a CVDisplayLink, one callback per refresh of the display the window is on),
//  for pacing the presents in phase with the refreshes (RefreshRateController::sleepThreadIfNeeded presents just
//  after each refresh, so the frame is flushed early in the refresh's interval and the compositor shows it at the
//  next one, whatever the flush's own wait) and for a scripted test's record of the refreshes (Test.takeRefreshTrace).
//  Elsewhere a stub that never has a refresh, and the presents keep a schedule of their own. PHYSICS.md "The driver's
//  stutter" (2026-10-20).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class DisplayLink {
public:
    static DisplayLink& instance();

    // the display under that point of the global screen space (a CGDirectDisplayID), 0 when none or unsupported
    static uint32_t displayAt(int globalX, int globalY);

    // (re)starts on the display (0 for the main display); a no-op where unsupported
    void start(uint32_t displayId);
    void stop();
    bool isRunning() const { return _running; }
    // the display the link runs on (a CGDirectDisplayID), 0 when it is not running
    uint32_t displayId() const { return _running ? _displayId : 0; }
    // running, yet no refresh callback for half a second (since the start, or since the last one): the display
    // went quiet, or the link is on a display that is not refreshing; the plugin's watchdog restarts it
    bool isQuiet(int64_t nowNs) const;

    // The next refresh's instant on std::chrono::steady_clock (nanoseconds since its epoch), predicted from the last
    // callback, and the refresh period; false when there is no recent refresh (none within a quarter second: the
    // link is off, or the display went away)
    bool nextRefresh(int64_t nowNs, int64_t& refreshNs, int64_t& periodNs) const;

    // the trace: usecTimestampNow() of each refresh, from the callback's own prediction of it
    void setTracing(bool on);
    std::vector<uint64_t> takeTrace();

    // the callback (the display link's own thread)
    void onRefresh(double secondsToOutput, double periodSeconds);

private:
    DisplayLink() = default;
    ~DisplayLink();

    void* _link { nullptr };
    uint32_t _displayId { 0 };
    std::atomic<bool> _running { false };
    std::atomic<int64_t> _nextRefreshNs { 0 };
    std::atomic<int64_t> _periodNs { 0 };
    std::atomic<int64_t> _lastCallbackNs { 0 };
    std::atomic<int64_t> _startedNs { 0 };
    std::atomic<bool> _tracing { false };
    std::mutex _traceMutex;
    std::vector<uint64_t> _trace;
};
