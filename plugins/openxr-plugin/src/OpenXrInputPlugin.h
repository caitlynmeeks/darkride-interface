//
//  OpenXrInputPlugin.h
//  plugins/openxr-plugin/src
//
//  Created on 2026-10-16 for Quest 3 hand tracking over Link (Meta's PC OpenXR runtime).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_OpenXrInputPlugin_h
#define hifi_OpenXrInputPlugin_h

#include <array>
#include <map>
#include <memory>

#include <controllers/InputDevice.h>
#include <controllers/StandardControls.h>
#include <plugins/InputPlugin.h>

#include "OpenXrRuntime.h"

// The headset, controllers and tracked hands of the OpenXR runtime, as the "OpenXR" input device.
//
// Controllers come through OpenXR actions, with bindings for Touch, Index, Vive and the simple controller. Hands come
// through XR_EXT_hand_tracking: the wrist drives the hand, each finger's joints drive the avatar's finger joints, and
// a pinch of the thumb and index finger works the trigger.
class OpenXrInputPlugin : public InputPlugin {
    Q_OBJECT
public:
    OpenXrInputPlugin(std::shared_ptr<OpenXrRuntime> runtime);

    bool isSupported() const override;
    const QString getName() const override { return NAME; }
    QString getDeviceName() override { return _runtime->getSystemName(); }

    bool isHandController() const override { return true; }
    bool isHeadController() const override { return true; }

    bool activate() override;
    void deactivate() override;

    void pluginFocusOutEvent() override { _inputDevice->focusOutEvent(); }
    void pluginUpdate(float deltaTime, const controller::InputCalibrationData& inputCalibrationData) override;

private:
    class InputDevice : public controller::InputDevice {
    public:
        InputDevice(std::shared_ptr<OpenXrRuntime> runtime);
        ~InputDevice();

    private:
        controller::Input::NamedVector getAvailableInputs() const override;
        QString getDefaultMappingConfig() const override;
        void update(float deltaTime, const controller::InputCalibrationData& inputCalibrationData) override;
        void focusOutEvent() override;
        bool triggerHapticPulse(float strength, float duration, controller::Hand hand) override;

        enum ActionId {
            GRIP_POSE = 0,
            TRIGGER,
            TRIGGER_TOUCH,
            SQUEEZE,
            THUMBSTICK,
            THUMBSTICK_CLICK,
            THUMBSTICK_TOUCH,
            PRIMARY,
            PRIMARY_TOUCH,
            SECONDARY,
            SECONDARY_TOUCH,
            MENU,
            HAPTIC,
            ACTION_COUNT
        };

        bool createActions();
        void suggestBindings();
        bool attachToSession();
        void releaseSessionObjects();

        bool getBool(ActionId action, int hand) const;
        float getFloat(ActionId action, int hand) const;
        glm::vec2 getVector2(ActionId action, int hand) const;

        void updateHead(const controller::InputCalibrationData& inputCalibrationData, const glm::mat4& sensorToAvatar, XrTime time);
        void updateController(int hand, const glm::mat4& sensorToAvatar, XrTime time);
        // true when the hand is tracked, and has set the hand and finger poses
        bool updateTrackedHand(int hand, const glm::mat4& sensorToAvatar, XrTime time);

        std::shared_ptr<OpenXrRuntime> _runtime;

        XrActionSet _actionSet { XR_NULL_HANDLE };
        std::array<XrAction, ACTION_COUNT> _actions;
        std::array<XrSpace, OpenXrRuntime::HandCount> _gripSpaces {{ XR_NULL_HANDLE, XR_NULL_HANDLE }};
        std::array<XrHandTrackerEXT, OpenXrRuntime::HandCount> _handTrackers {{ XR_NULL_HANDLE, XR_NULL_HANDLE }};
        // the session the action set is attached to
        uint32_t _attachedGeneration { 0 };
        bool _attached { false };
        std::array<bool, OpenXrRuntime::HandCount> _pinching {{ false, false }};

        friend class OpenXrInputPlugin;
    };

    std::shared_ptr<OpenXrRuntime> _runtime;
    std::shared_ptr<InputDevice> _inputDevice;
    bool _registeredWithInputMapper { false };

    static const char* NAME;
};

#endif // hifi_OpenXrInputPlugin_h
