//
//  Camera.h
//  interface/src
//
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: the Placing scope (the engine places the camera inside ViewState::updateCamera) and the pose setters
//   it guards (their bodies in Camera.cpp).
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//

#ifndef hifi_Camera_h
#define hifi_Camera_h

#include "../GLMHelpers.h"
#include "../RegisteredMetaTypes.h"
#include "../ViewFrustum.h"

#include <atomic>

enum CameraMode
{
    CAMERA_MODE_NULL = -1,
    CAMERA_MODE_THIRD_PERSON,
    CAMERA_MODE_FIRST_PERSON_LOOK_AT,
    CAMERA_MODE_FIRST_PERSON,
    CAMERA_MODE_MIRROR,
    CAMERA_MODE_INDEPENDENT,
    CAMERA_MODE_ENTITY,
    CAMERA_MODE_LOOK_AT,
    CAMERA_MODE_SELFIE,
    NUM_CAMERA_MODES
};

Q_DECLARE_METATYPE(CameraMode);

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((unused))
#endif
static int cameraModeId = qRegisterMetaType<CameraMode>();

class Camera : public QObject {
    Q_OBJECT

    Q_PROPERTY(glm::vec3 position READ getPosition WRITE setPosition)
    Q_PROPERTY(glm::quat orientation READ getOrientation WRITE setOrientation)
    Q_PROPERTY(QString mode READ getModeString WRITE setModeString NOTIFY modeUpdated)
    Q_PROPERTY(QVariantMap frustum READ getViewFrustum CONSTANT)
    Q_PROPERTY(bool disableLookAt READ getDisableLookAt WRITE setDisableLookAt)

public:
    Camera();
    virtual ~Camera() {}

    virtual void update();

    CameraMode getMode() const { return _mode; }
    void setMode(CameraMode m);

    void loadViewFrustum(ViewFrustum& frustum) const;

    const glm::mat4& getTransform() const { return _transform; }
    void setTransform(const glm::mat4& transform);

    const glm::mat4& getProjection() const { return _projection; }
    void setProjection(const glm::mat4& projection);

    // FROZEN (vehicle path): see the note at the top of this file.
    // Who placed the camera (team/performance/INSTRUMENTS-PLAN.md, item 11): the engine composes every desktop mode's
    // pose on the game loop inside ViewState::updateCamera, in a Placing scope; a pose setter called outside that
    // scope is a script's (Camera.position from a script's thread, on that thread's clock, a frame off at random),
    // noted until the frame trace reads it each frame (its cam.source column). A flag, not a count: a script's set
    // that lands while updateCamera itself runs is taken for the engine's, so a script's camera is missed now and
    // then, never reported falsely (no engine code places the camera outside ViewState::updateCamera).
    class Placing {
    public:
        Placing(Camera& camera) : _camera(camera) { _camera._placing = true; }
        ~Placing() { _camera._placing = false; }
    private:
        Camera& _camera;
    };
    bool takeSetOutside() { return _setOutside.exchange(false); }

    QVariantMap getViewFrustum();

public slots:
    /**jsdoc
     * Gets the current camera mode. You can also get the mode using the {@link Camera|Camera.mode} property.
     * @function Camera.getModeString
     * @returns {Camera.Mode} The current camera mode.
     */
    QString getModeString() const;
    
    /**jsdoc
     * Sets the camera mode. You can also set the mode using the {@link Camera|Camera.mode} property.
     * @function Camera.setModeString
     * @param {Camera.Mode} mode - The mode to set the camera to.
     */
    void setModeString(const QString& mode);

    /**jsdoc
     * Gets the current camera position. You can also get the position using the {@link Camera|Camera.position} property.
     * @function Camera.getPosition
     * @returns {Vec3} The current camera position.
     */
    glm::vec3 getPosition() const { return _position; }

    /**jsdoc
     * Sets the camera position. You can also set the position using the {@link Camera|Camera.position} property. Only works if 
     * the camera is in independent mode.
     * @function Camera.setPosition
     * @param {Vec3} position - The position to set the camera at.
     */
    void setPosition(const glm::vec3& position);

    /**jsdoc
     * Gets the current camera orientation. You can also get the orientation using the {@link Camera|Camera.orientation} 
     * property.
     * @function Camera.getOrientation
     * @returns {Quat} The current camera orientation.
     */
    glm::quat getOrientation() const { return _orientation; }

    /**jsdoc
     * Sets the camera orientation. You can also set the orientation using the {@link Camera|Camera.orientation} property. Only
     * works if the camera is in independent mode.
     * @function Camera.setOrientation
     * @param {Quat} orientation - The orientation to set the camera to.
     */
    void setOrientation(const glm::quat& orientation);

    /**jsdoc
     * Computes a {@link PickRay} based on the current camera configuration and the specified <code>x, y</code> position on the 
     * screen. The {@link PickRay} can be used in functions such as {@link Entities.findRayIntersection} and 
     * {@link Overlays.findRayIntersection}.
     * @function Camera.computePickRay
     * @param {number} x - X-coordinate on screen.
     * @param {number} y - Y-coordinate on screen.
     * @returns {PickRay} The computed {@link PickRay}.
     * @example <caption>Use a PickRay to detect mouse clicks on entities.</caption>
     * function onMousePressEvent(event) {
     *     var pickRay = Camera.computePickRay(event.x, event.y);
     *     var intersection = Entities.findRayIntersection(pickRay);
     *     if (intersection.intersects) {
     *         print("You clicked on entity " + intersection.entityID);
     *     }
     * }
     *
     * Controller.mousePressEvent.connect(onMousePressEvent);
     */
    virtual PickRay computePickRay(float x, float y) const = 0;

    /**jsdoc
     * Rotates the camera to look at the specified <code>position</code>. Only works if the camera is in independent mode.
     * @function Camera.lookAt
     * @param {Vec3} position - The position to look at.
     * @example <caption>Rotate your camera to look at entities as you click on them with your mouse.</caption>
     * function onMousePressEvent(event) {
     *     var pickRay = Camera.computePickRay(event.x, event.y);
     *     var intersection = Entities.findRayIntersection(pickRay);
     *     if (intersection.intersects) {
     *         // Switch to independent mode.
     *         Camera.mode = "independent";
     *         // Look at the entity that was clicked.
     *         var properties = Entities.getEntityProperties(intersection.entityID, "position");
     *         Camera.lookAt(properties.position);
     *     }
     * }
     *
     * Controller.mousePressEvent.connect(onMousePressEvent);
     */
    void lookAt(const glm::vec3& position);

    /**jsdoc
     * Sets the camera to continue looking at the specified <code>position</code> even while the camera moves. Only works if 
     * the camera is in independent mode.
     * @function Camera.keepLookingAt
     * @param {Vec3} position - The position to keep looking at.
     */
    void keepLookingAt(const glm::vec3& position);

    /**jsdoc
     * Stops the camera from continually looking at the position that was set with {@link Camera.keepLookingAt}.
     * @function Camera.stopLookingAt
     */
    void stopLooking() { _isKeepLookingAt = false; }

    /**jsdoc
     * Gets whether "look at" is disabled which will make the camera rotate with the avatar's orientation.
     * @function Camera.getDisableLookAt
     * @returns {boolean} 
     */
    bool getDisableLookAt();

    /**jsdoc
     * Sets whether "look at" is disabled which will make the camera rotate with the avatar's orientation.
     * @function Camera.setDisableLookAt
     * @param {boolean} disabled - 
     */
    void setDisableLookAt(const bool disabled);
signals:
    /**jsdoc
     * Triggered when the camera mode changes.
     * @function Camera.modeUpdated
     * @param {Camera.Mode} newMode - The new camera mode.
     * @returns {Signal}
     * @example <caption>Report camera mode changes.</caption>
     * function onCameraModeUpdated(newMode) {
     *     print("The camera mode has changed to " + newMode);
     * }
     *
     * Camera.modeUpdated.connect(onCameraModeUpdated);
     */
    void modeUpdated(const QString& newMode);

private:
    void recompose();
    void decompose();

    CameraMode _mode{ CAMERA_MODE_LOOK_AT };
    glm::mat4 _transform;
    glm::mat4 _projection;

    // derived
    glm::vec3 _position { 0.0f, 0.0f, 0.0f };
    glm::quat _orientation;
    bool _isKeepLookingAt{ false };
    glm::vec3 _lookingAt;
    bool _disableLookAt{ false };

    void noteSetter() { if (!_placing) { _setOutside = true; } }   // a pose set outside updateCamera's scope
    std::atomic<bool> _placing{ false };
    std::atomic<bool> _setOutside{ false };
};

#endif // hifi_Camera_h
