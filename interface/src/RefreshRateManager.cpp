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


#include "RefreshRateManager.h"

#include "InterfaceLogging.h"

#include <SharedUtil.h>

#include <QtGui/QScreen>
#include <QtGui/QWindow>

#include <array>

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
    _inactiveTimer->setInterval(INACTIVE_TIMER_LIMIT);
    _inactiveTimer->setSingleShot(true);
    QObject::connect(_inactiveTimer.get(), &QTimer::timeout, [&] {
        toggleInactive();
    });
}

void RefreshRateManager::resetInactiveTimer() {
    if (_uxMode == RefreshRateManager::UXMode::DESKTOP) {
        auto regime = getRefreshRateRegime();
        if (regime == RefreshRateRegime::FOCUS_ACTIVE || regime == RefreshRateRegime::FOCUS_INACTIVE) {
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
        updateRefreshRateController();
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
        int targetRefreshRate = queryRefreshRateTarget(_refreshRateProfile, _refreshRateRegime, _uxMode);
        // rounded, not truncated: a panel reporting 143.98 Hz is a 144 Hz one (truncated to 143, the pacing repeated a
        // frame about once a second, 10-21 review); 0 while the window has no screen yet
        QWindow* handle = qApp->getWindow() ? qApp->getWindow()->windowHandle() : nullptr;
        QScreen* screen = handle ? handle->screen() : nullptr;
        int displayRefreshRate = screen ? qRound(screen->refreshRate()) : 0;

        if (targetRefreshRate == RESTING_RATE) {
            const int MIN_RESTING_DISPLAY_RATE = 120;
            targetRefreshRate = displayRefreshRate >= MIN_RESTING_DISPLAY_RATE ? displayRefreshRate / 2 : 999;
        }
        if (targetRefreshRate >= 900) {
            targetRefreshRate = displayRefreshRate;
            // incase refreshRate() is incorrect
            if (targetRefreshRate < 60) {
                targetRefreshRate = 60;
            }
        }
        if (_screenCap > 0 && _uxMode == UXMode::DESKTOP && targetRefreshRate > _screenCap) {
            targetRefreshRate = _screenCap;
        }
        // DARKRIDE_REFRESH_CAP=<Hz> in the environment caps the desktop's rate at that number, for a hand test at a
        // whole fraction of the display's rate (72 or 48 on a 144 Hz panel; the presents then lock to every second or
        // third refresh, RefreshRateController::sleepThreadIfNeeded): PHYSICS.md "The monorail's judder", 2026-10-31
        static const int environmentCap = qEnvironmentVariableIntValue("DARKRIDE_REFRESH_CAP");
        if (environmentCap > 0 && _uxMode == UXMode::DESKTOP && targetRefreshRate > environmentCap) {
            targetRefreshRate = environmentCap;
        }

        _refreshRateOperator(targetRefreshRate, displayRefreshRate);
        _activeRefreshRate = targetRefreshRate;
    }
}
