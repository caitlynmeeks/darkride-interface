//
//  CharacterController.h
//  libraries/physics/src
//
//  Created by Andrew Meadows 2015.10.21
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_CharacterController_h
#define hifi_CharacterController_h

#include <assert.h>
#include <stdint.h>
#include <atomic>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <GLMHelpers.h>
#include <NumericalConstants.h>

#include "AvatarConstants.h"
#include "PhysicsEngine.h"
#include "PhysicsHelpers.h"

const uint32_t PENDING_FLAG_ADD_TO_SIMULATION = 1U << 0;
const uint32_t PENDING_FLAG_REMOVE_FROM_SIMULATION = 1U << 1;
const uint32_t PENDING_FLAG_UPDATE_SHAPE = 1U << 2;
const uint32_t PENDING_FLAG_JUMP = 1U << 3;
const uint32_t PENDING_FLAG_UPDATE_COLLISION_MASK = 1U << 4;
const uint32_t PENDING_FLAG_RECOMPUTE_FLYING = 1U << 5;
const uint32_t PENDING_FLAG_ADD_DETAILED_TO_SIMULATION = 1U << 6;
const uint32_t PENDING_FLAG_REMOVE_DETAILED_FROM_SIMULATION = 1U << 7;
const float DEFAULT_MIN_FLOOR_NORMAL_DOT_UP = cosf(PI / 3.0f);

const uint32_t NUM_SUBSTEPS_FOR_STUCK_TRANSITION = 6; // physics substeps
const uint32_t NUM_SUBSTEPS_FOR_SAFE_LANDING_RETRY = NUM_SUBSTEPS_PER_SECOND / 2; // retry every half second

const float MAX_CHARACTER_MOTOR_TIMESCALE = 60.0f; // one minute
const float MIN_CHARACTER_MOTOR_TIMESCALE = 0.05f;

// MyAvatar's body in the physics world: Jolt's virtual character (collide-and-slide, stairs, slopes, and the
// velocity of whatever it stands on), driven by the engine's own motors and state machine. The character is
// not a rigid body; an inner kinematic body in the MY_AVATAR group is what other things collide with and
// what ray casts hit. The engine calls substep() before every 90 Hz step of the world.
class CharacterController {
public:
    CharacterController();
    virtual ~CharacterController();

    bool needsRemoval() const;
    bool needsAddition() const;
    virtual void addToWorld();
    void removeFromWorld();

    // the kinematic body that stands in for the character where a body is needed; invalid when not in the world
    JPH::BodyID getInnerBodyID() const;
    bool isInWorld() const { return _inWorld && _character != nullptr; }

    void setGravity(float gravity);
    float getGravity();

    void recomputeFlying();

    virtual void updateShapeIfNecessary() = 0;

    // the engine calls this before every substep of the world
    void substep(float dt);

    void jump(const glm::vec3& dir = glm::vec3(0.0f));
    bool onGround() const;
    // floor within reach straight below the capsule: what frame membership asks (an edge still holds a capsule up,
    // as Jolt's ground state says, while the floor below has already ended)
    bool hasFloorBelow() const;

    void clearMotors();
    void addMotor(const glm::vec3& velocity, const glm::quat& rotation, float horizTimescale, float vertTimescale = -1.0f);
    void applyMotor(int index, float dt, glm::vec3& worldVelocity, std::vector<glm::vec3>& velocities, std::vector<float>& weights);
    void setStepUpEnabled(bool enabled) { _stepUpEnabled = enabled; }
    void computeNewVelocity(float dt, glm::vec3& velocity);

    void setScaleFactor(float scaleFactor) { _scaleFactor = scaleFactor; }

    // HACK for legacy 'thrust' feature
    void setLinearAcceleration(const glm::vec3& acceleration) { _linearAcceleration = acceleration; }

    void preSimulation();
    void postSimulation();

    void setPositionAndOrientation(const glm::vec3& position, const glm::quat& orientation);
    void getPositionAndOrientation(glm::vec3& position, glm::quat& rotation) const;

    void setParentVelocity(const glm::vec3& parentVelocity);
    // the pose the follow steps the character toward, in the character's own world (the frame's coordinates in a frame)
    void setFollowParameters(const glm::mat4& desiredBodyMatrix, float timeRemaining);
    float getFollowTime() const { return _followTime; }
    glm::vec3 getFollowLinearDisplacement() const;
    glm::quat getFollowAngularDisplacement() const;
    glm::vec3 getFollowVelocity() const;
    glm::vec3 getLinearVelocity() const;
    glm::vec3 getVelocityChange() const;

    float getCapsuleRadius() const { return _radius; }
    float getCapsuleHalfHeight() const { return _halfHeight; }
    glm::vec3 getCapsuleLocalOffset() const { return _shapeLocalOffset; }

    enum class State {
        Ground = 0,
        Takeoff,
        InAir,
        Hover,
        Seated
    };

    State getState() const { return _state; }
    void updateState();

    void setLocalBoundingBox(const glm::vec3& minCorner, const glm::vec3& scale);

    void setPhysicsEngine(const PhysicsEnginePointer& engine);
    const PhysicsEnginePointer& getPhysicsEngine() const { return _physicsEngine; }
    bool isEnabledAndReady() const { return (bool)_physicsEngine; }

    // The world the character moves through: the root (the domain) or a moving frame's. Positions, orientations
    // and velocities given to and taken from the controller are in that world's coordinates. Changing the frame
    // takes the character out of its world; it is put into the new one at the next preSimulation(), at the
    // position set by then, with the velocity set by setLinearVelocity().
    PhysicsWorld* getWorld() const { return _world; }
    const QUuid& getFrameID() const { return _frameID; }
    void setFrame(const QUuid& frameID);
    void setLinearVelocity(const glm::vec3& velocity);
    bool isStuck() const { return _isStuck; }
    float getCollisionBrakeAttenuationFactor() const;

    void setCollisionless(bool collisionless);

    // Only the controller for MyAvatar's avatar needs to know about the avatar's collision mask
    virtual int32_t computeCollisionMask() const = 0;
    virtual void handleChangedCollisionMask() = 0;

    bool getRigidBodyLocation(glm::vec3& avatarRigidBodyPosition, glm::quat& avatarRigidBodyRotation);

    void setZoneFlyingAllowed(bool value) { _zoneFlyingAllowed = value; }
    void setComfortFlyingAllowed(bool value) { _comfortFlyingAllowed = value; }
    void setHoverWhenUnsupported(bool value) { _hoverWhenUnsupported = value; }
    void setCollisionlessAllowed(bool value);

    void setPendingFlagsUpdateCollisionMask(){ _pendingFlags |= PENDING_FLAG_UPDATE_COLLISION_MASK; }
    void setSeated(bool isSeated) { _isSeated = isSeated;  }
    bool getSeated() { return _isSeated; }

    void resetStuckCounter() { _numStuckSubsteps = 0; }

protected:
#ifdef DEBUG_STATE_CHANGE
    void setState(State state, const char* reason);
#else
    void setState(State state);
#endif

    // sets _mass from the character's dimensions
    virtual void updateMassProperties() = 0;
    void updateCurrentGravity();
    void updateUpAxis(const glm::quat& rotation);
    bool checkForSupport();
    void measureFloorDistance(float rayLength, bool& rayHasHit);
    void liftOutOfFloor();
    void recordContacts();
    JPH::ObjectLayer characterLayer() const;

protected:
    struct CharacterMotor {
        CharacterMotor(const glm::vec3& vel, const glm::quat& rot, float horizTimescale, float vertTimescale = -1.0f);

        glm::vec3 velocity { 0.0f }; // local-frame
        glm::quat rotation; // local-to-world
        float hTimescale { MAX_CHARACTER_MOTOR_TIMESCALE }; // horizontal
        float vTimescale { MAX_CHARACTER_MOTOR_TIMESCALE }; // vertical
    };

    std::vector<CharacterMotor> _motors;
    JPH::Ref<JPH::CharacterVirtual> _character;
    JPH::RefConst<JPH::Shape> _shape;
    glm::vec3 _currentUp { 0.0f, 1.0f, 0.0f };
    glm::vec3 _targetVelocity { 0.0f };
    glm::vec3 _parentVelocity { 0.0f };
    glm::vec3 _preSimulationVelocity { 0.0f };
    glm::vec3 _velocityChange { 0.0f };
    glm::vec3 _followDesiredPosition { 0.0f };
    glm::quat _followDesiredRotation;
    float _followTimeRemaining { 0.0f };
    glm::vec3 _position { 0.0f };
    glm::quat _rotation;
    mutable glm::vec3 _handedOutPosition { 0.0f }; // the pose last handed out (getPositionAndOrientation)
    glm::vec3 _substepStartPosition { 0.0f };       // the character's position before its last substep, for the presentation
    uint32_t _substepStartStep { 0 };
    glm::vec3 _shapeLocalOffset { 0.0f };

    glm::vec3 _boxScale { 1.0f }; // used to compute capsule shape

    quint64 _rayHitStartTime { 0 };
    quint64 _takeoffToInAirStartTime { 0 };
    quint64 _jumpButtonDownStartTime { 0 };
    quint32 _jumpButtonDownCount { 0 };
    quint32 _takeoffJumpButtonID { 0 };

    float _minStepHeight { 0.0f };
    float _maxStepHeight { 0.0f };
    float _minFloorNormalDotUp { DEFAULT_MIN_FLOOR_NORMAL_DOT_UP };

    float _halfHeight { 0.0f };
    float _radius { 0.0f };
    float _mass { 70.0f };

    float _floorDistance { 0.0f };
    bool _stepUpEnabled { true };
    bool _hasSupport { false };
    bool _floorBelowNow { false }; // the floor ray reached floor in this update, with none of the grace period below
    bool _liftedOutOfFloor { false }; // the last substep found the feet under a floor and lifted them out

    float _currentGravity { 0.0f };
    float _gravity { DEFAULT_AVATAR_GRAVITY };

    float _followTime { 0.0f };
    glm::vec3 _followLinearDisplacement { 0.0f };
    glm::quat _followAngularDisplacement;
    glm::vec3 _linearAcceleration { 0.0f };

    State _state { State::Hover };
    bool _isPushingUp { false };
    bool _isStuck { false };
    bool _isSeated { false };
    float _collisionBrake { 0.0f };

    PhysicsEnginePointer _physicsEngine { nullptr };
    PhysicsWorld* _world { nullptr };
    QUuid _frameID;
    glm::vec3 _pendingVelocity { 0.0f };
    bool _hasPendingVelocity { false };
    uint32_t _pendingFlags { 0 };
    uint32_t _previousFlags { 0 };
    uint32_t _stuckTransitionCount { 0 };
    uint32_t _numStuckSubsteps { 0 };

    bool _inWorld { false };
    bool _zoneFlyingAllowed { true };
    bool _comfortFlyingAllowed { true };
    bool _hoverWhenUnsupported { true };
    bool _collisionless { false };
    bool _collisionlessAllowed { true };

    float _scaleFactor { 1.0f };
};

#endif // hifi_CharacterController_h
