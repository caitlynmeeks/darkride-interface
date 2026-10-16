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
    void destroySession();
    // Present thread. Handles session state changes; returns false when the runtime wants the app to quit.
    bool pollEvents();

    XrSession getSession() const { return _session; }
    XrSpace getReferenceSpace() const { return _referenceSpace; }
    XrSpace getViewSpace() const { return _viewSpace; }
    bool isSessionRunning() const { return _sessionRunning; }
    bool isSessionFocused() const { return _sessionState == XR_SESSION_STATE_FOCUSED; }
    bool shouldRender() const;
    bool isUserPresent() const { return _userPresent; }

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
    std::atomic<bool> _sessionRunning { false };
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
