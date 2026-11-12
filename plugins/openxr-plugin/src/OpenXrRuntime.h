//
//  OpenXrRuntime.h
//  plugins/openxr-plugin/src
//
//  Created on 2026-10-16 for Quest 3 hand tracking over Link (Meta's PC OpenXR runtime).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_OpenXrRuntime_h
#define hifi_OpenXrRuntime_h

#include <array>
#include <atomic>
#include <mutex>
#include <thread>

#include <QtCore/QLoggingCategory>
#include <QtCore/QString>

#if defined(Q_OS_WIN)
#include <Windows.h>
#define XR_USE_PLATFORM_WIN32
#endif
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <GLMHelpers.h>
#include <controllers/Pose.h>

Q_DECLARE_LOGGING_CATEGORY(openxr)

// The OpenXR instance, system and session, shared by the display plugin and the input plugin.
//
// The instance and system are made when the plugin is loaded, so the plugin is only offered when an OpenXR headset is
// connected. The session needs the present thread's OpenGL context, so the display plugin makes it in
// customizeContext(), and the input plugin waits for it.
//
// Poses are in "sensor" space: the stage (the floor at the centre of the play area) when the runtime has one, with
// metres, +Y up and -Z forward, the same as OpenVR's standing universe.
class OpenXrRuntime {
public:
    enum Hand { LeftHand = 0, RightHand, HandCount };

    OpenXrRuntime();
    ~OpenXrRuntime();

    // An OpenXR runtime is installed and active, and it reports a headset.
    bool isSupported() const { return _system != XR_NULL_SYSTEM_ID; }
    const QString& getSystemName() const { return _systemName; }
    const QString& getRuntimeName() const { return _runtimeName; }
    XrInstance getInstance() const { return _instance; }
    XrSystemId getSystemId() const { return _system; }

    // Present thread, with the display plugin's OpenGL context current.
    bool createSession();
    void destroySession();   // a running session is ended first (endRunningSession)
    // Present thread. Handles session state changes; returns false when the runtime wants the app to quit.
    bool pollEvents();
    // Quit: ends a running session (quickly, with no frame loop: the GL context may be gone by then), destroys the
    // session and the instance, so nothing of the runtime's is torn down under it when this library unloads after Qt
    // and GL. Idempotent; the destructor calls it too.
    void shutdown();

    XrSession getSession() const { return _session; }
    XrSpace getReferenceSpace() const { return _referenceSpace; }
    XrSpace getViewSpace() const { return _viewSpace; }
    bool isSessionRunning() const { return _sessionRunning; }
    // The runtime took the session away (the link to the headset dropped, the runtime is going): no frame call is made
    // on it again, from either thread; the next createSession destroys it and makes a new one
    bool isSessionLost() const { return _sessionLost; }
    // the lost session couldn't be replaced in this run (the runtime refused a new one, or its instance went)
    bool needsRestart() const { return _needsRestart; }
    // The OpenXR display is the active one. Leaving VR keeps the session (Meta's runtime refuses a second one in the same
    // run), so the input side stops reading the headset and controllers while this is false.
    bool isDisplayActive() const { return _displayActive; }
    // Off: a running session is fed empty frames by a thread of its own until VR is entered (startIdleFrames). On: that
    // thread stops before the present thread's frames begin.
    void setDisplayActive(bool active);
    bool isSessionFocused() const { return _sessionState == XR_SESSION_STATE_FOCUSED; }
    bool shouldRender() const;
    bool isUserPresent() const { return _userPresent; }
    // The headset is off the player's head: the runtime stopped the session (Meta's over Link: FOCUSED, VISIBLE,
    // SYNCHRONIZED, STOPPING, IDLE within a second of taking it off; READY again when it goes back on), or a presence
    // event said so. Before a session, unknown: not off.
    bool isHeadsetOff() const;

    // Frame timing, written by the present thread after each xrWaitFrame.
    void setFrameTiming(XrTime predictedDisplayTime, XrDuration predictedDisplayPeriod);
    // The time the frame being simulated now is likely to be shown.
    XrTime getNextDisplayTime() const;
    XrDuration getDisplayPeriod() const { return _displayPeriod; }

    // The pose of a space in sensor space, or an invalid pose.
    controller::Pose locate(XrSpace space, XrTime time) const;

    // XR_EXT_hand_tracking, when the runtime offers it (Meta's PC runtime does only with
    // "Developer runtime features" on in the Meta Horizon Link app).
    bool supportsHandTracking() const { return _handTrackingSupported; }
    PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT { nullptr };
    PFN_xrDestroyHandTrackerEXT xrDestroyHandTrackerEXT { nullptr };
    PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT { nullptr };

    // Logs a failed call and returns false.
    bool check(XrResult result, const char* what) const;

    XrPath toPath(const char* path) const;
    const std::array<XrPath, HandCount>& getHandPaths() const { return _handPaths; }

    // Increments every time the session is made, so the input plugin knows to remake its actions.
    uint32_t getSessionGeneration() const { return _sessionGeneration; }

private:
    void endRunningSession(bool withFrames);   // withFrames: the frame loop keeps going until STOPPING (present thread)
    void beginSession();
    // A session running while the OpenXR display is off (the headset put on at the desk, or VR left with it on) is fed
    // empty frames by a thread of its own, so that its state at the runtime and here stay the same. Begun and left
    // without frames, Meta's runtime over Link walked the session up to FOCUSED and down to STOPPING by itself within a
    // second, refused the end (not running) and never offered the session again: the next entry into VR began it and
    // drew nothing, a black headset with a live preview (Caity's Quest 3, 2026-10-29).
    void startIdleFrames();
    void stopIdleFrames();   // joins the thread (which takes no lock but _frameMutex); from any thread but its own
    // One frame of nothing: wait, begin, end with no layers. Under _frameMutex, so the session isn't ended mid-frame.
    bool submitEmptyFrame();
    bool createInstance();
    bool findSystem();
    void updateSessionState(XrSessionState state);

    XrInstance _instance { XR_NULL_HANDLE };
    XrSystemId _system { XR_NULL_SYSTEM_ID };
    XrSession _session { XR_NULL_HANDLE };
    XrSpace _referenceSpace { XR_NULL_HANDLE };
    XrSpace _viewSpace { XR_NULL_HANDLE };
    std::array<XrPath, HandCount> _handPaths {{ XR_NULL_PATH, XR_NULL_PATH }};

    QString _systemName;
    QString _runtimeName;

    bool _handTrackingExtension { false };
    bool _handTrackingSupported { false };
    bool _userPresenceExtension { false };

    std::atomic<XrSessionState> _sessionState { XR_SESSION_STATE_UNKNOWN };
    // mutable: check() is const and is where a lost session is seen (its failure branch only)
    mutable std::atomic<bool> _sessionRunning { false };
    mutable std::atomic<bool> _sessionLost { false };
    mutable std::atomic<bool> _needsRestart { false };
    std::atomic<bool> _exitAsked { false };   // we asked the session to exit (xrRequestExitSession)
    void markSessionLost() const;
    std::atomic<bool> _displayActive { false };
    std::mutex _eventMutex;   // pollEvents and the session's begin and end, from whichever thread polls
    std::mutex _frameMutex;   // a frame (wait, begin, end) and the session's end never overlap; taken after _eventMutex
    std::thread _idleFrames;
    std::atomic<bool> _idleFramesStop { false };
    std::atomic<bool> _userPresent { true };
    std::atomic<uint32_t> _sessionGeneration { 0 };

    std::atomic<XrTime> _displayTime { 0 };
    std::atomic<XrDuration> _displayPeriod { 0 };
};

inline glm::vec3 toGlm(const XrVector3f& v) {
    return glm::vec3(v.x, v.y, v.z);
}

inline glm::quat toGlm(const XrQuaternionf& q) {
    return glm::quat(q.w, q.x, q.y, q.z);
}

#endif // hifi_OpenXrRuntime_h
