//
//  Flow.h
//
//  Created by Luis Cuenca on 1/21/2019.
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
//  Flow bones: secondary animation for hair, tails, skirts and the like (a spring-bone system).
//  Joints named flow_<group>_<n> or sim<Group><n> form chains. Each chain is simulated as particles
//  on a rope at a fixed 60 Hz step; the joint rotations are then fitted to the particle positions and
//  written into the rig's poses, so they travel to other clients in the ordinary avatar packet.
//  See FLOW-BONES.md in the docs repo for the design and the reasons behind it.
//

#ifndef hifi_Flow_h
#define hifi_Flow_h

#include <cmath>
#include <memory>
#include <qstring.h>
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <vector>
#include <map>
#include <quuid.h>
#include "AnimPose.h"

class Rig;
class AnimSkeleton;

const QString FLOW_JOINT_PREFIX = "flow";
const QString SIM_JOINT_PREFIX = "sim";

const std::vector<QString> HAND_COLLISION_JOINTS = { "RightHandMiddle1", "RightHandThumb3", "LeftHandMiddle1", "LeftHandThumb3", "RightHandMiddle3", "LeftHandMiddle3" };

const float HAND_COLLISION_RADIUS = 0.03f;
const float HELPER_JOINT_LENGTH = 0.05f;

// The FST-facing settings. The numbers keep the meaning they had in the original per-frame simulation
// at 60 Hz, so existing avatars load unchanged; FlowPhysicsSettings converts them to physical units.
const float DEFAULT_STIFFNESS = 0.0f;
const float DEFAULT_GRAVITY = -0.0096f;
const float DEFAULT_DAMPING = 0.85f;
const float DEFAULT_INERTIA = 0.8f;
const float DEFAULT_DELTA = 0.55f;
const float DEFAULT_RADIUS = 0.01f;
const float DEFAULT_MAX_ANGLE = 45.0f;  // degrees a bone may swing away from its rest direction
const float DEFAULT_LIFT = 0.0f;        // degrees each bone's rest direction is curled up from the model's pose
// Whether a bone collides along its length (Flow's way) or at its tip only (a VRM spring bone's: hair whose roots
// start inside the head's collider would otherwise be pushed out of it on every step)
const bool DEFAULT_TIP_COLLISION = false;

// The reference step: the settings are expressed per 1/60 s. Each frame is simulated in one step of the
// frame's own length, or in up to FLOW_MAX_STEPS_PER_FRAME equal steps when it is longer than a step and a
// half; a frame longer than the steps cover is cut to them. (A fixed step at the display's own rate skipped
// and doubled frames whenever the two drifted against each other.)
const float FLOW_STEP_HZ = 60.0f;
const float FLOW_STEP = 1.0f / FLOW_STEP_HZ;
const int FLOW_MAX_STEPS_PER_FRAME = 4;

// The chains are simulated in the avatar's own frame (translation removed, world orientation kept), so the
// avatar's position may judder from frame to frame without the hair jittering. The avatar's movement reaches
// the chains as an inertial force from its acceleration, smoothed over this time and capped.
const float FLOW_BODY_FILTER_TIME = 0.08f;      // s
const float FLOW_MAX_BODY_ACCELERATION = 30.0f; // m/s^2

struct FlowPhysicsSettings {
    FlowPhysicsSettings() {};
    FlowPhysicsSettings(bool active, float stiffness, float gravity, float damping, float inertia, float delta, float radius, float maxAngle = DEFAULT_MAX_ANGLE, float lift = DEFAULT_LIFT) {
        _active = active;
        _stiffness = stiffness;
        _gravity = gravity;
        _damping = damping;
        _inertia = inertia;
        _delta = delta;
        _radius = radius;
        _maxAngle = maxAngle;
        _lift = lift;
    }
    bool _active{ true };
    float _stiffness{ DEFAULT_STIFFNESS };
    float _gravity{ DEFAULT_GRAVITY };
    float _damping{ DEFAULT_DAMPING };
    float _inertia{ DEFAULT_INERTIA };
    float _delta{ DEFAULT_DELTA };
    float _radius{ DEFAULT_RADIUS };
    float _maxAngle{ DEFAULT_MAX_ANGLE };
    float _lift{ DEFAULT_LIFT };
    bool _tipCollision{ DEFAULT_TIP_COLLISION };

    // Physical meanings. Gravity keeps what the old step produced at 60 Hz (gravity * delta^2 per frame^2,
    // a factor of 3600 per second^2: -0.0096 is about -10.5 m/s^2). Stiffness is a spring toward the rest
    // position: at 1 it closes the whole gap in one step; under default gravity a bone droops about 3/stiffness
    // millimetres (the old cubic law left anything under 0.7 hanging limp).
    float springRate() const { return glm::clamp(_stiffness, 0.0f, 1.0f) * FLOW_STEP_HZ * FLOW_STEP_HZ; }  // 1/s^2
    float gravityAcceleration() const { return _gravity * _delta * _delta * FLOW_STEP_HZ * FLOW_STEP_HZ; }  // m/s^2
    float drag() const { return glm::clamp(1.0f - _damping, 0.0f, 1.0f); }  // velocity lost per step
    // How much of the gap to the rest position a step closes: springRate * dt^2, never more than all of it.
    float springStep(float dt) const { return glm::clamp(springRate() * dt * dt, 0.0f, 1.0f); }
    // The fraction of its velocity a node keeps over a step: the FST damping (given per 1/60 s) scaled to the
    // step, or less when the spring needs it. The stepped spring x' = x + c(x - x_prev) + K(rest - x) is
    // critically damped (no ringing, fastest return) at c = (1 - sqrt(K))^2; anything above that rings.
    float velocityKept(float dt) const {
        float kept = powf(1.0f - drag(), dt * FLOW_STEP_HZ);
        float K = springStep(dt);
        float critical = (1.0f - sqrtf(K)) * (1.0f - sqrtf(K));
        return glm::min(kept, critical);
    }
    // How much of the avatar's acceleration the chain feels: 1 is all of it, 0 makes the chain ignore the
    // avatar's movement (it still swings with the joints it hangs from).
    float inertiaBlend() const { return glm::clamp(_inertia, 0.0f, 1.0f); }
};

enum FlowCollisionType {
    CollisionSphere = 0,
    CollisionCapsule = 1
};

struct FlowCollisionSettings {
    FlowCollisionSettings() {};
    FlowCollisionSettings(const QUuid& id, const FlowCollisionType& type, const glm::vec3& offset, float radius) {
        _entityID = id;
        _type = type;
        _offset = offset;
        _radius = radius;
    };
    QUuid _entityID;
    FlowCollisionType _type { FlowCollisionType::CollisionSphere };
    float _radius { 0.05f };
    glm::vec3 _offset;
    // capsules run from the joint (plus offset) to the end joint (plus end offset)
    int _endJointIndex { -1 };
    glm::vec3 _endOffset;
};

const FlowPhysicsSettings DEFAULT_JOINT_SETTINGS;

// A sphere or capsule attached to a skeleton joint, in world space.
class FlowCollider {
public:
    FlowCollider() {};
    FlowCollider(int jointIndex, const FlowCollisionSettings& settings, bool isTouch = false);
    void setPosition(const glm::vec3& position) { _position = _endPosition = position; }
    // Moves `point` so that the segment from `segmentStart` to `point`, thickened by `pointRadius`,
    // clears the shape. Returns true when it had to.
    bool pushOut(const glm::vec3& segmentStart, glm::vec3& point, float pointRadius, bool tipOnly = false) const;

    QUuid _entityID;
    FlowCollisionType _type { FlowCollisionType::CollisionSphere };

    glm::vec3 _offset;
    glm::vec3 _initialOffset;
    glm::vec3 _position;
    glm::vec3 _endOffset;
    glm::vec3 _initialEndOffset;
    glm::vec3 _endPosition;

    bool _isTouch { false };
    int _jointIndex { -1 };
    int _endJointIndex { -1 };
    float _radius { 0.0f };
    float _initialRadius{ 0.0f };
};

class FlowCollisionSystem {
public:
    FlowCollisionSystem() {};
    void addCollisionSphere(int jointIndex, const FlowCollisionSettings& settings, const glm::vec3& position = { 0.0f, 0.0f, 0.0f }, bool isSelfCollision = true, bool isTouch = false);
    // Pushes `point` out of every active shape; returns true when any shape moved it.
    bool pushOut(const glm::vec3& segmentStart, glm::vec3& point, float pointRadius, bool tipOnly = false) const;

    std::vector<FlowCollider>& getSelfCollisions() { return _selfCollisions; };
    std::vector<FlowCollider>& getSelfTouchCollisions() { return _selfTouchCollisions; };
    // Builds this frame's list; `origin` is subtracted from the other avatars' hands, which arrive in world space.
    void prepareCollisions(const glm::vec3& origin);
    void resetCollisions();
    void resetOthersCollisions() { _othersCollisions.clear(); }
    void setScale(float scale);
    FlowCollisionSettings getCollisionSettingsByJoint(int jointIndex);
    void setCollisionSettingsByJoint(int jointIndex, const FlowCollisionSettings& settings);
    void setActive(bool active) { _active = active; }
    bool getActive() const { return _active; }
    const std::vector<FlowCollider>& getCollisions() const { return _selfCollisions; }
    void clearSelfCollisions() { _selfCollisions.clear(); }
protected:
    std::vector<FlowCollider> _selfCollisions;
    std::vector<FlowCollider> _othersCollisions;
    std::vector<FlowCollider> _selfTouchCollisions;
    std::vector<FlowCollider> _allCollisions;
    float _scale { 1.0f };
    bool _active { false };
};

// One simulated joint: a particle at the joint's position, plus the rotation fitted for it.
class FlowJoint {
public:
    friend class FlowThread;
    friend class Flow;

    FlowJoint() {};
    FlowJoint(int jointIndex, int parentIndex, int childIndex, const QString& name, const QString& group, const FlowPhysicsSettings& settings);
    void toHelperJoint(const glm::vec3& initialPosition, float length);
    void setInitialData(const glm::vec3& initialPosition, const glm::vec3& initialTranslation, const glm::quat& initialRotation, const glm::vec3& parentPosition);

    bool isAnchored() const { return _anchored; }
    void setAnchored(bool anchored) { _anchored = anchored; }
    bool isHelper() const { return _isHelper; }

    const FlowPhysicsSettings& getSettings() { return _settings; }
    void setSettings(const FlowPhysicsSettings& settings) { _settings = settings; }

    const glm::vec3& getCurrentPosition() const { return _currentPosition; }
    int getIndex() const { return _index; }
    int getParentIndex() const { return _parentIndex; }
    void setChildIndex(int index) { _childIndex = index; }
    const QString& getGroup() const { return _group; }
    const QString& getName() const { return _name; }
    const glm::quat& getCurrentRotation() const { return _currentRotation; }
    const glm::vec3& getInitialPosition() const { return _initialPosition; }
    const glm::quat& getInitialRotation() const { return _initialRotation; }
    bool isColliding() const { return _colliding; }

protected:
    int _index{ -1 };
    int _parentIndex{ -1 };
    int _childIndex{ -1 };
    QString _name;
    QString _group;

    bool _isHelper{ false };
    bool _anchored { false };
    bool _colliding { false };

    FlowPhysicsSettings _settings;

    glm::vec3 _initialPosition;
    glm::vec3 _initialTranslation;
    glm::quat _initialRotation;
    glm::vec3 _restDirection { 0.0f, 1.0f, 0.0f };  // to the next joint of the chain, in this joint's own frame

    // from this frame's animation
    glm::vec3 _animPosition;        // world
    glm::quat _animRotation;        // relative to the parent
    glm::quat _parentWorldRotation; // the parent's world rotation (used for the chain root)
    float _length { 0.0f };         // of the bone from the parent to this joint, this frame

    // simulation state
    glm::vec3 _currentPosition;
    glm::vec3 _previousPosition;
    glm::quat _worldRotation;       // fitted, world
    glm::quat _currentRotation;     // fitted, relative to the parent
};

// A chain of flow joints, root first.
class FlowThread {
public:
    FlowThread() {};
    FlowThread(int rootIndex, std::map<int, FlowJoint>* joints);

    void computeFlowThread(int rootIndex);
    void reset();
    // One fixed step: the root follows `rootPosition`; the rest swing, feel the avatar's acceleration,
    // spring back, stay within the cone, stay out of the colliders, and the rotations are fitted root to tip.
    void step(float dt, const glm::vec3& rootPosition, const glm::quat& rootParentRotation,
              const glm::vec3& bodyAcceleration, float scale, const FlowCollisionSystem& collisionSystem);

    std::vector<int> _joints;
    std::map<int, FlowJoint>* _jointsPointer { nullptr };
    glm::vec3 _lastRootPosition;
    glm::quat _lastRootParentRotation;
    bool _hasLastRoot { false };
};

class Flow : public QObject{
    Q_OBJECT
public:
    Flow() { }
    bool getActive() const { return _active; }
    void setActive(bool active) { _active = active; }
    bool isInitialized() const { return _initialized; }
    float getScale() const { return _scale; }
    void calculateConstraints(const std::shared_ptr<AnimSkeleton>& skeleton, AnimPoseVec& relativePoses, AnimPoseVec& absolutePoses);
    void update(float deltaTime, AnimPoseVec& relativePoses, AnimPoseVec& absolutePoses, const std::vector<bool>& overrideFlags);
    // Copies the flow joints' rotations from one pose set to another (the network pose set gets the same chain).
    void copyJointRotations(const AnimPoseVec& fromRelativePoses, AnimPoseVec& toRelativePoses) const;
    void setTransform(float scale, const glm::vec3& position, const glm::quat& rotation);
    const std::map<int, FlowJoint>& getJoints() const { return _flowJointData; }
    const std::vector<FlowThread>& getThreads() const { return _jointThreads; }
    void setOthersCollision(const QUuid& otherId, int jointIndex, const glm::vec3& position);
    FlowCollisionSystem& getCollisionSystem() { return _collisionSystem; }
    void setPhysicsSettingsForGroup(const QString& group, const FlowPhysicsSettings& settings);
    const std::map<QString, FlowPhysicsSettings>& getGroupSettings() const { return _groupSettings; }
    void cleanUp();
    void updateScale() { setScale(_scale); }

signals:
    void onCleanup();

private:
    void updateAbsolutePoses(const AnimPoseVec& relativePoses, AnimPoseVec& absolutePoses);
    bool getJointPositionInWorldFrame(const AnimPoseVec& absolutePoses, int jointIndex, glm::vec3& position, glm::vec3 translation, glm::quat rotation) const;
    bool getJointRotationInWorldFrame(const AnimPoseVec& absolutePoses, int jointIndex, glm::quat& result, const glm::quat& rotation) const;

    void updateAnchors(const AnimPoseVec& relativePoses, const AnimPoseVec& absolutePoses);
    void setJoints(AnimPoseVec& relativePoses, const std::vector<bool>& overrideFlags);
    void updateCollisionJoint(FlowCollider& collision, const AnimPoseVec& absolutePoses);
    void updateGroupSettings(const QString& group, const FlowPhysicsSettings& settings);
    void setScale(float scale);

    float _scale { 1.0f };
    float _lastScale{ 1.0f };
    glm::vec3 _entityPosition;
    glm::quat _entityRotation;
    glm::vec3 _lastEntityPosition;
    glm::vec3 _bodyVelocity;
    glm::vec3 _bodyAcceleration;
    bool _hasBodyMotion { false };
    std::map<int, FlowJoint> _flowJointData;
    std::map<QString, FlowPhysicsSettings> _groupSettings;
    std::vector<FlowThread> _jointThreads;
    std::vector<QString> _flowJointKeywords;
    FlowCollisionSystem _collisionSystem;
    bool _initialized { false };
    bool _active { false };
};

#endif // hifi_Flow_h
