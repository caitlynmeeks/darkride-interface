//
//  DisplayLink.cpp
//  libraries/display-plugins/src/display-plugins
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "DisplayLink.h"

#include <algorithm>
#include <chrono>

#include <QtCore/QtGlobal>

#include <SharedUtil.h>

#include "Logging.h"

#if defined(Q_OS_MACOS)
#include <CoreGraphics/CGDirectDisplay.h>
#include <CoreVideo/CVDisplayLink.h>
#include <CoreVideo/CVHostTime.h>
#endif

static int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

DisplayLink& DisplayLink::instance() {
    static DisplayLink link;
    return link;
}

DisplayLink::~DisplayLink() {
    stop();
}

bool DisplayLink::isQuiet(int64_t nowNs) const {
    static const int64_t QUIET_NS = 500000000;
    if (!_running) {
        return false;
    }
    int64_t last = std::max(_lastCallbackNs.load(), _startedNs.load());
    return last > 0 && nowNs - last > QUIET_NS;
}

bool DisplayLink::nextRefresh(int64_t nowNs, int64_t& refreshNs, int64_t& periodNs) const {
    static const int64_t STALE_NS = 250000000;   // a quarter second without a callback: the display went quiet
    int64_t period = _periodNs.load();
    int64_t lastCallback = _lastCallbackNs.load();
    if (!_running || period <= 0 || lastCallback == 0 || nowNs - lastCallback > STALE_NS) {
        return false;
    }
    int64_t refresh = _nextRefreshNs.load();
    while (refresh < nowNs) {
        refresh += period;
    }
    refreshNs = refresh;
    periodNs = period;
    return true;
}

void DisplayLink::setTracing(bool on) {
    std::lock_guard<std::mutex> lock(_traceMutex);
    _trace.clear();
    _tracing = on;
}

std::vector<uint64_t> DisplayLink::takeTrace() {
    std::lock_guard<std::mutex> lock(_traceMutex);
    std::vector<uint64_t> trace;
    trace.swap(_trace);
    return trace;
}

void DisplayLink::onRefresh(double secondsToOutput, double periodSeconds) {
    int64_t now = steadyNowNs();
    int64_t toOutput = (int64_t)(secondsToOutput * 1.0e9);
    _nextRefreshNs = now + toOutput;
    if (periodSeconds > 0.0) {
        _periodNs = (int64_t)(periodSeconds * 1.0e9);
    }
    _lastCallbackNs = now;
    if (_tracing) {
        std::lock_guard<std::mutex> lock(_traceMutex);
        if (_trace.size() < 200000) {
            _trace.push_back(usecTimestampNow() + (uint64_t)(toOutput / 1000));
        }
    }
}

#if defined(Q_OS_MACOS)

static CVReturn displayLinkCallback(CVDisplayLinkRef link, const CVTimeStamp* now, const CVTimeStamp* outputTime,
                                    CVOptionFlags, CVOptionFlags*, void* context) {
    // the output time is the refresh at which a frame rendered now is shown; the host times are in the host clock's
    // units, whose frequency CoreVideo reports
    double frequency = CVGetHostClockFrequency();
    double secondsToOutput = frequency > 0.0 ? (double)(int64_t)(outputTime->hostTime - CVGetCurrentHostTime()) / frequency : 0.0;
    double period = CVDisplayLinkGetActualOutputVideoRefreshPeriod(link);
    if (period <= 0.0) {
        CVTime nominal = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);
        if (!(nominal.flags & kCVTimeIsIndefinite) && nominal.timeScale > 0) {
            period = (double)nominal.timeValue / (double)nominal.timeScale;
        }
    }
    static_cast<DisplayLink*>(context)->onRefresh(secondsToOutput, period);
    return kCVReturnSuccess;
}

uint32_t DisplayLink::displayAt(int globalX, int globalY) {
    CGDirectDisplayID id = 0;
    uint32_t count = 0;
    if (CGGetDisplaysWithPoint(CGPointMake(globalX, globalY), 1, &id, &count) != kCGErrorSuccess || count == 0) {
        return 0;
    }
    return id;
}

void DisplayLink::start(uint32_t displayId) {
    CGDirectDisplayID id = displayId ? displayId : CGMainDisplayID();
    if (_running && _link && id == _displayId) {
        return;
    }
    stop();
    CVDisplayLinkRef link = nullptr;
    if (CVDisplayLinkCreateWithCGDisplay(id, &link) != kCVReturnSuccess || !link) {
        qCWarning(displayPlugins) << "Display link: no link for display" << id;
        return;
    }
    CVDisplayLinkSetOutputCallback(link, displayLinkCallback, this);
    if (CVDisplayLinkStart(link) != kCVReturnSuccess) {
        qCWarning(displayPlugins) << "Display link: could not start on display" << id;
        CVDisplayLinkRelease(link);
        return;
    }
    _link = link;
    _displayId = id;
    _startedNs = steadyNowNs();
    _running = true;
    CVTime nominal = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);
    double nominalHz = (!(nominal.flags & kCVTimeIsIndefinite) && nominal.timeValue > 0) ? (double)nominal.timeScale / (double)nominal.timeValue : 0.0;
    qCDebug(displayPlugins) << "Display link on display" << id << "nominal" << nominalHz << "Hz";
}

void DisplayLink::stop() {
    if (_link) {
        CVDisplayLinkRef link = static_cast<CVDisplayLinkRef>(_link);
        _running = false;
        CVDisplayLinkStop(link);
        CVDisplayLinkRelease(link);
        _link = nullptr;
    }
    _lastCallbackNs = 0;
}

#else

uint32_t DisplayLink::displayAt(int, int) {
    return 0;
}

void DisplayLink::start(uint32_t) {
}

void DisplayLink::stop() {
    _running = false;
    _lastCallbackNs = 0;
}

#endif
