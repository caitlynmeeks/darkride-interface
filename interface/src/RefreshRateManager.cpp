//
//  RefreshRateManager.cpp
//  interface/src/
//
//  Created by Dante Ruiz on 2019-04-15.
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: the whole file (the refresh regimes and the resting rate, the screen caps, the ride-in-view exemption,
// the cadence governor and its constants).
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.


#include "RefreshRateManager.h"

#include "InterfaceLogging.h"

#include <SharedUtil.h>
#include <display-plugins/RefreshRateController.h>   // PRESENT_SLACK_FLOOR_US, for the drop's line

#include <QtCore/QProcessEnvironment>
#include <QtGui/QScreen>
#include <QtGui/QWindow>

#include <array>
#include <chrono>

#include "Application.h"
#include "MainWindow.h"

// TODO: update using a vr plugin. valve index uses 144 hz
static const int VR_TARGET_RATE = 144;

/**jsdoc
 * <p>Refresh rate profile.</p>
 * <table>
 *   <thead>
 *     <tr>
 *       <th>Value</th>
 *       <th>Description</th>
 *     </tr>
 *   </thead>
 *   <tbody>
 *     <tr>
 *       <td><code>"Eco"</code></td>
 *       <td>20 FPS target rate, reduced when Interface doesn't have focus or is minimized</td>
 *     </tr>
 *     <tr>
 *       <td><code>"Interactive"</code></td>
 *       <td>30 FPS target rate, reduced when Interface doesn't have focus or is minimized</td>
 *     </tr>
 *     <tr>
 *       <td><code>"Realtime"</code></td>
 *       <td>60 FPS target rate, reduced when Interface doesn't have focus or is minimized</td>
 *     </tr>
 *     <tr>
 *       <td><code>"Display-based"</code></td>
*        <td>Targets the refresh rate of your screen, reduced when Interface doesn't have focus or is minimized</td>
 *     </tr>
 *   </tbody>
 * </table>
 * @typedef {string} RefreshRateProfileName
 */
static const std::array<std::string, RefreshRateManager::RefreshRateProfile::PROFILE_NUM> REFRESH_RATE_PROFILE_TO_STRING = {
    { "Eco", "Interactive", "Realtime", "Display-based" }
};

/**jsdoc
 * <p>Interface states that affect the refresh rate.</p>
 * <table>
 *   <thead>
 *     <tr><th>Value</th><th>Description</th></tr>
 *   </thead>
 *   <tbody>
 *     <tr><td><code>"FocusActive"</code></td><td>Interface has focus and the user is active or is in VR.</td></tr>
 *     <tr><td><code>"FocusInactive"</code></td><td>Interface has focus and the user is inactive.</td></tr>
 *     <tr><td><code>"Unfocus"</code></td><td>Interface doesn't have focus.</td></tr>
 *     <tr><td><code>"Minimized"</code></td><td>Interface is minimized.</td></tr>
 *     <tr><td><code>"StartUp"</code></td><td>Interface is starting up.</td></tr>
 *     <tr><td><code>"ShutDown"</code></td><td>Interface is shutting down.</td></tr>
 *   </tbody>
 * </table>
 * @typedef {string} RefreshRateRegimeName
 */
static const std::array<std::string, RefreshRateManager::RefreshRateRegime::REGIME_NUM> REFRESH_RATE_REGIME_TO_STRING = {
    { "FocusActive", "FocusInactive", "Unfocus", "Minimized", "StartUp", "ShutDown" }
};

/**jsdoc
 * <p>User experience (UX) modes.</p>
 * <table>
 *   <thead>
 *     <tr><th>Value</th><th>Description</th></tr>
 *   </thead>
 *   <tbody>
 *     <tr><td><code>"Desktop"</code></td><td>Desktop user experience.</td></tr>
 *     <tr><td><code>"VR"</code></td><td>VR user experience.</td></tr>
 *   </tbody>
 * </table>
 * @typedef {string} UXModeName
 */
static const std::array<std::string, RefreshRateManager::UXMode::UX_NUM> UX_MODE_TO_STRING = {
    { "Desktop", "VR" }
};

static const std::map<std::string, RefreshRateManager::RefreshRateProfile> REFRESH_RATE_PROFILE_FROM_STRING = {
    { "Eco", RefreshRateManager::RefreshRateProfile::ECO },
    { "Interactive", RefreshRateManager::RefreshRateProfile::INTERACTIVE },
    { "Realtime", RefreshRateManager::RefreshRateProfile::REALTIME },
    { "Display-based", RefreshRateManager::RefreshRateProfile::DISPLAY_BASED }
};


// Porfile regimes are:
//  { { "FocusActive", "FocusInactive", "Unfocus", "Minimized", "StartUp", "ShutDown" } }

static const std::array<int, RefreshRateManager::RefreshRateRegime::REGIME_NUM> ECO_PROFILE = {
    { 20, 10, 5, 2, 30, 30 }
};

static const std::array<int, RefreshRateManager::RefreshRateRegime::REGIME_NUM> INTERACTIVE_PROFILE = {
    { 30, 20, 10, 2, 30, 30 }
};

static const std::array<int, RefreshRateManager::RefreshRateRegime::REGIME_NUM> REALTIME_PROFILE = {
    { 60, 60, 60, 2, 30, 30}
};

// Unfocus 30: while another app is in front the frames are at most glanced at, so the display's rate (999) only burns
// cores and battery. 30, not 15: a player often keeps the world in view beside the app in front (another screen, half
// the screen), where 30 still moves smoothly. The game loop (physics, this avatar's data to others) has its own pace.
// FocusInactive RESTING_RATE (Caity, 10-27: "resting rate in"): in front but untouched and still, half the display's
// rate, every other refresh, from 120 Hz up (72 on 144 Hz); below 120 Hz it stays at the display's rate, since a rate
// under 60, or one that isn't a whole fraction of the display's, would move others' avatars unevenly. On a 144 Hz Mac,
// 60 instead of 144 halved the client's CPU energy standing still (performance, 10-27).
static const std::array<int, RefreshRateManager::RefreshRateRegime::REGIME_NUM> DISPLAY_BASED_PROFILE = {
    { 999, RefreshRateManager::RESTING_RATE, 30, 2, 30, 30}
};

static const std::array<
    std::array<int, RefreshRateManager::RefreshRateRegime::REGIME_NUM>,
    RefreshRateManager::RefreshRateProfile::PROFILE_NUM
> REFRESH_RATE_PROFILES = {
    { ECO_PROFILE, INTERACTIVE_PROFILE, REALTIME_PROFILE, DISPLAY_BASED_PROFILE }
};


static const int INACTIVE_TIMER_LIMIT = 3000;

// the cadence governor's numbers (RefreshRateManager.h): presents in a second that showed the frame before again,
// landed two or more refreshes apart, or were handed to the display with under a millisecond before their refresh
// (the compositor shows those a refresh late while the count stays whole: entry 34) before the target drops (3 of
// 144 is a hitch the eye sees; 1 or 2 are a world loading something); the hold of fitting seconds before a return, its first value and its ceiling after failed
// returns; the seconds after a return in which a drop marks it failed (a world that hitches every ten seconds would
// otherwise return and drop without the hold ever growing); the clean seconds at full rate that forgive
// the back-off. A second at half rate counts as fitting when each stage's slow count (frames over 70 % of a
// full-rate refresh in the main thread, the render thread's build or the present's own work), scaled to the full
// rate, stays under the drop threshold, and the GPU's average is under 70 %: at 72 on 144 at most one slow frame a
// second per stage, since each would be a miss at 144 (performance's re-ride, 2026-10-31: two slow executes a second
// at 72 passed a tenth, returned, and were 4-6 misses a second at 144, a drop again). A maximum would let one
// texture upload a second hold the half rate for ever; this is a tail the full rate could carry.
static const int GOVERNOR_DROP_MISSES = 3;
// the seconds after a world's arrival (the domain connect) in which slipped presents are counted and shown but not
// dropped on: a world's loading second slips 3-5 presents on every build, and a heavy world then never earns the
// climb-back, so without this no ride there ever runs at the display's rate (entry 28)
static const int GOVERNOR_ARRIVAL_GRACE_SECONDS = 10;
// A measurement-only switch (entry 24, Amendment 3e): with TIVOLI_GOVERNOR_HOLD set the governor never drops to the
// half rate, so a run in front measures at the display's own rate through a world's arrival. Read once at start-up;
// unset, which is every client a visitor runs, nothing differs. A held run is a measurement, never a ride: its log
// says so, and the diag line's rate word reads "held".
static bool s_governorHeld = false;
static const int GOVERNOR_HOLD_MIN_SECONDS = 5;
static const int GOVERNOR_HOLD_MAX_SECONDS = 60;
static const int GOVERNOR_PROBE_SECONDS = 30;
static const int GOVERNOR_FORGIVE_SECONDS = 60;
// after a change of target the presents already paced on the old spacing settle within this: a counted second that
// began before the change plus this is mixed and carries no verdict (governorTick)
static const int64_t GOVERNOR_SETTLE_NS = 50000000;

static int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// DARKRIDE_REFRESH_CAP=<Hz> in the environment caps the desktop's rate at that number, for a hand test at a whole
// fraction of the display's rate (72 or 48 on a 144 Hz panel; the presents then lock to every second or third
// refresh, RefreshRateController::sleepThreadIfNeeded): PHYSICS.md "The monorail's judder", 2026-10-31
static int environmentRefreshCap() {
    static const int cap = qEnvironmentVariableIntValue("DARKRIDE_REFRESH_CAP");
    return cap;
}

// "Half" the display's rate for the resting rate and the governor: the largest whole fraction at or under half, so
// the presents lock to every second, third or fourth refresh (RefreshRateController): 72 on 144, 60 on 120, 55 on
// 165 (82 would be no whole fraction and run the free schedule, worse than the hitch it cures: performance's review,
// 2026-10-31); 0 when the rate has none
static int halfRateOf(int displayRate) {
    for (int n = 2; n <= 4; n++) {
        if (displayRate > 0 && displayRate % n == 0) {
            return displayRate / n;
        }
    }
    return 0;
}


std::string RefreshRateManager::refreshRateProfileToString(RefreshRateManager::RefreshRateProfile refreshRateProfile) {
    return REFRESH_RATE_PROFILE_TO_STRING.at(refreshRateProfile);
}

RefreshRateManager::RefreshRateProfile RefreshRateManager::refreshRateProfileFromString(std::string refreshRateProfile) {
    return REFRESH_RATE_PROFILE_FROM_STRING.at(refreshRateProfile);
}

std::string RefreshRateManager::refreshRateRegimeToString(RefreshRateManager::RefreshRateRegime refreshRateRegime) {
    return REFRESH_RATE_REGIME_TO_STRING.at(refreshRateRegime);
}

std::string RefreshRateManager::uxModeToString(RefreshRateManager::RefreshRateManager::UXMode uxMode) {
    return UX_MODE_TO_STRING.at(uxMode);
}

RefreshRateManager::RefreshRateManager() {
    _refreshRateProfile = (RefreshRateManager::RefreshRateProfile) _refreshRateProfileSetting.get();
    s_governorHeld = qEnvironmentVariableIntValue("TIVOLI_GOVERNOR_HOLD") > 0;
    if (s_governorHeld) {
        qCDebug(interfaceapp) << "Refresh governor: HELD at full rate by TIVOLI_GOVERNOR_HOLD (a measurement; not for riding)";
    }
    _inactiveTimer->setInterval(INACTIVE_TIMER_LIMIT);
    _inactiveTimer->setSingleShot(true);
    QObject::connect(_inactiveTimer.get(), &QTimer::timeout, [&] {
        toggleInactive();
    });
    _governorTimer->setInterval(1000);
    _governorTimer->setTimerType(Qt::CoarseTimer);
    QObject::connect(_governorTimer.get(), &QTimer::timeout, [&] {
        governorTick();
    });
    _governorTimer->start();
}

bool RefreshRateManager::governable(int displayRate) const {
    if (_uxMode != UXMode::DESKTOP || displayRate < MIN_GOVERNED_DISPLAY_RATE || halfRateOf(displayRate) == 0) {
        return false;
    }
    // the profile must ask for the display's own rate; a cap under it already paces slower
    if (queryRefreshRateTarget(getRefreshRateProfile(), effectiveRegime(), _uxMode) < 900) {
        return false;
    }
    if (_screenCap > 0 && _screenCap < displayRate) {
        return false;
    }
    if (environmentRefreshCap() > 0 && environmentRefreshCap() < displayRate) {
        return false;
    }
    return true;
}

RefreshRateManager::RefreshRateRegime RefreshRateManager::effectiveRegime() const {
    return (_refreshRateRegime == RefreshRateRegime::UNFOCUS && _rideKeepsFullRate) ? RefreshRateRegime::FOCUS_ACTIVE
                                                                                     : _refreshRateRegime;
}

void RefreshRateManager::updateRideExemption() {
    bool keep = false;
    if (_refreshRateRegime == RefreshRateRegime::UNFOCUS && _uxMode == UXMode::DESKTOP && _rideInViewSource) {
        QWindow* handle = qApp->getWindow() ? qApp->getWindow()->windowHandle() : nullptr;
        const bool visible = handle && handle->visibility() != QWindow::Minimized && handle->visibility() != QWindow::Hidden;
        keep = visible && _rideInViewSource();
    }
    if (keep != _rideKeepsFullRate) {
        _rideKeepsFullRate = keep;
        qCDebug(interfaceapp) << (keep ? "Refresh regime: unfocused, but a ride is in view: the full rate stays"
                                       : "Refresh regime: unfocused and no ride in view: the unfocused rate");
        _governorChangeNs = steadyNowNs();
    }
}

void RefreshRateManager::noteArrival() {
    _arrivalNs = steadyNowNs();
}

bool RefreshRateManager::inArrivalGrace() const {
    return _arrivalNs > 0 && steadyNowNs() - _arrivalNs < (int64_t)GOVERNOR_ARRIVAL_GRACE_SECONDS * 1000000000LL;
}

void RefreshRateManager::governorTick() {
    if (_refreshRateRegime == RefreshRateRegime::UNFOCUS) {
        const bool kept = _rideKeepsFullRate;
        updateRideExemption();
        if (kept != _rideKeepsFullRate) {
            updateRefreshRateController();
        }
    }
    if (!_cadenceSource) {
        return;
    }
    CadenceSample sample = _cadenceSource();
    if (sample.rolledAtNs == 0 || sample.rolledAtNs == _governorLastRollNs) {
        return;   // no new second counted (no presents: the window hidden, a pause)
    }
    _governorLastRollNs = sample.rolledAtNs;
    _governorDisplayRate = sample.displayRate;
    governorDiag(sample);
    // a second that began before the last change of target (a governor switch, a regime change, a rest's end), or
    // within the presents' settling after it, is mixed by construction: it carries no verdict (2026-10-31: the first
    // second after a return counted the half rate's own two-refresh presents as misses and dropped again). Judged by
    // the second's own start, not by a count of ticks: a grace of one tick was spent on the second rolled BEFORE a
    // click's change of regime and the mixed second was judged (Caity's ride of 11:3x dropped to the half rate 2 s
    // after each FocusInactive -> FocusActive flip, on the resting rate's own two-refresh presents, with 0 held)
    if (sample.startedAtNs < _governorChangeNs + GOVERNOR_SETTLE_NS) {
        updateGovernorStatus();
        return;
    }
    if (effectiveRegime() != RefreshRateRegime::FOCUS_ACTIVE) {
        // at rest, unfocused or minimized another rate rules; the governor's state keeps for the wake. Asked first:
        // the unfocused regime's own 30 Hz read as "a cap under the display's rate" below and reset the half rate
        // every time the window lost the focus with no ride in view, so a ride began at the full rate again after a
        // click away (performance's walled run and Caity's 11:45, 2026-10-31)
        return;
    }
    if (!governable(sample.displayRate)) {
        if (_halfRate) {
            _halfRate = false;
            _governorFitSeconds = 0;
            qCDebug(interfaceapp) << "Refresh governor: full rate again, the rate is set elsewhere now (profile, a cap, VR or the display)";
            updateRefreshRateController();
            updateGovernorStatus();
        }
        return;
    }
    const int displayRate = sample.displayRate;
    if (!_halfRate) {
        if (sample.presents < displayRate / 2) {
            return;   // not presenting for the whole second: no verdict
        }
        const int misses = sample.repeated + sample.onTwoOrMoreRefreshes + sample.missed;
        if (_governorProbeSecondsLeft > 0) {
            _governorProbeSecondsLeft--;
        }
        _governorCleanSeconds = misses == 0 ? _governorCleanSeconds + 1 : 0;
        if (_governorCleanSeconds >= GOVERNOR_FORGIVE_SECONDS) {
            _governorHoldSeconds = GOVERNOR_HOLD_MIN_SECONDS;
        }
        if (misses >= GOVERNOR_DROP_MISSES && !inArrivalGrace() && !s_governorHeld) {   // arriving or held: counted and shown, never acted on
            const bool returnFailed = _governorProbeSecondsLeft > 0;
            if (returnFailed) {
                _governorHoldSeconds = std::min(_governorHoldSeconds * 2, GOVERNOR_HOLD_MAX_SECONDS);
            }
            _halfRate = true;
            _governorFitSeconds = 0;
            _governorHalfRateSeconds = 0;
            _governorProbeSecondsLeft = 0;
            _governorChangeNs = steadyNowNs();
            qCDebug(interfaceapp).nospace() << "Refresh governor: half rate, " << halfRateOf(displayRate) << " of " << displayRate
                << " Hz: in the last second " << sample.repeated << " presents showed the frame before again, "
                << sample.onTwoOrMoreRefreshes << " landed two or more refreshes apart and " << sample.missed
                << " reached the display with under " << RefreshRateController::PRESENT_SLACK_FLOOR_US / 1000 << " ms to spare"
                << (returnFailed ? " (the return failed; hold now " : " (hold ") << _governorHoldSeconds << " s)";
            updateRefreshRateController();
        }
    } else {
        _governorHalfRateSeconds++;
        // the way back: each stage's own work inside 70 % of a full-rate refresh (the main thread's frame, the render
        // thread's build, the present's own work; a pipelined 144 Hz needs each stage under a refresh, not their sum),
        // with a stage's slow frames a second, scaled to the full rate, under the drop threshold: what the full rate
        // could carry without a miss
        const float refreshUs = 1000000.0f / (float)displayRate;
        const int halfRate = std::max(1, halfRateOf(displayRate));
        const int scale = std::max(1, displayRate / halfRate);
        const bool fits = sample.newFrames > 0 && sample.presents > 0 &&
            sample.slowMainFrames * scale < GOVERNOR_DROP_MISSES &&
            sample.slowBuildFrames * scale < GOVERNOR_DROP_MISSES &&
            sample.slowExecutes * scale < GOVERNOR_DROP_MISSES &&
            sample.gpuMs * 1000.0f < refreshUs * 0.7f;
        _governorFitSeconds = fits ? _governorFitSeconds + 1 : 0;
        if (_governorFitSeconds >= _governorHoldSeconds) {
            _halfRate = false;
            _governorProbeSecondsLeft = GOVERNOR_PROBE_SECONDS;
            _governorFitSeconds = 0;
            _governorCleanSeconds = 0;
            _governorChangeNs = steadyNowNs();
            qCDebug(interfaceapp).nospace() << "Refresh governor: full rate, " << displayRate << " Hz, after "
                << _governorHalfRateSeconds << " s at half rate: the frames fitted a refresh for " << _governorHoldSeconds << " s";
            updateRefreshRateController();
        }
    }
    updateGovernorStatus();   // once a second, here: the Stats line's reader only copies it
}

void RefreshRateManager::updateGovernorStatus() {
    if (_halfRate) {
        _governorStatus = QString("half rate (%1 of %2 Hz), back after %3 s of fitting frames (%4 so far)")
            .arg(halfRateOf(_governorDisplayRate)).arg(_governorDisplayRate).arg(_governorHoldSeconds).arg(_governorFitSeconds);
    } else if (_governorHoldSeconds > GOVERNOR_HOLD_MIN_SECONDS) {
        _governorStatus = QString("full rate (hold %1 s after a failed return)").arg(_governorHoldSeconds);
    } else {
        _governorStatus = QString("full rate");
    }
}

// TIVOLI_GOVERNOR_DIAG in the environment: a line a second with what the governor saw and holds, for a ride's record
// (the stages' slow counts and maxima at both rates are what tunes the thresholds)
void RefreshRateManager::governorDiag(const CadenceSample& sample) const {
    static const bool GOVERNOR_DIAG = QProcessEnvironment::systemEnvironment().contains("TIVOLI_GOVERNOR_DIAG");
    if (!GOVERNOR_DIAG) {
        return;
    }
    qCDebug(interfaceapp).nospace() << "Refresh governor diag: " << (s_governorHeld ? "held" : (_halfRate ? "half" : "full")) << " rate, target "
        << _activeRefreshRate << " of " << sample.displayRate << " Hz; last second: presents " << sample.presents
        << " (on 1 refresh " << sample.onOneRefresh << ", on 2+ " << sample.onTwoOrMoreRefreshes << ", held " << sample.repeated
        << "), frames " << sample.newFrames << " (main slow " << sample.slowMainFrames << ", max " << sample.maxMainUs / 1000.0
        << " ms; build slow " << sample.slowBuildFrames << ", max " << sample.maxBuildUs / 1000.0 << " ms), execute slow "
        << sample.slowExecutes << " (max " << sample.maxExecuteUs / 1000.0 << " ms), GPU " << sample.gpuMs << " ms, slack min "
        << sample.minSlackUs / 1000.0 << " ms, under " << RefreshRateController::PRESENT_SLACK_FLOOR_US / 1000 << " ms " << sample.missed
        << " of " << sample.slackPresents << "; fit "
        << _governorFitSeconds << " of " << _governorHoldSeconds << " s"
        << (sample.startedAtNs < _governorChangeNs + GOVERNOR_SETTLE_NS ? ", mixed (no verdict)" : "")
        << (_governorProbeSecondsLeft > 0 ? ", probe" : "")
        << (inArrivalGrace() ? "; arriving" : "");
}

void RefreshRateManager::resetInactiveTimer(bool deliberate) {
    if (_uxMode == RefreshRateManager::UXMode::DESKTOP) {
        auto regime = getRefreshRateRegime();
        if (regime == RefreshRateRegime::FOCUS_ACTIVE || regime == RefreshRateRegime::FOCUS_INACTIVE ||
            (deliberate && regime == RefreshRateRegime::UNFOCUS)) {
            _inactiveTimer->start();
            setRefreshRateRegime(RefreshRateManager::RefreshRateRegime::FOCUS_ACTIVE);
        }
    }
}

void RefreshRateManager::toggleInactive() {
    if (_uxMode == RefreshRateManager::UXMode::DESKTOP &&
        getRefreshRateRegime() == RefreshRateManager::RefreshRateRegime::FOCUS_ACTIVE) {
        setRefreshRateRegime(RefreshRateManager::RefreshRateRegime::FOCUS_INACTIVE);
    }
}

void RefreshRateManager::setRefreshRateProfile(RefreshRateManager::RefreshRateProfile refreshRateProfile) {
    if (isValidRefreshRateProfile(refreshRateProfile) && (_refreshRateProfile != refreshRateProfile)) {
        _refreshRateProfileSettingLock.withWriteLock([&] {
            _refreshRateProfile = refreshRateProfile;
            _refreshRateProfileSetting.set((int) refreshRateProfile);
        });
        updateRefreshRateController();
    }
}

RefreshRateManager::RefreshRateProfile RefreshRateManager::getRefreshRateProfile() const {
    RefreshRateManager::RefreshRateProfile profile = RefreshRateManager::RefreshRateProfile::DISPLAY_BASED;

    if (getUXMode() != RefreshRateManager::UXMode::VR) {
        return _refreshRateProfile;
    }

    return profile;
}

RefreshRateManager::RefreshRateRegime RefreshRateManager::getRefreshRateRegime() const {
    if (getUXMode() == RefreshRateManager::UXMode::VR) {
        return RefreshRateManager::RefreshRateRegime::FOCUS_ACTIVE;
    } else {
        return _refreshRateRegime;
    }
}

void RefreshRateManager::setRefreshRateRegime(RefreshRateManager::RefreshRateRegime refreshRateRegime) {
    if (isValidRefreshRateRegime(refreshRateRegime) && (_refreshRateRegime != refreshRateRegime)) {
        _refreshRateRegime = refreshRateRegime;
        // in front again (or for the first time, after start-up): the inactive count starts now, not at the first input
        if (refreshRateRegime == RefreshRateRegime::FOCUS_ACTIVE && _uxMode == UXMode::DESKTOP) {
            _inactiveTimer->start();
        }
        updateRideExemption();   // a ride in view keeps the full rate through the unfocused regime (no blip at the change)
        updateRefreshRateController();
        _governorChangeNs = steadyNowNs();   // a second that began before this carries no verdict (governorTick)
        // one line a change (a wake or a rest every few seconds at most), with the time, to line up with an input's
        qCDebug(interfaceapp).noquote() << "Refresh regime" << QString::fromStdString(refreshRateRegimeToString(refreshRateRegime))
            << "target" << _activeRefreshRate << "Hz at usec" << usecTimestampNow();
    }
}

void RefreshRateManager::noteCameraTransform(const glm::mat4& eyeToWorld) {
    // measured from the pose at the last activity, not from the last frame, so the slow sway of an idle avatar's head
    // (first person) doesn't count while a ride's or a vehicle's steady motion does, within a few frames
    static const float MOVE_METERS = 0.02f;
    static const float TURN_COSINE = 0.99985f;  // about 1 degree
    const glm::vec3 position(eyeToWorld[3]);
    const glm::vec3 forward = glm::normalize(-glm::vec3(eyeToWorld[2]));
    if (!_restPoseValid || glm::distance(position, _restPosition) > MOVE_METERS ||
        glm::dot(forward, _restForward) < TURN_COSINE) {
        _restPosition = position;
        _restForward = forward;
        _restPoseValid = true;
        resetInactiveTimer();
    }
}

void RefreshRateManager::setScreenCap(int maxRate) {
    if (_screenCap != maxRate) {
        _screenCap = maxRate;
        qCDebug(interfaceapp) << "Screen cap:" << (maxRate > 0 ? QString::number(maxRate) + " Hz" : QString("none"));
        updateRefreshRateController();
    }
}

void RefreshRateManager::setUXMode(RefreshRateManager::UXMode uxMode) {
    if (isValidUXMode(uxMode) && (_uxMode != uxMode)) {
        _uxMode = uxMode;
        updateRefreshRateController();
    }
}

int RefreshRateManager::queryRefreshRateTarget(RefreshRateProfile profile, RefreshRateRegime regime, UXMode uxMode) const {
    int targetRefreshRate = VR_TARGET_RATE;
    if (uxMode == RefreshRateManager::UXMode::DESKTOP) {
        targetRefreshRate = REFRESH_RATE_PROFILES[profile][regime];
    }
    return targetRefreshRate;
}

void RefreshRateManager::updateRefreshRateController() const {
    if (_refreshRateOperator) {
        int targetRefreshRate = queryRefreshRateTarget(_refreshRateProfile, effectiveRegime(), _uxMode);
        // rounded, not truncated: a panel reporting 143.98 Hz is a 144 Hz one (truncated to 143, the pacing repeated a
        // frame about once a second, 10-21 review); 0 while the window has no screen yet
        QWindow* handle = qApp->getWindow() ? qApp->getWindow()->windowHandle() : nullptr;
        QScreen* screen = handle ? handle->screen() : nullptr;
        int displayRefreshRate = screen ? qRound(screen->refreshRate()) : 0;

        if (targetRefreshRate == RESTING_RATE) {
            const int MIN_RESTING_DISPLAY_RATE = 120;
            const int restingRate = halfRateOf(displayRefreshRate);
            targetRefreshRate = displayRefreshRate >= MIN_RESTING_DISPLAY_RATE && restingRate > 0 ? restingRate : 999;
        }
        if (targetRefreshRate >= 900) {
            targetRefreshRate = displayRefreshRate;
            // incase refreshRate() is incorrect
            if (targetRefreshRate < 60) {
                targetRefreshRate = 60;
            }
            // the cadence governor's half rate (governorTick), in phase: every frame for the same two refreshes
            if (_halfRate && _uxMode == UXMode::DESKTOP && displayRefreshRate >= MIN_GOVERNED_DISPLAY_RATE &&
                halfRateOf(displayRefreshRate) > 0) {
                targetRefreshRate = halfRateOf(displayRefreshRate);
            }
        }
        if (_screenCap > 0 && _uxMode == UXMode::DESKTOP && targetRefreshRate > _screenCap) {
            targetRefreshRate = _screenCap;
        }
        // the environment's cap, for hand tests (environmentRefreshCap)
        const int environmentCap = environmentRefreshCap();
        if (environmentCap > 0 && _uxMode == UXMode::DESKTOP && targetRefreshRate > environmentCap) {
            targetRefreshRate = environmentCap;
        }

        _refreshRateOperator(targetRefreshRate, displayRefreshRate);
        _activeRefreshRate = targetRefreshRate;
    }
}
