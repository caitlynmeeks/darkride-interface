//
//  RefreshRateManager.h
//  interface/src/
//
//  Created by Dante Ruiz on 2019-04-15.
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: the whole file (see RefreshRateManager.cpp).
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.

#ifndef hifi_RefreshRateManager_h
#define hifi_RefreshRateManager_h

#include <map>
#include <string>
#include <functional>

#include <QTimer>
#include <glm/glm.hpp>
#include <QScreen>

#include <SettingHandle.h>
#include <shared/ReadWriteLockable.h>

class RefreshRateManager {
public:
    enum RefreshRateProfile {
        ECO = 0,
        INTERACTIVE,
        REALTIME,
        DISPLAY_BASED,
        PROFILE_NUM
    };
    static bool isValidRefreshRateProfile(RefreshRateProfile value) {
        return (value >= RefreshRateProfile::ECO && value <= RefreshRateProfile::DISPLAY_BASED);
    }

    /**jsdoc
     * <p>Interface states that affect the refresh rate.</p>
     * <table>
     *   <thead>
     *     <tr><th>Value</th><th>Name</th><th>Description</th>
     *   </thead>
     *   <tbody>
     *     <tr><td><code>0</code></td><td>FOCUS_ACTIVE</td><td>Interface has focus and the user is active or is in VR.</td></tr>
     *     <tr><td><code>1</code></td><td>FOCUS_INACTIVE</td><td>Interface has focus and the user is inactive.</td></tr>
     *     <tr><td><code>2</code></td><td>UNFOCUS</td><td>Interface doesn't have focus.</td></tr>
     *     <tr><td><code>3</code></td><td>MINIMIZED</td><td>Interface is minimized.</td></tr>
     *     <tr><td><code>4</code></td><td>STARTUP</td><td>Interface is starting up.</td></tr>
     *     <tr><td><code>5</code></td><td>SHUTDOWN</td><td>Interface is shutting down.</td></tr>
     *   </tbody>
     * </table>
     * @typedef {number} RefreshRateRegime
     */
    enum RefreshRateRegime {
        FOCUS_ACTIVE = 0,
        FOCUS_INACTIVE,
        UNFOCUS,
        MINIMIZED,
        STARTUP,
        SHUTDOWN,
        REGIME_NUM
    };
    static bool isValidRefreshRateRegime(RefreshRateRegime value) { return (value >= RefreshRateRegime::FOCUS_ACTIVE && value <= RefreshRateRegime::SHUTDOWN); }

    /**jsdoc
     * <p>User experience (UX) modes.</p>
     * <table>
     *   <thead>
     *     <tr><th>Value</th><th>Name</th><th>Description</th>
     *   </thead>
     *   <tbody>
     *     <tr><td><code>0</code></td><td>DESKTOP</td><td>Desktop user experience.</td></tr>
     *     <tr><td><code>1</code></td><td>VR</td><td>VR use experience.</td></tr>
     *   </tbody>
     * </table>
     * @typedef {number} UXMode
     */
    enum UXMode {
        DESKTOP = 0,
        VR,
        UX_NUM
    };
    static bool isValidUXMode(UXMode value) { return (value >= UXMode::DESKTOP && value <= UXMode::VR); }

    RefreshRateManager();
    ~RefreshRateManager() = default;

    void setRefreshRateProfile(RefreshRateProfile refreshRateProfile);
    RefreshRateProfile getRefreshRateProfile() const;

    void setRefreshRateRegime(RefreshRateRegime refreshRateRegime);
    RefreshRateRegime getRefreshRateRegime() const;

    void setUXMode(UXMode uxMode);
    UXMode getUXMode() const { return _uxMode; }

    // (the target rate, the display's own rate or 0 when unknown)
    void setRefreshRateOperator(std::function<void(int, int)> refreshRateOperator) { _refreshRateOperator = refreshRateOperator; }

    // The cadence governor (Caity, 2026-10-31, "go on the half-rate fix"). At the display's own rate, when the frames
    // don't fit a refresh (presents showing the frame before again, or landing two refreshes apart: the picture
    // holds a refresh and then steps twice, the hitch of a heavy world at 144 Hz), the target drops to half the
    // display's rate, in phase, where every frame shows for the same two refreshes and the motion is even. It comes
    // back up once the frames have fitted comfortably inside a refresh for a hold (their latency, the present's cost
    // and the GPU's time, from the present cadence's counters), and the hold doubles after each return that failed,
    // so it cannot flap. Desktop only, displays of 100 Hz and up, the display-based profile's active regime; the
    // resting rate and the caps stand on top. What it reads comes once a second through the cadence source.
    struct CadenceSample {
        int displayRate { 0 };
        int presents { 0 };
        int repeated { 0 };
        int onTwoOrMoreRefreshes { 0 };
        int onOneRefresh { 0 };
        int newFrames { 0 };
        int slowMainFrames { 0 };
        int slowBuildFrames { 0 };
        int slowExecutes { 0 };
        int maxMainUs { 0 };
        int maxBuildUs { 0 };
        int maxExecuteUs { 0 };
        float gpuMs { 0.0f };
        int64_t rolledAtNs { 0 };
        int64_t startedAtNs { 0 };
    };
    void setCadenceSource(std::function<CadenceSample()> source) { _cadenceSource = source; }

    // Caity, 2026-10-31: no drop to the unfocused rate while the window is visible and a ride is in the picture (the
    // client's avatar on a frame or vehicle, a moving vehicle, train car, frame or platform with parts drawn in the
    // view within the last second, or another avatar riding one in the view): the full regime stays and the governor
    // decides. A minimized or hidden window, or nobody riding, drops as before; a ride that ends while unfocused
    // drops within a second, with a grace second for the governor. The source answers "is a ride in view", asked
    // when the regime turns unfocused and once a second while it is.
    void setRideInViewSource(std::function<bool()> source) { _rideInViewSource = source; }
    bool isRideKeepingFullRate() const { return _rideKeepsFullRate; }
    bool isHalfRate() const { return _halfRate; }
    int getGovernorHoldSeconds() const { return _governorHoldSeconds; }
    // one line for the Stats panel: "full rate" / "half rate (72 of 144 Hz), back after 5 s of fitting frames (2 so far)";
    // rebuilt once a second by the tick, so a reader every frame only copies it
    const QString& getGovernorStatus() const { return _governorStatus; }
    static const int MIN_GOVERNED_DISPLAY_RATE = 100;

    // A cap for what is on the desktop's screen, in Hz, 0 for none; Application::update sets it each frame. The sign-in
    // screen moves nothing faster than a caret (SIGN_IN_SCREEN_RATE); a loading screen keeps 60 (LOADING_SCREEN_RATE),
    // so a load paces its per-frame work (texture uploads, renderable updates) no slower than on a 60 Hz display today.
    // Without it a 144 or 240 Hz display drew the scene behind those screens at its full rate, the GPU at 100 % through
    // the whole start-up (a Windows player, 10-23). A headset keeps its rate.
    void setScreenCap(int maxRate);
    static const int SIGN_IN_SCREEN_RATE = 30;
    static const int LOADING_SCREEN_RATE = 60;
    int getActiveRefreshRate() const { return _activeRefreshRate; }
    void updateRefreshRateController() const;

    // query the refresh rate target at the specified combination
    int queryRefreshRateTarget(RefreshRateProfile profile, RefreshRateRegime regime, UXMode uxMode) const;

    // an input ends the rest (FOCUS_INACTIVE -> FOCUS_ACTIVE); a deliberate one (a button, a key, the wheel: not a mouse
    // move on the way to another app) also ends the unfocused regime, since macOS delivers a right click to a window
    // without activating it (Caity, 2026-10-31: the camera turned at 30 fps after a right click back into the window)
    void resetInactiveTimer(bool deliberate = false);
    void toggleInactive();

    // The resting rate (Display-based's FocusInactive, RESTING_RATE): after INACTIVE_TIMER_LIMIT with no input and this
    // view held still, half the display's rate, back to full on the next input or move. Application::update gives the
    // camera each frame: a camera that moves without input (a ride, a vehicle) counts as activity.
    void noteCameraTransform(const glm::mat4& eyeToWorld);
    // A world's arrival (the domain handler's connectedToDomain): for GOVERNOR_ARRIVAL_GRACE_SECONDS after it the cadence
    // governor counts and shows a second's slipped presents but does not drop on them (entry 28): the loading seconds
    // are not the world's cadence. A new domain stamps again.
    void noteArrival();
    bool inArrivalGrace() const;
    static const int RESTING_RATE = 998;

    static std::string refreshRateProfileToString(RefreshRateProfile refreshRateProfile);
    static RefreshRateProfile refreshRateProfileFromString(std::string refreshRateProfile);
    static std::string uxModeToString(UXMode uxMode);
    static std::string refreshRateRegimeToString(RefreshRateRegime refreshRateRegime);

private:
    mutable int _activeRefreshRate { 60 };
    RefreshRateProfile _refreshRateProfile { RefreshRateProfile::DISPLAY_BASED };
    RefreshRateRegime _refreshRateRegime { RefreshRateRegime::STARTUP };
    UXMode _uxMode;

    mutable ReadWriteLockable _refreshRateProfileSettingLock;
    Setting::Handle<int> _refreshRateProfileSetting { "refreshRateProfile", RefreshRateProfile::DISPLAY_BASED };

    std::function<void(int, int)> _refreshRateOperator { nullptr };
    int _screenCap { 0 };

    std::shared_ptr<QTimer> _inactiveTimer { std::make_shared<QTimer>() };

    void governorTick();
    void updateRideExemption();
    RefreshRateRegime effectiveRegime() const;
    std::function<bool()> _rideInViewSource;
    bool _rideKeepsFullRate { false };
    void updateGovernorStatus();
    void governorDiag(const CadenceSample& sample) const;
    bool governable(int displayRate) const;
    QString _governorStatus { "full rate" };
    int64_t _governorChangeNs { 0 };      // when the target last changed, on std::chrono::steady_clock: a second that began before it (or within the presents' settling after it) carries no verdict
    int64_t _arrivalNs { 0 };            // when the client last connected to a domain (steady clock); 0 = never
    std::function<CadenceSample()> _cadenceSource;
    std::shared_ptr<QTimer> _governorTimer { std::make_shared<QTimer>() };
    bool _halfRate { false };
    int _governorHoldSeconds { 5 };       // seconds of fitting frames before a return; doubles after a failed return
    int _governorFitSeconds { 0 };        // fitting seconds in a row at half rate
    int _governorProbeSecondsLeft { 0 };  // after a return: a drop within these marks the return failed
    int _governorCleanSeconds { 0 };      // clean seconds in a row at full rate: a long run forgives the back-off
    int _governorHalfRateSeconds { 0 };
    int64_t _governorLastRollNs { 0 };
    int _governorDisplayRate { 0 };

    // the camera's pose at the last activity
    bool _restPoseValid { false };
    glm::vec3 _restPosition;
    glm::vec3 _restForward;
};

#endif
