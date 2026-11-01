//
//  VehicleSimulation.cpp
//  libraries/physics/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: these functions, the server's states and the corrections: drainReceivedStates, setBodyState,
//   setInternalState, correctDriver, correctFollower, each with everything it calls on the vehicle's path.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//

#include "VehicleSimulation.h"

#include <algorithm>
#include <cmath>
#include <limits>


#include <glm/gtx/norm.hpp>
#include <glm/gtx/quaternion.hpp>

#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include <EntityItemProperties.h>
#include <NLPacket.h>
#include <NodeList.h>
#include <NumericalConstants.h>
#include <PhysicsHelpers.h>
#include <SharedUtil.h>

#include "EntityMotionState.h"
#include "JoltUtil.h"
#include "PhysicsLogging.h"
#include "PhysicsWorld.h"
#include "TrackSimulation.h"

namespace {
    const glm::vec3 UP(0.0f, 1.0f, 0.0f);
    const glm::vec3 FORWARD(0.0f, 0.0f, -1.0f);   // an entity's forward, as an avatar's
    const float GRAVITY = 9.8f;
    const float WHEEL_CAST_CONVEX_RADIUS = 0.05f;   // the cast cylinder's rounding, as a fraction of the radius
    const float ANTI_ROLL_BAR_STIFFNESS = 1000.0f;  // N/m, Jolt's default; keeps a tall car on its wheels in a bend

    float angleBetween(const glm::quat& a, const glm::quat& b) {
        float dot = glm::clamp(fabsf(glm::dot(glm::normalize(a), glm::normalize(b))), 0.0f, 1.0f);
        return 2.0f * acosf(dot);
    }

    JPH::WheeledVehicleController* controllerOf(JPH::VehicleConstraint* constraint) {
        return constraint ? static_cast<JPH::WheeledVehicleController*>(constraint->GetController()) : nullptr;
    }

    VehicleInput inputFromState(const VehiclePropertyGroup& group) {
        VehicleInput input;
        input.sequence = group.getInputSequence();
        input.throttle = group.getThrottle();
        input.steering = group.getSteering();
        input.brake = group.getBrake();
        input.handBrake = group.getHandBrake();
        input.pitch = group.getPitch();
        input.lift = group.getLift();
        input.trim = group.getTrim();
        return input;
    }
}

VehicleSimulation::VehicleSimulation() {
}

VehicleSimulation::~VehicleSimulation() {
    clear();
}

PhysicsWorld* VehicleSimulation::rootWorld() const {
    return _engine ? _engine->getRootWorld() : nullptr;
}

void VehicleSimulation::attach(EntityMotionState* state) {
    if (!state || !state->getEntity() || !state->getEntity()->isVehicle()) {
        return;
    }
    if (_vehicles.find(state) != _vehicles.end()) {
        return;
    }
    Vehicle vehicle;
    vehicle.state = state;
    vehicle.entity = state->getEntity();
    _byEntity[vehicle.entity->getID()] = state;
    // every peer simulates the vehicle itself; what the network says of its transform is a correction, not a placement
    vehicle.entity->setTransformDrivenLocally(true);
    _vehicles.emplace(state, std::move(vehicle));
    _stats.vehicles = (int)_vehicles.size();
}

void VehicleSimulation::detach(EntityMotionState* state) {
    auto it = _vehicles.find(state);
    if (it == _vehicles.end()) {
        return;
    }
    tearDown(it->second);
    if (it->second.entity) {
        it->second.entity->setTransformDrivenLocally(false);
        _byEntity.erase(it->second.entity->getID());
    }
    _vehicles.erase(it);
    _stats.vehicles = (int)_vehicles.size();
}

void VehicleSimulation::clear() {
    for (auto& entry : _vehicles) {
        tearDown(entry.second);
        if (entry.second.entity) {
            entry.second.entity->setTransformDrivenLocally(false);
        }
    }
    _vehicles.clear();
    _byEntity.clear();
    _snapshots.clear();
    _stats.vehicles = 0;
}

void VehicleSimulation::tearDown(Vehicle& vehicle) {
    PhysicsWorld* world = rootWorld();
    if (vehicle.constraint && world) {
        JPH::PhysicsSystem* system = world->getPhysicsSystem();
        system->RemoveStepListener(vehicle.constraint);
        system->RemoveConstraint(vehicle.constraint);
    }
    vehicle.constraint = nullptr;
    vehicle.tester = nullptr;
    vehicle.bodyID = JPH::BodyID();
    vehicle.visuals.clear();
    vehicle.airshipBuilt = false;
    vehicle.aircraftBuilt = false;
}

// The body wears its shape inside an offset-centre-of-mass shape when the spec moves the centre (the motion state
// composes it with a frame's interior copy, round 8).
void VehicleSimulation::wearCenterOfMass(Vehicle& vehicle, const glm::vec3& centerOfMass) {
    vehicle.state->setCenterOfMassOffset(centerOfMass);
}

// An airship from its spec (PHYSICS.md round 7): the hull's body with the centre of mass where the spec puts it, the
// spec's numbers parsed, the drag areas derived from the hull's dimensions when the spec gives none, and the ballast
// and fill from the state (the author's numbers the first time, the authority's after).
bool VehicleSimulation::rebuildAirship(Vehicle& vehicle, JPH::Body* body, PhysicsWorld* world) {
    const VehiclePropertyGroup& group = vehicle.entity->getVehicleProperties();
    AirshipSpec& spec = vehicle.airshipSpec;
    spec = AirshipSpec();
    spec.mass = group.getMass();
    spec.centerOfMass = group.getCenterOfMass();
    spec.envelopeVolume = glm::max(0.0f, group.getEnvelopeVolume());
    spec.centerOfBuoyancy = group.getCenterOfBuoyancy();
    spec.gasDensity = glm::max(0.0f, group.getGasDensity());
    spec.maxThrust = glm::max(0.0f, group.getMaxThrust());
    spec.maxVerticalThrust = glm::max(0.0f, group.getMaxVerticalThrust());
    spec.thrustPoint = group.getThrustPoint();
    glm::vec3 dimensions = vehicle.entity->getScaledDimensions();
    spec.dragArea = glm::length2(group.getDragArea()) > 0.0f ? glm::max(group.getDragArea(), glm::vec3(0.0f)) : AirshipModel::defaultDragArea(dimensions);
    spec.angularDragArea = glm::length2(group.getAngularDragArea()) > 0.0f ? glm::max(group.getAngularDragArea(), glm::vec3(0.0f))
        : AirshipModel::defaultAngularDragArea(dimensions);
    spec.rudderArea = glm::max(0.0f, group.getRudderArea());
    spec.elevatorArea = glm::max(0.0f, group.getElevatorArea());
    spec.finPoint = group.getFinPoint();
    spec.maxFinAngle = glm::clamp(group.getMaxFinAngle(), 0.0f, 80.0f);
    spec.ballastRate = glm::max(0.0f, group.getBallastRate());
    spec.ventRate = glm::max(0.0f, group.getVentRate());
    if (!spec.isValid()) {
        if (!vehicle.logged) {
            qCWarning(physics) << "airship" << vehicle.entity->getName() << vehicle.entity->getID() << "has no envelope: set vehicle.envelopeVolume";
            vehicle.logged = true;
        }
        return false;
    }
    wearCenterOfMass(vehicle, spec.centerOfMass);
    world->getBodyInterface().ActivateBody(body->GetID());
    vehicle.airshipState.ballast = glm::max(0.0f, group.getBallast());
    vehicle.airshipState.fill = glm::clamp(group.getFill(), 0.0f, 1.0f);
    vehicle.airshipBuilt = true;
    qCDebug(physics) << "airship" << vehicle.entity->getName() << vehicle.entity->getID() << "built: envelope" << spec.envelopeVolume
        << "m3, mass" << vehicle.state->getMass() << "kg, ballast" << vehicle.airshipState.ballast << "kg, thrust" << spec.maxThrust
        << "N, fins" << spec.rudderArea << spec.elevatorArea << "m2 at" << spec.finPoint.x << spec.finPoint.y << spec.finPoint.z
        << (Physics::isServerAuthority() ? "(physics server)" : "");
    return true;
}

// An aircraft from its spec (PHYSICS.md round 8): the body with the centre of mass where the spec puts it, the wing, the
// tail and the engines parsed, the drag areas derived from the body's dimensions when the spec gives none, and the
// nacelles' tilt from the state (the author's the first time, the authority's after).
bool VehicleSimulation::rebuildAircraft(Vehicle& vehicle, JPH::Body* body, PhysicsWorld* world) {
    const VehiclePropertyGroup& group = vehicle.entity->getVehicleProperties();
    AircraftSpec& spec = vehicle.aircraftSpec;
    spec = AircraftSpec();
    spec.mass = group.getMass();
    spec.centerOfMass = group.getCenterOfMass();
    spec.maxThrust = glm::max(0.0f, group.getMaxThrust());
    spec.thrustPoint = group.getThrustPoint();
    spec.wingArea = glm::max(0.0f, group.getWingArea());
    spec.wingSpan = glm::max(0.0f, group.getWingSpan());
    spec.wingPoint = group.getWingPoint();
    spec.elevatorArea = glm::max(0.0f, group.getElevatorArea());
    spec.rudderArea = glm::max(0.0f, group.getRudderArea());
    spec.finPoint = group.getFinPoint();
    spec.maxFinAngle = glm::clamp(group.getMaxFinAngle(), 0.0f, 80.0f);
    glm::vec3 dimensions = vehicle.entity->getScaledDimensions();
    spec.dragArea = glm::length2(group.getDragArea()) > 0.0f ? glm::max(group.getDragArea(), glm::vec3(0.0f)) : AirshipModel::defaultDragArea(dimensions);
    spec.angularDragArea = glm::length2(group.getAngularDragArea()) > 0.0f ? glm::max(group.getAngularDragArea(), glm::vec3(0.0f))
        : AirshipModel::defaultAngularDragArea(dimensions);
    if (!spec.isValid()) {
        if (!vehicle.logged) {
            qCWarning(physics) << "aircraft" << vehicle.entity->getName() << vehicle.entity->getID()
                << "needs a wing and engines: set vehicle.wingArea, wingSpan and maxThrust";
            vehicle.logged = true;
        }
        return false;
    }
    wearCenterOfMass(vehicle, spec.centerOfMass);
    world->getBodyInterface().ActivateBody(body->GetID());
    vehicle.aircraftState.tilt = glm::clamp(group.getTilt(), 0.0f, 1.0f);
    vehicle.aircraftBuilt = true;
    qCDebug(physics) << "aircraft" << vehicle.entity->getName() << vehicle.entity->getID() << "built: wing" << spec.wingArea << "m2 span"
        << spec.wingSpan << "m (aspect ratio" << spec.aspectRatio() << "), mass" << vehicle.state->getMass() << "kg, thrust" << spec.maxThrust
        << "N, tail" << spec.elevatorArea << spec.rudderArea << "m2 at" << spec.finPoint.x << spec.finPoint.y << spec.finPoint.z
        << ", tilt" << vehicle.aircraftState.tilt << (Physics::isServerAuthority() ? "(physics server)" : "");
    return true;
}

// The Jolt vehicle from the entity's spec: the chassis body wears its shape inside an offset-centre-of-mass shape when
// the spec moves the centre, a WheelSettingsWV per wheel, the engine, an automatic gearbox, one differential per driven
// axle and an anti-roll bar per axle.
bool VehicleSimulation::rebuild(Vehicle& vehicle) {
    tearDown(vehicle);
    JPH::Body* body = vehicle.state->getBody();
    PhysicsWorld* world = vehicle.state->getWorld();
    if (!body || !world || !world->isRoot()) {
        return false;
    }
    if (!body->IsDynamic()) {
        if (!vehicle.logged) {
            qCWarning(physics) << "vehicle" << vehicle.entity->getName() << vehicle.entity->getID()
                << "is not a dynamic body: set dynamic true (and gravity) on the chassis";
            vehicle.logged = true;
        }
        return false;
    }
    vehicle.airship = vehicle.entity->isAirship();
    vehicle.aircraft = vehicle.entity->isAircraft();
    bool built = vehicle.airship ? rebuildAirship(vehicle, body, world)
        : vehicle.aircraft ? rebuildAircraft(vehicle, body, world) : rebuildWheeled(vehicle, body, world);
    if (!built) {
        return false;
    }
    vehicle.bodyID = body->GetID();
    vehicle.definitionVersion = vehicle.entity->getVehicleDefinitionVersion();
    vehicle.definitionHash = vehicle.entity->getVehicleProperties().definitionHash();
    vehicle.history.clear();
    vehicle.visuals.clear();
    vehicle.childCount = -1;
    vehicle.logged = false;
    setInternalState(vehicle, stateFromRelay(vehicle));
    return true;
}

bool VehicleSimulation::rebuildWheeled(Vehicle& vehicle, JPH::Body* body, PhysicsWorld* world) {
    const VehiclePropertyGroup& group = vehicle.entity->getVehicleProperties();
    WheeledVehicleSpec& spec = vehicle.spec;
    spec = WheeledVehicleSpec();
    QString error;
    if (!WheeledVehicleSpec::parseWheels(group.getWheels(), spec.wheels, &error) || spec.wheels.empty()) {
        if (!vehicle.logged) {
            qCWarning(physics) << "vehicle" << vehicle.entity->getName() << vehicle.entity->getID() << "has no usable wheels:" << error;
            vehicle.logged = true;
        }
        return false;
    }
    WheeledVehicleSpec::parseSeats(group.getSeats(), spec.seats, nullptr);
    WheeledVehicleSpec::parseGearRatios(group.getGearRatios(), spec.gearRatios, nullptr);
    spec.mass = group.getMass();
    spec.centerOfMass = group.getCenterOfMass();
    spec.maxSteerAngle = glm::clamp(group.getMaxSteerAngle(), 0.0f, 80.0f);
    spec.maxTorque = glm::max(0.0f, group.getMaxTorque());
    spec.minRPM = glm::max(1.0f, group.getMinRPM());
    spec.maxRPM = glm::max(spec.minRPM + 1.0f, group.getMaxRPM());
    spec.reverseGearRatio = glm::max(0.01f, group.getReverseGearRatio());
    spec.finalDriveRatio = glm::max(0.01f, group.getFinalDriveRatio());
    spec.maxBrakeTorque = glm::max(0.0f, group.getMaxBrakeTorque());
    spec.maxHandBrakeTorque = glm::max(0.0f, group.getMaxHandBrakeTorque());
    spec.driveType = WheeledVehicleSpec::parseDriveType(group.getDriveType());
    spec.suspensionFrequency = group.getSuspensionFrequency();
    spec.suspensionDamping = glm::max(0.0f, group.getSuspensionDamping());

    JPH::BodyInterface& bodies = world->getBodyInterface();
    wearCenterOfMass(vehicle, spec.centerOfMass);

    float gravity = glm::length(vehicle.state->getBodyGravity());
    if (gravity <= 0.0f) {
        gravity = GRAVITY;
    }

    JPH::VehicleConstraintSettings settings;
    settings.mUp = glmToJolt(UP);
    settings.mForward = glmToJolt(FORWARD);
    settings.mMaxPitchRollAngle = JPH::JPH_PI;
    for (const WheelSpec& wheel : spec.wheels) {
        JPH::WheelSettingsWV* ws = new JPH::WheelSettingsWV();
        ws->mPosition = glmToJolt(wheel.center);
        ws->mSuspensionDirection = glmToJolt(-UP);
        ws->mSteeringAxis = glmToJolt(UP);
        ws->mWheelUp = glmToJolt(UP);
        ws->mWheelForward = glmToJolt(FORWARD);
        ws->mSuspensionMinLength = 0.0f;
        ws->mSuspensionMaxLength = glm::max(0.01f, wheel.suspensionDistance);
        ws->mSuspensionPreloadLength = 0.0f;
        ws->mSuspensionForcePoint = glmToJolt(wheel.center - UP * (wheel.suspensionDistance * wheel.targetPosition));
        ws->mEnableSuspensionForcePoint = wheel.enableSuspensionForcePoint;
        if (wheel.spring > 0.0f) {
            // Unity's spring and damper, as they are
            ws->mSuspensionSpring = JPH::SpringSettings(JPH::ESpringMode::StiffnessAndDamping, wheel.spring, wheel.damper);
        } else {
            // the vehicle's frequency; or, when it gives none, the one that rests the suspension at the wheel's targetPosition
            float frequency = spec.suspensionFrequency;
            if (frequency <= 0.0f) {
                float restCompression = glm::max(0.01f, wheel.suspensionDistance * wheel.targetPosition);
                frequency = sqrtf(gravity / restCompression) / TWO_PI;
            }
            ws->mSuspensionSpring = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, frequency, spec.suspensionDamping);
        }
        ws->mRadius = wheel.radius;
        ws->mWidth = wheel.width;
        ws->mInertia = 0.5f * wheel.mass * wheel.radius * wheel.radius;
        // a negative angle steers the other way (Jolt applies -input * angle), so a rear wheel can counter-steer
        ws->mMaxSteerAngle = glm::radians(spec.maxSteerAngle * wheel.steer);
        ws->mMaxBrakeTorque = spec.maxBrakeTorque * wheel.brake;
        ws->mMaxHandBrakeTorque = spec.maxHandBrakeTorque * wheel.handBrake;
        settings.mWheels.push_back(ws);
    }

    JPH::WheeledVehicleControllerSettings* controller = new JPH::WheeledVehicleControllerSettings();
    controller->mEngine.mMaxTorque = spec.maxTorque;
    controller->mEngine.mMinRPM = spec.minRPM;
    controller->mEngine.mMaxRPM = spec.maxRPM;
    controller->mTransmission.mMode = JPH::ETransmissionMode::Auto;
    if (!spec.gearRatios.empty()) {
        controller->mTransmission.mGearRatios.clear();
        for (float ratio : spec.gearRatios) {
            controller->mTransmission.mGearRatios.push_back(ratio);
        }
    }
    controller->mTransmission.mReverseGearRatios.clear();
    controller->mTransmission.mReverseGearRatios.push_back(-spec.reverseGearRatio);
    controller->mTransmission.mShiftUpRPM = 0.8f * spec.maxRPM;
    controller->mTransmission.mShiftDownRPM = glm::max(1.5f * spec.minRPM, 0.35f * spec.maxRPM);
    std::vector<std::pair<int, int>> axles = spec.axles();
    int drivenAxles = 0;
    for (const auto& axle : axles) {
        if (spec.isDriven(axle.first) || spec.isDriven(axle.second)) {
            ++drivenAxles;
        }
    }
    for (const auto& axle : axles) {
        bool left = spec.isDriven(axle.first);
        bool right = spec.isDriven(axle.second);
        if (left || right) {
            JPH::VehicleDifferentialSettings differential;
            differential.mLeftWheel = left ? axle.first : -1;
            differential.mRightWheel = right && axle.second != axle.first ? axle.second : (left ? -1 : axle.second);
            if (axle.first == axle.second) {
                differential.mLeftWheel = axle.first;
                differential.mRightWheel = -1;
            }
            differential.mDifferentialRatio = spec.finalDriveRatio;
            differential.mEngineTorqueRatio = 1.0f / (float)glm::max(1, drivenAxles);
            controller->mDifferentials.push_back(differential);
        }
        if (axle.first != axle.second) {
            JPH::VehicleAntiRollBar bar;
            bar.mLeftWheel = axle.first;
            bar.mRightWheel = axle.second;
            bar.mStiffness = ANTI_ROLL_BAR_STIFFNESS;
            settings.mAntiRollBars.push_back(bar);
        }
    }
    settings.mController = controller;

    vehicle.constraint = new JPH::VehicleConstraint(*body, settings);
    vehicle.tester = new JPH::VehicleCollisionTesterCastCylinder(body->GetObjectLayer(), WHEEL_CAST_CONVEX_RADIUS);
    vehicle.constraint->SetVehicleCollisionTester(vehicle.tester);
    JPH::PhysicsSystem* system = world->getPhysicsSystem();
    system->AddConstraint(vehicle.constraint);
    system->AddStepListener(vehicle.constraint);
    bodies.ActivateBody(body->GetID());

    qCDebug(physics) << "vehicle" << vehicle.entity->getName() << vehicle.entity->getID() << "built:" << spec.wheels.size()
        << "wheels," << drivenAxles << "driven axles, mass" << vehicle.state->getMass() << "kg, centre of mass"
        << spec.centerOfMass.x << spec.centerOfMass.y << spec.centerOfMass.z << (Physics::isServerAuthority() ? "(physics server)" : "");
    return true;
}

bool VehicleSimulation::isAuthority(const Vehicle& vehicle) const {
    return _serverless || vehicle.state->isLocallyOwned();
}

bool VehicleSimulation::localControls(const Vehicle& vehicle, VehicleInput& input) const {
    EntityItem::VehicleControls controls;
    if (!vehicle.entity->getLocalVehicleControls(controls)) {
        return false;
    }
    input.throttle = controls.throttle;
    input.steering = controls.steering;
    input.brake = controls.brake;
    input.handBrake = controls.handBrake;
    input.pitch = controls.pitch;
    input.lift = controls.lift;
    input.trim = controls.trim;
    return true;
}

bool VehicleSimulation::isLocalDriver(EntityMotionState* state) const {
    auto it = _vehicles.find(state);
    if (it == _vehicles.end()) {
        return false;
    }
    const Vehicle& vehicle = it->second;
    VehicleInput input;
    if (!localControls(vehicle, input)) {
        return false;
    }
    return _serverless || vehicle.state->isLocallyOwned()
        || vehicle.entity->getVehicleProperties().getDriver() == Physics::getSessionUUID();
}

// FROZEN (vehicle path): see the note at the top of this file.
// The stream's states, as they arrived since the last substep, into their vehicles' buffers (round 9).
void VehicleSimulation::drainReceivedStates() {
    std::vector<std::pair<VehicleState, quint64>> received;
    {
        std::lock_guard<std::mutex> lock(_streamMutex);
        received.swap(_receivedStates);
    }
    for (const auto& entry : received) {
        auto found = _byEntity.find(entry.first.id);
        if (found == _byEntity.end()) {
            continue;
        }
        auto it = _vehicles.find(found->second);
        if (it == _vehicles.end()) {
            continue;
        }
        Vehicle& vehicle = it->second;
        if (vehicle.snapshots.push(entry.first, entry.second)) {
            vehicle.lastStreamArrival = std::max(vehicle.lastStreamArrival, entry.second);
            ++_stats.streamStatesReceived;
        }
    }
}

void VehicleSimulation::preSubstep(uint32_t step, float dt) {
    drainReceivedStates();
    for (auto& entry : _vehicles) {
        Vehicle& vehicle = entry.second;
        JPH::Body* body = vehicle.state->getBody();
        bool stale = !body || body->GetID() != vehicle.bodyID || !isBuilt(vehicle)
            || vehicle.entity->getVehicleDefinitionVersion() != vehicle.definitionVersion;
        if (stale && !rebuild(vehicle)) {
            continue;
        }
        bool authority = isAuthority(vehicle);
        if (authority != vehicle.wasAuthority) {
            vehicle.wasAuthority = authority;
            vehicle.history.clear();
            {
                std::lock_guard<std::mutex> lock(_inputMutex);
                vehicle.inputs.reset();
            }
            vehicle.lead.reset();
            if (Physics::isServerAuthority() || authority) {
                qCDebug(physics) << "vehicle" << vehicle.entity->getName() << (authority ? "is simulated here" : "is simulated elsewhere")
                    << (Physics::isServerAuthority() ? "(physics server)" : "");
            }
        }
        bool localDriver = !authority && isLocalDriver(vehicle.state);
        if (localDriver != vehicle.wasLocalDriver) {
            vehicle.wasLocalDriver = localDriver;
            if (localDriver) {
                // the wheel taken (again): nothing of an earlier drive is kept. A second sit after standing found the
                // round trips and the lead's window holding the standing time and jumped the clock a second ahead
                // (round 12); the server aligns its buffer afresh from the first input after a gap, so the sequence
                // itself need not start over.
                vehicle.history.clear();
                vehicle.lead.reset();
                vehicle.inputSendTimes.clear();
                vehicle.recent.clear();
                vehicle.unsent.clear();
                vehicle.rtt = 0.0f;
                vehicle.driveStartSequence = vehicle.nextSequence;
                qCDebug(physics) << "vehicle" << vehicle.entity->getName() << "driven from here from input" << vehicle.nextSequence;
            }
        }
        checkCorrection(vehicle, step);
        chooseInput(vehicle, step);
        applyModelInput(vehicle, vehicle.applied, dt);
    }
}

void VehicleSimulation::postSubstep(uint32_t step, float dt) {
    if (needsSnapshots()) {
        takeSnapshot(step);
    } else if (!_snapshots.empty()) {
        _snapshots.clear();
    }
    recordTelemetry();
    captureStream(step);
    for (auto& entry : _vehicles) {
        Vehicle& vehicle = entry.second;
        if (vehicle.pendingRecord && !vehicle.history.empty()) {
            Record& record = vehicle.history.back();
            vehicle.state->getBodyTransform(record.position, record.rotation);
            record.velocity = vehicle.state->getBodyLinearVelocity();
            record.angularVelocity = vehicle.state->getBodyAngularVelocity();
            vehicle.pendingRecord = false;
        }
    }
}

void VehicleSimulation::recordTelemetry() {
    Telemetry t;
    // the vehicle this client drives first: what its scripts read here (the flight assistant's airspeed and tilt, a
    // readout) is that vehicle's; with none driven, the first of the map as before. With a second aircraft in the world
    // the flight assistant flew on a parked one's airspeed and tilt (10-24).
    std::vector<Vehicle*> order;
    order.reserve(_vehicles.size());
    for (auto& entry : _vehicles) {
        if (isLocalDriver(entry.first)) {
            order.insert(order.begin(), &entry.second);
        } else {
            order.push_back(&entry.second);
        }
    }
    for (Vehicle* each : order) {
        Vehicle& vehicle = *each;
        if (vehicle.aircraft && vehicle.aircraftBuilt) {
            const AircraftForces& f = vehicle.aircraftForces;
            t.valid = true;
            t.aircraft = true;
            glm::quat rotation;
            glm::vec3 position;
            vehicle.state->getBodyTransform(position, rotation);
            glm::vec3 velocity = vehicle.state->getBodyLinearVelocity();
            t.speed = glm::dot(velocity, rotation * FORWARD);
            t.climbRate = velocity.y;
            t.altitude = position.y;
            t.airDensity = f.airDensity;
            t.thrust = f.thrust;
            t.tilt = f.tilt;
            t.airspeed = f.airspeed;
            t.angleOfAttack = glm::degrees(f.angleOfAttack);
            t.sideslip = glm::degrees(f.sideslip);
            t.wingLift = f.lift;
            t.wingDrag = f.drag;
            t.liftCoefficient = f.liftCoefficient;
            t.forward = vehicle.applied.throttle;
            t.right = vehicle.applied.steering;
            t.pitch = vehicle.applied.pitch;
            t.lift = vehicle.applied.lift;
            t.trim = vehicle.applied.trim;
            break;
        }
        if (vehicle.airship && vehicle.airshipBuilt) {
            const AirshipForces& f = vehicle.airshipForces;
            t.valid = true;
            t.airship = true;
            glm::quat rotation;
            glm::vec3 position;
            vehicle.state->getBodyTransform(position, rotation);
            glm::vec3 velocity = vehicle.state->getBodyLinearVelocity();
            t.speed = glm::dot(velocity, rotation * FORWARD);
            t.climbRate = velocity.y;
            t.altitude = (position + rotation * (vehicle.airshipSpec.centerOfBuoyancy - vehicle.airshipSpec.centerOfMass)).y;
            t.airDensity = f.airDensity;
            t.buoyancy = f.buoyancy;
            t.ballastWeight = f.ballastWeight;
            t.thrust = f.thrust;
            t.verticalThrust = f.verticalThrust;
            t.airspeed = f.airspeed;
            t.sideslip = glm::degrees(f.sideslip);
            t.incidence = glm::degrees(f.incidence);
            t.ballast = vehicle.airshipState.ballast;
            t.fill = vehicle.airshipState.fill;
            t.forward = vehicle.applied.throttle;
            t.right = vehicle.applied.steering;
            t.pitch = vehicle.applied.pitch;
            t.lift = vehicle.applied.lift;
            t.trim = vehicle.applied.trim;
            break;
        }
        JPH::WheeledVehicleController* controller = controllerOf(vehicle.constraint);
        if (!controller) {
            continue;
        }
        t.valid = true;
        t.engineRPM = controller->GetEngine().GetCurrentRPM();
        t.gear = controller->GetTransmission().GetCurrentGear();
        t.clutch = controller->GetTransmission().GetClutchFriction();
        t.forward = controller->GetForwardInput();
        t.right = controller->GetRightInput();
        t.brake = controller->GetBrakeInput();
        t.handBrake = controller->GetHandBrakeInput();
        glm::quat rotation;
        glm::vec3 position;
        vehicle.state->getBodyTransform(position, rotation);
        t.speed = glm::dot(vehicle.state->getBodyLinearVelocity(), rotation * FORWARD);
        for (const JPH::Wheel* wheel : vehicle.constraint->GetWheels()) {
            t.wheelRPM.push_back(wheel->GetAngularVelocity() * 60.0f / TWO_PI);
            t.wheelSlip.push_back(static_cast<const JPH::WheelWV*>(wheel)->mLongitudinalSlip);
            t.suspension.push_back(wheel->GetSuspensionLength());
            t.contact.push_back(wheel->HasContact());
        }
        break;
    }
    std::lock_guard<std::mutex> lock(_telemetryMutex);
    _telemetry = std::move(t);
}

// The ring of world snapshots is for rewinding: the driver's reconciliation, and a follower's correction from the
// relay. A follower on a live stream interpolates and never rewinds, so it needs none.
bool VehicleSimulation::needsSnapshots() const {
    quint64 now = usecTimestampNow();
    for (const auto& entry : _vehicles) {
        const Vehicle& vehicle = entry.second;
        if (isBuilt(vehicle) && !isAuthority(vehicle) && (isLocalDriver(vehicle.state) || !streamLive(vehicle, now))) {
            return true;
        }
    }
    return false;
}

int VehicleSimulation::relayUpdateSteps() {
    return Physics::isServerAuthority() ? RELAY_UPDATE_STEPS : UPDATE_STEPS;
}

// Which input this substep applies: the authority takes its driver's (local controls here, or the queue of a remote
// driver's packets, one per substep); a driver who is not the authority predicts with its own and records it; everyone
// else runs the authority's last applied input on.
void VehicleSimulation::chooseInput(Vehicle& vehicle, uint32_t step) {
    quint64 now = usecTimestampNow();
    VehicleInput local;
    bool haveLocal = localControls(vehicle, local);
    if (haveLocal) {
        shapeSteering(vehicle, local);
    } else {
        vehicle.steering = 0.0f;   // the next driver starts with the wheels straight
    }
    const VehiclePropertyGroup& group = vehicle.entity->getVehicleProperties();

    if (isAuthority(vehicle)) {
        if (haveLocal) {
            local.sequence = vehicle.nextSequence++;
            vehicle.applied = local;
            vehicle.lastInputTime = now;
            return;
        }
        std::lock_guard<std::mutex> lock(_inputMutex);
        // the input buffer (round 10): this substep's input is the one the driver's tick names, held from the last
        // when none arrived for it
        VehicleInput input;
        bool taken = vehicle.inputs.take(step, input);
        // the tick and margin the stream reports; none while the buffer is quiet (a second sit found the margin of the
        // standing time, hundreds of ticks, clamped to -127 in the first states after the wheel was taken again, and the
        // driver's lead jumped its clock a second ahead on it, round 12)
        bool quiet = vehicle.inputs.quiet(step);
        vehicle.lastTick = quiet ? 0 : vehicle.inputs.tickForStep(step);
        vehicle.lastMargin = quiet ? 0 : vehicle.inputs.margin(step);
        _stats.bufferMargin = vehicle.lastMargin;
        if (taken) {
            vehicle.applied = input;
            vehicle.lastInputTime = now;
            return;
        }
        if (vehicle.inputs.aligned() && vehicle.applied.sequence != 0 && !group.getDriver().isNull()) {
            ++_stats.inputsStarved;   // the driver's input for this substep is not here: hold the last
        }
        if (group.getDriver().isNull() || (vehicle.lastInputTime != 0 && now - vehicle.lastInputTime > DRIVER_SILENCE)) {
            // nobody at the wheel, or the driver has gone quiet: ease off and hold the brake
            easeOff(vehicle);
        }
        return;
    }

    if (haveLocal && (group.getDriver() == Physics::getSessionUUID())) {
        // the driver, predicting
        local.sequence = vehicle.nextSequence++;
        vehicle.applied = local;
        Record record;
        record.sequence = local.sequence;
        record.step = step;
        record.input = local;
        vehicle.history.push_back(record);
        while ((int)vehicle.history.size() > SNAPSHOT_RING) {
            vehicle.history.pop_front();
        }
        vehicle.pendingRecord = true;
        vehicle.unsent.push_back(local);
        return;
    }

    // a follower on a live stream: the input followStream chose from the states (round 9)
    if (streamLive(vehicle, now)) {
        return;
    }
    // a follower: the authority's last applied input; when the authority has gone quiet (its states are older than
    // the driver-silence limit) the vehicle eases off and holds the brake, as the authority would for a silent driver,
    // rather than driving on with the last input for ever
    vehicle.applied = inputFromState(group);
    quint64 stateLocalTime = group.getStateTime() ? TrackSimulation::localTime(group.getStateTime()) : 0;
    if (stateLocalTime == 0 || now > stateLocalTime + DRIVER_SILENCE) {
        easeOff(vehicle);
    }
}

// The local driver's steering (keyboard, gamepad or script), eased toward the ask and held to what the tyres take at
// this speed (WheeledVehicleSpec::steeringFor), before it is applied, recorded and sent: the authority applies what the
// driver saw, a 1.0.2 server included. Wheeled vehicles only; the airship's rudder and the aircraft's flight assistant
// have their own.
void VehicleSimulation::shapeSteering(Vehicle& vehicle, VehicleInput& input) {
    if (vehicle.airship || vehicle.aircraft) {
        return;
    }
    float speed = 0.0f;
    if (JPH::Body* body = vehicle.state->getBody()) {
        // over the ground, not along the nose: a slide must not raise the limit and feed itself
        glm::vec3 velocity = joltToGLM(body->GetLinearVelocity());
        glm::vec3 gravity = vehicle.state->getBodyGravity();
        glm::vec3 up = glm::length2(gravity) > 0.0f ? -glm::normalize(gravity) : glm::vec3(0.0f, 1.0f, 0.0f);
        speed = glm::length(velocity - up * glm::dot(velocity, up));
    }
    input.steering = vehicle.spec.steeringFor(vehicle.steering, input.steering, speed, PHYSICS_ENGINE_FIXED_SUBSTEP);
    vehicle.steering = input.steering;
}

// Nobody is driving, or whoever was has gone quiet: a car eases off and holds the brake, an airship stops thrusting,
// centres its fins and leaves ballast and gas alone (it drifts, as an airship does).
void VehicleSimulation::easeOff(Vehicle& vehicle) {
    vehicle.applied.throttle = 0.0f;
    vehicle.applied.steering = 0.0f;
    vehicle.applied.pitch = 0.0f;
    vehicle.applied.lift = 0.0f;
    vehicle.applied.trim = 0.0f;
    if (!vehicle.airship) {
        vehicle.applied.brake = glm::max(vehicle.applied.brake, 0.5f);
    }
}

// The model takes this substep's input. A wheeled vehicle's controller holds it until the next; an airship's forces
// are added to its body for the coming update (Jolt clears them after), with the ballast and gas integrated first.
void VehicleSimulation::applyModelInput(Vehicle& vehicle, const VehicleInput& input, float dt) {
    PhysicsWorld* world = vehicle.state->getWorld();
    if (!world) {
        return;
    }
    JPH::BodyInterface& bodies = world->getBodyInterface();
    if (vehicle.aircraft) {
        if (!vehicle.aircraftBuilt) {
            return;
        }
        AircraftModel::integrateTilt(vehicle.aircraftSpec, vehicle.aircraftState, input, dt);
        JPH::Body* body = vehicle.state->getBody();
        if (!body) {
            return;
        }
        glm::vec3 position = joltToGLM(body->GetCenterOfMassPosition());
        glm::quat rotation = glm::normalize(joltToGLM(body->GetRotation()));
        glm::vec3 velocity = joltToGLM(body->GetLinearVelocity());
        glm::vec3 angularVelocity = joltToGLM(body->GetAngularVelocity());
        float gravity = glm::length(vehicle.state->getBodyGravity());
        if (gravity <= 0.0f) {
            gravity = GRAVITY;
        }
        vehicle.aircraftForces = AircraftModel::forces(vehicle.aircraftSpec, vehicle.aircraftState, input, position, rotation, velocity,
                                                       angularVelocity, gravity);
        for (const AirshipForce& f : vehicle.aircraftForces.forces) {
            bodies.AddForce(vehicle.bodyID, glmToJolt(f.force), glmToJoltR(f.point), JPH::EActivation::Activate);
        }
        if (glm::length2(vehicle.aircraftForces.torque) > 0.0f) {
            bodies.AddTorque(vehicle.bodyID, glmToJolt(vehicle.aircraftForces.torque), JPH::EActivation::Activate);
        }
        return;
    }
    if (vehicle.airship) {
        if (!vehicle.airshipBuilt) {
            return;
        }
        AirshipModel::integrateTrim(vehicle.airshipSpec, vehicle.airshipState, input, dt);
        JPH::Body* body = vehicle.state->getBody();
        if (!body) {
            return;
        }
        glm::vec3 position = joltToGLM(body->GetCenterOfMassPosition());
        glm::quat rotation = glm::normalize(joltToGLM(body->GetRotation()));
        glm::vec3 velocity = joltToGLM(body->GetLinearVelocity());
        glm::vec3 angularVelocity = joltToGLM(body->GetAngularVelocity());
        float gravity = glm::length(vehicle.state->getBodyGravity());
        if (gravity <= 0.0f) {
            gravity = GRAVITY;
        }
        vehicle.airshipForces = AirshipModel::forces(vehicle.airshipSpec, vehicle.airshipState, input, position, rotation, velocity,
                                                     angularVelocity, gravity);
        for (const AirshipForce& f : vehicle.airshipForces.forces) {
            bodies.AddForce(vehicle.bodyID, glmToJolt(f.force), glmToJoltR(f.point), JPH::EActivation::Activate);
        }
        if (glm::length2(vehicle.airshipForces.torque) > 0.0f) {
            bodies.AddTorque(vehicle.bodyID, glmToJolt(vehicle.airshipForces.torque), JPH::EActivation::Activate);
        }
        return;
    }
    JPH::WheeledVehicleController* controller = controllerOf(vehicle.constraint);
    if (!controller) {
        return;
    }
    controller->SetDriverInput(input.throttle, input.steering, input.brake, input.handBrake);
    if (!input.isIdle()) {
        bodies.ActivateBody(vehicle.bodyID);
    }
}

void VehicleSimulation::checkCorrection(Vehicle& vehicle, uint32_t step) {
    EntityItem::NetworkPose pose = vehicle.entity->getNetworkPose();
    bool newPose = pose.version != vehicle.networkPoseVersion;
    vehicle.networkPoseVersion = pose.version;
    if (!isAuthority(vehicle) && streamLive(vehicle, usecTimestampNow())) {
        // the stream carries the authority's state (round 9); the relay's pose, older, is not used while it is live
        if (isLocalDriver(vehicle.state)) {
            reconcileStream(vehicle, step);
        } else {
            followStream(vehicle, step);
        }
        return;
    }
    if (!newPose) {
        return;
    }
    if (isAuthority(vehicle)) {
        // (before its first update the authority cannot tell its own bid echoed from an edit: nothing is adopted)
        // (and the teleport it last took, delivered again while the entity server holds it, is the same edit)
        const float SAME_TELEPORT_DISTANCE = 0.01f;   // m
        const float SAME_TELEPORT_ANGLE = 0.01f;      // rad
        bool sameTeleport = vehicle.teleported && glm::distance(pose.position, vehicle.teleportPosition) < SAME_TELEPORT_DISTANCE
            && angleBetween(pose.rotation, vehicle.teleportRotation) < SAME_TELEPORT_ANGLE;
        // An edit delivered seconds late describes where the vehicle was, not where it is put: the entity server's
        // stream to the physics server lagged 14 to 25 s on 10-13 under a crew walking the deck, and the pilot's edits of
        // the ship's description, each streamed with the pose of its moment, threw the ship 200 m back when they landed.
        const int64_t STALE_EDIT_AGE_MS = 3000;
        int64_t ageMs = ((int64_t)usecTimestampNow() - (int64_t)pose.lastEdited) / 1000;
        if (pose.teleport && !sameTeleport && !vehicle.sent.empty() && ageMs > STALE_EDIT_AGE_MS
                && !recentlySent(vehicle, pose.position, pose.rotation)) {
            qCDebug(physics) << "vehicle" << vehicle.entity->getName() << "edit" << ageMs << "ms old ignored, not a teleport:"
                << pose.position.x << pose.position.y << pose.position.z << "edited by" << pose.editedBy;
            return;
        }
        if (pose.teleport && !sameTeleport && !vehicle.sent.empty() && !recentlySent(vehicle, pose.position, pose.rotation)) {
            // an explicit edit by someone else (the editor moving the ship) while this process drives it: adopted as a
            // teleport, standing still where it was put (PHYSICS.md round 7); the next update carries it to everyone
            glm::vec3 before;
            glm::quat beforeRotation;
            vehicle.state->getBodyTransform(before, beforeRotation);
            setBodyState(vehicle, pose.position, pose.rotation, glm::vec3(0.0f), glm::vec3(0.0f));
            vehicle.history.clear();
            vehicle.teleported = true;
            vehicle.teleportPosition = pose.position;
            vehicle.teleportRotation = pose.rotation;
            float nearest = std::numeric_limits<float>::max();
            float nearestSpeed = 0.0f;
            for (const auto& sent : vehicle.sent) {
                float d = glm::distance(sent.position, pose.position);
                if (d < nearest) {
                    nearest = d;
                    nearestSpeed = glm::length(sent.velocity);
                }
            }
            qCDebug(physics) << "vehicle" << vehicle.entity->getName() << "teleported by an edit:" << glm::distance(before, pose.position)
                << "m to" << pose.position.x << pose.position.y << pose.position.z << "; the nearest of" << (int)vehicle.sent.size()
                << "sent poses" << nearest << "m away at" << nearestSpeed << "m/s; edited by" << pose.editedBy
                << (int64_t)(usecTimestampNow() - pose.lastEdited) / 1000 << "ms ago" << (Physics::isServerAuthority() ? "(physics server)" : "");
        }
        return;
    }
    const VehiclePropertyGroup& group = vehicle.entity->getVehicleProperties();
    quint64 stateTime = group.getStateTime();
    if (stateTime == 0 || stateTime <= vehicle.lastStateTime) {
        return;   // no state, or one already adopted
    }
    vehicle.lastStateTime = stateTime;
    vehicle.extrapolating = true;   // a follower: the next stream placement, if the stream comes back, blends from here
    VehicleState state = stateFromRelay(vehicle);
    if (isLocalDriver(vehicle.state)) {
        correctDriver(vehicle, state, step, DRIVER_BLEND_TIME);
    } else {
        correctFollower(vehicle, state, step);
    }
}

// The entity's relayed state (the round 6 path): the pose the entity server delivered and the vehicle group's state,
// its domain time made local.
VehicleState VehicleSimulation::stateFromRelay(const Vehicle& vehicle) const {
    const VehiclePropertyGroup& group = vehicle.entity->getVehicleProperties();
    EntityItem::NetworkPose pose = vehicle.entity->getNetworkPose();
    VehicleState s;
    s.id = vehicle.entity->getID();
    s.time = group.getStateTime() ? TrackSimulation::localTime(group.getStateTime()) : 0;
    s.position = pose.position;
    s.rotation = pose.rotation;
    s.velocity = pose.velocity;
    s.angularVelocity = pose.angularVelocity;
    s.input = inputFromState(group);
    s.engineRPM = group.getEngineRPM();
    s.gear = (int8_t)group.getGear();
    s.ballast = group.getBallast();
    s.fill = group.getFill();
    s.tilt = group.getTilt();
    return s;
}

// The authority's own state after a substep, for the stream.
VehicleState VehicleSimulation::stateFromBody(const Vehicle& vehicle, uint32_t step) const {
    VehicleState s;
    s.id = vehicle.entity->getID();
    s.time = ObjectMotionState::getSubstepTime();
    s.step = step;
    vehicle.state->getBodyTransform(s.position, s.rotation);
    s.velocity = vehicle.state->getBodyLinearVelocity();
    s.angularVelocity = vehicle.state->getBodyAngularVelocity();
    s.input = vehicle.applied;
    if (JPH::WheeledVehicleController* controller = controllerOf(vehicle.constraint)) {
        s.engineRPM = controller->GetEngine().GetCurrentRPM();
        s.gear = (int8_t)controller->GetTransmission().GetCurrentGear();
    }
    s.ballast = vehicle.airshipState.ballast;
    s.fill = vehicle.airshipState.fill;
    s.tilt = vehicle.aircraftState.tilt;
    s.tick = vehicle.lastTick;
    s.margin = (int8_t)glm::clamp(vehicle.lastMargin, -127, 127);
    return s;
}

// The server's side of the stream: every STREAM_STEPS substeps, the state of every vehicle it simulates, to go out
// from sendStream.
void VehicleSimulation::captureStream(uint32_t step) {
    if (!Physics::isServerAuthority() || step % STREAM_STEPS != 0) {
        return;
    }
    _pendingStream.clear();
    for (auto& entry : _vehicles) {
        Vehicle& vehicle = entry.second;
        JPH::Body* body = vehicle.state->getBody();
        if (!isBuilt(vehicle) || !isAuthority(vehicle) || !body) {
            continue;
        }
        // a parked vehicle (its body asleep) is streamed for a second more, then not at all: its followers keep the
        // last state, and the first state after it wakes blends from there
        if (body->IsActive()) {
            vehicle.lastActiveStep = step;
        } else if (vehicle.lastActiveStep == 0 || step > vehicle.lastActiveStep + STREAM_PARKED_STEPS) {
            continue;
        }
        _pendingStream.push_back(stateFromBody(vehicle, step));
    }
}

// A follower on the stream (snapshot interpolation, PHYSICS.md 4.6): the vehicle is shown a little behind the newest
// state, the body placed every substep on the line between the two states that bracket that time, so it moves as the
// authority's did; when no state is past the shown time yet (a late or lost packet) the body runs on under the model
// from where it is, with the newest input, and the first placement after that blends the difference away.
void VehicleSimulation::followStream(Vehicle& vehicle, uint32_t step) {
    SnapshotBuffer& buffer = vehicle.snapshots;
    if (buffer.empty()) {
        return;
    }
    quint64 substepTime = ObjectMotionState::getSubstepTime();
    quint64 now = substepTime ? substepTime : usecTimestampNow();
    quint64 delay = (quint64)(buffer.delay() * (float)USECS_PER_SECOND);
    quint64 shownTime = now > delay ? now - delay : 0;
    _stats.delayMs = buffer.delay() * 1000.0f;
    _stats.streamIntervalMs = buffer.interval() * 1000.0f;
    _stats.streamJitterMs = buffer.jitter() * 1000.0f;
    VehicleState s;
    if (!buffer.sample(shownTime, s)) {
        ++_stats.streamExtrapolations;
        vehicle.applied = buffer.newest().input;
        vehicle.extrapolating = true;
        return;
    }
    glm::vec3 shownPosition = vehicle.entity->getWorldPosition();
    glm::quat shownRotation = vehicle.entity->getWorldOrientation();
    glm::vec3 before;
    glm::quat beforeRotation;
    vehicle.state->getBodyTransform(before, beforeRotation);
    setBodyState(vehicle, s.position, s.rotation, s.velocity, s.angularVelocity);
    setInternalState(vehicle, s);
    vehicle.applied = s.input;
    if (vehicle.extrapolating) {
        float error = glm::distance(before, s.position);
        if (error > REPLAY_POSITION_THRESHOLD) {
            ++_stats.corrections;
            _stats.lastError = error;
            _stats.maxError = glm::max(_stats.maxError, error);
            _corrections.note(error, error > SNAP_DISTANCE ? 0.0f : error / OTHERS_BLEND_TIME, error > SNAP_DISTANCE);
        }
        smooth(vehicle, shownPosition, shownRotation, OTHERS_BLEND_TIME);
        vehicle.extrapolating = false;
    }
}

// The driver on the stream: every new state is an acknowledgement of an input, reconciled against the record of that
// input (correctDriver), the residual blended over the measured round trip rather than a fixed time.
void VehicleSimulation::reconcileStream(Vehicle& vehicle, uint32_t step) {
    SnapshotBuffer& buffer = vehicle.snapshots;
    if (buffer.empty() || buffer.newest().time <= vehicle.lastStreamTime) {
        return;
    }
    const VehicleState newest = buffer.newest();
    vehicle.lastStreamTime = newest.time;
    quint64 now = usecTimestampNow();
    uint32_t ack = newest.tick != 0 ? newest.tick : newest.input.sequence;
    // a state acked outside what this sit has sent says nothing about its lead or round trip: before its first input, or
    // past its newest (the server's tick under an alignment the sit's first input then replaces)
    bool thisDrive = (int32_t)(ack - vehicle.driveStartSequence) >= 0 && (int32_t)(ack - vehicle.nextSequence) < 0;
    if (thisDrive) {
        noteRoundTrip(vehicle, ack, now);
    }
    correctDriver(vehicle, newest, step, driverBlendTime(vehicle));
    if (newest.tick != 0 && thisDrive) {
        // the lead (round 10): the server's margin at that substep, and a shift of this client's ticks when the least
        // over a window says so: a few ticks by time dilation, a stall's worth by a jump of the counter
        double t = (double)now * 1.0e-6;
        vehicle.lead.note(newest.margin, t);
        _stats.bufferMargin = newest.margin;
        int shift = vehicle.lead.shift(t);
        if (shift != 0) {
            ++_stats.leadShifts;
            if (LeadController::isJump(shift)) {
                ++_stats.leadJumps;
                vehicle.nextSequence += (uint32_t)shift;
                qCDebug(physics) << "vehicle" << vehicle.entity->getName() << "lead: jumped" << shift << "ticks ahead (the least margin"
                    << vehicle.lead.lastMinimum() << ")";
            } else if (_engine) {
                _engine->requestTimeShift((float)shift * PHYSICS_ENGINE_FIXED_SUBSTEP);
                qCDebug(physics) << "vehicle" << vehicle.entity->getName() << "lead: shifting" << shift << "ticks by time dilation (the least margin"
                    << vehicle.lead.lastMinimum() << ")";
            }
        }
    }
    _stats.timeShiftMs = _engine ? _engine->getTimeShiftPending() * 1000.0f : 0.0f;
}

// The round trip from an input's first sending to the state that acknowledges it, smoothed.
void VehicleSimulation::noteRoundTrip(Vehicle& vehicle, uint32_t ack, quint64 now) {
    quint64 sentAt = 0;
    while (!vehicle.inputSendTimes.empty() && vehicle.inputSendTimes.front().first <= ack) {
        if (vehicle.inputSendTimes.front().first == ack) {
            sentAt = vehicle.inputSendTimes.front().second;
        }
        vehicle.inputSendTimes.pop_front();
    }
    if (sentAt == 0 || now < sentAt) {
        return;
    }
    float sample = (float)(now - sentAt) * 1.0e-6f;
    vehicle.rtt = vehicle.rtt > 0.0f ? vehicle.rtt + RTT_SMOOTHING * (sample - vehicle.rtt) : sample;
    _stats.rttMs = vehicle.rtt * 1000.0f;
}

float VehicleSimulation::driverBlendTime(const Vehicle& vehicle) const {
    return vehicle.rtt > 0.0f ? glm::clamp(vehicle.rtt, DRIVER_BLEND_MIN, DRIVER_BLEND_MAX) : DRIVER_BLEND_TIME;
}

// Whether a pose is one this authority published lately (its own update, echoed) or a stranger's. The entity server
// moves what it holds on by its velocities between updates, so an echo is a sent pose plus up to a few hundred
// milliseconds of its motion: the tolerance is a few centimetres plus half a second of the sent velocity.
bool VehicleSimulation::recentlySent(const Vehicle& vehicle, const glm::vec3& position, const glm::quat& rotation) const {
    const float POSITION_TOLERANCE = 0.05f;   // m
    const float ANGLE_TOLERANCE = 0.02f;      // rad
    const float ECHO_TIME = 0.5f;             // s of the sent motion an echo may carry
    for (const auto& pose : vehicle.sent) {
        float positionTolerance = POSITION_TOLERANCE + ECHO_TIME * glm::length(pose.velocity);
        float angleTolerance = ANGLE_TOLERANCE + ECHO_TIME * glm::length(pose.angularVelocity);
        if (glm::distance2(pose.position, position) < positionTolerance * positionTolerance
                && angleBetween(pose.rotation, rotation) < angleTolerance) {
            return true;
        }
    }
    return false;
}

// FROZEN (vehicle path): see the note at the top of this file.
void VehicleSimulation::setBodyState(Vehicle& vehicle, const glm::vec3& position, const glm::quat& rotation,
                                     const glm::vec3& velocity, const glm::vec3& angularVelocity) {
    PhysicsWorld* world = vehicle.state->getWorld();
    if (!world) {
        return;
    }
    JPH::BodyInterface& bodies = world->getBodyInterface();
    bodies.SetPositionAndRotation(vehicle.bodyID, glmToJoltR(position), glmToJolt(glm::normalize(rotation)), JPH::EActivation::Activate);
    bodies.SetLinearAndAngularVelocity(vehicle.bodyID, glmToJolt(velocity), glmToJolt(angularVelocity));
}

// FROZEN (vehicle path): see the note at the top of this file.
void VehicleSimulation::setInternalState(Vehicle& vehicle, const VehicleState& state) {
    if (vehicle.aircraft) {
        vehicle.aircraftState.tilt = glm::clamp(state.tilt, 0.0f, 1.0f);
        return;
    }
    if (vehicle.airship) {
        // the author's ballast and fill the first time, the authority's after
        vehicle.airshipState.ballast = glm::max(0.0f, state.ballast);
        vehicle.airshipState.fill = glm::clamp(state.fill, 0.0f, 1.0f);
        return;
    }
    JPH::WheeledVehicleController* controller = controllerOf(vehicle.constraint);
    if (!controller) {
        return;
    }
    if (state.time != 0) {
        controller->GetEngine().SetCurrentRPM(state.engineRPM);
        controller->GetTransmission().Set(state.gear, 1.0f);
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
// The driver's reconciliation: the record for the sequence the authority reached is compared with its state; within the
// thresholds the prediction stands, else the world is rewound to that substep and the inputs since are replayed.
void VehicleSimulation::correctDriver(Vehicle& vehicle, const VehicleState& pose, uint32_t step, float blendTime) {
    // the record to compare: the tick the state's substep applied (round 10's buffer, which may have held an older
    // input for it), or, from a relay or a server without the buffer, the input it applied
    uint32_t ack = pose.tick != 0 ? pose.tick : pose.input.sequence;
    const Record* acked = nullptr;
    for (auto it = vehicle.history.rbegin(); it != vehicle.history.rend(); ++it) {
        if (it->sequence == ack) {
            acked = &(*it);
            break;
        }
        if (it->sequence < ack) {
            break;
        }
    }
    if (!acked) {
        // before this client's first input, or older than the history: the state is adopted as a follower would
        correctFollower(vehicle, pose, step);
        return;
    }
    if (glm::length2(pose.position - acked->position) > 1.0e6f) {
        // a thousand metres off: not a state of this flight (an echo of an old pose through the relay, round 9's
        // baseline saw 1.35 km); taken as the follower would, logged, never replayed through
        qCDebug(physics) << "vehicle" << vehicle.entity->getName() << "state" << glm::distance(pose.position, acked->position)
            << "m from the prediction at input" << ack << ": adopted outright";
        setBodyState(vehicle, pose.position, pose.rotation, pose.velocity, pose.angularVelocity);
        setInternalState(vehicle, pose);
        vehicle.history.clear();
        vehicle.state->setCorrection(glm::vec3(0.0f), glm::quat(), 0.0f);
        return;
    }
    float error = glm::distance(acked->position, pose.position);
    float angle = angleBetween(acked->rotation, pose.rotation);
    if (error < REPLAY_POSITION_THRESHOLD && angle < REPLAY_ANGLE_THRESHOLD) {
        return;   // the prediction holds
    }
    ++_stats.corrections;
    _stats.lastError = error;
    _stats.maxError = glm::max(_stats.maxError, error);
    _corrections.note(error, error > SNAP_DISTANCE ? 0.0f : error / blendTime, error > SNAP_DISTANCE);

    // every correction, for the parity measurement (PHYSICS.md round 9, item 3): the error in the vehicle's own axes
    // (right, up, forward), the speed, the height above whatever is below, so a log can tell a timing error (along the
    // velocity) from a ground one (up, near the ground)
    {
        glm::vec3 delta = pose.position - acked->position;
        glm::vec3 local = glm::inverse(acked->rotation) * delta;
        float height = -1.0f;
        if (PhysicsWorld* world = rootWorld()) {
            RayHit hit;
            glm::vec3 start = acked->position - glm::vec3(0.0f, 1.0f, 0.0f);
            if (world->castRayClosest(start, start - glm::vec3(0.0f, 200.0f, 0.0f), BULLET_COLLISION_GROUP_DYNAMIC,
                                      BULLET_COLLISION_MASK_DYNAMIC, nullptr, hit)) {
                height = 1.0f + hit.fraction * 200.0f;
            }
        }
        qCDebug(physics) << "parity" << vehicle.entity->getName() << "input" << ack << "error" << error << "m right" << local.x
            << "up" << local.y << "forward" << -local.z << "angle" << glm::degrees(angle) << "deg; speed"
            << glm::length(acked->velocity) << "m/s, height" << height << "m, y" << acked->position.y;
    }

    glm::vec3 shownPosition = vehicle.entity->getWorldPosition();
    glm::quat shownRotation = vehicle.entity->getWorldOrientation();
    uint32_t fromStep = acked->step;
    uint32_t toStep = step - 1;
    if (toStep < fromStep || toStep - fromStep > (uint32_t)MAX_REPLAY_STEPS) {
        // too far back to replay: take the state and run on from it
        setBodyState(vehicle, pose.position, pose.rotation, pose.velocity, pose.angularVelocity);
        setInternalState(vehicle, pose);
    } else {
        std::map<uint32_t, VehicleInput> inputs;
        for (const Record& record : vehicle.history) {
            inputs[record.step] = record.input;
        }
        VehicleInput last = acked->input;
        replay(vehicle, pose, fromStep, toStep, [&](uint32_t s) {
            auto it = inputs.find(s);
            if (it != inputs.end()) {
                last = it->second;
            }
            return last;
        });
    }
    if (error > 0.25f || angle > 0.17f) {
        qCDebug(physics) << "vehicle" << vehicle.entity->getName() << "corrected by" << error << "m" << glm::degrees(angle)
            << "deg at input" << ack << "(" << (toStep - fromStep) << "substeps replayed, blend" << blendTime << "s)";
    }
    smooth(vehicle, shownPosition, shownRotation, blendTime);
}

// FROZEN (vehicle path): see the note at the top of this file.
// Everyone else: rewind to the state's time and run the same vehicle on from it with the authority's last inputs.
void VehicleSimulation::correctFollower(Vehicle& vehicle, const VehicleState& pose, uint32_t step) {
    quint64 stateLocalTime = pose.time;
    glm::vec3 shownPosition = vehicle.entity->getWorldPosition();
    glm::quat shownRotation = vehicle.entity->getWorldOrientation();
    glm::vec3 bodyPosition;
    glm::quat bodyRotation;
    vehicle.state->getBodyTransform(bodyPosition, bodyRotation);

    const Snapshot* snapshot = snapshotBefore(stateLocalTime);
    uint32_t toStep = step - 1;
    if (!snapshot || snapshot->step >= step || toStep - snapshot->step > (uint32_t)MAX_REPLAY_STEPS) {
        // nothing to rewind to: the state, read ahead by its age along its velocity
        quint64 now = usecTimestampNow();
        float age = now > stateLocalTime ? glm::min(0.5f, (float)(now - stateLocalTime) * 1.0e-6f) : 0.0f;
        glm::vec3 position = pose.position + pose.velocity * age;
        glm::quat rotation = pose.rotation;
        if (glm::length2(pose.angularVelocity) > 0.0f && age > 0.0f) {
            rotation = glm::normalize(computeBulletRotationStep(pose.angularVelocity, age) * rotation);
        }
        setBodyState(vehicle, position, rotation, pose.velocity, pose.angularVelocity);
        setInternalState(vehicle, pose);
    } else {
        VehicleInput input = pose.input;
        replay(vehicle, pose, snapshot->step, toStep, [&](uint32_t) { return input; });
    }
    float error = glm::distance(bodyPosition, shownPosition);
    glm::vec3 after;
    glm::quat afterRotation;
    vehicle.state->getBodyTransform(after, afterRotation);
    error = glm::distance(after, bodyPosition);
    if (error > REPLAY_POSITION_THRESHOLD) {
        ++_stats.corrections;
        _stats.lastError = error;
        _stats.maxError = glm::max(_stats.maxError, error);
        _corrections.note(error, error > SNAP_DISTANCE ? 0.0f : error / OTHERS_BLEND_TIME, error > SNAP_DISTANCE);
    }
    smooth(vehicle, shownPosition, shownRotation, OTHERS_BLEND_TIME);
}

// Rewind: the other bodies back to the snapshot after fromStep, the vehicle to the authority's state; then every
// substep up to toStep again, with the inputs the caller names, re-recording the driver's history and the snapshots.
void VehicleSimulation::replay(Vehicle& vehicle, const VehicleState& pose, uint32_t fromStep, uint32_t toStep,
                               const std::function<VehicleInput(uint32_t)>& inputAt) {
    PhysicsWorld* world = rootWorld();
    if (!world || !isBuilt(vehicle)) {
        return;
    }
    const Snapshot* snapshot = snapshotAt(fromStep);
    if (snapshot) {
        world->restoreBodies(snapshot->bodies, JPH::BodyID());
    }
    setBodyState(vehicle, pose.position, pose.rotation, pose.velocity, pose.angularVelocity);
    setInternalState(vehicle, pose);

    for (uint32_t s = fromStep + 1; s <= toStep; ++s) {
        VehicleInput input = inputAt(s);
        applyModelInput(vehicle, input, PHYSICS_ENGINE_FIXED_SUBSTEP);
        world->replayStep(PHYSICS_ENGINE_FIXED_SUBSTEP);
        ++_stats.replaySteps;
        for (Record& record : vehicle.history) {
            if (record.step == s) {
                vehicle.state->getBodyTransform(record.position, record.rotation);
                record.velocity = vehicle.state->getBodyLinearVelocity();
                record.angularVelocity = vehicle.state->getBodyAngularVelocity();
                break;
            }
        }
        for (Snapshot& old : _snapshots) {
            if (old.step == s) {
                old.bodies = world->saveBodies(JPH::BodyID());
                break;
            }
        }
    }
    // the input the coming substep would otherwise inherit (a wheeled controller holds its last; an airship's forces
    // are added fresh each substep)
    if (JPH::WheeledVehicleController* controller = controllerOf(vehicle.constraint)) {
        controller->SetDriverInput(vehicle.applied.throttle, vehicle.applied.steering, vehicle.applied.brake, vehicle.applied.handBrake);
    }
}

// The jump a correction makes is kept out of what is shown: the difference decays on the entity's shown pose.
void VehicleSimulation::smooth(Vehicle& vehicle, const glm::vec3& shownPosition, const glm::quat& shownRotation, float blendTime) {
    glm::vec3 position;
    glm::quat rotation;
    vehicle.state->getBodyTransform(position, rotation);
    glm::vec3 offset = shownPosition - position;
    if (glm::length2(offset) > SNAP_DISTANCE * SNAP_DISTANCE) {
        vehicle.state->setCorrection(glm::vec3(0.0f), glm::quat(), 0.0f);
        return;
    }
    glm::quat residual = glm::normalize(shownRotation * glm::inverse(rotation));
    vehicle.state->setCorrection(offset, residual, blendTime);
}

void VehicleSimulation::takeSnapshot(uint32_t step) {
    PhysicsWorld* world = rootWorld();
    if (!world) {
        return;
    }
    Snapshot snapshot;
    snapshot.step = step;
    snapshot.time = ObjectMotionState::getSubstepTime();
    snapshot.bodies = world->saveBodies(JPH::BodyID());
    _snapshots.push_back(std::move(snapshot));
    while ((int)_snapshots.size() > SNAPSHOT_RING) {
        _snapshots.pop_front();
    }
}

const VehicleSimulation::Snapshot* VehicleSimulation::snapshotAt(uint32_t step) const {
    for (auto it = _snapshots.rbegin(); it != _snapshots.rend(); ++it) {
        if (it->step == step) {
            return &(*it);
        }
        if (it->step < step) {
            break;
        }
    }
    return nullptr;
}

const VehicleSimulation::Snapshot* VehicleSimulation::snapshotBefore(quint64 time) const {
    for (auto it = _snapshots.rbegin(); it != _snapshots.rend(); ++it) {
        if (it->time <= time) {
            return &(*it);
        }
    }
    return nullptr;
}

void VehicleSimulation::scanVisuals(Vehicle& vehicle, uint32_t step) {
    vehicle.visuals.clear();
    vehicle.visualsScanStep = step;
    int count = 0;
    vehicle.entity->forEachChild([&](SpatiallyNestablePointer child) {
        ++count;
        if (child->getNestableType() != NestableType::Entity) {
            return;
        }
        EntityItemPointer childEntity = std::static_pointer_cast<EntityItem>(child);
        if (!childEntity->isWheel()) {
            return;
        }
        const VehiclePropertyGroup& group = childEntity->getVehicleProperties();
        for (size_t i = 0; i < vehicle.spec.wheels.size(); ++i) {
            if (vehicle.spec.wheels[i].name == group.getWheel()) {
                Visual visual;
                visual.entity = childEntity;
                visual.wheel = (int)i;
                float side = vehicle.spec.wheels[i].center.x >= 0.0f ? 1.0f : -1.0f;
                QString axle = group.getAxle().trimmed().toLower();
                if (axle == "y") {
                    visual.right = glm::vec3(0.0f, side, 0.0f);
                    visual.up = glm::vec3(0.0f, 0.0f, 1.0f);
                } else if (axle == "z") {
                    visual.right = glm::vec3(0.0f, 0.0f, side);
                    visual.up = glm::vec3(0.0f, 1.0f, 0.0f);
                } else {
                    visual.right = glm::vec3(side, 0.0f, 0.0f);
                    visual.up = glm::vec3(0.0f, 1.0f, 0.0f);
                }
                vehicle.visuals.push_back(visual);
                break;
            }
        }
    });
    vehicle.childCount = count;
}

void VehicleSimulation::updateVisuals() {
    uint32_t step = ObjectMotionState::getWorldSimulationStep();
    for (auto& entry : _vehicles) {
        Vehicle& vehicle = entry.second;
        if (!vehicle.constraint) {
            continue;
        }
        int count = 0;
        vehicle.entity->forEachChild([&](SpatiallyNestablePointer) { ++count; });
        if (count != vehicle.childCount || step > vehicle.visualsScanStep + 90) {
            scanVisuals(vehicle, step);
        }
        for (const Visual& visual : vehicle.visuals) {
            EntityItemPointer child = visual.entity.lock();
            if (!child || visual.wheel < 0 || visual.wheel >= (int)vehicle.constraint->GetWheels().size()) {
                continue;
            }
            JPH::Mat44 matrix = vehicle.constraint->GetWheelLocalTransform((JPH::uint)visual.wheel, glmToJolt(visual.right), glmToJolt(visual.up));
            Transform local = child->getLocalTransform();
            local.setTranslation(joltToGLM(matrix.GetTranslation()));
            local.setRotation(glm::normalize(joltToGLM(matrix.GetQuaternion())));
            child->setLocalTransformAndVelocities(local, glm::vec3(0.0f), glm::vec3(0.0f));
        }
    }
}

void VehicleSimulation::sendInputs() {
    auto nodeList = DependencyManager::get<NodeList>();
    SharedNodePointer server;
    for (auto& entry : _vehicles) {
        Vehicle& vehicle = entry.second;
        if (vehicle.unsent.empty()) {
            continue;
        }
        if (isAuthority(vehicle) || !isLocalDriver(vehicle.state)) {
            vehicle.unsent.clear();
            continue;
        }
        quint64 now = usecTimestampNow();
        for (const VehicleInput& input : vehicle.unsent) {
            vehicle.recent.push_back(input);
            vehicle.inputSendTimes.push_back({ input.sequence, now });   // its first sending, for the round trip
        }
        vehicle.unsent.clear();
        while (vehicle.inputSendTimes.size() > 256) {
            vehicle.inputSendTimes.pop_front();
        }
        while ((int)vehicle.recent.size() > VehicleInputPacket::MAX_INPUTS) {
            vehicle.recent.erase(vehicle.recent.begin());
        }
        if (!server && nodeList) {
            server = nodeList->soloNodeOfType(NodeType::PhysicsServer);
        }
        if (!server) {
            continue;
        }
        QByteArray payload = VehicleInputPacket::pack(vehicle.entity->getID(), vehicle.recent);
        auto packet = NLPacket::create(PacketType::VehicleInput, payload.size());
        packet->write(payload);
        nodeList->sendUnreliablePacket(*packet, *server);
        ++_stats.inputsSent;
    }
}

void VehicleSimulation::receiveInputs(const QUuid& vehicleID, const QUuid& sender, const std::vector<VehicleInput>& inputs) {
    auto found = _byEntity.find(vehicleID);
    if (found == _byEntity.end() || inputs.empty()) {
        return;
    }
    auto it = _vehicles.find(found->second);
    if (it == _vehicles.end()) {
        return;
    }
    Vehicle& vehicle = it->second;
    if (vehicle.entity->getVehicleProperties().getDriver() != sender) {
        return;   // not the one in the driver's seat
    }
    std::lock_guard<std::mutex> lock(_inputMutex);
    if (vehicle.queueDriver != sender) {
        // a new driver: aligned afresh from its first input
        vehicle.queueDriver = sender;
        vehicle.inputs.reset();
    }
    uint32_t step = ObjectMotionState::getWorldSimulationStep();
    int realigned = vehicle.inputs.counts().realigned;
    for (const VehicleInput& input : inputs) {
        switch (vehicle.inputs.push(input, step)) {
            case InputBuffer::Push::Stored:
                ++_stats.inputsReceived;
                break;
            case InputBuffer::Push::Late:
                ++_stats.inputsLate;
                ++_stats.inputsDropped;
                break;
            case InputBuffer::Push::Early:
                ++_stats.inputsEarly;
                ++_stats.inputsDropped;
                break;
            case InputBuffer::Push::Duplicate:
                break;
        }
    }
    if (vehicle.inputs.counts().realigned != realigned) {
        _stats.bufferRealigned += vehicle.inputs.counts().realigned - realigned;
        qCDebug(physics) << "vehicle" << vehicle.entity->getName() << "input buffer aligned again at tick" << inputs.front().sequence
            << "(late" << vehicle.inputs.counts().late << "early" << vehicle.inputs.counts().early << "held" << vehicle.inputs.counts().holds << ")";
    }
}

// The stream's packets (round 9)

void VehicleSimulation::receiveStates(const std::vector<VehicleState>& states, qint64 clockSkew, quint64 arrivalTime) {
    std::lock_guard<std::mutex> lock(_streamMutex);
    for (VehicleState state : states) {
        state.time = (quint64)((qint64)state.time - clockSkew);   // the server's clock to this one's
        _receivedStates.push_back({ state, arrivalTime });
    }
}

void VehicleSimulation::receiveStateRequest(const QUuid& node, const std::vector<QUuid>& vehicleIDs) {
    std::lock_guard<std::mutex> lock(_streamMutex);
    Subscriber& subscriber = _subscribers[node];
    subscriber.vehicles = std::set<QUuid>(vehicleIDs.begin(), vehicleIDs.end());
    subscriber.time = usecTimestampNow();
}

// The server: the captured states to each subscriber, those of the vehicles it asked for, unreliable.
void VehicleSimulation::sendStream() {
    if (_pendingStream.empty()) {
        return;
    }
    std::vector<VehicleState> states;
    states.swap(_pendingStream);
    auto nodeList = DependencyManager::get<NodeList>();
    if (!nodeList) {
        return;
    }
    quint64 now = usecTimestampNow();
    std::map<QUuid, Subscriber> subscribers;
    {
        std::lock_guard<std::mutex> lock(_streamMutex);
        for (auto it = _subscribers.begin(); it != _subscribers.end();) {
            if (now > it->second.time + SUBSCRIPTION_TIMEOUT) {
                it = _subscribers.erase(it);
            } else {
                ++it;
            }
        }
        subscribers = _subscribers;
    }
    _stats.streamSubscribers = (int)subscribers.size();
    for (const auto& entry : subscribers) {
        SharedNodePointer node = nodeList->nodeWithUUID(entry.first);
        if (!node || !node->getActiveSocket()) {
            continue;
        }
        std::vector<VehicleState> wanted;
        for (const VehicleState& state : states) {
            if (entry.second.vehicles.find(state.id) != entry.second.vehicles.end()) {
                wanted.push_back(state);
            }
        }
        for (size_t first = 0; first < wanted.size(); first += VehicleStatePacket::MAX_STATES) {
            std::vector<VehicleState> batch(wanted.begin() + first,
                                            wanted.begin() + std::min(wanted.size(), first + (size_t)VehicleStatePacket::MAX_STATES));
            QByteArray payload = VehicleStatePacket::pack(batch);
            auto packet = NLPacket::create(PacketType::VehicleState, payload.size());
            packet->write(payload);
            nodeList->sendUnreliablePacket(*packet, *node);
            _stats.streamStatesSent += (int)batch.size();
        }
    }
}

// A client: the vehicles it simulates, to the physics server, once a second and when the set changes.
void VehicleSimulation::sendStateRequests() {
    if (Physics::isServerAuthority() || _serverless || _vehicles.empty()) {
        return;
    }
    quint64 now = usecTimestampNow();
    if (_vehicles.size() == _lastRequestCount && now < _lastRequestTime + SUBSCRIPTION_INTERVAL) {
        return;
    }
    auto nodeList = DependencyManager::get<NodeList>();
    SharedNodePointer server = nodeList ? nodeList->soloNodeOfType(NodeType::PhysicsServer) : nullptr;
    if (!server || !server->getActiveSocket()) {
        return;
    }
    std::vector<QUuid> ids;
    for (const auto& entry : _vehicles) {
        ids.push_back(entry.second.entity->getID());
    }
    QByteArray payload = VehicleStateRequestPacket::pack(ids);
    auto packet = NLPacket::create(PacketType::VehicleStateRequest, payload.size());
    packet->write(payload);
    nodeList->sendUnreliablePacket(*packet, *server);
    _lastRequestTime = now;
    _lastRequestCount = _vehicles.size();
}

void VehicleSimulation::fillState(EntityMotionState* state, EntityItemProperties& properties) {
    auto it = _vehicles.find(state);
    if (it == _vehicles.end()) {
        return;
    }
    Vehicle& vehicle = it->second;
    JPH::WheeledVehicleController* controller = controllerOf(vehicle.constraint);
    if (vehicle.sent.empty()) {
        qCDebug(physics) << "vehicle" << vehicle.entity->getName() << "first update published" << (Physics::isServerAuthority() ? "(physics server)" : "");
    }
    vehicle.sent.push_back({ properties.getPosition(), properties.getRotation(), properties.getVelocity(), properties.getAngularVelocity() });
    while ((int)vehicle.sent.size() > SENT_RING) {
        vehicle.sent.pop_front();
    }
    // signed, so its echo is known for its own however late it comes back (a teleport is an edit by someone else)
    properties.setLastEditedBy(DependencyManager::get<NodeList>()->getSessionUUID());
    VehiclePropertyGroup& group = properties.getVehicle();
    group.setInputSequence(vehicle.applied.sequence);
    group.setThrottle(vehicle.applied.throttle);
    group.setSteering(vehicle.applied.steering);
    group.setBrake(vehicle.applied.brake);
    group.setHandBrake(vehicle.applied.handBrake);
    if (controller) {
        group.setEngineRPM(controller->GetEngine().GetCurrentRPM());
        group.setGear(controller->GetTransmission().GetCurrentGear());
    }
    if (vehicle.airship) {
        group.setPitch(vehicle.applied.pitch);
        group.setLift(vehicle.applied.lift);
        group.setTrim(vehicle.applied.trim);
        group.setBallast(vehicle.airshipState.ballast);
        group.setFill(vehicle.airshipState.fill);
    }
    if (vehicle.aircraft) {
        group.setPitch(vehicle.applied.pitch);
        group.setLift(vehicle.applied.lift);
        group.setTrim(vehicle.applied.trim);
        group.setTilt(vehicle.aircraftState.tilt);
    }
    group.setStateTime(TrackSimulation::domainTime(usecTimestampNow()));
}
