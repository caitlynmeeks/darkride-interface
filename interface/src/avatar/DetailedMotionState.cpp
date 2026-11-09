//
//  DetailedMotionState.cpp
//  interface/src/avatar/
//
//  Created by Luis Cuenca 1/11/2019
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "DetailedMotionState.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyInterface.h>

#include <PhysicsCollisionGroups.h>
#include <PhysicsEngine.h>
#include <PhysicsHelpers.h>

#include "MyAvatar.h"

DetailedMotionState::DetailedMotionState(AvatarPointer avatar, const JPH::Shape* shape, int32_t jointIndex) :
    ObjectMotionState(shape), _avatar(avatar), _jointIndex(jointIndex) {
    assert(_avatar);
    if (!_avatar->isMyAvatar()) {
        _otherAvatar = std::static_pointer_cast<OtherAvatar>(_avatar);
    }
    _type = MOTIONSTATE_TYPE_DETAILED;
}

void DetailedMotionState::handleEasyChanges(uint32_t& flags) {
    ObjectMotionState::handleEasyChanges(flags);
    if (flags & Simulation::DIRTY_PHYSICS_ACTIVATION && !isActive()) {
        activateBody();
    }
}

DetailedMotionState::~DetailedMotionState() {
    assert(_avatar);
    _avatar = nullptr;
}

// virtual
uint32_t DetailedMotionState::getIncomingDirtyFlags() const {
    return _body ? _dirtyFlags : 0;
}

void DetailedMotionState::clearIncomingDirtyFlags(uint32_t mask) {
    if (_body) {
        _dirtyFlags &= ~mask;
    }
}

PhysicsMotionType DetailedMotionState::computePhysicsMotionType() const {
    return MOTION_TYPE_KINEMATIC;
}

// virtual and protected
bool DetailedMotionState::isMoving() const {
    return false;
}

// Before every substep: follow the joint, with a velocity, so a hand can push a ball.
void DetailedMotionState::pushTransformToBody(float dt) {
    if (_body && bodyInterface()) {
        bodyInterface()->MoveKinematic(_body->GetID(), glmToJoltR(getObjectPosition()), glmToJolt(getObjectRotation()), dt);
    }
}

// These pure virtual methods must be implemented for each MotionState type
// and make it possible to implement more complicated methods in this base class.

// virtual
float DetailedMotionState::getObjectRestitution() const {
    return 0.5f;
}

// virtual
float DetailedMotionState::getObjectFriction() const {
    return 0.5f;
}

// virtual
float DetailedMotionState::getObjectLinearDamping() const {
    return 0.5f;
}

// virtual
float DetailedMotionState::getObjectAngularDamping() const {
    return 0.5f;
}

// The joints are known in world coordinates; riding a frame, the body lives in the frame's world and the
// pose goes in through the frame's transform.
// virtual
glm::vec3 DetailedMotionState::getObjectPosition() const {
    glm::vec3 position;
    if (_otherAvatar != nullptr && _otherAvatar->getBodyLOD() == OtherAvatar::BodyLOD::Sphere) {
        position = _avatar->getFitBounds().calcCenter();
    } else {
        position = _avatar->getJointPosition(_jointIndex);
    }
    Transform frameTransform;
    if (ObjectMotionState::getFrameTransform(_avatar, frameTransform)) {
        return glm::inverse(frameTransform.getRotation()) * (position - frameTransform.getTranslation());
    }
    return position;
}

// virtual
glm::quat DetailedMotionState::getObjectRotation() const {
    glm::quat rotation = _avatar->getWorldOrientation() * _avatar->getAbsoluteJointRotationInObjectFrame(_jointIndex);
    Transform frameTransform;
    if (ObjectMotionState::getFrameTransform(_avatar, frameTransform)) {
        return glm::inverse(frameTransform.getRotation()) * rotation;
    }
    return rotation;
}

// virtual
QUuid DetailedMotionState::getFrameID() const {
    SpatiallyNestablePointer frame = ObjectMotionState::findFrameAncestor(_avatar);
    return frame ? frame->getID() : QUuid();
}

// virtual
glm::vec3 DetailedMotionState::getObjectLinearVelocity() const {
    return glm::vec3(0.0f);
}

// virtual
glm::vec3 DetailedMotionState::getObjectAngularVelocity() const {
    return glm::vec3(0.0f);
}

// virtual
glm::vec3 DetailedMotionState::getObjectGravity() const {
    return glm::vec3(0.0f);
}

// virtual
const QUuid DetailedMotionState::getObjectID() const {
    return _avatar->getSessionUUID();
}

QString DetailedMotionState::getName() const {
    return _avatar->getName() + "_" + QString::number(_jointIndex);
}

// virtual
QUuid DetailedMotionState::getSimulatorID() const {
    return _avatar->getSessionUUID();
}

// virtual
void DetailedMotionState::computeCollisionGroupAndMask(int32_t& group, int32_t& mask) const {
    group = BULLET_COLLISION_GROUP_DETAILED_AVATAR;
    mask = Physics::getDefaultCollisionMask(group);
}

// virtual
float DetailedMotionState::getMass() const {
    return 0.0f;
}

void DetailedMotionState::setShape(const JPH::Shape* shape) {
    if (_shape != shape) {
        if (_shape) {
            getShapeManager()->releaseShape(_shape);
        }
        _shape = shape;
        if (_body) {
            assert(_shape);
            bodyInterface()->SetShape(_body->GetID(), _shape, false, JPH::EActivation::DontActivate);
        }
    } else if (shape) {
        // we need to release unused reference to shape
        getShapeManager()->releaseShape(shape);
    }
}

void DetailedMotionState::forceActive() {
    if (_body && !_body->IsActive()) {
        activateBody();
    }
}
