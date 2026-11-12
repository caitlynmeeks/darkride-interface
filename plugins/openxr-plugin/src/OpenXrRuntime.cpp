//
//  OpenXrRuntime.cpp
//  plugins/openxr-plugin/src
//
//  Created on 2026-10-16 for Quest 3 hand tracking over Link (Meta's PC OpenXR runtime).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "OpenXrRuntime.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <QtCore/QCoreApplication>
#include <QtCore/QSettings>

Q_LOGGING_CATEGORY(openxr, "hifi.plugins.openxr")

static const char* APPLICATION_NAME = "Darkride";

// SteamVR is already reached through the openvr plugin, and making an OpenXR instance on it starts SteamVR, so the
// OpenXR plugin leaves it alone unless it is asked for with --openxr.
static bool activeRuntimeIsSteamVr() {
    QString runtimeJson = qEnvironmentVariable("XR_RUNTIME_JSON");
#if defined(Q_OS_WIN)
    if (runtimeJson.isEmpty()) {
        QSettings khronos("HKEY_LOCAL_MACHINE\\SOFTWARE\\Khronos\\OpenXR\\1", QSettings::NativeFormat);
        runtimeJson = khronos.value("ActiveRuntime").toString();
    }
#endif
    return runtimeJson.contains("steamvr", Qt::CaseInsensitive);
}

OpenXrRuntime::OpenXrRuntime() {
    auto arguments = QCoreApplication::arguments();
    if (arguments.contains("--no-openxr")) {
        return;
    }
    if (!arguments.contains("--openxr") && activeRuntimeIsSteamVr()) {
        qCInfo(openxr) << "The active OpenXR runtime is SteamVR, which the OpenVR plugin handles; use --openxr to use it through OpenXR";
        return;
    }
    if (!createInstance()) {
        return;
    }
    findSystem();
}

OpenXrRuntime::~OpenXrRuntime() {
    shutdown();
}

void OpenXrRuntime::shutdown() {
    stopIdleFrames();
    if (_session != XR_NULL_HANDLE) {
        if (_sessionRunning) {
            endRunningSession(false);
        }
        destroySession();
    }
    if (_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(_instance);
        _instance = XR_NULL_HANDLE;
        _system = XR_NULL_SYSTEM_ID;
        qCDebug(openxr) << "OpenXR instance destroyed";
    }
}

void OpenXrRuntime::setDisplayActive(bool active) {
    // under the event lock, so a READY answered meanwhile (beginSession, which starts the idle frames while the display
    // is off) sees the flag as it is now, never an idle thread started after the stop below
    std::lock_guard<std::mutex> lock(_eventMutex);
    _displayActive = active;
    if (active) {
        // the present thread's frame loop takes over from the idle frames
        stopIdleFrames();
        if (_session != XR_NULL_HANDLE && !_sessionRunning && _sessionState == XR_SESSION_STATE_READY) {
            // offered while the display was off and not begun then (a refused begin): once more
            beginSession();
        }
    } else if (_sessionRunning) {
        startIdleFrames();
    }
}

void OpenXrRuntime::beginSession() {
    XrSessionBeginInfo beginInfo { XR_TYPE_SESSION_BEGIN_INFO };
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    _sessionRunning = check(xrBeginSession(_session, &beginInfo), "xrBeginSession");
    if (_sessionRunning && !_displayActive) {
        qCDebug(openxr) << "Session begun while the display is off: fed empty frames until VR is entered";
        startIdleFrames();
    }
}

void OpenXrRuntime::startIdleFrames() {
    stopIdleFrames();
    _idleFramesStop = false;
    _idleFrames = std::thread([this] {
        // until VR is entered (stopIdleFrames) or the session ends (a headset taken off: STOPPING, answered by whoever
        // polls the events); a frame the runtime refuses ends the feeding, and the runtime then stops the session itself
        while (!_idleFramesStop && _sessionRunning && submitEmptyFrame()) {
        }
    });
}

void OpenXrRuntime::stopIdleFrames() {
    _idleFramesStop = true;
    if (_idleFrames.joinable()) {
        _idleFrames.join();
    }
}

bool OpenXrRuntime::submitEmptyFrame() {
    std::lock_guard<std::mutex> lock(_frameMutex);
    if (!_sessionRunning) {
        return false;
    }
    XrFrameWaitInfo waitInfo { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState frameState { XR_TYPE_FRAME_STATE };
    if (!check(xrWaitFrame(_session, &waitInfo, &frameState), "xrWaitFrame (empty frame)")) {
        return false;
    }
    setFrameTiming(frameState.predictedDisplayTime, frameState.predictedDisplayPeriod);
    XrFrameBeginInfo beginInfo { XR_TYPE_FRAME_BEGIN_INFO };
    if (!check(xrBeginFrame(_session, &beginInfo), "xrBeginFrame (empty frame)")) {
        return false;
    }
    XrFrameEndInfo endInfo { XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = 0;
    return check(xrEndFrame(_session, &endInfo), "xrEndFrame (empty frame)");
}

bool OpenXrRuntime::check(XrResult result, const char* what) const {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    char resultName[XR_MAX_RESULT_STRING_SIZE] { 0 };
    if (_instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(_instance, result, resultName))) {
        snprintf(resultName, sizeof(resultName), "XrResult %d", (int)result);
    }
    qCWarning(openxr) << what << "failed:" << resultName;
    if (result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST) {
        if (result == XR_ERROR_INSTANCE_LOST) {
            _needsRestart = true;   // a new instance isn't made in this run
        }
        markSessionLost();
    }
    return false;
}

// From whichever thread sees it first: a loss event (pollEvents) or a call the runtime refused with SESSION_LOST.
// With _sessionRunning false, the present thread's frame loop sleeps (OpenXrDisplayPlugin::beginXrFrame), the idle
// frames stop and the input plugin's update returns early: no OpenXR call is made on the lost session again.
void OpenXrRuntime::markSessionLost() const {
    if (!_sessionLost.exchange(true)) {
        qCWarning(openxr) << "The OpenXR session is lost (the link to the headset dropped, or the runtime is going)";
    }
    _sessionRunning = false;
}

XrPath OpenXrRuntime::toPath(const char* path) const {
    XrPath result { XR_NULL_PATH };
    check(xrStringToPath(_instance, path, &result), path);
    return result;
}

bool OpenXrRuntime::createInstance() {
    uint32_t extensionCount { 0 };
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
    if (result == XR_ERROR_RUNTIME_UNAVAILABLE) {
        qCInfo(openxr) << "No OpenXR runtime is installed or active";
        return false;
    }
    if (!check(result, "xrEnumerateInstanceExtensionProperties")) {
        return false;
    }

    std::vector<XrExtensionProperties> extensions(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
    if (!check(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()),
               "xrEnumerateInstanceExtensionProperties")) {
        return false;
    }

    bool hasOpenGl { false };
    for (const auto& extension : extensions) {
        if (strcmp(extension.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0) {
            hasOpenGl = true;
        } else if (strcmp(extension.extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME) == 0) {
            _handTrackingExtension = true;
        } else if (strcmp(extension.extensionName, XR_EXT_USER_PRESENCE_EXTENSION_NAME) == 0) {
            _userPresenceExtension = true;
        }
    }
    if (!hasOpenGl) {
        qCWarning(openxr) << "The OpenXR runtime doesn't support OpenGL (" XR_KHR_OPENGL_ENABLE_EXTENSION_NAME ")";
        return false;
    }
    if (!_handTrackingExtension) {
        qCInfo(openxr) << "The OpenXR runtime doesn't offer" << XR_EXT_HAND_TRACKING_EXTENSION_NAME
                       << "(for a Quest over Link, turn on Developer runtime features in the Meta Horizon Link app)";
    }

    std::vector<const char*> enabledExtensions { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    if (_handTrackingExtension) {
        enabledExtensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    }
    if (_userPresenceExtension) {
        enabledExtensions.push_back(XR_EXT_USER_PRESENCE_EXTENSION_NAME);
    }

    XrInstanceCreateInfo createInfo { XR_TYPE_INSTANCE_CREATE_INFO };
    strncpy(createInfo.applicationInfo.applicationName, APPLICATION_NAME, XR_MAX_APPLICATION_NAME_SIZE - 1);
    strncpy(createInfo.applicationInfo.engineName, APPLICATION_NAME, XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();
    if (!check(xrCreateInstance(&createInfo, &_instance), "xrCreateInstance")) {
        _instance = XR_NULL_HANDLE;
        return false;
    }

    XrInstanceProperties instanceProperties { XR_TYPE_INSTANCE_PROPERTIES };
    if (check(xrGetInstanceProperties(_instance, &instanceProperties), "xrGetInstanceProperties")) {
        _runtimeName = QString("%1 %2.%3.%4")
                           .arg(instanceProperties.runtimeName)
                           .arg(XR_VERSION_MAJOR(instanceProperties.runtimeVersion))
                           .arg(XR_VERSION_MINOR(instanceProperties.runtimeVersion))
                           .arg(XR_VERSION_PATCH(instanceProperties.runtimeVersion));
        qCInfo(openxr) << "OpenXR runtime:" << _runtimeName;
    }

    _handPaths[LeftHand] = toPath("/user/hand/left");
    _handPaths[RightHand] = toPath("/user/hand/right");
    return true;
}

bool OpenXrRuntime::findSystem() {
    XrSystemGetInfo systemInfo { XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult result = xrGetSystem(_instance, &systemInfo, &_system);
    if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        qCInfo(openxr) << "No headset is connected to the OpenXR runtime";
        _system = XR_NULL_SYSTEM_ID;
        return false;
    }
    if (!check(result, "xrGetSystem")) {
        _system = XR_NULL_SYSTEM_ID;
        return false;
    }

    XrSystemProperties properties { XR_TYPE_SYSTEM_PROPERTIES };
    XrSystemHandTrackingPropertiesEXT handTrackingProperties { XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT };
    if (_handTrackingExtension) {
        properties.next = &handTrackingProperties;
    }
    if (!check(xrGetSystemProperties(_instance, _system, &properties), "xrGetSystemProperties")) {
        _system = XR_NULL_SYSTEM_ID;
        return false;
    }
    _systemName = QString::fromUtf8(properties.systemName);
    qCInfo(openxr) << "OpenXR headset:" << _systemName;

    _handTrackingSupported = _handTrackingExtension && handTrackingProperties.supportsHandTracking == XR_TRUE;
    if (_handTrackingSupported) {
        _handTrackingSupported =
            check(xrGetInstanceProcAddr(_instance, "xrCreateHandTrackerEXT", (PFN_xrVoidFunction*)&xrCreateHandTrackerEXT),
                  "xrGetInstanceProcAddr(xrCreateHandTrackerEXT)") &&
            check(xrGetInstanceProcAddr(_instance, "xrDestroyHandTrackerEXT", (PFN_xrVoidFunction*)&xrDestroyHandTrackerEXT),
                  "xrGetInstanceProcAddr(xrDestroyHandTrackerEXT)") &&
            check(xrGetInstanceProcAddr(_instance, "xrLocateHandJointsEXT", (PFN_xrVoidFunction*)&xrLocateHandJointsEXT),
                  "xrGetInstanceProcAddr(xrLocateHandJointsEXT)");
    }
    qCInfo(openxr) << "Hand tracking:" << (_handTrackingSupported ? "available" : "not available");
    qCInfo(openxr) << "Presence events (" XR_EXT_USER_PRESENCE_EXTENSION_NAME "):" << (_userPresenceExtension ? "offered" : "not offered");
    return true;
}

bool OpenXrRuntime::createSession() {
    if (_needsRestart) {
        return false;
    }
    // a lost session is replaced: it goes first (it isn't running, so it isn't ended), and the headset is asked for
    // again. Until a new one is made the loss stands, so no frame call is made meanwhile, and a later pick tries again
    const bool replacing = _sessionLost;
    if (replacing) {
        destroySession();
        if (!findSystem()) {
            return false;
        }
    }
    if (_session != XR_NULL_HANDLE) {
        return true;
    }
    if (!isSupported()) {
        return false;
    }

    // The spec requires asking for the graphics requirements before making a session.
    PFN_xrGetOpenGLGraphicsRequirementsKHR getOpenGlRequirements { nullptr };
    if (!check(xrGetInstanceProcAddr(_instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getOpenGlRequirements),
               "xrGetInstanceProcAddr(xrGetOpenGLGraphicsRequirementsKHR)")) {
        return false;
    }
    XrGraphicsRequirementsOpenGLKHR requirements { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    if (!check(getOpenGlRequirements(_instance, _system, &requirements), "xrGetOpenGLGraphicsRequirementsKHR")) {
        return false;
    }

    XrSessionCreateInfo createInfo { XR_TYPE_SESSION_CREATE_INFO };
    createInfo.systemId = _system;
#if defined(Q_OS_WIN)
    XrGraphicsBindingOpenGLWin32KHR binding { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
    binding.hDC = wglGetCurrentDC();
    binding.hGLRC = wglGetCurrentContext();
    if (!binding.hDC || !binding.hGLRC) {
        qCWarning(openxr) << "No OpenGL context is current, so no OpenXR session can be made";
        return false;
    }
    qCDebug(openxr) << "Making a session with the GL context" << (void*)binding.hGLRC << "on device context" << (void*)binding.hDC;
    createInfo.next = &binding;
#else
#error "The OpenXR plugin has no graphics binding for this platform yet"
#endif
    if (!check(xrCreateSession(_instance, &createInfo, &_session), "xrCreateSession")) {
        _session = XR_NULL_HANDLE;
        if (replacing) {
            // Meta's runtime over Link has refused a second session on one GL context in one run (2026-10-28)
            qCWarning(openxr) << "The runtime gave no new session after the lost one: the headset can be used again after a restart";
            _needsRestart = true;
        }
        return false;
    }

    // Prefer the stage, whose origin is on the floor like OpenVR's standing universe; local is at the head's height.
    uint32_t spaceCount { 0 };
    check(xrEnumerateReferenceSpaces(_session, 0, &spaceCount, nullptr), "xrEnumerateReferenceSpaces");
    std::vector<XrReferenceSpaceType> spaceTypes(spaceCount);
    check(xrEnumerateReferenceSpaces(_session, spaceCount, &spaceCount, spaceTypes.data()), "xrEnumerateReferenceSpaces");
    XrReferenceSpaceType referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    for (auto type : spaceTypes) {
        if (type == XR_REFERENCE_SPACE_TYPE_STAGE) {
            referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        }
    }
    if (referenceSpaceType != XR_REFERENCE_SPACE_TYPE_STAGE) {
        qCWarning(openxr) << "The runtime has no stage space; using the local space, whose origin is at head height";
    }

    XrReferenceSpaceCreateInfo spaceInfo { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    spaceInfo.referenceSpaceType = referenceSpaceType;
    if (!check(xrCreateReferenceSpace(_session, &spaceInfo, &_referenceSpace), "xrCreateReferenceSpace")) {
        destroySession();
        return false;
    }
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!check(xrCreateReferenceSpace(_session, &spaceInfo, &_viewSpace), "xrCreateReferenceSpace(view)")) {
        destroySession();
        return false;
    }

    _sessionState = XR_SESSION_STATE_UNKNOWN;
    _sessionRunning = false;
    _sessionLost = false;
    _exitAsked = false;
    _sessionGeneration++;
    qCDebug(openxr) << "OpenXR session created";
    return true;
}

void OpenXrRuntime::destroySession() {
    if (_session == XR_NULL_HANDLE) {
        return;
    }
    stopIdleFrames();
    if (_sessionRunning) {
        endRunningSession(true);
    }
    // Destroying the session destroys its spaces, swapchains, action spaces and hand trackers.
    {
        std::lock_guard<std::mutex> lock(_frameMutex);   // never inside a frame
        xrDestroySession(_session);
    }
    _session = XR_NULL_HANDLE;
    _referenceSpace = XR_NULL_HANDLE;
    _viewSpace = XR_NULL_HANDLE;
    _sessionState = XR_SESSION_STATE_UNKNOWN;
    _sessionRunning = false;
    _displayTime = 0;
    qCDebug(openxr) << "OpenXR session destroyed";
}

// A running session ends the way the spec's lifecycle has it before it is destroyed: the app asks to exit, keeps its
// frame loop going (empty frames) until the runtime moves the session to STOPPING, and then ends it (updateSessionState).
// Destroying a session that was still running left Meta's runtime refusing the next one with
// XR_ERROR_GRAPHICS_DEVICE_INVALID, so a player who left VR couldn't come back without restarting (Caity's Quest 3 over
// Link, 2026-10-28). Called on the present thread with no frame begun (uncustomizeContext ends one first).
void OpenXrRuntime::endRunningSession(bool withFrames) {
    stopIdleFrames();
    // at quit (withFrames false) no frames: the GL context the session was made with may be gone, and the runtime moves
    // to STOPPING on the exit request anyway, or is ended regardless after the shorter wait
    const auto EXIT_TIMEOUT = std::chrono::milliseconds(withFrames ? 2000 : 1000);
    const auto deadline = std::chrono::steady_clock::now() + EXIT_TIMEOUT;
    _exitAsked = true;   // the EXITING that follows is ours
    if (check(xrRequestExitSession(_session), "xrRequestExitSession")) {
        while (_sessionRunning && std::chrono::steady_clock::now() < deadline) {
            XrSessionState state = _sessionState;
            if (withFrames && (state == XR_SESSION_STATE_SYNCHRONIZED || state == XR_SESSION_STATE_VISIBLE ||
                               state == XR_SESSION_STATE_FOCUSED)) {
                if (!submitEmptyFrame()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            pollEvents();   // STOPPING calls xrEndSession and clears _sessionRunning
        }
    }
    if (_sessionRunning) {
        qCWarning(openxr) << "The session didn't reach STOPPING after the exit request; ending it anyway";
        check(xrEndSession(_session), "xrEndSession");
        _sessionRunning = false;
    } else {
        qCDebug(openxr) << "The session ended cleanly";
    }
}

bool OpenXrRuntime::shouldRender() const {
    XrSessionState state = _sessionState;
    return _sessionRunning &&
           (state == XR_SESSION_STATE_SYNCHRONIZED || state == XR_SESSION_STATE_VISIBLE || state == XR_SESSION_STATE_FOCUSED);
}

bool OpenXrRuntime::isHeadsetOff() const {
    XrSessionState state = _sessionState;
    return !_userPresent || state == XR_SESSION_STATE_IDLE || state == XR_SESSION_STATE_STOPPING;
}

void OpenXrRuntime::updateSessionState(XrSessionState state) {
    qCDebug(openxr) << "Session state" << (int)_sessionState.load() << "->" << (int)state;
    _sessionState = state;

    switch (state) {
        case XR_SESSION_STATE_READY:
            // begun at once, whether or not the OpenXR display is the active one (off, it is fed empty frames)
            beginSession();
            break;
        case XR_SESSION_STATE_STOPPING: {
            // between frames: whoever is feeding the session (the idle thread, or this one) is not inside one
            std::lock_guard<std::mutex> lock(_frameMutex);
            check(xrEndSession(_session), "xrEndSession");
            _sessionRunning = false;
            break;
        }
        case XR_SESSION_STATE_LOSS_PENDING:
        case XR_SESSION_STATE_EXITING:
            // EXITING after our own exit request (leaving for good, at quit) isn't a loss; EXITING the runtime asked
            // for (Quit picked in its menu) is: the session is over and VR has no picture
            if (state == XR_SESSION_STATE_LOSS_PENDING || !_exitAsked) {
                markSessionLost();
            }
            break;
        default:
            break;
    }
}

bool OpenXrRuntime::pollEvents() {
    std::lock_guard<std::mutex> lock(_eventMutex);
    if (_instance == XR_NULL_HANDLE) {
        return false;
    }
    bool keepRunning { true };
    XrEventDataBuffer event { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(_instance, &event) == XR_SUCCESS) {
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto& stateChanged = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                updateSessionState(stateChanged.state);
                if (stateChanged.state == XR_SESSION_STATE_EXITING || stateChanged.state == XR_SESSION_STATE_LOSS_PENDING) {
                    keepRunning = false;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                qCWarning(openxr) << "The OpenXR runtime is going away";
                _needsRestart = true;   // a new instance isn't made in this run
                markSessionLost();
                keepRunning = false;
                break;
            case XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT:
                _userPresent = reinterpret_cast<XrEventDataUserPresenceChangedEXT*>(&event)->isUserPresent == XR_TRUE;
                break;
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                for (int hand = 0; hand < HandCount; hand++) {
                    XrInteractionProfileState profile { XR_TYPE_INTERACTION_PROFILE_STATE };
                    if (check(xrGetCurrentInteractionProfile(_session, _handPaths[hand], &profile), "xrGetCurrentInteractionProfile") &&
                        profile.interactionProfile != XR_NULL_PATH) {
                        char name[XR_MAX_PATH_LENGTH] { 0 };
                        uint32_t length { 0 };
                        xrPathToString(_instance, profile.interactionProfile, XR_MAX_PATH_LENGTH, &length, name);
                        qCDebug(openxr) << (hand == LeftHand ? "Left" : "Right") << "hand profile:" << name;
                    }
                }
                break;
            default:
                break;
        }
        event = { XR_TYPE_EVENT_DATA_BUFFER };
    }
    return keepRunning;
}

void OpenXrRuntime::setFrameTiming(XrTime predictedDisplayTime, XrDuration predictedDisplayPeriod) {
    _displayTime = predictedDisplayTime;
    _displayPeriod = predictedDisplayPeriod;
}

XrTime OpenXrRuntime::getNextDisplayTime() const {
    // The present thread is a frame behind the simulation, so look one display period past its frame.
    XrTime displayTime = _displayTime;
    return displayTime == 0 ? 0 : displayTime + _displayPeriod;
}

controller::Pose OpenXrRuntime::locate(XrSpace space, XrTime time) const {
    controller::Pose pose;
    if (space == XR_NULL_HANDLE || _referenceSpace == XR_NULL_HANDLE || time == 0) {
        return pose;
    }
    XrSpaceVelocity velocity { XR_TYPE_SPACE_VELOCITY };
    XrSpaceLocation location { XR_TYPE_SPACE_LOCATION };
    location.next = &velocity;
    if (XR_FAILED(xrLocateSpace(space, _referenceSpace, time, &location))) {
        return pose;
    }
    const XrSpaceLocationFlags VALID = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((location.locationFlags & VALID) != VALID) {
        return pose;
    }
    pose = controller::Pose(toGlm(location.pose.position), glm::normalize(toGlm(location.pose.orientation)));
    if (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
        pose.velocity = toGlm(velocity.linearVelocity);
    }
    if (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) {
        pose.angularVelocity = toGlm(velocity.angularVelocity);
    }
    return pose;
}
