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

#include <cstdio>
#include <cstring>
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
    destroySession();
    if (_instance != XR_NULL_HANDLE) {
        xrDestroyInstance(_instance);
        _instance = XR_NULL_HANDLE;
    }
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
    return false;
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
    return true;
}

bool OpenXrRuntime::createSession() {
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
    createInfo.next = &binding;
#else
#error "The OpenXR plugin has no graphics binding for this platform yet"
#endif
    if (!check(xrCreateSession(_instance, &createInfo, &_session), "xrCreateSession")) {
        _session = XR_NULL_HANDLE;
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
    _sessionGeneration++;
    qCDebug(openxr) << "OpenXR session created";
    return true;
}

void OpenXrRuntime::destroySession() {
    if (_session == XR_NULL_HANDLE) {
        return;
    }
    if (_sessionRunning) {
        xrEndSession(_session);
        _sessionRunning = false;
    }
    // Destroying the session destroys its spaces, swapchains, action spaces and hand trackers.
    xrDestroySession(_session);
    _session = XR_NULL_HANDLE;
    _referenceSpace = XR_NULL_HANDLE;
    _viewSpace = XR_NULL_HANDLE;
    _sessionState = XR_SESSION_STATE_UNKNOWN;
    _displayTime = 0;
    qCDebug(openxr) << "OpenXR session destroyed";
}

bool OpenXrRuntime::shouldRender() const {
    XrSessionState state = _sessionState;
    return _sessionRunning &&
           (state == XR_SESSION_STATE_SYNCHRONIZED || state == XR_SESSION_STATE_VISIBLE || state == XR_SESSION_STATE_FOCUSED);
}

void OpenXrRuntime::updateSessionState(XrSessionState state) {
    qCDebug(openxr) << "Session state" << (int)_sessionState.load() << "->" << (int)state;
    _sessionState = state;

    switch (state) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo beginInfo { XR_TYPE_SESSION_BEGIN_INFO };
            beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            _sessionRunning = check(xrBeginSession(_session, &beginInfo), "xrBeginSession");
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            check(xrEndSession(_session), "xrEndSession");
            _sessionRunning = false;
            break;
        default:
            break;
    }
}

bool OpenXrRuntime::pollEvents() {
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
