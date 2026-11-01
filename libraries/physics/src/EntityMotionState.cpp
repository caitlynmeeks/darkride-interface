//
//  EntityMotionState.cpp
//  libraries/entities/src
//
//  Created by Andrew Meadows on 2014.11.06
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: these functions: pullTransformFromBody (the presented pose and a vehicle's correction as it decays)
//   and setCorrection (and getCorrectionDistance in EntityMotionState.h), each with everything it calls on the
//   vehicle's path.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//

#include "EntityMotionState.h"

#include <glm/gtx/norm.hpp>

#include <EntityItem.h>
#include <EntityTree.h>
#include <EntityItemProperties.h>
#include <EntityEditPacketSender.h>
#include <LogHandler.h>
#include <PhysicsCollisionGroups.h>
#include <Profile.h>

#include "JoltUtil.h"
#include "PhysicsEngine.h"
#include "PhysicsHelpers.h"
#include "PhysicsLogging.h"
#include "TrackSimulation.h"
#include "VehicleSimulation.h"

const uint8_t LOOPS_FOR_SIMULATION_ORPHAN = 50;
const quint64 USECS_BETWEEN_OWNERSHIP_BIDS = USECS_PER_SECOND / 15;
const int MAX_NESTING_DEPTH = 16;

// A child of something that moves has to move with it in the simulation, as a kinematic body with a velocity,
// so that it carries and pushes whatever it touches: the walls of a moving cabin, the seat of a car. Before this
// such children were static bodies that stayed behind when their parent moved under them.
// The walk stops at a moving reference frame: the frame moves in its parent's world, not in its own, so its
// contents hold still there.
static bool hasMovingEntityAncestor(const EntityItemPointer& entity) {
    bool success = false;
    SpatiallyNestablePointer parent = entity->getParentPointer(success);
    int depth = 0;
    while (success && parent && depth < MAX_NESTING_DEPTH) {
        if (parent->getNestableType() == NestableType::Entity) {
            EntityItemPointer parentEntity = std::static_pointer_cast<EntityItem>(parent);
            if (parentEntity->isFrame()) {
                return false;
            }
            if (parentEntity->getDynamic() || parentEntity->hasActions() || parentEntity->isMovingRelativeToParent()) {
                return true;
            }
        } else if (parent->getNestableType() == NestableType::Avatar) {
            return true;
        }
        parent = parent->getParentPointer(success);
        ++depth;
    }
    return false;
}

static bool parentIsFrame(const EntityItemPointer& entity) {
    bool success = false;
    SpatiallyNestablePointer parent = entity->getParentPointer(success);
    return success && parent && parent->getNestableType() == NestableType::Entity &&
        std::static_pointer_cast<EntityItem>(parent)->isFrame();
}


EntityMotionState::EntityMotionState(const JPH::Shape* shape, EntityItemPointer entity) :
    ObjectMotionState(nullptr),
    _entity(entity),
    _serverPosition(0.0f),
    _serverRotation(),
    _serverVelocity(0.0f),
    _serverAngularVelocity(0.0f),
    _serverGravity(0.0f),
    _serverAcceleration(0.0f),
    _serverActionData(QByteArray()),
    _lastVelocity(0.0f),
    _measuredAcceleration(0.0f),
    _nextBidExpiry(0),
    _measuredDeltaTime(0.0f),
    _lastMeasureStep(0),
    _lastStep(0),
    _loopsWithoutOwner(0),
    _accelerationNearlyGravityCount(0),
    _numInactiveUpdates(1)
{
    // Why is _numInactiveUpdates initialied to 1?
    // Because: when an entity is first created by a LOCAL operatioin its local simulation ownership is assumed,
    // which causes it to be immediately placed on the 'owned' list, but in this case an "update" already just
    // went out for the object's creation and there is no need to send another.  By initializing _numInactiveUpdates
    // to 1 here we trick remoteSimulationOutOfSync() to return "false" the first time through for this case.

    _type = MOTIONSTATE_TYPE_ENTITY;
    assert(_entity);
    // we need the side-effects of EntityMotionState::setShape() so we call it explicitly here
    // rather than pass the legit shape pointer to the ObjectMotionState ctor above.
    setShape(shape);
    // after the shape: the mass is kept as a density over the shape's bounds, and without a shape it was lost (1 kg/m^3)
    setMass(_entity->computeMass());

    _trackDriven = !_entity->getTrackID().isNull();
    _vehicle = _entity->isVehicle();

    if (_entity->isAvatarEntity() && !_entity->isMyAvatarEntity()) {
        // avatar entities are always thus, so we cache this fact in _ownershipState
        _ownershipState = EntityMotionState::OwnershipState::Unownable;
    }

    Transform localTransform;
    _entity->getLocalTransformAndVelocities(localTransform, _serverVelocity, _serverAngularVelocity);
    _serverPosition = localTransform.getTranslation();
    _serverRotation = localTransform.getRotation();
    _serverAcceleration = _entity->getAcceleration();
    _serverActionData = _entity->getDynamicData();
}

EntityMotionState::~EntityMotionState() {
    if (_entity) {
        assert(_entity->getPhysicsInfo() == this);
        _entity->setPhysicsInfo(nullptr);
        _entity.reset();
    }
}

void EntityMotionState::updateServerPhysicsVariables() {
    // only slam these values if we are NOT the simulation owner
    Transform localTransform;
    _entity->getLocalTransformAndVelocities(localTransform, _serverVelocity, _serverAngularVelocity);
    _serverPosition = localTransform.getTranslation();
    _serverRotation = localTransform.getRotation();
    _serverAcceleration = _entity->getAcceleration();
    _serverActionData = _entity->getDynamicData();
    _lastStep = ObjectMotionState::getWorldSimulationStep();
}

void EntityMotionState::handleDeactivation() {
    if (_entity->getDirtyFlags() & (Simulation::DIRTY_TRANSFORM | Simulation::DIRTY_VELOCITIES)) {
        // Some non-physical event (script-call or network-packet) has modified the entity's transform and/or
        // velocities at the last minute before deactivation --> the values stored in _server* and _body are stale.
        // We assume the EntityMotionState is the last to know, so we copy from EntityItem to _server* variables
        // here but don't clear the flags --> the will body be set straight before next simulation step.
        updateServerPhysicsVariables();
    } else if ((_body->IsStatic() || _body->IsKinematic()) && _ownershipState != EntityMotionState::OwnershipState::LocallyOwned) {
            // To allow the ESS to move entities around in a kinematic way we had to remove the requirement that
            // every moving+simulated entity has an authoritative simulation owner.  As a result, we cannot rely
            // on a final authoritative update of kinmatic objects prior to deactivation in the local simulation.
            // For this case (unowned kinematic objects) we update the _server* variables for good measure but
            // leave the entity and body alone. They should have been updated correctly in the last call to
            // EntityMotionState::getWorldTransform().
            updateServerPhysicsVariables();
    } else {
        // copy _server data to entity
        Transform localTransform = _entity->getLocalTransform();
        localTransform.setTranslation(_serverPosition);
        localTransform.setRotation(_serverRotation);
        _entity->setLocalTransformAndVelocities(localTransform, ENTITY_ITEM_ZERO_VEC3, ENTITY_ITEM_ZERO_VEC3);
        // and also to the body
        setBodyTransform(getObjectPosition(), getObjectRotation(), false);
        // no need to update velocities... should already be zero
    }
    if (!isLocallyOwned()) {
        // HACK: To allow the ESS to move entities around in a kinematic way we had to remove the requirement that
        // every moving+simulated entity has an authoritative simulation owner.  As a result, we cannot rely
        // on a simulation owner to update the QueryAACube on the entity-server.
        _entity->updateQueryAACube();
    }
}

// virtual
void EntityMotionState::handleEasyChanges(uint32_t& flags) {
    if (_ownershipState != EntityMotionState::OwnershipState::LocallyOwned) {
        updateServerPhysicsVariables();
    }
    ObjectMotionState::handleEasyChanges(flags);

    if (flags & Simulation::DIRTY_SIMULATOR_ID) {
        if (serverTakes()) {
            qCInfo(physics) << "physics-server: car" << _entity->getName() << _entity->getID() << "owner now"
                << _entity->getSimulatorID() << "at priority" << (int)_entity->getSimulationPriority()
                << (isLocallyOwned() ? "(this server)" : "");
        }
        if (_entity->getSimulatorID().isNull()) {
            // simulation ownership has been removed
            if (glm::length2(_entity->getWorldVelocity()) == 0.0f) {
            // TODO: also check angularVelocity
                // this object is coming to rest
                flags &= ~Simulation::DIRTY_PHYSICS_ACTIVATION;
                // the engine puts it to sleep on its own once it is slow for long enough
            } else {
                // disowned object is still moving --> start timer for ownership bid
                // TODO? put a delay in here proportional to distance from object?
                _bumpedPriority = glm::max(_bumpedPriority, VOLUNTEER_SIMULATION_PRIORITY);
                _nextBidExpiry = usecTimestampNow() + USECS_BETWEEN_OWNERSHIP_BIDS;
            }
            _loopsWithoutOwner = 0;
            _numInactiveUpdates = 0;
        } else if (!isLocallyOwned()) {
            // the entity is owned by someone else
            _nextBidExpiry = usecTimestampNow() + USECS_BETWEEN_OWNERSHIP_BIDS;
            _numInactiveUpdates = 0;
        }
    }
    if (flags & Simulation::DIRTY_SIMULATION_OWNERSHIP_PRIORITY) {
        // The DIRTY_SIMULATOR_OWNERSHIP_PRIORITY bit means one of the following:
        // (1) we own it but may need to change the priority OR...
        // (2) we don't own it but should bid (because a local script has been changing physics properties)
        // reset bid expiry so that we bid ASAP
        _nextBidExpiry = 0;
    }
    if ((flags & Simulation::DIRTY_PHYSICS_ACTIVATION) && !_body->IsActive()) {
        if (_body->IsKinematic()) {
            _lastKinematicStep = ObjectMotionState::getWorldSimulationStep();
        }
        activateBody();
    }
}


PhysicsMotionType EntityMotionState::computePhysicsMotionType() const {
    if (!_entity) {
        return MOTION_TYPE_STATIC;
    }

    if (_entity->getLocked()) {
        if (_entity->isMoving()) {
            return MOTION_TYPE_KINEMATIC;
        }
        return MOTION_TYPE_STATIC;
    }

    _trackDriven = !_entity->getTrackID().isNull();
    _vehicle = _entity->isVehicle();
    if (_trackDriven) {
        // a car on a track: moved along the curve by its owner, and by the owner's velocities everywhere else
        return MOTION_TYPE_KINEMATIC;
    }

    if (_entity->getDynamic()) {
        if (!_entity->getParentID().isNull() && !parentIsFrame(_entity)) {
            // if something would have been dynamic but is a child of something else, force it to be kinematic, instead.
            // (A child of a frame is the exception: it is dynamic in the frame's own world.)
            return MOTION_TYPE_KINEMATIC;
        }
        return MOTION_TYPE_DYNAMIC;
    }
    if (_entity->hasActions() ||
        _entity->isMovingRelativeToParent() ||
        _entity->hasAncestorOfType(NestableType::Avatar) ||
        hasMovingEntityAncestor(_entity)) {
        return MOTION_TYPE_KINEMATIC;
    }
    return MOTION_TYPE_STATIC;
}

int EntityMotionState::getNestingDepth() const {
    int depth = 0;
    bool success = false;
    SpatiallyNestablePointer parent = _entity ? _entity->getParentPointer(success) : nullptr;
    while (success && parent && depth < MAX_NESTING_DEPTH) {
        ++depth;
        parent = parent->getParentPointer(success);
    }
    return depth;
}

bool EntityMotionState::isMoving() const {
    return _entity && _entity->isMovingRelativeToParent();
}

QUuid EntityMotionState::getFrameID() const {
    SpatiallyNestablePointer frame = ObjectMotionState::findFrameAncestor(_entity);
    return frame ? frame->getID() : QUuid();
}

bool EntityMotionState::isFrame() const {
    return _entity && _entity->isFrame();
}

// The box, in the frame's coordinates, inside which something belongs to the frame: the one the entity names
// in its userData, else its own bounds with room above for whoever stands on it.
bool EntityMotionState::getFrameVolume(glm::vec3& minCorner, glm::vec3& maxCorner) const {
    if (!_entity || !_entity->isFrame()) {
        return false;
    }
    if (_entity->getFrameVolume(minCorner, maxCorner)) {
        return true;
    }
    // the entity's own box with headroom, and no margin at the sides: membership keeps a rider standing on the floor
    // in past the edge (PhysicsEngine::frameContaining), and one who has stepped past it should leave at the edge
    // itself, where the platform or the floor's twin in the world holds them up at the same height
    const glm::vec3 HEADROOM(0.0f, 3.0f, 0.0f);
    glm::vec3 dimensions = _entity->getScaledDimensions();
    glm::vec3 registration = _entity->getRegistrationPoint();
    minCorner = -registration * dimensions;
    maxCorner = (glm::vec3(1.0f) - registration) * dimensions + HEADROOM;
    return true;
}

// A nested frame's own record is in its frame ancestor's coordinates (getObject*); the domain transform composes
// that with the ancestor's, which composes with its own, up to the domain. An ancestor without a motion state here
// (not yet in the physics) leaves the entity's world transform as the best there is.
bool EntityMotionState::getFrameTransformInDomain(glm::vec3& position, glm::quat& rotation, glm::vec3& linearVelocity, glm::vec3& angularVelocity, bool asSimulated) const {
    if (!_entity || !_entity->isFrame()) {
        return false;
    }
    if (asSimulated && _body && !_body->IsStatic()) {
        // inside the step the frame is where its body is; its record presents a substep behind
        getBodyTransform(position, rotation);
    } else {
        position = getObjectPosition();
        rotation = getObjectRotation();
    }
    linearVelocity = getObjectLinearVelocity();
    angularVelocity = getObjectAngularVelocity();
    SpatiallyNestablePointer ancestor = ObjectMotionState::findFrameAncestor(_entity);
    if (!ancestor) {
        return true;
    }
    EntityMotionState* ancestorState = static_cast<EntityMotionState*>(std::static_pointer_cast<EntityItem>(ancestor)->getPhysicsInfo());
    glm::vec3 ancestorPosition, ancestorLinear, ancestorAngular;
    glm::quat ancestorRotation;
    if (ancestorState && ancestorState->getFrameTransformInDomain(ancestorPosition, ancestorRotation, ancestorLinear, ancestorAngular, asSimulated)) {
        glm::vec3 offset = ancestorRotation * position;
        linearVelocity = ancestorLinear + glm::cross(ancestorAngular, offset) + ancestorRotation * linearVelocity;
        angularVelocity = ancestorAngular + ancestorRotation * angularVelocity;
        position = ancestorPosition + offset;
        rotation = glm::normalize(ancestorRotation * rotation);
        return true;
    }
    position = _entity->getWorldPosition() - ObjectMotionState::getWorldOffset();
    rotation = _entity->getWorldOrientation();
    linearVelocity = _entity->getWorldVelocity();
    angularVelocity = _entity->getWorldAngularVelocity();
    return true;
}

int EntityMotionState::getFrameDepth() const {
    int depth = 0;
    SpatiallyNestablePointer ancestor = ObjectMotionState::findFrameAncestor(_entity);
    while (ancestor && depth < 16) {
        ++depth;
        ancestor = ObjectMotionState::findFrameAncestor(ancestor);
    }
    return depth;
}

bool EntityMotionState::getParentTransformInFrame(Transform& transform, glm::vec3& linearVelocity, glm::vec3& angularVelocity, bool asSimulated) const {
    return ObjectMotionState::getParentTransformInFrame(_entity, transform, linearVelocity, angularVelocity, asSimulated);
}

void EntityMotionState::bodyPoseAsLocal(Transform& local) const {
    glm::vec3 position;
    glm::quat rotation;
    getBodyTransform(position, rotation);
    local = _entity->getLocalTransform();   // keeps the scale
    Transform parentInFrame;
    glm::vec3 v, w;
    Transform body;
    body.setTranslation(position);
    body.setRotation(rotation);
    if (getParentTransformInFrame(parentInFrame, v, w, true)) {
        // the body is in its frame's coordinates
        Transform relative;
        Transform::inverseMult(relative, parentInFrame, body);
        local.setTranslation(relative.getTranslation());
        local.setRotation(relative.getRotation());
        return;
    }
    // the body is in the domain, less the offset; the record is relative to the parent, if any
    body.setTranslation(position + ObjectMotionState::getWorldOffset());
    bool success = false;
    Transform parent = _entity->getParentTransform(success);
    if (success && !_entity->getParentID().isNull()) {
        Transform relative;
        Transform::inverseMult(relative, parent, body);
        local.setTranslation(relative.getTranslation());
        local.setRotation(relative.getRotation());
    } else {
        local.setTranslation(body.getTranslation());
        local.setRotation(body.getRotation());
    }
}

void EntityMotionState::localToBodyPose(const Transform& local, glm::vec3& position, glm::quat& rotation) const {
    Transform parentInFrame;
    glm::vec3 v, w;
    if (getParentTransformInFrame(parentInFrame, v, w, true)) {
        position = parentInFrame.transform(local.getTranslation());
        rotation = glm::normalize(parentInFrame.getRotation() * local.getRotation());
        return;
    }
    bool success = false;
    Transform parent = _entity->getParentTransform(success);
    if (success && !_entity->getParentID().isNull()) {
        position = parent.transform(local.getTranslation());
        rotation = glm::normalize(parent.getRotation() * local.getRotation());
    } else {
        position = local.getTranslation();
        rotation = local.getRotation();
    }
    position -= ObjectMotionState::getWorldOffset();
}

glm::vec3 EntityMotionState::getObjectPosition() const {
    Transform parentInFrame;
    glm::vec3 v, w;
    if (getParentTransformInFrame(parentInFrame, v, w)) {
        return parentInFrame.transform(_entity->getLocalPosition());
    }
    return _entity->getWorldPosition() - ObjectMotionState::getWorldOffset();
}

glm::quat EntityMotionState::getObjectRotation() const {
    Transform parentInFrame;
    glm::vec3 v, w;
    if (getParentTransformInFrame(parentInFrame, v, w)) {
        return parentInFrame.getRotation() * _entity->getLocalOrientation();
    }
    return _entity->getWorldOrientation();
}

glm::vec3 EntityMotionState::getObjectLinearVelocity() const {
    Transform parentInFrame;
    glm::vec3 v, w;
    if (getParentTransformInFrame(parentInFrame, v, w)) {
        return v + parentInFrame.getRotation() * _entity->getLocalVelocity();
    }
    return _entity->getWorldVelocity();
}

glm::vec3 EntityMotionState::getObjectAngularVelocity() const {
    Transform parentInFrame;
    glm::vec3 v, w;
    if (getParentTransformInFrame(parentInFrame, v, w)) {
        return w + parentInFrame.getRotation() * _entity->getLocalAngularVelocity();
    }
    return _entity->getWorldAngularVelocity();
}

// Called by the engine before every substep. Kinematic entities integrate their keyframed motion here and
// move their body toward the result over the substep, so whatever they touch is pushed properly.
void EntityMotionState::pushTransformToBody(float dt) {
    if (!_entity || !_body || _motionType != MOTION_TYPE_KINEMATIC) {
        return;
    }
    DETAILED_PROFILE_RANGE(simulation_physics, "kinematicIntegration");
    uint32_t thisStep = ObjectMotionState::getWorldSimulationStep();
    if (hasInternalKinematicChanges()) {
        // ACTION_CAN_CONTROL_KINEMATIC_OBJECT_HACK: This kinematic body was moved by an Action
        // and doesn't require transform update because the body is authoritative and its transform
        // has already been copied out --> do no kinematic integration.
        clearInternalKinematicChanges();
        _lastKinematicStep = thisStep;
        return;
    }
    bool driven = false;
    if (_trackDriven && _tracks) {
        // a car on a track is driven along the curve by the TrackSimulation (PHYSICS.md 4.8 and round 5): on the physics
        // server, on every client while the track carries the server's train state, and otherwise by the client that
        // owns the car or has the world to itself (serverless). The record is set to the train's pose at this substep
        // and read ahead from there as any keyframed entity's is; while it is driven here the network's transform for
        // it is not applied (EntityItem::setTransformDrivenLocally).
        glm::vec3 position, linearVelocity, angularVelocity;
        glm::quat rotation;
        driven = _tracks->drive(_entity, thisStep, isLocallyOwned() || isServerlessMode(), position, rotation,
                                linearVelocity, angularVelocity);
        _entity->setTransformDrivenLocally(driven);
        if (driven) {
            // the train's pose at this substep is the body's; the record presents it after the step
            Transform local = _entity->getLocalTransform();
            local.setTranslation(position);
            local.setRotation(rotation);
            _entity->setAcceleration(Vectors::ZERO);
            _entity->setLocalVelocity(linearVelocity);
            _entity->setLocalAngularVelocity(angularVelocity);
            _lastKinematicStep = thisStep;
            glm::vec3 bodyPosition;
            glm::quat bodyRotation;
            localToBodyPose(local, bodyPosition, bodyRotation);
            bodyInterface()->MoveKinematic(_body->GetID(), glmToJoltR(bodyPosition), glmToJolt(bodyRotation), dt);
        }
    }
    if (!driven) {
        // Physical kinematic motion, stepped strictly by the substep count, with full gravity for acceleration.
        // The body is the simulated state: its pose (where the last substep left it) is run on with the record's
        // velocities, and the velocities go back to the record; the record's own pose is the presentation, set
        // after the step (pullTransformFromBody), a substep behind.
        _entity->setAcceleration(_entity->getGravity());
        float stepDt = (thisStep - _lastKinematicStep) * PHYSICS_ENGINE_FIXED_SUBSTEP;
        _lastKinematicStep = thisStep;
        Transform local;
        bodyPoseAsLocal(local);
        glm::vec3 linearVelocity = _entity->getLocalVelocity();
        glm::vec3 angularVelocity = _entity->getLocalAngularVelocity();
        if (stepDt > 0.0f) {
            _entity->integrateKinematicMotion(local, linearVelocity, angularVelocity, stepDt);
            _entity->setLocalVelocity(linearVelocity);
            _entity->setLocalAngularVelocity(angularVelocity);
        }
        glm::vec3 bodyPosition;
        glm::quat bodyRotation;
        localToBodyPose(local, bodyPosition, bodyRotation);
        bodyInterface()->MoveKinematic(_body->GetID(), glmToJoltR(bodyPosition), glmToJolt(bodyRotation), dt);
    }

    // and set the acceleration-matches-gravity count high so that if we send an update
    // it will use the correct acceleration for remote simulations
    _accelerationNearlyGravityCount = (uint8_t)(-1);

    // a child that is still a static body has to become kinematic now that its parent moves (not so for a
    // frame: its children live in its own world, where it does not move)
    if (_entity->isFrame()) {
        return;
    }
    EntityTreePointer tree = _entity->getTree();
    _entity->forEachChild([&](SpatiallyNestablePointer child) {
        if (child->getNestableType() == NestableType::Entity) {
            EntityItemPointer childEntity = std::static_pointer_cast<EntityItem>(child);
            EntityMotionState* childState = static_cast<EntityMotionState*>(childEntity->getPhysicsInfo());
            if (childState && childState->getBody() && childState->getBody()->IsStatic()) {
                childEntity->flagForMotionStateChange();
                if (tree) {
                    tree->entityChanged(childEntity);
                }
            }
        }
    });
}

// FROZEN (vehicle path): see the note at the top of this file.
// Called by the engine after a step for every body that was active: the body's result goes to the entity.
void EntityMotionState::pullTransformFromBody() {
    if (!_entity || !_body) {
        return;
    }
    measureBodyAcceleration();

    // The body is at the last substep; the record presents it blended between the start and the end of that
    // substep (one substep behind the present), so what is rendered and sent moves evenly at any display rate.
    // A keyframed body's velocity is the record's own and is never written back (below).
    glm::vec3 position;
    glm::quat rotation;
    getPresentedBodyTransform(position, rotation);
    const quint64 presentedNow = usecTimestampNow() - (quint64)(ObjectMotionState::getPresentationLag() * (float)USECS_PER_SECOND);
    if (_correctionBlendTime > 0.0f) {
        // a vehicle's correction (PHYSICS.md 4.6): the jump is shown as a residual that decays, never a snap
        uint64_t now = usecTimestampNow();
        float dt = _correctionTime ? (float)(now - _correctionTime) * 1.0e-6f : 0.0f;
        _correctionTime = now;
        float keep = expf(-dt / _correctionBlendTime);
        _correctionOffset *= keep;
        _correctionRotation = glm::normalize(glm::slerp(glm::quat(), _correctionRotation, keep));
        const float DONE_DISTANCE = 0.001f;   // a millimetre
        const float DONE_DOT = 0.9999999f;
        if (glm::length2(_correctionOffset) < DONE_DISTANCE * DONE_DISTANCE && fabsf(_correctionRotation.w) > DONE_DOT) {
            _correctionBlendTime = 0.0f;
        } else {
            position += _correctionOffset;
            rotation = glm::normalize(_correctionRotation * rotation);
        }
    }

    Transform parentInFrame;
    glm::vec3 parentVelocity, parentAngularVelocity;
    if (getParentTransformInFrame(parentInFrame, parentVelocity, parentAngularVelocity)) {
        // the body is in its frame's coordinates; the entity keeps its pose relative to its parent
        Transform bodyInFrame;
        bodyInFrame.setTranslation(position);
        bodyInFrame.setRotation(rotation);
        Transform relative;
        Transform::inverseMult(relative, parentInFrame, bodyInFrame);
        Transform local = _entity->getLocalTransform();
        local.setTranslation(relative.getTranslation());
        local.setRotation(relative.getRotation());
        glm::quat toParent = glm::inverse(parentInFrame.getRotation());

        uint32_t flags = _entity->getDirtyFlags() & (Simulation::DIRTY_TRANSFORM | Simulation::DIRTY_VELOCITIES);
        glm::vec3 linearVelocity = _entity->getLocalVelocity();
        glm::vec3 angularVelocity = _entity->getLocalAngularVelocity();
        if (!_body->IsKinematic()) {
            // (a keyframed body's velocity is never written back, see below)
            if (!(flags & Simulation::DIRTY_LINEAR_VELOCITY)) {
                linearVelocity = toParent * (getBodyLinearVelocity() - parentVelocity);
            }
            if (!(flags & Simulation::DIRTY_ANGULAR_VELOCITY)) {
                angularVelocity = toParent * (getBodyAngularVelocity() - parentAngularVelocity);
            }
        }
        if (flags & Simulation::DIRTY_TRANSFORM) {
            local = _entity->getLocalTransform();
        }
        _entity->setLocalTransformAndVelocities(local, linearVelocity, angularVelocity);
        _entity->setLastSimulated(presentedNow);
        return;
    }
    position += ObjectMotionState::getWorldOffset();

    if (_body->IsKinematic()) {
        // A keyframed body moved by an action: the body is authoritative, only its transform is taken; the
        // entity's own velocity stays the truth and the body's must not be written back.
        uint32_t kinematicFlags = _entity->getDirtyFlags() & Simulation::DIRTY_TRANSFORM;
        if (!kinematicFlags) {
            _entity->setWorldTransform(position, rotation);
        }
        _entity->setLastSimulated(presentedNow);
        return;
    }

    // If transform or velocities are flagged as dirty it means a network or scripted change
    // occured between the beginning and end of the stepSimulation() and we DON'T want to apply
    // these physics simulation results.
    uint32_t flags = _entity->getDirtyFlags() & (Simulation::DIRTY_TRANSFORM | Simulation::DIRTY_VELOCITIES);
    if (!flags) {
        // flags are clear
        _entity->setWorldTransform(position, rotation);
        _entity->setWorldVelocity(getBodyLinearVelocity());
        _entity->setWorldAngularVelocity(getBodyAngularVelocity());
        _entity->setLastSimulated(presentedNow);
    } else {
        // only set properties NOT flagged
        if (!(flags & Simulation::DIRTY_TRANSFORM)) {
            _entity->setWorldTransform(position, rotation);
        }
        if (!(flags & Simulation::DIRTY_LINEAR_VELOCITY)) {
            _entity->setWorldVelocity(getBodyLinearVelocity());
        }
        if (!(flags & Simulation::DIRTY_ANGULAR_VELOCITY)) {
            _entity->setWorldAngularVelocity(getBodyAngularVelocity());
        }
        if (flags != (Simulation::DIRTY_TRANSFORM | Simulation::DIRTY_VELOCITIES)) {
            _entity->setLastSimulated(presentedNow);
        }
    }

    if (_entity->getSimulatorID().isNull()) {
        _loopsWithoutOwner++;
        if (_loopsWithoutOwner > LOOPS_FOR_SIMULATION_ORPHAN && usecTimestampNow() > _nextBidExpiry) {
            _bumpedPriority = glm::max(_bumpedPriority, VOLUNTEER_SIMULATION_PRIORITY);
        }
    }
}


const uint8_t MAX_NUM_INACTIVE_UPDATES = 20;

bool EntityMotionState::remoteSimulationOutOfSync(uint32_t simulationStep) {
    // NOTE: this method is only ever called when the entity simulation is locally owned
    DETAILED_PROFILE_RANGE(simulation_physics, "CheckOutOfSync");

    // Since we own the simulation: make sure _bidPriority is not less than current owned priority
    // because: a _bidPriority of zero indicates that we should drop ownership in the send.
    // TODO: need to be able to detect when logic dictates we *decrease* priority
    // WIP: print info whenever _bidPriority mismatches what is known to the entity

    int numSteps = simulationStep - _lastStep;
    float dt = (float)(numSteps) * PHYSICS_ENGINE_FIXED_SUBSTEP;

    if (_vehicle) {
        bool handled = false;
        bool out = vehicleOutOfSync(simulationStep, handled);
        if (handled) {
            return out;
        }
    }

    if (_trackDriven) {
        // a car on a track is never "inactive" to its owner: it stands at a station between runs and its owner (the
        // physics server, or a client when there is none) keeps it, sending an update before the ownership would
        // expire, and otherwise when the others' dead reckoning would be out (PHYSICS.md rounds 4 and 5)
        // (with half the period in hand: the entity server expires an owner a second after this expiry, and one
        // update in that second lost or late handed the physics server's car back)
        const uint64_t KEEP_ALIVE_MARGIN = MAX_OUTGOING_SIMULATION_UPDATE_PERIOD / 2;
        if (usecTimestampNow() + KEEP_ALIVE_MARGIN > _entity->getSimulationOwnershipExpiry()) {
            return true;
        }
        return trackCarOutOfSync(simulationStep);
    }

    if (_numInactiveUpdates > 0) {
        if (_numInactiveUpdates > MAX_NUM_INACTIVE_UPDATES) {
            // clear local ownership (stop sending updates) and let the server clear itself
            _entity->clearSimulationOwnership();
            return false;
        }
        // we resend the inactive update with a growing delay: every INACTIVE_UPDATE_PERIOD * _numInactiveUpdates
        // until it is removed from the owned list
        // (which happens when we no longer own the simulation)
        const float INACTIVE_UPDATE_PERIOD = 0.5f;
        return (dt > INACTIVE_UPDATE_PERIOD * (float)_numInactiveUpdates);
    }

    if (!_body->IsActive()) {
        // object has gone inactive but our last send was moving --> send non-moving update immediately
        return true;
    }

    if (usecTimestampNow() > _entity->getSimulationOwnershipExpiry()) {
        // send update every so often else server will revoke our ownership
        return true;
    }

    if (_body->IsStatic() || _body->IsKinematic()) {
        return false;
    }

    _lastStep = simulationStep;
    if (glm::length2(_serverVelocity) > 0.0f) {
        // the entity-server doesn't know where avatars are, so it doesn't do simple extrapolation for children of
        // avatars.  We are trying to guess what values the entity server has, so we don't do it here, either.  See
        // related code in EntitySimulation::moveSimpleKinematics.
        bool ancestryIsKnown;
        _entity->getMaximumAACube(ancestryIsKnown);
        bool hasAvatarAncestor = _entity->hasAncestorOfType(NestableType::Avatar);

        if (ancestryIsKnown && !hasAvatarAncestor) {
            _serverVelocity += _serverAcceleration * dt;
            _serverVelocity *= powf(1.0f - _entity->getDamping(), dt);

            // NOTE: we ignore the second-order acceleration term when integrating
            // the position forward because Bullet also does this.
            _serverPosition += dt * _serverVelocity;
        }
    }

    // Else we measure the error between current and extrapolated transform (according to expected behavior
    // of remote EntitySimulation) and return true if the error is significant.

    // NOTE: math is done in the simulation-frame, which is NOT necessarily the same as the world-frame
    // due to _worldOffset.
    // TODO: compensate for _worldOffset offset here

    // compute position error
    bool parentTransformSuccess;
    Transform localToWorld = _entity->getParentTransform(parentTransformSuccess);
    Transform worldToLocal;
    if (parentTransformSuccess) {
        localToWorld.evalInverse(worldToLocal);
    }

    glm::vec3 bodyPosition;
    glm::quat bodyRotation;
    getBodyTransform(bodyPosition, bodyRotation);
    glm::vec3 position = worldToLocal.transform(bodyPosition + ObjectMotionState::getWorldOffset());

    float dx2 = glm::distance2(position, _serverPosition);
    const float MAX_POSITION_ERROR_SQUARED = 0.000004f; // corresponds to 2mm
    if (dx2 > MAX_POSITION_ERROR_SQUARED) {
        // we don't mind larger position error when the object has high speed
        // so we divide by speed and check again
        float speed2 = glm::length2(_serverVelocity);
        const float MIN_ERROR_RATIO_SQUARED = 0.0025f; // corresponds to 5% error in 1 second
        const float MIN_SPEED_SQUARED = 1.0e-6f; // corresponds to 1mm/sec
        if (speed2 < MIN_SPEED_SQUARED || dx2 / speed2 > MIN_ERROR_RATIO_SQUARED) {
            return true;
        }
    }

    if (glm::length2(_serverAngularVelocity) > 0.0f) {
        // compute rotation error
        //

        // Bullet caps the effective rotation velocity inside its rotation integration step, therefore
        // we must integrate with the same algorithm and timestep in order achieve similar results.
        float attenuation = powf(1.0f - _entity->getAngularDamping(), PHYSICS_ENGINE_FIXED_SUBSTEP);
        _serverAngularVelocity *= attenuation;
        glm::quat rotation = computeBulletRotationStep(_serverAngularVelocity, PHYSICS_ENGINE_FIXED_SUBSTEP);
        for (int i = 1; i < numSteps; ++i) {
            _serverAngularVelocity *= attenuation;
            rotation = computeBulletRotationStep(_serverAngularVelocity, PHYSICS_ENGINE_FIXED_SUBSTEP) * rotation;
        }
        _serverRotation = glm::normalize(rotation * _serverRotation);
        const float MIN_ROTATION_DOT = 0.99999f; // This corresponds to about 0.5 degrees of rotation
        glm::quat actualRotation = worldToLocal.getRotation() * bodyRotation;
        return (fabsf(glm::dot(actualRotation, _serverRotation)) < MIN_ROTATION_DOT);
    }
    return false;
}

// An owned car on a track moves along the curve, and everyone else moves it straight on from the last update (as
// the entity server does), so an update goes out whenever that would be a centimetre or half a degree out, and at
// most every few substeps.
bool EntityMotionState::trackCarOutOfSync(uint32_t simulationStep) {
    // the physics server sends far fewer: every client runs the train's state itself, and the entity server's own
    // record of the car (for a client that has no state yet, and for scripts there) may lag a quarter of a metre
    bool server = Physics::isServerAuthority();
    const uint32_t MIN_STEPS_BETWEEN_UPDATES = server ? 22 : 3;   // 4 Hz on the server, 33 ms at 90 Hz on a client
    const float MAX_POSITION_ERROR_SQUARED = server ? 0.0625f : 0.0001f;   // a quarter of a metre, or a centimetre
    const float MIN_ROTATION_DOT = server ? 0.99905f : 0.9999905f;   // five degrees, or half a degree
    int numSteps = simulationStep - _lastStep;
    float dt = (float)numSteps * PHYSICS_ENGINE_FIXED_SUBSTEP;
    _lastStep = simulationStep;
    _serverPosition += dt * _serverVelocity;
    if (glm::length2(_serverAngularVelocity) > 0.0f && dt > 0.0f) {
        _serverRotation = glm::normalize(computeBulletRotationStep(_serverAngularVelocity, dt) * _serverRotation);
    }
    if (simulationStep < _lastTrackUpdateStep + MIN_STEPS_BETWEEN_UPDATES) {
        return false;
    }
    Transform local = _entity->getLocalTransform();
    bool out = glm::distance2(local.getTranslation(), _serverPosition) > MAX_POSITION_ERROR_SQUARED
        || fabsf(glm::dot(local.getRotation(), _serverRotation)) < MIN_ROTATION_DOT;
    if (out) {
        _lastTrackUpdateStep = simulationStep;
    }
    return out;
}

// A vehicle's authority publishes its state at a steady rate while the vehicle is awake (every UPDATE_STEPS substeps,
// 10 Hz), and keeps its ownership while someone sits at the wheel; a vehicle nobody drives sleeps and is released like
// any other body. `handled` says the generic rule need not run.
bool EntityMotionState::vehicleOutOfSync(uint32_t simulationStep, bool& handled) {
    handled = false;
    bool seated = !_entity->getVehicleProperties().getDriver().isNull();
    bool server = Physics::isServerAuthority();
    if (!_body->IsActive()) {
        if (seated || server) {
            handled = true;
            const uint64_t KEEP_ALIVE_MARGIN = MAX_OUTGOING_SIMULATION_UPDATE_PERIOD / 2;
            if (usecTimestampNow() + KEEP_ALIVE_MARGIN > _entity->getSimulationOwnershipExpiry()) {
                _lastVehicleUpdateStep = simulationStep;
                return true;
            }
        }
        return false;
    }
    handled = true;
    if (simulationStep >= _lastVehicleUpdateStep + (uint32_t)VehicleSimulation::relayUpdateSteps()) {
        _lastVehicleUpdateStep = simulationStep;
        return true;
    }
    return false;
}


float EntityMotionState::getMass() const {
    if (_entity && _entity->isVehicle()) {
        float mass = _entity->getVehicleProperties().getMass();
        if (mass > 0.0f) {
            return mass;
        }
    }
    return ObjectMotionState::getMass();
}

bool EntityMotionState::hasLocalVehicleControls() const {
    EntityItem::VehicleControls controls;
    return _vehicle && _entity->getLocalVehicleControls(controls);
}

bool EntityMotionState::serverTakes() const {
    return (_trackDriven || _vehicle) && Physics::isServerAuthority();
}

// FROZEN (vehicle path): see the note at the top of this file.
void EntityMotionState::setCorrection(const glm::vec3& offset, const glm::quat& rotation, float blendTime) {
    _correctionOffset = offset;
    _correctionRotation = rotation;
    _correctionBlendTime = blendTime;
    _correctionTime = usecTimestampNow();
}

bool EntityMotionState::shouldSendUpdate(uint32_t simulationStep) {
    // NOTE: this method is only ever called when the entity simulation is locally owned
    DETAILED_PROFILE_RANGE(simulation_physics, "ShouldSend");
    // NOTE: we expect _entity and _body to be valid in this context, since shouldSendUpdate() is only called
    // after doesNotNeedToSendUpdate() returns false and that call should return 'true' if _entity or _body are NULL.

    // this case is prevented by setting _ownershipState to OwnershipState::Unownable in EntityMotionState::ctor
    assert(!(_entity->isAvatarEntity() && !_entity->isMyAvatarEntity()));

    if (_entity->getTransitingWithAvatar()) {
        return false;
    }
    if (_entity->dynamicDataNeedsTransmit()) {
        return true;
    }
    if (_entity->shouldSuppressLocationEdits()) {
        // "shouldSuppressLocationEdits" really means: "the entity has a 'Hold' action therefore
        // we don't need send an update unless the entity is not contained by its queryAACube"
        return _entity->queryAACubeNeedsUpdate();
    }

    return remoteSimulationOutOfSync(simulationStep);
}

void EntityMotionState::updateSendVelocities() {
    if (!_body->IsActive()) {
        if (!_body->IsKinematic()) {
            clearObjectVelocities();
        }
        if (_trackDriven || (_vehicle && (!_entity->getVehicleProperties().getDriver().isNull() || Physics::isServerAuthority()))) {
            // a standing car is not released by its owner, nor a vehicle with a driver aboard (see remoteSimulationOutOfSync)
            _numInactiveUpdates = 0;
        } else if (_entity->getEntityHostType() == entity::HostType::AVATAR) {
            // AvatarEntities only ever need to send one update (their updates are sent over a lossless protocol)
            // so we set the count to the max to prevent resends
            _numInactiveUpdates = MAX_NUM_INACTIVE_UPDATES;
        } else {
            ++_numInactiveUpdates;
        }
    } else {
        glm::vec3 gravity = _entity->getGravity();

        // if this entity has been accelerated at close to gravity for a certain number of simulation-steps
        // let the entity server's estimates include gravity.
        const uint8_t STEPS_TO_DECIDE_BALLISTIC = 4;
        if (_accelerationNearlyGravityCount >= STEPS_TO_DECIDE_BALLISTIC) {
            _entity->setAcceleration(gravity);
        } else {
            _entity->setAcceleration(Vectors::ZERO);
        }

        if (!(_body->IsStatic() || _body->IsKinematic())) {
            const float DYNAMIC_LINEAR_VELOCITY_THRESHOLD = 0.05f;  // 5 cm/sec
            const float DYNAMIC_ANGULAR_VELOCITY_THRESHOLD = 0.087266f;  // ~5 deg/sec

            bool movingSlowlyLinear =
                glm::length2(_entity->getWorldVelocity()) < (DYNAMIC_LINEAR_VELOCITY_THRESHOLD * DYNAMIC_LINEAR_VELOCITY_THRESHOLD);
            bool movingSlowlyAngular = glm::length2(_entity->getWorldAngularVelocity()) <
                    (DYNAMIC_ANGULAR_VELOCITY_THRESHOLD * DYNAMIC_ANGULAR_VELOCITY_THRESHOLD);
            bool movingSlowly = movingSlowlyLinear && movingSlowlyAngular && _entity->getAcceleration() == Vectors::ZERO;

            if (movingSlowly) {
                // velocities might not be zero, but we'll fake them as such, which will hopefully help convince
                // other simulating observers to deactivate their own copies
                clearObjectVelocities();
            }
        }
        _numInactiveUpdates = 0;
    }
}

void EntityMotionState::sendBid(OctreeEditPacketSender* packetSender, uint32_t step) {
    DETAILED_PROFILE_RANGE(simulation_physics, "Bid");

    updateSendVelocities();

    EntityItemProperties properties;
    Transform localTransform;
    glm::vec3 linearVelocity;
    glm::vec3 angularVelocity;
    _entity->getLocalTransformAndVelocities(localTransform, linearVelocity, angularVelocity);
    properties.setPosition(localTransform.getTranslation());
    properties.setRotation(localTransform.getRotation());
    properties.setVelocity(linearVelocity);
    properties.setAcceleration(_entity->getAcceleration());
    properties.setAngularVelocity(angularVelocity);

    // we don't own the simulation for this entity yet, but we're sending a bid for it
    quint64 now = usecTimestampNow();
    uint8_t finalBidPriority = computeFinalBidPriority();
    _entity->prepareForSimulationOwnershipBid(properties, now, finalBidPriority);

    EntityTreeElementPointer element = _entity->getElement();
    EntityTreePointer tree = element ? element->getTree() : nullptr;

    EntityItemID id(_entity->getID());
    EntityEditPacketSender* entityPacketSender = static_cast<EntityEditPacketSender*>(packetSender);
    entityPacketSender->queueEditEntityMessage(PacketType::EntityPhysics, tree, id, properties);
    if (serverTakes()) {
        qCInfo(physics) << "physics-server: bid for car" << _entity->getName() << _entity->getID() << "at priority"
            << (int)finalBidPriority << "(owner" << _entity->getSimulatorID() << "at" << (int)_entity->getSimulationPriority() << ")";
    }

    // NOTE: we don't descend to children for ownership bid.  Instead, if we win ownership of the parent
    // then in sendUpdate() we'll walk descendents and send updates for their QueryAACubes if necessary.

    _lastStep = step;
    _nextBidExpiry = now + USECS_BETWEEN_OWNERSHIP_BIDS;

    // after sending a bid/update we clear _bumpedPriority
    // which might get promoted again next frame (after local script or simulation interaction)
    // or we might win the bid
    _bumpedPriority = 0;
}

void EntityMotionState::sendUpdate(OctreeEditPacketSender* packetSender, uint32_t step) {
    DETAILED_PROFILE_RANGE(simulation_physics, "Send");
    assert(isLocallyOwned());

    updateSendVelocities();

    // remember _serverFoo data for local prediction of server state
    Transform localTransform;
    _entity->getLocalTransformAndVelocities(localTransform, _serverVelocity, _serverAngularVelocity);
    _serverPosition = localTransform.getTranslation();
    _serverRotation = localTransform.getRotation();
    _serverAcceleration = _entity->getAcceleration();
    _serverActionData = _entity->getDynamicData();

    EntityItemProperties properties;
    properties.setPosition(_entity->getLocalPosition());
    properties.setRotation(_entity->getLocalOrientation());
    properties.setVelocity(_serverVelocity);
    properties.setAcceleration(_serverAcceleration);
    properties.setAngularVelocity(_serverAngularVelocity);
    if (_entity->dynamicDataNeedsTransmit()) {
        _entity->setDynamicDataNeedsTransmit(false);
        properties.setActionData(_serverActionData);
    }
    if (_vehicle && _vehicles) {
        // the applied input, its sequence, the engine and the gear travel with every update (PHYSICS.md round 6)
        _vehicles->fillState(this, properties);
    }
    if (_parentChanged) {
        // a frame crossing (round 8): the position and rotation above are already in the new parent's coordinates
        properties.setParentID(_entity->getParentID());
        properties.setParentJointIndex(_entity->getParentJointIndex());
        _parentChanged = false;
    }

    if (_entity->updateQueryAACube()) {
        // due to parenting, the server may not know where something is in world-space, so include the bounding cube.
        properties.setQueryAACube(_entity->getQueryAACube());
    }

    // set the LastEdited of the properties but NOT the entity itself
    quint64 now = usecTimestampNow();
    properties.setLastEdited(now);
    _entity->setSimulationOwnershipExpiry(now + MAX_OUTGOING_SIMULATION_UPDATE_PERIOD);

    if (_numInactiveUpdates > 0 && _entity->getScriptSimulationPriority() == 0) {
        // the entity is stopped and inactive so we tell the server we're clearing simulatorID
        // but we remember we do still own it...  and rely on the server to tell us we don't
        properties.clearSimulationOwner();
        _entity->setPendingOwnershipPriority(0);
    } else {
        uint8_t newPriority = computeFinalBidPriority();
        _entity->clearScriptSimulationPriority();
        // if we get here then we own the simulation and the object is NOT going inactive
        // if newPriority is zero, then it must be outside of R1, which means we should really set it to YIELD
        // which we achive by just setting it to the max of the two
        newPriority = glm::max(newPriority, YIELD_SIMULATION_PRIORITY);
        // the physics server says who owns a car in every update: should the entity server have expired its ownership
        // on a lost update, the next one takes the car back without waiting to hear of the loss
        bool reassert = serverTakes();
        if (reassert || (newPriority != _entity->getSimulationPriority() &&
                !(newPriority == VOLUNTEER_SIMULATION_PRIORITY && _entity->getSimulationPriority() == RECRUIT_SIMULATION_PRIORITY))) {
            // our desired priority has changed
            if (newPriority == 0) {
                // we should release ownership
                properties.clearSimulationOwner();
            } else {
                // we just need to inform the entity-server
                properties.setSimulationOwner(Physics::getSessionUUID(), newPriority);
            }
            _entity->setPendingOwnershipPriority(newPriority);
        }
    }

    EntityItemID id(_entity->getID());
    EntityEditPacketSender* entityPacketSender = static_cast<EntityEditPacketSender*>(packetSender);

    EntityTreeElementPointer element = _entity->getElement();
    EntityTreePointer tree = element ? element->getTree() : nullptr;

    properties.setEntityHostType(_entity->getEntityHostType());
    properties.setOwningAvatarID(_entity->getOwningAvatarID());

    entityPacketSender->queueEditEntityMessage(PacketType::EntityPhysics, tree, id, properties);
    _entity->setLastBroadcast(now); // for debug/physics status icons

    // if we've moved an entity with children, check/update the queryAACube of all descendents and tell the server
    // if they've changed.
    _entity->forEachDescendant([&](SpatiallyNestablePointer descendant) {
        if (descendant->getNestableType() == NestableType::Entity) {
            EntityItemPointer entityDescendant = std::static_pointer_cast<EntityItem>(descendant);
            if (descendant->updateQueryAACube()) {
                EntityItemProperties newQueryCubeProperties;
                newQueryCubeProperties.setQueryAACube(descendant->getQueryAACube());
                newQueryCubeProperties.setLastEdited(properties.getLastEdited());
                newQueryCubeProperties.setEntityHostType(entityDescendant->getEntityHostType());
                newQueryCubeProperties.setOwningAvatarID(entityDescendant->getOwningAvatarID());

                entityPacketSender->queueEditEntityMessage(PacketType::EntityPhysics, tree,
                                                           descendant->getID(), newQueryCubeProperties);
                entityDescendant->setLastBroadcast(now); // for debug/physics status icons
            }
        }
    });

    _lastStep = step;

    // after sending a bid/update we clear _bumpedPriority
    // which might get promoted again next frame (after local script or simulation interaction)
    // or we might win the bid
    _bumpedPriority = 0;
}

uint32_t EntityMotionState::getIncomingDirtyFlags() const {
    uint32_t dirtyFlags = 0;
    if (_body && _entity) {
        dirtyFlags = _entity->getDirtyFlags();

        if (dirtyFlags & Simulation::DIRTY_SIMULATOR_ID) {
            // when SIMULATOR_ID changes we must check for reinterpretation of asymmetric collision mask
            // bits for the avatar groups (e.g. MY_AVATAR vs OTHER_AVATAR)
            uint8_t entityCollisionMask = _entity->getCollisionless() ? 0 : _entity->getCollisionMask();
            if ((bool)(entityCollisionMask & USER_COLLISION_GROUP_MY_AVATAR) !=
                    (bool)(entityCollisionMask & USER_COLLISION_GROUP_OTHER_AVATAR)) {
                // bits are asymmetric --> flag for reinsertion in physics simulation
                dirtyFlags |= Simulation::DIRTY_COLLISION_GROUP;
            }
        }
        // we add DIRTY_MOTION_TYPE if the body's motion type disagrees with entity velocity settings
        bool isMoving = _entity->isMovingRelativeToParent() || hasMovingEntityAncestor(_entity);
        if (_body->IsStatic() && isMoving) {
            // TODO -- there is opportunity for an optimization here (kinematic and not moving), but it causes
            // excessive re-insertion of the body.
            dirtyFlags |= Simulation::DIRTY_MOTION_TYPE;
        }
    }
    return dirtyFlags;
}

void EntityMotionState::clearIncomingDirtyFlags(uint32_t mask) {
    if (_body && _entity) {
        _entity->clearDirtyFlags(mask);
    }
}

// virtual
uint8_t EntityMotionState::getSimulationPriority() const {
    return _entity->getSimulationPriority();
}

void EntityMotionState::workerBidPriority() {
    _bumpedPriority = glm::max(_bumpedPriority, _entity->getSimulationPriority());
}

// virtual
QUuid EntityMotionState::getSimulatorID() const {
    return _entity->getSimulatorID();
}

void EntityMotionState::bump(uint8_t priority) {
    assert(priority != 0);
    _bumpedPriority = glm::max(_bumpedPriority, --priority);
}

void EntityMotionState::resetMeasuredBodyAcceleration() {
    _lastMeasureStep = ObjectMotionState::getWorldSimulationStep();
    if (_body) {
        _lastVelocity = getBodyLinearVelocityGTSigma();
    } else {
        _lastVelocity = Vectors::ZERO;
    }
    _measuredAcceleration = Vectors::ZERO;
}

void EntityMotionState::measureBodyAcceleration() {
    DETAILED_PROFILE_RANGE(simulation_physics, "MeasureAccel");
    // try to manually measure the true acceleration of the object
    uint32_t thisStep = ObjectMotionState::getWorldSimulationStep();
    uint32_t numSubsteps = thisStep - _lastMeasureStep;
    if (numSubsteps > 0) {
        float dt = ((float)numSubsteps * PHYSICS_ENGINE_FIXED_SUBSTEP);
        float invDt = 1.0f / dt;
        _lastMeasureStep = thisStep;
        _measuredDeltaTime = dt;

        // Note: the integration equation for velocity uses damping (D):   v1 = (v0 + a * dt) * (1 - D)^dt
        // hence the equation for acceleration is: a = (v1 / (1 - D)^dt - v0) / dt
        glm::vec3 velocity = getBodyLinearVelocityGTSigma();

        const float MIN_DAMPING_FACTOR = 0.01f;
        float invDampingAttenuationFactor = 1.0f / glm::max(powf(1.0f - _entity->getDamping(), dt), MIN_DAMPING_FACTOR);
        _measuredAcceleration = (velocity * invDampingAttenuationFactor - _lastVelocity) * invDt;
        _lastVelocity = velocity;
        if (numSubsteps > PHYSICS_ENGINE_MAX_NUM_SUBSTEPS) {
            // we fall in here when _lastMeasureStep is old: the body has just become active
            _loopsWithoutOwner = 0;
            _lastStep = ObjectMotionState::getWorldSimulationStep();
            _numInactiveUpdates = 0;
        }

        glm::vec3 gravity = _entity->getGravity();
        float gravityLength = glm::length(gravity);
        float accVsGravity = glm::abs(glm::length(_measuredAcceleration) - gravityLength);
        const float ACCELERATION_EQUIVALENT_EPSILON_RATIO = 0.1f;
        if (accVsGravity < ACCELERATION_EQUIVALENT_EPSILON_RATIO * gravityLength) {
            // acceleration measured during the most recent simulation step was close to gravity.
            if (_accelerationNearlyGravityCount < (uint8_t)(-2)) {
                ++_accelerationNearlyGravityCount;
            }
        } else {
            // acceleration wasn't similar to this entities gravity, so reset the went-ballistic counter
            _accelerationNearlyGravityCount = 0;
        }
    }
}

glm::vec3 EntityMotionState::getObjectLinearVelocityChange() const {
    // This is the dampened change in linear velocity, as calculated in measureBodyAcceleration: dv = a * dt
    // It is generally only meaningful during the lifespan of collision. In particular, it is not meaningful
    // when the entity first starts moving via direct user action.
    return _measuredAcceleration * _measuredDeltaTime;
}

bool EntityMotionState::shouldBeInPhysicsSimulation() const {
    return _region < workload::Region::R3 && _entity->shouldBePhysical();
}

// virtual
void EntityMotionState::setMotionType(PhysicsMotionType motionType) {
    ObjectMotionState::setMotionType(motionType);
    resetMeasuredBodyAcceleration();
}

// virtual
QString EntityMotionState::getName() const {
    return _entity->getName();
}

// virtual
void EntityMotionState::computeCollisionGroupAndMask(int32_t& group, int32_t& mask) const {
    _entity->computeCollisionGroupAndFinalMask(group, mask);
}

bool EntityMotionState::shouldSendBid() const {
    // NOTE: this method is only ever called when the entity's simulation is NOT locally owned
    // A train needs a driver, moving or not: the physics server takes every car from whoever has it, at SERVER
    // priority; a client bids for an unowned car only while no server drives its track (PHYSICS.md round 5).
    // A vehicle (round 6) likewise; and the client at its wheel bids for it, moving or not, when no server holds it.
    bool serverTakesCar = serverTakes();
    bool serverDriven = _trackDriven && _tracks && _tracks->isServerDriven(_entity->getTrackID());
    bool unownedTrackCar = _trackDriven && _entity->getSimulatorID().isNull() && !serverDriven;
    bool driverWants = hasLocalVehicleControls();
    return (_body->IsActive() || unownedTrackCar || serverTakesCar || driverWants)
        && (_region == workload::Region::R1 || serverTakesCar)
        && _ownershipState != EntityMotionState::OwnershipState::Unownable
        && computeFinalBidPriority() >= _entity->getSimulationPriority()
        && !_entity->getLocked()
        && (!(_body->IsStatic() || _body->IsKinematic()) || _entity->stillHasMyGrab() || unownedTrackCar || serverTakesCar);
}

void EntityMotionState::setBody(JPH::Body* body, PhysicsWorld* world) {
    ObjectMotionState::setBody(body, world);
    if (_body) {
        _entity->markSpecialFlags(Simulation::SPECIAL_FLAG_IN_PHYSICS_SIMULATION);
    } else {
        _entity->clearSpecialFlags(Simulation::SPECIAL_FLAG_IN_PHYSICS_SIMULATION);
    }
}

uint8_t EntityMotionState::computeFinalBidPriority() const {
    if (serverTakes()) {
        return SERVER_SIMULATION_PRIORITY;   // the physics server's claim on a car or a vehicle outranks any client's
    }
    if (_region != workload::Region::R1) {
        return 0;
    }
    uint8_t priority = glm::max(glm::max(VOLUNTEER_SIMULATION_PRIORITY, _bumpedPriority), _entity->getScriptSimulationPriority());
    if (hasLocalVehicleControls()) {
        // the driver holds the wheel as a hand holds a grabbed thing
        priority = glm::max(priority, SCRIPT_GRAB_SIMULATION_PRIORITY);
    }
    return priority;
}

bool EntityMotionState::isLocallyOwned() const {
    return _entity->getSimulatorID() == Physics::getSessionUUID() || _entity->isMyAvatarEntity();
}

bool EntityMotionState::isStaticPriority() const {
    return (_entity->getEntityPriority() == EntityPriority::STATIC);
}

bool EntityMotionState::isLocallyOwnedOrShouldBe() const {
    // this method could also be called "shouldGenerateCollisionEventForLocalScripts()"
    // because that is the only reason it's used
    if (_entity->getSimulatorID() == Physics::getSessionUUID()) {
        return true;
    } else {
        return computeFinalBidPriority() > glm::max(VOLUNTEER_SIMULATION_PRIORITY, _entity->getSimulationPriority());
    }
}

void EntityMotionState::setRegion(uint8_t region) {
    _region = region;
}

void EntityMotionState::initForBid() {
    if (_ownershipState != EntityMotionState::OwnershipState::Unownable) {
        _ownershipState = EntityMotionState::OwnershipState::PendingBid;
    }
}

void EntityMotionState::initForOwned() {
    if (_ownershipState != EntityMotionState::OwnershipState::Unownable) {
        _ownershipState = EntityMotionState::OwnershipState::LocallyOwned;
    }
}

void EntityMotionState::clearOwnershipState() {
    if (_ownershipState != OwnershipState::Unownable) {
        _ownershipState = OwnershipState::NotLocallyOwned;
    }
}

void EntityMotionState::clearObjectVelocities() const {
    // If transform or velocities are flagged as dirty it means a network or scripted change
    // occured between the beginning and end of the stepSimulation() and we DON'T want to apply
    // these physics simulation results.
    uint32_t flags = _entity->getDirtyFlags() & (Simulation::DIRTY_TRANSFORM | Simulation::DIRTY_VELOCITIES);
    if (!flags) {
        _entity->setWorldVelocity(glm::vec3(0.0f));
        _entity->setWorldAngularVelocity(glm::vec3(0.0f));
    } else {
        if (!(flags & Simulation::DIRTY_LINEAR_VELOCITY)) {
            _entity->setWorldVelocity(glm::vec3(0.0f));
        }
        if (!(flags & Simulation::DIRTY_ANGULAR_VELOCITY)) {
            _entity->setWorldAngularVelocity(glm::vec3(0.0f));
        }
    }
    _entity->setAcceleration(glm::vec3(0.0f));
}

bool EntityMotionState::isServerlessMode() {
    EntityTreePointer tree = _entity ? _entity->getTree() : nullptr;
    return tree && tree->isServerlessMode();
}
