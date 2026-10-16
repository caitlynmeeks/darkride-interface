//
//  OpenXrInputPlugin.cpp
//  plugins/openxr-plugin/src
//
//  Created on 2026-10-16 for Quest 3 hand tracking over Link (Meta's PC OpenXR runtime).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "OpenXrInputPlugin.h"

#include <cstring>
#include <string>
#include <vector>

#include <AvatarConstants.h>
#include <NumericalConstants.h>
#include <PathUtils.h>
#include <PerfStat.h>
#include <controllers/UserInputMapper.h>

const char* OpenXrInputPlugin::NAME = "OpenXR";

// A pinch starts below the first distance between the thumb and index finger tips and is full at the second.
static const float PINCH_OPEN_DISTANCE = 0.04f;
static const float PINCH_CLOSED_DISTANCE = 0.015f;

// Where the avatar's wrist is from an OpenXR grip pose, in the hand's frame (+Y towards the fingers, +Z out of the
// palm): the grip pose is in the middle of the closed fist, palm-side of the knuckles.
static const glm::vec3 GRIP_TO_WRIST(0.0f, -0.085f, -0.03f);

// Joint frames, and how they become the avatar's.
//
// An OpenXR hand joint has +Y out of the back of the hand (the fingernail), +Z back along the bone towards the wrist,
// and +X = Y x Z, on both hands. An avatar's hand and finger joints have +Y along the bone towards the fingertips and
// +Z out of the palm. So the avatar's axes, in the OpenXR joint's frame, are X = -X, Y = -Z and Z = -Y: a quarter turn
// about X followed by a half turn about Z.
// (The axes are spelled out, as Vectors:: constants may not be initialized yet.)
static const glm::quat XR_JOINT_TO_AVATAR_JOINT =
    glm::angleAxis(PI_OVER_TWO, glm::vec3(1.0f, 0.0f, 0.0f)) * glm::angleAxis(PI, glm::vec3(0.0f, 0.0f, 1.0f));

// OpenXR's thumb joints have +Y out of the thumbnail, which faces sideways, a quarter turn about the thumb from the
// back of the hand. The avatar's thumb joints are oriented like its other fingers, so roll the thumb's joints a quarter
// turn about their bones first: +90 degrees on the left hand and -90 degrees on the right.
static const glm::quat XR_THUMB_ROLL[OpenXrRuntime::HandCount] = { glm::angleAxis(PI_OVER_TWO, glm::vec3(0.0f, 0.0f, 1.0f)),
                                                                   glm::angleAxis(-PI_OVER_TWO, glm::vec3(0.0f, 0.0f, 1.0f)) };

// The avatar's hand axes in the OpenXR grip pose's frame. The grip pose has -Z along the fist's tube out past the
// thumb, and +X to the user's right on both hands, so out of the left palm and into the right one; the hand points
// along -Y.
//   left:  X = +Z, Y = -Y, Z = +X
//   right: X = -Z, Y = -Y, Z = -X
static const glm::quat GRIP_TO_AVATAR_HAND[OpenXrRuntime::HandCount] = {
    glm::quat_cast(glm::mat3(glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f))),
    glm::quat_cast(glm::mat3(glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f)))
};

using namespace controller;

// Each avatar finger joint and the OpenXR joint that drives it, from the knuckle to the tip.
static const int FINGER_COUNT = 5;
static const int JOINTS_PER_FINGER = 4;
static const XrHandJointEXT XR_FINGER_JOINTS[FINGER_COUNT][JOINTS_PER_FINGER] = {
    { XR_HAND_JOINT_THUMB_METACARPAL_EXT, XR_HAND_JOINT_THUMB_PROXIMAL_EXT, XR_HAND_JOINT_THUMB_DISTAL_EXT, XR_HAND_JOINT_THUMB_TIP_EXT },
    { XR_HAND_JOINT_INDEX_PROXIMAL_EXT, XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT, XR_HAND_JOINT_INDEX_DISTAL_EXT, XR_HAND_JOINT_INDEX_TIP_EXT },
    { XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT, XR_HAND_JOINT_MIDDLE_INTERMEDIATE_EXT, XR_HAND_JOINT_MIDDLE_DISTAL_EXT, XR_HAND_JOINT_MIDDLE_TIP_EXT },
    { XR_HAND_JOINT_RING_PROXIMAL_EXT, XR_HAND_JOINT_RING_INTERMEDIATE_EXT, XR_HAND_JOINT_RING_DISTAL_EXT, XR_HAND_JOINT_RING_TIP_EXT },
    { XR_HAND_JOINT_LITTLE_PROXIMAL_EXT, XR_HAND_JOINT_LITTLE_INTERMEDIATE_EXT, XR_HAND_JOINT_LITTLE_DISTAL_EXT, XR_HAND_JOINT_LITTLE_TIP_EXT },
};
static const StandardPoseChannel AVATAR_FINGER_JOINTS[OpenXrRuntime::HandCount][FINGER_COUNT][JOINTS_PER_FINGER] = {
    {
        { LEFT_HAND_THUMB1, LEFT_HAND_THUMB2, LEFT_HAND_THUMB3, LEFT_HAND_THUMB4 },
        { LEFT_HAND_INDEX1, LEFT_HAND_INDEX2, LEFT_HAND_INDEX3, LEFT_HAND_INDEX4 },
        { LEFT_HAND_MIDDLE1, LEFT_HAND_MIDDLE2, LEFT_HAND_MIDDLE3, LEFT_HAND_MIDDLE4 },
        { LEFT_HAND_RING1, LEFT_HAND_RING2, LEFT_HAND_RING3, LEFT_HAND_RING4 },
        { LEFT_HAND_PINKY1, LEFT_HAND_PINKY2, LEFT_HAND_PINKY3, LEFT_HAND_PINKY4 },
    },
    {
        { RIGHT_HAND_THUMB1, RIGHT_HAND_THUMB2, RIGHT_HAND_THUMB3, RIGHT_HAND_THUMB4 },
        { RIGHT_HAND_INDEX1, RIGHT_HAND_INDEX2, RIGHT_HAND_INDEX3, RIGHT_HAND_INDEX4 },
        { RIGHT_HAND_MIDDLE1, RIGHT_HAND_MIDDLE2, RIGHT_HAND_MIDDLE3, RIGHT_HAND_MIDDLE4 },
        { RIGHT_HAND_RING1, RIGHT_HAND_RING2, RIGHT_HAND_RING3, RIGHT_HAND_RING4 },
        { RIGHT_HAND_PINKY1, RIGHT_HAND_PINKY2, RIGHT_HAND_PINKY3, RIGHT_HAND_PINKY4 },
    }
};

OpenXrInputPlugin::OpenXrInputPlugin(std::shared_ptr<OpenXrRuntime> runtime) :
    _runtime(runtime), _inputDevice(std::make_shared<InputDevice>(runtime)) {
}

bool OpenXrInputPlugin::isSupported() const {
    return _runtime->isSupported();
}

bool OpenXrInputPlugin::activate() {
    InputPlugin::activate();
    auto userInputMapper = DependencyManager::get<controller::UserInputMapper>();
    userInputMapper->registerDevice(_inputDevice);
    _registeredWithInputMapper = true;
    return true;
}

void OpenXrInputPlugin::deactivate() {
    InputPlugin::deactivate();
    _inputDevice->_poseStateMap.clear();
    auto userInputMapper = DependencyManager::get<controller::UserInputMapper>();
    userInputMapper->removeDevice(_inputDevice->_deviceID);
    _registeredWithInputMapper = false;
}

void OpenXrInputPlugin::pluginUpdate(float deltaTime, const controller::InputCalibrationData& inputCalibrationData) {
    auto userInputMapper = DependencyManager::get<controller::UserInputMapper>();
    userInputMapper->withLock([&, this]() { _inputDevice->update(deltaTime, inputCalibrationData); });
}

OpenXrInputPlugin::InputDevice::InputDevice(std::shared_ptr<OpenXrRuntime> runtime) :
    controller::InputDevice("OpenXR"), _runtime(runtime) {
    _actions.fill(XR_NULL_HANDLE);
}

OpenXrInputPlugin::InputDevice::~InputDevice() {
    // the action set's actions go with it
    if (_actionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(_actionSet);
    }
}

bool OpenXrInputPlugin::InputDevice::createActions() {
    XrActionSetCreateInfo setInfo { XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy(setInfo.actionSetName, "darkride", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(setInfo.localizedActionSetName, "Darkride", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (!_runtime->check(xrCreateActionSet(_runtime->getInstance(), &setInfo, &_actionSet), "xrCreateActionSet")) {
        _actionSet = XR_NULL_HANDLE;
        return false;
    }

    struct ActionInfo {
        const char* name;
        const char* localizedName;
        XrActionType type;
    };
    static const ActionInfo ACTION_INFO[ACTION_COUNT] = {
        { "grip_pose", "Hand pose", XR_ACTION_TYPE_POSE_INPUT },
        { "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT },
        { "trigger_touch", "Trigger touch", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT },
        { "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT },
        { "thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "thumbstick_touch", "Thumbstick touch", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "primary", "A or X", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "primary_touch", "A or X touch", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "secondary", "B or Y", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "secondary_touch", "B or Y touch", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { "haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT },
    };

    const auto& handPaths = _runtime->getHandPaths();
    for (int i = 0; i < ACTION_COUNT; i++) {
        XrActionCreateInfo actionInfo { XR_TYPE_ACTION_CREATE_INFO };
        strncpy(actionInfo.actionName, ACTION_INFO[i].name, XR_MAX_ACTION_NAME_SIZE - 1);
        strncpy(actionInfo.localizedActionName, ACTION_INFO[i].localizedName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        actionInfo.actionType = ACTION_INFO[i].type;
        actionInfo.countSubactionPaths = (uint32_t)handPaths.size();
        actionInfo.subactionPaths = handPaths.data();
        if (!_runtime->check(xrCreateAction(_actionSet, &actionInfo, &_actions[i]), ACTION_INFO[i].name)) {
            return false;
        }
    }

    suggestBindings();
    return true;
}

void OpenXrInputPlugin::InputDevice::suggestBindings() {
    struct Binding {
        ActionId action;
        const char* path; // under /user/hand/left and /user/hand/right
    };
    struct Profile {
        const char* path;
        std::vector<Binding> bothHands;
        std::vector<Binding> leftHand;
        std::vector<Binding> rightHand;
    };

    // The runtime picks the profile closest to the controllers in use; Quest 3's Touch Plus controllers use Touch.
    static const std::vector<Profile> PROFILES = {
        { "/interaction_profiles/oculus/touch_controller",
          { { GRIP_POSE, "/input/grip/pose" }, { TRIGGER, "/input/trigger/value" }, { TRIGGER_TOUCH, "/input/trigger/touch" },
            { SQUEEZE, "/input/squeeze/value" }, { THUMBSTICK, "/input/thumbstick" },
            { THUMBSTICK_CLICK, "/input/thumbstick/click" }, { THUMBSTICK_TOUCH, "/input/thumbstick/touch" },
            { HAPTIC, "/output/haptic" } },
          { { PRIMARY, "/input/x/click" }, { PRIMARY_TOUCH, "/input/x/touch" }, { SECONDARY, "/input/y/click" },
            { SECONDARY_TOUCH, "/input/y/touch" }, { MENU, "/input/menu/click" } },
          { { PRIMARY, "/input/a/click" }, { PRIMARY_TOUCH, "/input/a/touch" }, { SECONDARY, "/input/b/click" },
            { SECONDARY_TOUCH, "/input/b/touch" } } },
        { "/interaction_profiles/valve/index_controller",
          { { GRIP_POSE, "/input/grip/pose" }, { TRIGGER, "/input/trigger/value" }, { TRIGGER_TOUCH, "/input/trigger/touch" },
            { SQUEEZE, "/input/squeeze/value" }, { THUMBSTICK, "/input/thumbstick" },
            { THUMBSTICK_CLICK, "/input/thumbstick/click" }, { THUMBSTICK_TOUCH, "/input/thumbstick/touch" },
            { PRIMARY, "/input/a/click" }, { PRIMARY_TOUCH, "/input/a/touch" }, { SECONDARY, "/input/b/click" },
            { SECONDARY_TOUCH, "/input/b/touch" }, { HAPTIC, "/output/haptic" } },
          {},
          {} },
        { "/interaction_profiles/htc/vive_controller",
          { { GRIP_POSE, "/input/grip/pose" }, { TRIGGER, "/input/trigger/value" }, { SQUEEZE, "/input/squeeze/click" },
            { THUMBSTICK, "/input/trackpad" }, { THUMBSTICK_CLICK, "/input/trackpad/click" },
            { THUMBSTICK_TOUCH, "/input/trackpad/touch" }, { MENU, "/input/menu/click" }, { HAPTIC, "/output/haptic" } },
          {},
          {} },
        { "/interaction_profiles/khr/simple_controller",
          { { GRIP_POSE, "/input/grip/pose" }, { TRIGGER, "/input/select/click" }, { MENU, "/input/menu/click" },
            { HAPTIC, "/output/haptic" } },
          {},
          {} },
    };

    static const char* HAND_PREFIXES[OpenXrRuntime::HandCount] = { "/user/hand/left", "/user/hand/right" };
    for (const auto& profile : PROFILES) {
        std::vector<XrActionSuggestedBinding> suggested;
        auto add = [&](const std::vector<Binding>& bindings, int hand) {
            for (const auto& binding : bindings) {
                std::string path = std::string(HAND_PREFIXES[hand]) + binding.path;
                suggested.push_back({ _actions[binding.action], _runtime->toPath(path.c_str()) });
            }
        };
        for (int hand = 0; hand < OpenXrRuntime::HandCount; hand++) {
            add(profile.bothHands, hand);
        }
        add(profile.leftHand, OpenXrRuntime::LeftHand);
        add(profile.rightHand, OpenXrRuntime::RightHand);

        XrInteractionProfileSuggestedBinding profileBindings { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        profileBindings.interactionProfile = _runtime->toPath(profile.path);
        profileBindings.countSuggestedBindings = (uint32_t)suggested.size();
        profileBindings.suggestedBindings = suggested.data();
        _runtime->check(xrSuggestInteractionProfileBindings(_runtime->getInstance(), &profileBindings), profile.path);
    }
}

bool OpenXrInputPlugin::InputDevice::attachToSession() {
    XrSession session = _runtime->getSession();
    XrSessionActionSetsAttachInfo attachInfo { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &_actionSet;
    if (!_runtime->check(xrAttachSessionActionSets(session, &attachInfo), "xrAttachSessionActionSets")) {
        return false;
    }

    const auto& handPaths = _runtime->getHandPaths();
    for (int hand = 0; hand < OpenXrRuntime::HandCount; hand++) {
        XrActionSpaceCreateInfo spaceInfo { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.action = _actions[GRIP_POSE];
        spaceInfo.subactionPath = handPaths[hand];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        _runtime->check(xrCreateActionSpace(session, &spaceInfo, &_gripSpaces[hand]), "xrCreateActionSpace");

        if (_runtime->supportsHandTracking()) {
            XrHandTrackerCreateInfoEXT trackerInfo { XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT };
            trackerInfo.hand = hand == OpenXrRuntime::LeftHand ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
            trackerInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            if (!_runtime->check(_runtime->xrCreateHandTrackerEXT(session, &trackerInfo, &_handTrackers[hand]),
                                 "xrCreateHandTrackerEXT")) {
                _handTrackers[hand] = XR_NULL_HANDLE;
            }
        }
    }
    return true;
}

void OpenXrInputPlugin::InputDevice::releaseSessionObjects() {
    // The session that owned these is gone, and took them with it.
    _gripSpaces.fill(XR_NULL_HANDLE);
    _handTrackers.fill(XR_NULL_HANDLE);
    _attached = false;
}

bool OpenXrInputPlugin::InputDevice::getBool(ActionId action, int hand) const {
    XrActionStateGetInfo getInfo { XR_TYPE_ACTION_STATE_GET_INFO };
    getInfo.action = _actions[action];
    getInfo.subactionPath = _runtime->getHandPaths()[hand];
    XrActionStateBoolean state { XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(xrGetActionStateBoolean(_runtime->getSession(), &getInfo, &state)) && state.isActive &&
           state.currentState;
}

float OpenXrInputPlugin::InputDevice::getFloat(ActionId action, int hand) const {
    XrActionStateGetInfo getInfo { XR_TYPE_ACTION_STATE_GET_INFO };
    getInfo.action = _actions[action];
    getInfo.subactionPath = _runtime->getHandPaths()[hand];
    XrActionStateFloat state { XR_TYPE_ACTION_STATE_FLOAT };
    if (XR_SUCCEEDED(xrGetActionStateFloat(_runtime->getSession(), &getInfo, &state)) && state.isActive) {
        return state.currentState;
    }
    return 0.0f;
}

glm::vec2 OpenXrInputPlugin::InputDevice::getVector2(ActionId action, int hand) const {
    XrActionStateGetInfo getInfo { XR_TYPE_ACTION_STATE_GET_INFO };
    getInfo.action = _actions[action];
    getInfo.subactionPath = _runtime->getHandPaths()[hand];
    XrActionStateVector2f state { XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_SUCCEEDED(xrGetActionStateVector2f(_runtime->getSession(), &getInfo, &state)) && state.isActive) {
        return glm::vec2(state.currentState.x, state.currentState.y);
    }
    return glm::vec2(0.0f);
}

void OpenXrInputPlugin::InputDevice::update(float deltaTime, const controller::InputCalibrationData& inputCalibrationData) {
    PerformanceTimer perfTimer("OpenXrInputPlugin::update");
    _poseStateMap.clear();
    _buttonPressedMap.clear();
    _axisStateMap.clear();

    if (_runtime->getSession() == XR_NULL_HANDLE) {
        releaseSessionObjects();
        return;
    }
    if (_actionSet == XR_NULL_HANDLE && !createActions()) {
        return;
    }
    if (!_attached || _attachedGeneration != _runtime->getSessionGeneration()) {
        releaseSessionObjects();
        _attachedGeneration = _runtime->getSessionGeneration();
        _attached = attachToSession();
        if (!_attached) {
            return;
        }
    }

    XrTime time = _runtime->getNextDisplayTime();
    if (time == 0 || !_runtime->isSessionRunning()) {
        return;
    }

    glm::mat4 sensorToAvatar = glm::inverse(inputCalibrationData.avatarMat) * inputCalibrationData.sensorToWorldMat;
    updateHead(inputCalibrationData, sensorToAvatar, time);

    // Actions only update while the session has the input focus.
    if (_runtime->isSessionFocused()) {
        XrActiveActionSet activeSet { _actionSet, XR_NULL_PATH };
        XrActionsSyncInfo syncInfo { XR_TYPE_ACTIONS_SYNC_INFO };
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &activeSet;
        _runtime->check(xrSyncActions(_runtime->getSession(), &syncInfo), "xrSyncActions");
    }

    for (int hand = 0; hand < OpenXrRuntime::HandCount; hand++) {
        // a tracked hand takes over from the controller, which has been put down
        if (!updateTrackedHand(hand, sensorToAvatar, time)) {
            updateController(hand, sensorToAvatar, time);
        }
    }
}

void OpenXrInputPlugin::InputDevice::updateHead(const controller::InputCalibrationData& inputCalibrationData,
                                                const glm::mat4& sensorToAvatar, XrTime time) {
    controller::Pose hmd = _runtime->locate(_runtime->getViewSpace(), time);
    if (!hmd.isValid()) {
        return;
    }

    // perform a 180 flip to make the HMD face the +z instead of -z, because the head faces +z
    glm::mat4 matYFlip = hmd.getMatrix() * Matrices::Y_180;
    controller::Pose pose(extractTranslation(matYFlip), glmExtractRotation(matYFlip), hmd.velocity, hmd.angularVelocity);

    glm::mat4 defaultHeadOffset;
    if (inputCalibrationData.hmdAvatarAlignmentType == controller::HmdAvatarAlignmentType::Eyes) {
        // align the eyes of the user with the eyes of the avatar
        defaultHeadOffset = glm::inverse(inputCalibrationData.defaultCenterEyeMat) * inputCalibrationData.defaultHeadMat;
    } else {
        // align the head of the user with the head of the avatar
        defaultHeadOffset = createMatFromQuatAndPos(Quaternions::IDENTITY, -DEFAULT_AVATAR_HEAD_TO_MIDDLE_EYE_OFFSET);
    }
    _poseStateMap[HEAD] = pose.postTransform(defaultHeadOffset).transform(sensorToAvatar);
}

void OpenXrInputPlugin::InputDevice::updateController(int hand, const glm::mat4& sensorToAvatar, XrTime time) {
    bool isLeftHand = hand == OpenXrRuntime::LeftHand;

    controller::Pose grip = _runtime->locate(_gripSpaces[hand], time);
    if (grip.isValid()) {
        glm::quat rotation = grip.rotation * GRIP_TO_AVATAR_HAND[hand];
        glm::vec3 offset = rotation * GRIP_TO_WRIST;
        controller::Pose pose(grip.translation + offset, rotation);
        // the wrist moves faster than the grip when the hand turns
        pose.velocity = grip.velocity + glm::cross(grip.angularVelocity, offset);
        pose.angularVelocity = grip.angularVelocity;
        _poseStateMap[isLeftHand ? LEFT_HAND : RIGHT_HAND] = pose.transform(sensorToAvatar);
    }

    glm::vec2 stick = getVector2(THUMBSTICK, hand);
    _axisStateMap[isLeftHand ? LX : RX].value = stick.x;
    _axisStateMap[isLeftHand ? LY : RY].value = stick.y;
    _axisStateMap[isLeftHand ? LT : RT].value = getFloat(TRIGGER, hand);
    _axisStateMap[isLeftHand ? LEFT_GRIP : RIGHT_GRIP].value = getFloat(SQUEEZE, hand);

    bool primaryTouch = getBool(PRIMARY_TOUCH, hand);
    bool secondaryTouch = getBool(SECONDARY_TOUCH, hand);
    bool stickTouch = getBool(THUMBSTICK_TOUCH, hand);
    bool triggerTouch = getBool(TRIGGER_TOUCH, hand);

    auto press = [&](bool pressed, StandardButtonChannel channel) {
        if (pressed) {
            _buttonPressedMap.insert(channel);
        }
    };
    press(getBool(PRIMARY, hand), isLeftHand ? X : A);
    press(getBool(SECONDARY, hand), isLeftHand ? Y : B);
    press(getBool(THUMBSTICK_CLICK, hand), isLeftHand ? LS : RS);
    press(getBool(MENU, hand), isLeftHand ? BACK : START);
    press(primaryTouch, isLeftHand ? LEFT_PRIMARY_THUMB_TOUCH : RIGHT_PRIMARY_THUMB_TOUCH);
    press(secondaryTouch, isLeftHand ? LEFT_SECONDARY_THUMB_TOUCH : RIGHT_SECONDARY_THUMB_TOUCH);
    press(stickTouch, isLeftHand ? LS_TOUCH : RS_TOUCH);
    press(triggerTouch, isLeftHand ? LEFT_PRIMARY_INDEX_TOUCH : RIGHT_PRIMARY_INDEX_TOUCH);

    // Hand poses from the capacitive sensors, as the Touch controllers' own SDK reports them. Only controllers with a
    // touch sensor on the trigger can tell.
    XrActionStateGetInfo getInfo { XR_TYPE_ACTION_STATE_GET_INFO };
    getInfo.action = _actions[TRIGGER_TOUCH];
    getInfo.subactionPath = _runtime->getHandPaths()[hand];
    XrActionStateBoolean triggerTouchState { XR_TYPE_ACTION_STATE_BOOLEAN };
    if (XR_SUCCEEDED(xrGetActionStateBoolean(_runtime->getSession(), &getInfo, &triggerTouchState)) && triggerTouchState.isActive) {
        press(!primaryTouch && !secondaryTouch && !stickTouch, isLeftHand ? LEFT_THUMB_UP : RIGHT_THUMB_UP);
        press(!triggerTouch, isLeftHand ? LEFT_INDEX_POINT : RIGHT_INDEX_POINT);
    }
}

bool OpenXrInputPlugin::InputDevice::updateTrackedHand(int hand, const glm::mat4& sensorToAvatar, XrTime time) {
    if (_handTrackers[hand] == XR_NULL_HANDLE) {
        return false;
    }

    std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints;
    XrHandJointLocationsEXT locations { XR_TYPE_HAND_JOINT_LOCATIONS_EXT };
    locations.jointCount = (uint32_t)joints.size();
    locations.jointLocations = joints.data();
    XrHandJointsLocateInfoEXT locateInfo { XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT };
    locateInfo.baseSpace = _runtime->getReferenceSpace();
    locateInfo.time = time;
    if (XR_FAILED(_runtime->xrLocateHandJointsEXT(_handTrackers[hand], &locateInfo, &locations)) || !locations.isActive) {
        _pinching[hand] = false;
        return false;
    }

    const XrSpaceLocationFlags VALID = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    auto jointValid = [&](XrHandJointEXT joint) { return (joints[joint].locationFlags & VALID) == VALID; };
    auto jointPose = [&](XrHandJointEXT joint, const glm::quat& xrToAvatar) {
        const auto& xrPose = joints[joint].pose;
        controller::Pose pose(toGlm(xrPose.position), glm::normalize(toGlm(xrPose.orientation) * xrToAvatar));
        return pose.transform(sensorToAvatar);
    };

    if (!jointValid(XR_HAND_JOINT_WRIST_EXT)) {
        return false;
    }

    bool isLeftHand = hand == OpenXrRuntime::LeftHand;
    _poseStateMap[isLeftHand ? LEFT_HAND : RIGHT_HAND] = jointPose(XR_HAND_JOINT_WRIST_EXT, XR_JOINT_TO_AVATAR_JOINT);

    // MySkeletonModel::updateFingers() turns these into each joint's rotation relative to the one before it.
    for (int finger = 0; finger < FINGER_COUNT; finger++) {
        glm::quat xrToAvatar = finger == 0 ? XR_THUMB_ROLL[hand] * XR_JOINT_TO_AVATAR_JOINT : XR_JOINT_TO_AVATAR_JOINT;
        for (int joint = 0; joint < JOINTS_PER_FINGER; joint++) {
            XrHandJointEXT xrJoint = XR_FINGER_JOINTS[finger][joint];
            if (jointValid(xrJoint)) {
                _poseStateMap[AVATAR_FINGER_JOINTS[hand][finger][joint]] = jointPose(xrJoint, xrToAvatar);
            }
        }
    }

    // A pinch works the trigger, for lasers and grabbing. The mapping turns the value into a click.
    if (jointValid(XR_HAND_JOINT_THUMB_TIP_EXT) && jointValid(XR_HAND_JOINT_INDEX_TIP_EXT)) {
        float distance = glm::distance(toGlm(joints[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position),
                                       toGlm(joints[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position));
        float pinch = glm::clamp((PINCH_OPEN_DISTANCE - distance) / (PINCH_OPEN_DISTANCE - PINCH_CLOSED_DISTANCE), 0.0f, 1.0f);
        _axisStateMap[isLeftHand ? LT : RT].value = pinch;
        bool pinching = pinch >= 1.0f;
        if (pinching != _pinching[hand]) {
            _pinching[hand] = pinching;
            qCDebug(openxr) << (isLeftHand ? "Left" : "Right") << "pinch" << (pinching ? "started" : "ended");
        }
    }
    return true;
}

bool OpenXrInputPlugin::InputDevice::triggerHapticPulse(float strength, float duration, controller::Hand hand) {
    if (!_attached || _runtime->getSession() == XR_NULL_HANDLE) {
        return false;
    }
    XrHapticVibration vibration { XR_TYPE_HAPTIC_VIBRATION };
    vibration.amplitude = glm::clamp(strength, 0.0f, 1.0f);
    // duration is in milliseconds
    vibration.duration = (XrDuration)(duration * 1.0e6f);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

    bool result = false;
    for (int i = 0; i < OpenXrRuntime::HandCount; i++) {
        bool wanted = hand == controller::BOTH || (hand == controller::LEFT) == (i == OpenXrRuntime::LeftHand);
        if (!wanted) {
            continue;
        }
        XrHapticActionInfo hapticInfo { XR_TYPE_HAPTIC_ACTION_INFO };
        hapticInfo.action = _actions[HAPTIC];
        hapticInfo.subactionPath = _runtime->getHandPaths()[i];
        if (strength == 0.0f) {
            result |= XR_SUCCEEDED(xrStopHapticFeedback(_runtime->getSession(), &hapticInfo));
        } else {
            result |= XR_SUCCEEDED(
                xrApplyHapticFeedback(_runtime->getSession(), &hapticInfo, (const XrHapticBaseHeader*)&vibration));
        }
    }
    return result;
}

void OpenXrInputPlugin::InputDevice::focusOutEvent() {
    _axisStateMap.clear();
    _buttonPressedMap.clear();
}

controller::Input::NamedVector OpenXrInputPlugin::InputDevice::getAvailableInputs() const {
    QVector<Input::NamedPair> availableInputs {
        makePair(A, "A"),
        makePair(B, "B"),
        makePair(X, "X"),
        makePair(Y, "Y"),

        makePair(LX, "LX"),
        makePair(LY, "LY"),
        makePair(RX, "RX"),
        makePair(RY, "RY"),
        makePair(LT, "LT"),
        makePair(RT, "RT"),
        makePair(LEFT_GRIP, "LeftGrip"),
        makePair(RIGHT_GRIP, "RightGrip"),
        makePair(LS, "LS"),
        makePair(RS, "RS"),

        makePair(LEFT_HAND, "LeftHand"),
        makePair(RIGHT_HAND, "RightHand"),
        makePair(HEAD, "Head"),

        makePair(LEFT_PRIMARY_THUMB_TOUCH, "LeftPrimaryThumbTouch"),
        makePair(LEFT_SECONDARY_THUMB_TOUCH, "LeftSecondaryThumbTouch"),
        makePair(RIGHT_PRIMARY_THUMB_TOUCH, "RightPrimaryThumbTouch"),
        makePair(RIGHT_SECONDARY_THUMB_TOUCH, "RightSecondaryThumbTouch"),
        makePair(LEFT_PRIMARY_INDEX_TOUCH, "LeftPrimaryIndexTouch"),
        makePair(RIGHT_PRIMARY_INDEX_TOUCH, "RightPrimaryIndexTouch"),
        makePair(LS_TOUCH, "LSTouch"),
        makePair(RS_TOUCH, "RSTouch"),
        makePair(LEFT_THUMB_UP, "LeftThumbUp"),
        makePair(RIGHT_THUMB_UP, "RightThumbUp"),
        makePair(LEFT_INDEX_POINT, "LeftIndexPoint"),
        makePair(RIGHT_INDEX_POINT, "RightIndexPoint"),

        makePair(BACK, "LeftApplicationMenu"),
        makePair(START, "RightApplicationMenu"),
    };

    static const char* HAND_NAMES[OpenXrRuntime::HandCount] = { "LeftHand", "RightHand" };
    static const char* FINGER_NAMES[FINGER_COUNT] = { "Thumb", "Index", "Middle", "Ring", "Pinky" };
    for (int hand = 0; hand < OpenXrRuntime::HandCount; hand++) {
        for (int finger = 0; finger < FINGER_COUNT; finger++) {
            for (int joint = 0; joint < JOINTS_PER_FINGER; joint++) {
                QString name = QString("%1%2%3").arg(HAND_NAMES[hand]).arg(FINGER_NAMES[finger]).arg(joint + 1);
                availableInputs.append(makePair(AVATAR_FINGER_JOINTS[hand][finger][joint], name));
            }
        }
    }
    return availableInputs;
}

QString OpenXrInputPlugin::InputDevice::getDefaultMappingConfig() const {
    static const QString MAPPING_JSON = PathUtils::resourcesPath() + "/controllers/openxr.json";
    return MAPPING_JSON;
}
