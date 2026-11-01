//
//  ObjectMotionState.cpp
//  libraries/physics/src
//
//  Created by Andrew Meadows 2014.11.05
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: the presentation alpha and the presented pose, blended between the last two substeps: setLeftoverTime,
//   getPresentationLag, getPresentationAlpha, rememberSubstepStart, getPresentedBodyTransform, each with everything
//   it calls on the vehicle's path.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//

#include "ObjectMotionState.h"

#include <math.h>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>

#include <EntityItem.h>

#include "JoltUtil.h"
#include "PhysicsEngine.h"
#include <GLMHelpers.h>
#include <PhysicsHelpers.h>
#include "PhysicsWorld.h"
#include "PhysicsHelpers.h"
#include "PhysicsLogging.h"

// these thresholds determine what updates (object-->body) will activate the physical object
const float ACTIVATION_POSITION_DELTA = 0.005f;
const float ACTIVATION_ALIGNMENT_DOT = 0.99990f;
const float ACTIVATION_LINEAR_VELOCITY_DELTA = 0.01f;
const float ACTIVATION_GRAVITY_DELTA = 0.1f;
const float ACTIVATION_ANGULAR_VELOCITY_DELTA = 0.03f;

// origin of physics simulation in world-frame
glm::vec3 _worldOffset(0.0f);

// the time past the last substep that results are read ahead by
float _leftoverTime(0.0f);

// FROZEN (vehicle path): see the note at the top of this file.
// static
void ObjectMotionState::setLeftoverTime(float seconds) {
    _leftoverTime = seconds;
}

// static
bool _interpolatePresentation(true);

void ObjectMotionState::setInterpolatePresentation(bool interpolate) {
    _interpolatePresentation = interpolate;
}

bool ObjectMotionState::getInterpolatePresentation() {
    return _interpolatePresentation;
}

// FROZEN (vehicle path): see the note at the top of this file.
float ObjectMotionState::getPresentationLag() {
    return _interpolatePresentation ? PHYSICS_ENGINE_FIXED_SUBSTEP : 0.0f;
}

// FROZEN (vehicle path): see the note at the top of this file.
float ObjectMotionState::getPresentationAlpha() {
    return _interpolatePresentation ? glm::clamp(_leftoverTime / PHYSICS_ENGINE_FIXED_SUBSTEP, 0.0f, 1.0f) : 1.0f;
}

float ObjectMotionState::getLeftoverTime() {
    return _leftoverTime;
}

uint64_t _substepTime(0);

// static
void ObjectMotionState::setSubstepTime(uint64_t usecs) {
    _substepTime = usecs;
}

// static
uint64_t ObjectMotionState::getSubstepTime() {
    return _substepTime;
}

static bool isFrameEntity(const SpatiallyNestablePointer& object) {
    return object && object->getNestableType() == NestableType::Entity &&
        std::static_pointer_cast<EntityItem>(object)->isFrame();
}

const int MAX_FRAME_NESTING_DEPTH = 16;

// static
SpatiallyNestablePointer ObjectMotionState::findFrameAncestor(const SpatiallyNestablePointer& object) {
    if (!object) {
        return nullptr;
    }
    bool success = false;
    SpatiallyNestablePointer parent = object->getParentPointer(success);
    int depth = 0;
    while (success && parent && depth < MAX_FRAME_NESTING_DEPTH) {
        if (isFrameEntity(parent)) {
            return parent;
        }
        parent = parent->getParentPointer(success);
        ++depth;
    }
    return nullptr;
}

// static
// Walks from the object's parent up to its frame, composing local transforms and velocities (each local
// velocity is in its parent's rotation; angular motion of the chain is left out, as SpatiallyNestable does).
bool ObjectMotionState::getParentTransformInFrame(const SpatiallyNestablePointer& object, Transform& transform,
                                                  glm::vec3& linearVelocity, glm::vec3& angularVelocity, bool asSimulated) {
    transform = Transform();
    linearVelocity = glm::vec3(0.0f);
    angularVelocity = glm::vec3(0.0f);
    if (!object) {
        return false;
    }
    bool success = false;
    SpatiallyNestablePointer parent = object->getParentPointer(success);
    if (!success || !parent) {
        return false;
    }
    // the chain from the parent up to (not including) the frame
    std::vector<SpatiallyNestablePointer> chain;
    int depth = 0;
    while (success && parent && depth < MAX_FRAME_NESTING_DEPTH) {
        if (isFrameEntity(parent)) {
            // frame found: compose top-down
            for (auto itr = chain.rbegin(); itr != chain.rend(); ++itr) {
                const SpatiallyNestablePointer& node = *itr;
                glm::quat rotationSoFar = transform.getRotation();
                linearVelocity += rotationSoFar * node->getLocalVelocity();
                angularVelocity += rotationSoFar * node->getLocalAngularVelocity();
                ObjectMotionState* nodeState = nullptr;
                if (asSimulated && node->getNestableType() == NestableType::Entity) {
                    nodeState = static_cast<ObjectMotionState*>(std::static_pointer_cast<EntityItem>(node)->getPhysicsInfo());
                }
                if (nodeState && nodeState->getBody() && !nodeState->getBody()->IsStatic()) {
                    // inside the step the node is where its body is (its record presents a substep behind)
                    glm::vec3 position;
                    glm::quat rotation;
                    nodeState->getBodyTransform(position, rotation);
                    transform = Transform();
                    transform.setTranslation(position);
                    transform.setRotation(rotation);
                } else {
                    Transform composed;
                    Transform::mult(composed, transform, node->getLocalTransform());
                    transform = composed;
                }
            }
            return true;
        }
        chain.push_back(parent);
        parent = parent->getParentPointer(success);
        ++depth;
    }
    return false;
}

// static
bool ObjectMotionState::getFrameTransform(const SpatiallyNestablePointer& object, Transform& frameTransform) {
    SpatiallyNestablePointer frame = findFrameAncestor(object);
    if (!frame) {
        return false;
    }
    bool success = false;
    frameTransform = frame->getTransform(success);
    // an entity's transform carries its dimensions as scale; a frame's coordinates are metres
    frameTransform.setScale(1.0f);
    return success;
}

// static
void ObjectMotionState::setWorldOffset(const glm::vec3& offset) {
    _worldOffset = offset;
}

// static
const glm::vec3& ObjectMotionState::getWorldOffset() {
    return _worldOffset;
}

// static
uint32_t worldSimulationStep = 0;
void ObjectMotionState::setWorldSimulationStep(uint32_t step) {
    assert(step > worldSimulationStep);
    worldSimulationStep = step;
}

// static
uint32_t ObjectMotionState::getWorldSimulationStep() {
    return worldSimulationStep;
}

// static
ShapeManager* shapeManager = nullptr;
void ObjectMotionState::setShapeManager(ShapeManager* manager) {
    assert(manager);
    shapeManager = manager;
}

ShapeManager* ObjectMotionState::getShapeManager() {
    assert(shapeManager); // you must properly set shapeManager before calling getShapeManager()
    return shapeManager;
}

ObjectMotionState::ObjectMotionState(const JPH::Shape* shape) :
    _lastKinematicStep(worldSimulationStep)
{
    setShape(shape);
}

ObjectMotionState::~ObjectMotionState() {
    assert(!_body);
    setShape(nullptr);
}

JPH::BodyID ObjectMotionState::getBodyID() const {
    return _body ? _body->GetID() : JPH::BodyID();
}

// the mass is the density of the shape's bounding box, so that a changed shape keeps its feel
static float boundsVolume(const JPH::Shape* shape) {
    JPH::AABox bounds = shape->GetLocalBounds();
    JPH::Vec3 diagonal = bounds.GetSize();
    return diagonal.GetX() * diagonal.GetY() * diagonal.GetZ();
}

void ObjectMotionState::setMass(float mass) {
    _density = 1.0f;
    if (_shape) {
        // we compute the density for the current shape's Aabb volume
        // and save that instead of the total mass
        float volume = boundsVolume(_shape);
        if (volume > EPSILON) {
            _density = fabsf(mass) / volume;
        }
    }
}

float ObjectMotionState::getMass() const {
    if (_shape && _shape->GetType() != JPH::EShapeType::Mesh) {
        // scale the density by the current Aabb volume to get mass
        float volume = boundsVolume(_shape);

        // cap the max mass for numerical stability
        const float MIN_OBJECT_MASS = 0.0f;
        const float MAX_OBJECT_DENSITY = 20000.0f; // kg/m^3 density of Tungsten
        const float MAX_OBJECT_VOLUME = 1.0e6f;
        const float MAX_OBJECT_MASS = MAX_OBJECT_DENSITY * MAX_OBJECT_VOLUME;
        return glm::clamp(_density * volume, MIN_OBJECT_MASS, MAX_OBJECT_MASS);
    }
    return 0.0f;
}

void ObjectMotionState::setBodyLinearVelocity(const glm::vec3& velocity) const {
    if (_body && bodyInterface() && !_body->IsStatic()) {
        bodyInterface()->SetLinearVelocity(_body->GetID(), glmToJolt(velocity));
    }
}

void ObjectMotionState::setBodyAngularVelocity(const glm::vec3& velocity) const {
    if (_body && bodyInterface() && !_body->IsStatic()) {
        bodyInterface()->SetAngularVelocity(_body->GetID(), glmToJolt(velocity));
    }
}

void ObjectMotionState::setBodyGravity(const glm::vec3& gravity) {
    // the world has no gravity of its own; the engine applies this one as a force each substep
    _gravity = gravity;
}

glm::vec3 ObjectMotionState::getBodyLinearVelocity() const {
    return (_body && !_body->IsStatic()) ? joltToGLM(_body->GetLinearVelocity()) : glm::vec3(0.0f);
}

glm::vec3 ObjectMotionState::getBodyLinearVelocityGTSigma() const {
    // NOTE: the threshold to use here relates to the linear displacement threshold (dX) for sending updates
    // to objects that are tracked server-side (e.g. entities which use dX = 2mm).  Hence an object moving
    // just under this velocity threshold would trigger an update about V/dX times per second.
    const float MIN_LINEAR_SPEED_SQUARED = 0.0036f; // 6 mm/sec

    glm::vec3 velocity = getBodyLinearVelocity();
    if (glm::length2(velocity) < MIN_LINEAR_SPEED_SQUARED) {
        velocity *= 0.0f;
    }
    return velocity;
}

glm::vec3 ObjectMotionState::getObjectLinearVelocityChange() const {
    return glm::vec3(0.0f);  // Subclasses override where meaningful.
}

glm::vec3 ObjectMotionState::getBodyAngularVelocity() const {
    return (_body && !_body->IsStatic()) ? joltToGLM(_body->GetAngularVelocity()) : glm::vec3(0.0f);
}

void ObjectMotionState::setMotionType(PhysicsMotionType motionType) {
    _motionType = motionType;
}

// Continuous collision detection (a linear cast per step) for the small, fast things that would otherwise
// pass through walls; big and mesh shapes don't need it.
void ObjectMotionState::updateCCDConfiguration() {
    assert(_body);
    if (!bodyInterface() || _body->IsStatic()) {
        return;
    }
    const float MAX_RADIUS_FOR_CCD = 0.5f; // meters
    bool wantsCCD = _shape && _shape->GetType() != JPH::EShapeType::Mesh && _shape->GetInnerRadius() < MAX_RADIUS_FOR_CCD;
    bodyInterface()->SetMotionQuality(_body->GetID(), wantsCCD ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete);
}

bool ObjectMotionState::getFrameTransformInDomain(glm::vec3& position, glm::quat& rotation, glm::vec3& linearVelocity, glm::vec3& angularVelocity, bool asSimulated) const {
    if (!isFrame()) {
        return false;
    }
    position = getObjectPosition();
    rotation = getObjectRotation();
    linearVelocity = getObjectLinearVelocity();
    angularVelocity = getObjectAngularVelocity();
    return true;
}

JPH::BodyInterface* ObjectMotionState::bodyInterface() const {
    return _world ? &_world->getBodyInterface() : nullptr;
}

void ObjectMotionState::setBody(JPH::Body* body, PhysicsWorld* world) {
    // give the body a back-pointer to this ObjectMotionState
    if (_body != body) {
        if (_body) {
            _body->SetUserData(0);
        }
        _body = body;
        _world = body ? world : nullptr;
        if (_body) {
            _body->SetUserData((JPH::uint64)(uintptr_t)this);
            assert(_body->GetShape() == getBodyShape());
            updateCCDConfiguration();
        }
    }
}

void ObjectMotionState::setShape(const JPH::Shape* shape) {
    if (_shape != shape) {
        if (_shape) {
            getShapeManager()->releaseShape(_shape);
        }
        _shape = shape;
        // an interior copy was built on the old shape; the engine rebuilds it when the body is reinserted
        _interiorShape = nullptr;
        _interiorParts.clear();
        refreshBodyShape(false);
    } else if (shape) {
        // we need to release unused reference to shape
        getShapeManager()->releaseShape(shape);
    }
}

void ObjectMotionState::setInteriorShape(const JPH::Shape* compound, std::vector<ObjectMotionState*>&& parts) {
    if (_interiorShape.GetPtr() == compound) {
        _interiorParts = std::move(parts);
        return;
    }
    _interiorShape = compound;
    _interiorParts = compound ? std::move(parts) : std::vector<ObjectMotionState*>();
    refreshBodyShape(false);
}

void ObjectMotionState::setCenterOfMassOffset(const glm::vec3& offset) {
    if (offset == _centerOfMassOffset) {
        return;
    }
    _centerOfMassOffset = offset;
    refreshBodyShape(true);
}

// private
void ObjectMotionState::refreshBodyShape(bool activate) {
    JPH::RefConst<JPH::Shape> worn = _interiorShape ? _interiorShape : JPH::RefConst<JPH::Shape>(_shape);
    if (worn && glm::length2(_centerOfMassOffset) > 1.0e-8f) {
        worn = new JPH::OffsetCenterOfMassShape(worn, glmToJolt(_centerOfMassOffset));
    }
    _bodyShape = (worn.GetPtr() == _shape) ? nullptr : worn;
    if (_body) {
        assert(getBodyShape());
        bodyInterface()->SetShape(_body->GetID(), getBodyShape(), false, activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        updateCCDConfiguration();
        updateBodyMassProperties();
    }
}

JPH::MassProperties ObjectMotionState::getOwnMassProperties() const {
    JPH::MassProperties massProperties;
    if (_shape) {
        massProperties = _shape->GetMassProperties();
        if (glm::length2(_centerOfMassOffset) > 1.0e-8f) {
            massProperties.Translate(glmToJolt(_centerOfMassOffset));   // as OffsetCenterOfMassShape computes it
        }
    }
    return massProperties;
}

// The compound's sub-shape user data numbers the parts from 1 (0 is the own shape); a decorated shape (the centre
// offset) passes sub-shape IDs through to the compound unchanged.
ObjectMotionState* ObjectMotionState::interiorPartAt(const JPH::SubShapeID& subShapeID) const {
    if (!_interiorShape || !_body || _interiorParts.empty()) {
        return nullptr;
    }
    const JPH::Shape* shape = _body->GetShape();
    while (shape && shape->GetType() == JPH::EShapeType::Decorated) {
        shape = static_cast<const JPH::DecoratedShape*>(shape)->GetInnerShape();
    }
    if (shape != _interiorShape.GetPtr() || shape->GetType() != JPH::EShapeType::Compound) {
        return nullptr;
    }
    const JPH::CompoundShape* compound = static_cast<const JPH::CompoundShape*>(shape);
    JPH::SubShapeID remainder;
    uint32_t index = compound->GetSubShapeIndexFromID(subShapeID, remainder);
    if (index >= compound->GetNumSubShapes()) {
        return nullptr;
    }
    uint32_t partNumber = compound->GetCompoundUserData(index);
    return (partNumber > 0 && partNumber <= _interiorParts.size()) ? _interiorParts[partNumber - 1] : nullptr;
}

void ObjectMotionState::setBodyTransform(const glm::vec3& position, const glm::quat& rotation, bool activate) {
    if (_body && bodyInterface()) {
        if (_body->IsStatic()) {
            // a wall that moved makes the frame's copy of it (round 8) stale; a wall placed where it already is
            // (a child's transform is pushed again whenever its parent moves) does not
            glm::vec3 bodyPosition;
            glm::quat bodyRotation;
            getBodyTransform(bodyPosition, bodyRotation);
            if (glm::distance2(bodyPosition, position) > 1.0e-10f || fabsf(glm::dot(bodyRotation, rotation)) < 1.0f - 1.0e-7f) {
                _world->markInteriorChanged();
            }
        }
        bodyInterface()->SetPositionAndRotation(_body->GetID(), glmToJoltR(position), glmToJolt(rotation),
                                               activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        // placed: it is where it is, with no blend from where it was
        _substepStartStep = 0;
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
void ObjectMotionState::rememberSubstepStart() {
    if (_body) {
        _substepStartPosition = joltToGLM(_body->GetPosition());
        _substepStartRotation = joltToGLM(_body->GetRotation());
        _substepStartStep = getWorldSimulationStep();
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
void ObjectMotionState::getPresentedBodyTransform(glm::vec3& position, glm::quat& rotation) const {
    getBodyTransform(position, rotation);
    if (!_body || !_interpolatePresentation || _substepStartStep != getWorldSimulationStep()) {
        // no substep moved it lately: it is where it is
        return;
    }
    float alpha = getPresentationAlpha();
    position = glm::mix(_substepStartPosition, position, alpha);
    rotation = glm::normalize(safeMix(_substepStartRotation, rotation, alpha));
}

void ObjectMotionState::getBodyTransform(glm::vec3& position, glm::quat& rotation) const {
    if (_body) {
        position = joltToGLM(_body->GetPosition());
        rotation = joltToGLM(_body->GetRotation());
    }
}

void ObjectMotionState::activateBody() {
    if (_body && bodyInterface() && !_body->IsStatic()) {
        bodyInterface()->ActivateBody(_body->GetID());
    }
}

void ObjectMotionState::deactivateBody() {
    if (_body && bodyInterface() && !_body->IsStatic()) {
        bodyInterface()->DeactivateBody(_body->GetID());
    }
}

void ObjectMotionState::handleEasyChanges(uint32_t& flags) {
    assert(_body && _shape);
    glm::vec3 bodyPosition;
    glm::quat bodyRotation;
    getBodyTransform(bodyPosition, bodyRotation);

    if (flags & (Simulation::DIRTY_POSITION | Simulation::DIRTY_ROTATION)) {
        glm::vec3 newPosition = bodyPosition;
        glm::quat newRotation = bodyRotation;
        if (flags & Simulation::DIRTY_POSITION) {
            newPosition = getObjectPosition();
            float delta = glm::length(newPosition - bodyPosition);
            if (delta > ACTIVATION_POSITION_DELTA) {
                flags |= Simulation::DIRTY_PHYSICS_ACTIVATION;
            }
        }
        if (flags & Simulation::DIRTY_ROTATION) {
            newRotation = getObjectRotation();
            float alignmentDot = fabsf(glm::dot(bodyRotation, newRotation));
            if (alignmentDot < ACTIVATION_ALIGNMENT_DOT) {
                flags |= Simulation::DIRTY_PHYSICS_ACTIVATION;
            }
        }
        // a moved static body is placed without activation; the engine wakes what it touches
        setBodyTransform(newPosition, newRotation, false);
    }

    if (!_body->IsStatic() && _shape->GetType() != JPH::EShapeType::Mesh) {
        if (flags & Simulation::DIRTY_LINEAR_VELOCITY) {
            glm::vec3 newLinearVelocity = getObjectLinearVelocity();
            if (!(flags & Simulation::DIRTY_PHYSICS_ACTIVATION)) {
                float delta = glm::length(newLinearVelocity - getBodyLinearVelocity());
                if (delta > ACTIVATION_LINEAR_VELOCITY_DELTA) {
                    flags |= Simulation::DIRTY_PHYSICS_ACTIVATION;
                }
            }
            setBodyLinearVelocity(newLinearVelocity);

            glm::vec3 newGravity = getObjectGravity();
            if (!(flags & Simulation::DIRTY_PHYSICS_ACTIVATION)) {
                float delta = glm::length(newGravity - _gravity);
                if (delta > ACTIVATION_GRAVITY_DELTA ||
                        (delta > 0.0f && glm::length2(_gravity) == 0.0f)) {
                    flags |= Simulation::DIRTY_PHYSICS_ACTIVATION;
                }
            }
            setBodyGravity(newGravity);
        }
        if (flags & Simulation::DIRTY_ANGULAR_VELOCITY) {
            glm::vec3 newAngularVelocity = getObjectAngularVelocity();
            if (!(flags & Simulation::DIRTY_PHYSICS_ACTIVATION)) {
                float delta = glm::length(newAngularVelocity - getBodyAngularVelocity());
                if (delta > ACTIVATION_ANGULAR_VELOCITY_DELTA) {
                    flags |= Simulation::DIRTY_PHYSICS_ACTIVATION;
                }
            }
            setBodyAngularVelocity(newAngularVelocity);
        }
    }

    if (flags & Simulation::DIRTY_MATERIAL) {
        updateBodyMaterialProperties();
    }

    if (flags & Simulation::DIRTY_MASS) {
        updateBodyMassProperties();
    }
}

// The entity's damping d means "the velocity loses the fraction d of itself each second": v(t) = v0 (1-d)^t.
// Jolt's damping c is the rate in dv/dt = -c v, so v(t) = v0 e^(-c t). The two agree when c = -ln(1-d).
static float joltDamping(float damping) {
    const float MAX_DAMPING = 0.999f;
    damping = glm::clamp(fabsf(damping), 0.0f, MAX_DAMPING);
    return -logf(1.0f - damping);
}

void ObjectMotionState::updateBodyMaterialProperties() {
    if (!_body || !bodyInterface()) {
        return;
    }
    bodyInterface()->SetRestitution(_body->GetID(), getObjectRestitution());
    bodyInterface()->SetFriction(_body->GetID(), getObjectFriction());
    JPH::MotionProperties* motion = _body->GetMotionPropertiesUnchecked();
    if (motion) {
        motion->SetLinearDamping(joltDamping(getObjectLinearDamping()));
        motion->SetAngularDamping(joltDamping(getObjectAngularDamping()));
    }
}

void ObjectMotionState::updateBodyVelocities() {
    setBodyLinearVelocity(getObjectLinearVelocity());
    setBodyAngularVelocity(getObjectAngularVelocity());
    setBodyGravity(getObjectGravity());
    activateBody();
}

void ObjectMotionState::updateLastKinematicStep() {
    // The step counter advances before each substep's push, so starting at the current step gives exactly
    // one substep of motion on the first push. (Starting a step earlier gave two, and that doubled velocity
    // was then read back from the body into the entity.)
    _lastKinematicStep = ObjectMotionState::getWorldSimulationStep();
}

void ObjectMotionState::updateBodyMassProperties() {
    if (!_body) {
        return;
    }
    JPH::MotionProperties* motion = _body->GetMotionPropertiesUnchecked();
    if (!motion || _body->GetMotionType() != JPH::EMotionType::Dynamic) {
        return;
    }
    float mass = getMass();
    const float MIN_DYNAMIC_MASS = 0.01f;
    if (mass != mass || mass < MIN_DYNAMIC_MASS) {
        mass = MIN_DYNAMIC_MASS;
    }
    JPH::MassProperties massProperties = getOwnMassProperties();
    massProperties.ScaleToMass(mass);
    motion->SetMassProperties(motion->GetAllowedDOFs(), massProperties);
}
