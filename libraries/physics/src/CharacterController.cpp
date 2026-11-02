//
//  CharacterController.cpp
//  libraries/physics/src
//
//  Created by Andrew Meadows 2015.10.21
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: these functions, the carry's follow and step: setFollowParameters, getFollowLinearDisplacement,
//   getFollowVelocity, preSimulation, postSimulation, each with everything it calls on the vehicle's path.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//

#include "CharacterController.h"

#include <glm/gtx/norm.hpp>
#include <glm/gtx/quaternion.hpp>

#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <AvatarConstants.h>
#include <NumericalConstants.h>
#include <PhysicsCollisionGroups.h>

#include "JoltUtil.h"
#include "ObjectMotionState.h"
#include "PhysicsHelpers.h"
#include "PhysicsLogging.h"
#include "PhysicsWorld.h"

const glm::vec3 LOCAL_UP_AXIS(0.0f, 1.0f, 0.0f);

#ifdef DEBUG_STATE_CHANGE
#define SET_STATE(desiredState, reason) setState(desiredState, reason)
#else
#define SET_STATE(desiredState, reason) setState(desiredState)
#endif

CharacterController::CharacterMotor::CharacterMotor(const glm::vec3& vel, const glm::quat& rot, float horizTimescale, float vertTimescale) {
    velocity = vel;
    rotation = rot;
    hTimescale = horizTimescale;
    if (hTimescale < MIN_CHARACTER_MOTOR_TIMESCALE) {
        hTimescale = MIN_CHARACTER_MOTOR_TIMESCALE;
    }
    vTimescale = vertTimescale;
    if (vTimescale < 0.0f) {
        vTimescale = hTimescale;
    } else if (vTimescale < MIN_CHARACTER_MOTOR_TIMESCALE) {
        vTimescale = MIN_CHARACTER_MOTOR_TIMESCALE;
    }
}

static uint32_t _numCharacterControllers { 0 };

CharacterController::CharacterController() {
    _floorDistance = _scaleFactor * DEFAULT_AVATAR_FALL_HEIGHT;
    _state = State::Hover;
    _pendingFlags = PENDING_FLAG_UPDATE_SHAPE;

    // ATM CharacterController is a singleton.
    ++_numCharacterControllers;
    assert(_numCharacterControllers == 1);
}

CharacterController::~CharacterController() {
    _character = nullptr;
    _shape = nullptr;
}

bool CharacterController::needsRemoval() const {
    return (_world && (_pendingFlags & PENDING_FLAG_REMOVE_FROM_SIMULATION) == PENDING_FLAG_REMOVE_FROM_SIMULATION);
}

bool CharacterController::needsAddition() const {
    return (_world && (_pendingFlags & PENDING_FLAG_ADD_TO_SIMULATION) == PENDING_FLAG_ADD_TO_SIMULATION);
}

JPH::BodyID CharacterController::getInnerBodyID() const {
    return _character ? _character->GetInnerBodyID() : JPH::BodyID();
}

JPH::ObjectLayer CharacterController::characterLayer() const {
    return JoltLayers::layerFor(BULLET_COLLISION_GROUP_MY_AVATAR, computeCollisionMask());
}

void CharacterController::removeFromWorld() {
    if (_inWorld) {
        // the character takes its inner body with it
        if (_world) {
            _world->removeContacts(nullptr);
        }
        _character = nullptr;
        _inWorld = false;
    }
    _pendingFlags &= ~PENDING_FLAG_REMOVE_FROM_SIMULATION;
}

void CharacterController::addToWorld() {
    if (!_shape || !_world || !_world->getPhysicsSystem()) {
        return;
    }
    if (_inWorld) {
        _pendingFlags &= ~PENDING_FLAG_ADD_DETAILED_TO_SIMULATION;
        return;
    }

    updateMassProperties();
    _pendingFlags &= ~PENDING_FLAG_ADD_DETAILED_TO_SIMULATION;
    _pendingFlags &= ~PENDING_FLAG_JUMP;

    JPH::CharacterVirtualSettings settings;
    settings.mShape = _shape;
    settings.mMass = _mass;
    settings.mMaxSlopeAngle = acosf(glm::clamp(_minFloorNormalDotUp, -1.0f, 1.0f));
    settings.mUp = glmToJolt(_currentUp);
    // contacts below the centre of the bottom hemisphere count as standing on something
    settings.mSupportingVolume = JPH::Plane(glmToJolt(_currentUp), -_halfHeight);
    settings.mMaxStrength = 100.0f;
    settings.mCharacterPadding = 0.02f;
    settings.mPenetrationRecoverySpeed = 1.0f;
    settings.mPredictiveContactDistance = 0.1f;
    settings.mEnhancedInternalEdgeRemoval = true;
    // what everything else collides with and what rays hit: a kinematic body in the MY_AVATAR group
    settings.mInnerBodyShape = _shape;
    settings.mInnerBodyLayer = characterLayer();

    _character = new JPH::CharacterVirtual(&settings, glmToJoltR(_position), glmToJolt(_rotation), 0, _world->getPhysicsSystem());
    _inWorld = true;
    if (_hasPendingVelocity) {
        // arriving from another world, still moving
        _character->SetLinearVelocity(glmToJolt(_pendingVelocity));
        _hasPendingVelocity = false;
    }

    if (_pendingFlags & PENDING_FLAG_UPDATE_SHAPE) {
        // shouldn't fall in here, but if we do make sure both ADD and REMOVE bits are still set
        _pendingFlags |= PENDING_FLAG_ADD_TO_SIMULATION | PENDING_FLAG_REMOVE_FROM_SIMULATION |
                         PENDING_FLAG_ADD_DETAILED_TO_SIMULATION | PENDING_FLAG_REMOVE_DETAILED_FROM_SIMULATION;
    } else {
        _pendingFlags &= ~PENDING_FLAG_ADD_TO_SIMULATION;
    }
    updateCurrentGravity();
}

// Is the character standing on something it can stand on? Jolt answers from its last update.
bool CharacterController::checkForSupport() {
    if (!_character) {
        return false;
    }
    JPH::CharacterVirtual::EGroundState groundState = _character->GetGroundState();
    if (groundState == JPH::CharacterVirtual::EGroundState::OnGround) {
        return true;
    }
    if (groundState == JPH::CharacterVirtual::EGroundState::OnSteepGround) {
        return glm::dot(joltToGLM(_character->GetGroundNormal()), _currentUp) > _minFloorNormalDotUp;
    }
    return false;
}

// The contacts the character made during its last update go to the engine's contact map, so that scripts
// get collision events for the avatar and so that what the avatar bumps gets simulated here.
void CharacterController::recordContacts() {
    if (!_character || !_world) {
        return;
    }
    const JPH::BodyLockInterfaceNoLock& lockInterface = _world->getPhysicsSystem()->GetBodyLockInterfaceNoLock();
    for (const JPH::CharacterContact& contact : _character->GetActiveContacts()) {
        if (!contact.mHadCollision || contact.mBodyB.IsInvalid()) {
            continue;
        }
        JPH::BodyLockRead lock(lockInterface, contact.mBodyB);
        if (!lock.Succeeded()) {
            continue;
        }
        ObjectMotionState* other = PhysicsEngine::motionStateOf(lock.GetBody());
        if (other) {
            // a frame's copy of its interior (round 8): the contact is with the part the sub-shape copies
            if (ObjectMotionState* part = other->interiorPartAt(contact.mSubShapeIDB)) {
                other = part;
            }
            _world->recordCharacterContact(other, joltToGLM(contact.mPosition), joltToGLM(contact.mContactNormal), contact.mDistance);
        }
    }
}

// a ray straight down from the centre of the bottom sphere, to know how far the floor is
void CharacterController::measureFloorDistance(float rayLength, bool& rayHasHit) {
    rayHasHit = false;
    if (!_character || !_world) {
        return;
    }
    glm::vec3 rayStart = joltToGLM(_character->GetPosition()) - _halfHeight * _currentUp;
    glm::vec3 rayEnd = rayStart - rayLength * _currentUp;
    JPH::IgnoreSingleBodyFilter ignoreMe(getInnerBodyID());
    RayHit hit;
    if (_world->castRayClosest(rayStart, rayEnd, BULLET_COLLISION_GROUP_MY_AVATAR, computeCollisionMask(), &ignoreMe, hit)) {
        rayHasHit = true;
        _floorDistance = rayLength * hit.fraction - _radius;
    }
}

// A triangle mesh (a Terrain, a static mesh model) is a surface with no inside. A character put partly under one (by a
// teleport, a scale-up, a respawn) meets its triangles edge-on and from behind, where Jolt's penetration recovery
// finds no way out: the character stays stuck, and judders when it is driven. A box floor has an inside and pushes
// it out by itself. So look down the character's own axis, from the centre of its top sphere to its bottom, for a
// floor seen from above (front faces only, static bodies only). The axis is inside the capsule, so a character
// standing, walking or sliding on anything never has a floor across it; only a buried one does: lift it onto that
// floor. A character wholly under the surface sees nothing here and stays where it is.
namespace {
class StaticBodiesOnly : public JPH::BodyFilter {
public:
    StaticBodiesOnly(const JPH::BodyID& ignore) : _ignore(ignore) {}
    bool ShouldCollide(const JPH::BodyID& id) const override { return id != _ignore; }
    bool ShouldCollideLocked(const JPH::Body& body) const override { return body.IsStatic(); }
private:
    JPH::BodyID _ignore;
};
}

void CharacterController::liftOutOfFloor() {
    const float MIN_LIFT = 0.04f; // shallower than this, Jolt's own recovery copes (its padding is 0.02 m)
    bool lifted = false;
    glm::vec3 position = joltToGLM(_character->GetPosition());
    glm::vec3 top = position + _halfHeight * _currentUp;
    float length = 2.0f * _halfHeight + _radius; // the top sphere's centre to the bottom of the capsule
    JPH::RRayCast ray(glmToJoltR(top), glmToJolt(-length * _currentUp));
    JPH::RayCastSettings settings;
    settings.SetBackFaceMode(JPH::EBackFaceMode::IgnoreBackFaces);
    settings.mTreatConvexAsSolid = false; // from inside a static box the ray sees only its back faces: Jolt handles boxes
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    JPH::ObjectLayer layer = characterLayer();
    const JoltLayers::Tables& layers = _world->getLayers();
    JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(layers.objectVsBroadPhase, layer);
    JPH::DefaultObjectLayerFilter objectLayerFilter(layers.objectLayerPairs, layer);
    StaticBodiesOnly bodyFilter(getInnerBodyID());
    _world->getPhysicsSystem()->GetNarrowPhaseQueryNoLock().CastRay(ray, settings, collector, broadPhaseFilter,
                                                                     objectLayerFilter, bodyFilter);
    if (collector.HadHit()) {
        float depth = (1.0f - collector.mHit.mFraction) * length; // how far the floor is above the capsule's bottom
        if (depth > MIN_LIFT) {
            _character->SetPosition(glmToJoltR(position + depth * _currentUp));
            glm::vec3 velocity = joltToGLM(_character->GetLinearVelocity());
            float upward = glm::dot(velocity, _currentUp);
            if (upward < 0.0f) {
                _character->SetLinearVelocity(glmToJolt(velocity - upward * _currentUp));
            }
            if (!_liftedOutOfFloor) {
                qCDebug(physics) << "CharacterController: the feet were" << depth << "m under a floor; lifted onto it";
            }
            lifted = true;
        }
    }
    _liftedOutOfFloor = lifted;
}

const float MIN_TARGET_SPEED = 0.001f;
const float MIN_TARGET_SPEED_SQUARED = MIN_TARGET_SPEED * MIN_TARGET_SPEED;

void CharacterController::substep(float dt) {
    if (!_character || !_inWorld || !_world) {
        return;
    }
    // where this substep starts from, for the presentation (getPositionAndOrientation)
    _substepStartPosition = joltToGLM(_character->GetPosition());
    _substepStartStep = ObjectMotionState::getWorldSimulationStep();

    liftOutOfFloor();

    // scan down for nearby floor
    const float FLOOR_PROXIMITY_THRESHOLD = 0.3f * _radius;
    bool rayHasHit = false;
    measureFloorDistance(_radius + FLOOR_PROXIMITY_THRESHOLD, rayHasHit);

    _hasSupport = checkForSupport();

    // The reference the motors work against: the parent's velocity when the avatar is parented, else the
    // velocity of the floor it stands on, so a platform carries whoever stands on it.
    glm::vec3 referenceVelocity = _parentVelocity;
    if (glm::length2(referenceVelocity) == 0.0f && _hasSupport) {
        referenceVelocity = joltToGLM(_character->GetGroundVelocity());
    }

    glm::vec3 velocity = joltToGLM(_character->GetLinearVelocity()) - referenceVelocity;
    computeNewVelocity(dt, velocity);

    // gravity: the character is not a rigid body, so it falls because we say so
    if (!_hasSupport || _state == State::InAir || _state == State::Takeoff) {
        velocity += (dt * _currentGravity) * _currentUp;
    } else if (_state == State::Ground && glm::dot(velocity, _currentUp) < 0.0f) {
        // standing on something: no sinking into it
        velocity -= glm::dot(velocity, _currentUp) * _currentUp;
    }

    const float MINIMUM_TIME_REMAINING = 0.005f;
    const float MAX_DISPLACEMENT = 0.5f * _radius;
    _followTimeRemaining -= dt;
    if (_followTimeRemaining >= MINIMUM_TIME_REMAINING) {
        glm::vec3 startPos = joltToGLM(_character->GetPosition());
        glm::vec3 deltaPos = _followDesiredPosition - startPos;
        glm::vec3 vel = deltaPos / _followTimeRemaining;
        glm::vec3 linearDisplacement = vel * dt;
        float displacementLength = glm::length(linearDisplacement);
        if (displacementLength > MAX_DISPLACEMENT) {
            linearDisplacement *= MAX_DISPLACEMENT / displacementLength; // clamp displacement to prevent tunneling.
        }
        glm::vec3 endPos = startPos + linearDisplacement;

        // resolve the simple linearDisplacement
        _followLinearDisplacement += linearDisplacement;

        // now for the rotational part...
        glm::quat startRot = joltToGLM(_character->GetRotation());
        glm::quat desiredRot = _followDesiredRotation;

        // startRot as default rotation
        glm::quat endRot = startRot;

        // the dot product between two quaternions is equal to +/- cos(angle/2)
        // where 'angle' is that of the rotation between them
        float qDot = glm::dot(desiredRot, startRot);

        // when the abs() value of the dot product is approximately 1.0
        // then the two rotations are effectively adjacent
        const float MIN_DOT_PRODUCT_OF_ADJACENT_QUATERNIONS = 0.99999f; // corresponds to approx 0.5 degrees
        if (fabsf(qDot) < MIN_DOT_PRODUCT_OF_ADJACENT_QUATERNIONS) {
            if (qDot < 0.0f) {
                // the quaternions are actually on opposite hyperhemispheres
                // so we move one to agree with the other and negate qDot
                desiredRot = -desiredRot;
                qDot = -qDot;
            }
            glm::quat deltaRot = desiredRot * glm::inverse(startRot);

            // the axis is the imaginary part, but scaled by sin(angle/2)
            glm::vec3 axis(deltaRot.x, deltaRot.y, deltaRot.z);
            axis /= sqrtf(1.0f - qDot * qDot);

            // compute the angle we will resolve for this dt, but don't overshoot
            float angle = 2.0f * acosf(qDot);
            if (dt < _followTimeRemaining) {
                angle *= dt / _followTimeRemaining;
            }

            // accumulate rotation
            deltaRot = glm::angleAxis(angle, glm::normalize(axis));
            _followAngularDisplacement = glm::normalize(deltaRot * _followAngularDisplacement);

            // in order to accumulate displacement of avatar position, we need to take _shapeLocalOffset into account.
            endRot = deltaRot * startRot;
            glm::vec3 swingDisplacement = endRot * (-_shapeLocalOffset) - startRot * (-_shapeLocalOffset);
            _followLinearDisplacement += swingDisplacement;
        }
        _character->SetPosition(glmToJoltR(endPos));
        _character->SetRotation(glmToJolt(endRot));
    }
    _followTime += dt;

    _character->SetLinearVelocity(glmToJolt(velocity + referenceVelocity));

    // let Jolt move the character through the world: collide and slide, stairs, and sticking to the floor
    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
    updateSettings.mStickToFloorStepDown = glmToJolt(-_currentUp * _maxStepHeight);
    updateSettings.mWalkStairsStepUp = _stepUpEnabled ? glmToJolt(_currentUp * _maxStepHeight) : JPH::Vec3::sZero();
    JPH::ObjectLayer layer = characterLayer();
    const JoltLayers::Tables& layers = _world->getLayers();
    JPH::DefaultBroadPhaseLayerFilter broadPhaseFilter(layers.objectVsBroadPhase, layer);
    JPH::DefaultObjectLayerFilter objectLayerFilter(layers.objectLayerPairs, layer);
    JPH::IgnoreSingleBodyFilter bodyFilter(getInnerBodyID());
    JPH::ShapeFilter shapeFilter;
    _character->ExtendedUpdate(dt, glmToJolt(_currentGravity * _currentUp), updateSettings,
                               broadPhaseFilter, objectLayerFilter, bodyFilter, shapeFilter, *_world->getTempAllocator());
    _character->UpdateGroundVelocity();

    recordContacts();
}

void CharacterController::jump(const glm::vec3& dir) {
    _pendingFlags |= PENDING_FLAG_JUMP;
}

bool CharacterController::onGround() const {
    const float FLOOR_PROXIMITY_THRESHOLD = 0.3f * _radius;
    return _floorDistance < FLOOR_PROXIMITY_THRESHOLD || _hasSupport;
}

bool CharacterController::hasFloorBelow() const {
    // not onGround(): that keeps a lost floor for half a second so the state machine does not flap, and in half a
    // second a walker is well past the edge
    return _floorBelowNow;
}

#ifdef DEBUG_STATE_CHANGE
static const char* stateToStr(CharacterController::State state) {
    switch (state) {
    case CharacterController::State::Ground:
        return "Ground";
    case CharacterController::State::Takeoff:
        return "Takeoff";
    case CharacterController::State::InAir:
        return "InAir";
    case CharacterController::State::Hover:
        return "Hover";
    case CharacterController::State::Seated:
        return "Seated";
    default:
        return "Unknown";
    }
}
#endif // #ifdef DEBUG_STATE_CHANGE

void CharacterController::updateCurrentGravity() {
    int32_t collisionMask = computeCollisionMask();
    if (_state == State::Hover || collisionMask == BULLET_COLLISION_MASK_COLLISIONLESS || _isStuck) {
        _currentGravity = 0.0f;
    } else {
        _currentGravity = _gravity;
    }
}

void CharacterController::setGravity(float gravity) {
    _gravity = gravity;
    updateCurrentGravity();
}

float CharacterController::getGravity() {
    return _gravity;
}

#ifdef DEBUG_STATE_CHANGE
void CharacterController::setState(State desiredState, const char* reason) {
#else
void CharacterController::setState(State desiredState) {
#endif

    if (desiredState != _state) {
#ifdef DEBUG_STATE_CHANGE
        qCDebug(physics) << "CharacterController::setState" << stateToStr(desiredState) << "from" << stateToStr(_state) << "," << reason;
#endif
        _state = desiredState;
        updateCurrentGravity();
    }
}

void CharacterController::recomputeFlying() {
    _pendingFlags |= PENDING_FLAG_RECOMPUTE_FLYING;
}

void CharacterController::setLocalBoundingBox(const glm::vec3& minCorner, const glm::vec3& scale) {
    float x = scale.x;
    float z = scale.z;
    float radius = 0.5f * sqrtf(0.5f * (x * x + z * z));
    float halfHeight = 0.5f * scale.y - radius;
    float MIN_HALF_HEIGHT = 0.0f;
    if (halfHeight < MIN_HALF_HEIGHT) {
        halfHeight = MIN_HALF_HEIGHT;
    }

    // compare dimensions
    if (glm::abs(radius - _radius) > FLT_EPSILON || glm::abs(halfHeight - _halfHeight) > FLT_EPSILON) {
        _radius = radius;
        _halfHeight = halfHeight;
        const float DEFAULT_MIN_STEP_HEIGHT_FACTOR = 0.005f;
        const float DEFAULT_MAX_STEP_HEIGHT_FACTOR = 0.65f;
        _minStepHeight = DEFAULT_MIN_STEP_HEIGHT_FACTOR * (_halfHeight + _radius);
        _maxStepHeight = DEFAULT_MAX_STEP_HEIGHT_FACTOR * (_halfHeight + _radius);

        _pendingFlags |= PENDING_FLAG_UPDATE_SHAPE | PENDING_FLAG_REMOVE_FROM_SIMULATION | PENDING_FLAG_REMOVE_DETAILED_FROM_SIMULATION |
            PENDING_FLAG_ADD_TO_SIMULATION | PENDING_FLAG_ADD_DETAILED_TO_SIMULATION;
    }

    // it's ok to change offset immediately -- there are no thread safety issues here
    _shapeLocalOffset = minCorner + 0.5f * scale;
}

void CharacterController::setPhysicsEngine(const PhysicsEnginePointer& engine) {
    if (!_physicsEngine && engine) {
        // ATM there is only one PhysicsEngine: it is a singleton, and we are taking advantage
        // of that assumption here.  If we ever introduce more and allow for this backpointer
        // to change then we'll have to overhaul this method.
        _physicsEngine = engine;
        _world = engine->getWorld(_frameID);
    }
}

void CharacterController::setFrame(const QUuid& frameID) {
    if (frameID == _frameID && (_world || !_physicsEngine)) {
        return;
    }
    _frameID = frameID;
    if (!_physicsEngine) {
        return;
    }
    PhysicsWorld* world = _physicsEngine->getWorld(frameID);
    if (world == _world) {
        return;
    }
    // out of the old world now; into the new one at the next preSimulation(), where the position is fresh
    if (_inWorld) {
        removeFromWorld();
        _pendingFlags |= PENDING_FLAG_ADD_TO_SIMULATION;
    }
    _world = world;
    qCDebug(physics) << "CharacterController: avatar now in the world of frame" << frameID;
}

void CharacterController::setLinearVelocity(const glm::vec3& velocity) {
    if (_character && _inWorld) {
        _character->SetLinearVelocity(glmToJolt(velocity));
    } else {
        _pendingVelocity = velocity;
        _hasPendingVelocity = true;
    }
}

float CharacterController::getCollisionBrakeAttenuationFactor() const {
    // _collisionBrake ranges from 0.0 (no brake) to 1.0 (max brake)
    // which we use to compute a corresponding attenutation factor from 1.0 to 0.5
    return 1.0f - 0.5f * _collisionBrake;
}

void CharacterController::setCollisionless(bool collisionless) {
    if (collisionless != _collisionless) {
        _collisionless = collisionless;
        _pendingFlags |= PENDING_FLAG_UPDATE_COLLISION_MASK;
    }
}

void CharacterController::updateUpAxis(const glm::quat& rotation) {
    _currentUp = rotation * LOCAL_UP_AXIS;
    if (_character) {
        _character->SetUp(glmToJolt(_currentUp));
    }
}

void CharacterController::setPositionAndOrientation(
        const glm::vec3& position,
        const glm::quat& orientation) {
    updateUpAxis(orientation);
    _rotation = orientation;
    const float UNCHANGED_DISTANCE2 = 1.0e-8f; // a tenth of a millimetre, squared
    if (_character && glm::distance2(position, _handedOutPosition) < UNCHANGED_DISTANCE2) {
        // the pose handed out at the last harvest came back untouched: it was the presentation, a substep
        // behind, so the body stays where the simulation left it; only the rotation is taken, and the shape's
        // offset swings with it about the avatar's origin
        glm::quat bodyRotation = joltToGLM(_character->GetRotation());
        _position = joltToGLM(_character->GetPosition()) + orientation * _shapeLocalOffset - bodyRotation * _shapeLocalOffset;
        return;
    }
    // placed from outside (a teleport, a script, the follow helper): the body goes there, with no blend from where it was
    _position = position + orientation * _shapeLocalOffset;
    _substepStartStep = 0;
}

// The pose as presented: the character's position blended between the start and the end of its last substep
// by the leftover time, as every simulated object's is (ObjectMotionState::getPresentedBodyTransform), so
// walking is even at any display rate; setPositionAndOrientation keeps the body where it is when this pose
// comes back unchanged. The rotation is the avatar's own and is not simulated.
void CharacterController::getPositionAndOrientation(glm::vec3& position, glm::quat& rotation) const {
    if (_character) {
        rotation = joltToGLM(_character->GetRotation());
        glm::vec3 bodyPosition = joltToGLM(_character->GetPosition());
        if (ObjectMotionState::getInterpolatePresentation() && _substepStartStep == ObjectMotionState::getWorldSimulationStep()) {
            bodyPosition = glm::mix(_substepStartPosition, bodyPosition, ObjectMotionState::getPresentationAlpha());
        }
        position = bodyPosition - rotation * _shapeLocalOffset;
        _handedOutPosition = position;
    }
}

void CharacterController::setParentVelocity(const glm::vec3& velocity) {
    _parentVelocity = velocity;
}

// FROZEN (vehicle path): see the note at the top of this file.
void CharacterController::setFollowParameters(const glm::mat4& desiredBodyMatrix, float timeRemaining) {
    _followTimeRemaining = timeRemaining;
    // the desired pose comes from the HMD, already in the character's own world: in a frame, its coordinates
    // (MyAvatar::FollowHelper::prePhysicsUpdate builds it with the sensor-to-local matrix, which is constant while the
    // frame moves; converting a domain pose here with the frame's simulated transform paired two moments, vehicle master
    // plan entry 11)
    glm::quat rotation = glm::normalize(glm::quat_cast(desiredBodyMatrix));
    glm::vec3 position = glm::vec3(desiredBodyMatrix[3]);
    _followDesiredRotation = rotation;
    _followDesiredPosition = position + rotation * _shapeLocalOffset;
}

// FROZEN (vehicle path): see the note at the top of this file.
glm::vec3 CharacterController::getFollowLinearDisplacement() const {
    return _followLinearDisplacement;
}

glm::quat CharacterController::getFollowAngularDisplacement() const {
    return _followAngularDisplacement;
}

// FROZEN (vehicle path): see the note at the top of this file.
glm::vec3 CharacterController::getFollowVelocity() const {
    if (_followTime > 0.0f) {
        return _followLinearDisplacement / _followTime;
    } else {
        return glm::vec3();
    }
}

glm::vec3 CharacterController::getLinearVelocity() const {
    glm::vec3 velocity(0.0f);
    if (_character) {
        velocity = joltToGLM(_character->GetLinearVelocity());
    }
    return velocity;
}

glm::vec3 CharacterController::getVelocityChange() const {
    if (_character) {
        return _velocityChange;
    }
    return glm::vec3(0.0f);
}

void CharacterController::clearMotors() {
    _motors.clear();
}

void CharacterController::addMotor(const glm::vec3& velocity, const glm::quat& rotation, float horizTimescale, float vertTimescale) {
    _motors.push_back(CharacterController::CharacterMotor(velocity, rotation, horizTimescale, vertTimescale));
}

void CharacterController::applyMotor(int index, float dt, glm::vec3& worldVelocity, std::vector<glm::vec3>& velocities, std::vector<float>& weights) {
    assert(index < (int)(_motors.size()));
    CharacterController::CharacterMotor& motor = _motors[index];
    if (motor.hTimescale >= MAX_CHARACTER_MOTOR_TIMESCALE && motor.vTimescale >= MAX_CHARACTER_MOTOR_TIMESCALE) {
        // nothing to do
        return;
    }

    // rotate into motor-frame
    glm::quat toMotor = glm::inverse(motor.rotation);
    glm::vec3 velocity = toMotor * worldVelocity;

    int32_t collisionMask = computeCollisionMask();
    if (collisionMask == BULLET_COLLISION_MASK_COLLISIONLESS ||
            _state == State::Hover || motor.hTimescale == motor.vTimescale) {
        // modify velocity
        float tau = dt / motor.hTimescale;
        if (tau > 1.0f) {
            tau = 1.0f;
        }
        velocity += tau * (motor.velocity - velocity);

        // rotate back into world-frame
        velocity = motor.rotation * velocity;
        _targetVelocity += motor.rotation * (tau * motor.velocity);

        // store the velocity and weight
        velocities.push_back(velocity);
        weights.push_back(tau);
    } else {
        // compute local UP
        glm::vec3 up = toMotor * _currentUp;
        glm::vec3 motorVelocity = motor.velocity;

        // save these non-adjusted components for later
        glm::vec3 vTargetVelocity = glm::dot(motorVelocity, up) * up;
        glm::vec3 hTargetVelocity = motorVelocity - vTargetVelocity;

        // split velocity into horizontal and vertical components
        glm::vec3 vVelocity = glm::dot(velocity, up) * up;
        glm::vec3 hVelocity = velocity - vVelocity;
        glm::vec3 vMotorVelocity = glm::dot(motorVelocity, up) * up;
        glm::vec3 hMotorVelocity = motorVelocity - vMotorVelocity;

        // modify each component separately
        float maxTau = 0.0f;
        if (motor.hTimescale < MAX_CHARACTER_MOTOR_TIMESCALE) {
            float tau = dt / motor.hTimescale;
            if (tau > 1.0f) {
                tau = 1.0f;
            }
            maxTau = tau;
            hVelocity += (hMotorVelocity - hVelocity) * tau;
        }
        if (motor.vTimescale < MAX_CHARACTER_MOTOR_TIMESCALE) {
            float tau = dt / motor.vTimescale;
            if (tau > 1.0f) {
                tau = 1.0f;
            }
            if (tau > maxTau) {
                maxTau = tau;
            }
            vVelocity += (vMotorVelocity - vVelocity) * tau;
        }

        // add components back together and rotate into world-frame
        velocity = motor.rotation * (hVelocity + vVelocity);
        _targetVelocity += maxTau * (motor.rotation * (hTargetVelocity + vTargetVelocity));

        // store velocity and weights
        velocities.push_back(velocity);
        weights.push_back(maxTau);
    }
}

void CharacterController::computeNewVelocity(float dt, glm::vec3& velocity) {
    if (glm::length2(velocity) < MIN_TARGET_SPEED_SQUARED) {
        velocity = glm::vec3(0.0f);
    }

    // measure velocity changes and their weights
    std::vector<glm::vec3> velocities;
    velocities.reserve(_motors.size());
    std::vector<float> weights;
    weights.reserve(_motors.size());
    _targetVelocity = glm::vec3(0.0f);
    for (int i = 0; i < (int)_motors.size(); ++i) {
        applyMotor(i, dt, velocity, velocities, weights);
    }
    assert(velocities.size() == weights.size());

    // blend velocity changes according to relative weights
    float totalWeight = 0.0f;
    for (size_t i = 0; i < weights.size(); ++i) {
        totalWeight += weights[i];
    }
    if (totalWeight > 0.0f) {
        velocity = glm::vec3(0.0f);
        for (size_t i = 0; i < velocities.size(); ++i) {
            velocity += (weights[i] / totalWeight) * velocities[i];
        }
        _targetVelocity /= totalWeight;
    }
    if (glm::length2(velocity) < MIN_TARGET_SPEED_SQUARED) {
        velocity = glm::vec3(0.0f);
    }

    // 'thrust' is applied at the very end
    _targetVelocity += dt * _linearAcceleration;
    velocity += dt * _linearAcceleration;
    // Note the differences between these two variables:
    // _targetVelocity = ideal final velocity according to input
    // velocity = new final velocity after motors are applied to currentVelocity

    // Jolt's character never tunnels or gets stuck in meshes the way a rigid body could, so the collision
    // brake Bullet needed stays released.
    _collisionBrake = 0.0f;
}

void CharacterController::updateState() {
    if (!_physicsEngine || !_character) {
        return;
    }
    if (_pendingFlags & PENDING_FLAG_RECOMPUTE_FLYING) {
         SET_STATE(CharacterController::State::Hover, "recomputeFlying");
         _hasSupport = false;
         _pendingFlags &= ~PENDING_FLAG_RECOMPUTE_FLYING;
    }

    const float FLY_TO_GROUND_THRESHOLD = 0.1f * _radius;
    const float GROUND_TO_FLY_THRESHOLD = 0.8f * _radius + _halfHeight;
    const quint64 TAKE_OFF_TO_IN_AIR_PERIOD = 250 * MSECS_PER_SECOND;
    const float MIN_HOVER_HEIGHT = _scaleFactor * DEFAULT_AVATAR_MIN_HOVER_HEIGHT;
    const quint64 JUMP_TO_HOVER_PERIOD = _scaleFactor < 1.0f ? _scaleFactor * 1100 * MSECS_PER_SECOND : 1100 * MSECS_PER_SECOND;

    // scan for distant floor
    // rayStart is at center of bottom sphere
    float rayLength = _radius;
    int32_t collisionMask = computeCollisionMask();
    if (collisionMask == BULLET_COLLISION_MASK_COLLISIONLESS) {
        rayLength += MIN_HOVER_HEIGHT;
    } else {
        rayLength += _scaleFactor * DEFAULT_AVATAR_FALL_HEIGHT;
    }

    bool rayHasHit = false;
    {
        glm::vec3 rayStart = _position;
        glm::vec3 rayEnd = rayStart - rayLength * _currentUp;
        JPH::IgnoreSingleBodyFilter ignoreMe(getInnerBodyID());
        RayHit hit;
        if (_world->castRayClosest(rayStart, rayEnd, BULLET_COLLISION_GROUP_MY_AVATAR, collisionMask, &ignoreMe, hit)) {
            rayHasHit = true;
            _floorDistance = rayLength * hit.fraction - (_radius + _halfHeight);
        }
        _floorBelowNow = rayHasHit && _floorDistance < 0.3f * _radius;
    }
    quint64 now = usecTimestampNow();
    if (rayHasHit) {
        _rayHitStartTime = now;
    } else {
        const quint64 RAY_HIT_START_PERIOD = 500 * MSECS_PER_SECOND;
        if ((now - _rayHitStartTime) < RAY_HIT_START_PERIOD) {
            rayHasHit = true;
        } else {
            _floorDistance = FLT_MAX;
        }
    }

    // record a time stamp when the jump button was first pressed.
    bool jumpButtonHeld = _pendingFlags & PENDING_FLAG_JUMP;
    if ((_previousFlags & PENDING_FLAG_JUMP) != (_pendingFlags & PENDING_FLAG_JUMP)) {
        if (_pendingFlags & PENDING_FLAG_JUMP) {
            _jumpButtonDownStartTime = now;
            _jumpButtonDownCount++;
        }
    }

    glm::vec3 velocity = _preSimulationVelocity;

    // disable normal state transitions while collisionless
    const float MAX_WALKING_SPEED = 2.65f;
    if (collisionMask == BULLET_COLLISION_MASK_COLLISIONLESS) {
        // when collisionless: only switch between State::Ground, State::Hover and State::Seated
        // and bypass state debugging
        if (_isSeated) {
            _state = State::Seated;
        } else if (rayHasHit) {
            if (glm::length(velocity) > (MAX_WALKING_SPEED)) {
                _state = State::Hover;
            } else {
                _state = State::Ground;
            }
        } else {
            _state = State::Hover;
        }
    } else {
        switch (_state) {
            case State::Ground:
                if (!rayHasHit && !_hasSupport) {
                    if (_hoverWhenUnsupported) {
                       SET_STATE(State::Hover, "no ground detected");
                    } else {
                       SET_STATE(State::InAir, "falling");
                    }
                } else if (_pendingFlags & PENDING_FLAG_JUMP && _jumpButtonDownCount != _takeoffJumpButtonID) {
                    _takeoffJumpButtonID = _jumpButtonDownCount;
                    _takeoffToInAirStartTime = now;
                    SET_STATE(State::Takeoff, "jump pressed");
                } else if (rayHasHit && !_hasSupport && _floorDistance > GROUND_TO_FLY_THRESHOLD) {
                    SET_STATE(State::InAir, "falling");
                }
                break;
            case State::Takeoff:
                if (_hoverWhenUnsupported && (!rayHasHit && !_hasSupport)) {
                    SET_STATE(State::Hover, "no ground");
                } else if ((now - _takeoffToInAirStartTime) > TAKE_OFF_TO_IN_AIR_PERIOD) {
                    SET_STATE(State::InAir, "takeoff done");

                    // compute jumpSpeed based on the scaled jump height for the default avatar in default gravity.
                    const float jumpHeight = std::max(_scaleFactor * DEFAULT_AVATAR_JUMP_HEIGHT, DEFAULT_AVATAR_MIN_JUMP_HEIGHT);
                    const float jumpSpeed = sqrtf(2.0f * -DEFAULT_AVATAR_GRAVITY * jumpHeight);
                    velocity += jumpSpeed * _currentUp;
                    _character->SetLinearVelocity(glmToJolt(velocity));
                }
                break;
            case State::InAir: {
                const float jumpHeight = std::max(_scaleFactor * DEFAULT_AVATAR_JUMP_HEIGHT, DEFAULT_AVATAR_MIN_JUMP_HEIGHT);
                const float jumpSpeed = sqrtf(2.0f * -DEFAULT_AVATAR_GRAVITY * jumpHeight);
                if ((glm::dot(velocity, _currentUp) <= (jumpSpeed / 2.0f)) && ((_floorDistance < FLY_TO_GROUND_THRESHOLD) || _hasSupport)) {
                    SET_STATE(State::Ground, "hit ground");
                } else if (_zoneFlyingAllowed) {
                    glm::vec3 desiredVelocity = _targetVelocity;
                    if (glm::length2(desiredVelocity) < MIN_TARGET_SPEED_SQUARED) {
                        desiredVelocity = glm::vec3(0.0f);
                    }
                    bool vertTargetSpeedIsNonZero = glm::dot(desiredVelocity, _currentUp) > MIN_TARGET_SPEED;
                    if (_comfortFlyingAllowed && (jumpButtonHeld || vertTargetSpeedIsNonZero) && (_takeoffJumpButtonID != _jumpButtonDownCount)) {
                        SET_STATE(State::Hover, "double jump button");
                    } else if (_comfortFlyingAllowed && (jumpButtonHeld || vertTargetSpeedIsNonZero) && (now - _jumpButtonDownStartTime) > JUMP_TO_HOVER_PERIOD) {
                        SET_STATE(State::Hover, "jump button held");
                    } else if (_hoverWhenUnsupported && ((!rayHasHit && !_hasSupport) || _floorDistance > _scaleFactor * DEFAULT_AVATAR_FALL_HEIGHT)) {
                        // Transition to hover if there's no ground beneath us or we are above the fall threshold, regardless of _comfortFlyingAllowed
                        SET_STATE(State::Hover, "above fall threshold");
                    }
                }
                break;
            }
            case State::Hover: {
                float horizontalSpeed = glm::length(velocity - glm::dot(velocity, _currentUp) * _currentUp);
                bool flyingFast = horizontalSpeed > (MAX_WALKING_SPEED * 0.75f);
                if (!_zoneFlyingAllowed) {
                    SET_STATE(State::InAir, "zone flying not allowed");
                } else if (!_comfortFlyingAllowed && (rayHasHit || _hasSupport || _floorDistance < FLY_TO_GROUND_THRESHOLD)) {
                    SET_STATE(State::InAir, "comfort flying not allowed");
                } else if ((_floorDistance < MIN_HOVER_HEIGHT) && !jumpButtonHeld && !flyingFast) {
                    SET_STATE(State::InAir, "near ground");
                } else if (((_floorDistance < FLY_TO_GROUND_THRESHOLD) || _hasSupport) && !flyingFast) {
                    SET_STATE(State::Ground, "touching ground");
                }
                break;
            }
            case State::Seated: {
                SET_STATE(State::Ground, "Standing up");
                break;
            }
        }
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
void CharacterController::preSimulation() {
    if (needsRemoval()) {
        removeFromWorld();
    }
    updateShapeIfNecessary();
    if (needsAddition()) {
        addToWorld();
    }

    if (_character) {
        // slam the character's transform and remember velocity
        _character->SetPosition(glmToJoltR(_position));
        _character->SetRotation(glmToJolt(_rotation));
        _preSimulationVelocity = joltToGLM(_character->GetLinearVelocity());

        updateState();
    }

    _previousFlags = _pendingFlags;
    _pendingFlags &= ~PENDING_FLAG_JUMP;

    _followTime = 0.0f;
    _followLinearDisplacement = glm::vec3(0.0f);
    _followAngularDisplacement = glm::quat();
}

// FROZEN (vehicle path): see the note at the top of this file.
void CharacterController::postSimulation() {
    if (_character) {
        _velocityChange = joltToGLM(_character->GetLinearVelocity()) - _preSimulationVelocity;
    }
}

bool CharacterController::getRigidBodyLocation(glm::vec3& avatarRigidBodyPosition, glm::quat& avatarRigidBodyRotation) {
    if (!_character) {
        return false;
    }
    // in the domain, whichever world the character is in
    avatarRigidBodyPosition = joltToGLM(_character->GetPosition());
    avatarRigidBodyRotation = joltToGLM(_character->GetRotation());
    if (_world && !_world->isRoot()) {
        avatarRigidBodyPosition = _world->toParent(avatarRigidBodyPosition);
        avatarRigidBodyRotation = _world->toParent(avatarRigidBodyRotation);
    }
    avatarRigidBodyPosition += ObjectMotionState::getWorldOffset();
    return true;
}

void CharacterController::setCollisionlessAllowed(bool value) {
    if (value != _collisionlessAllowed) {
        _collisionlessAllowed = value;
        _pendingFlags |= PENDING_FLAG_UPDATE_COLLISION_MASK;
    }
}
