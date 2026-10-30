//
//  VehiclePropertyGroup.cpp
//  libraries/entities/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "VehiclePropertyGroup.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>

#include <OctreePacketData.h>

#include "EntityItemProperties.h"
#include "EntityItemPropertiesMacros.h"

template <typename Value, typename Engine>
void VehiclePropertyGroup::copyToScriptValue(const EntityPropertyFlags& desiredProperties, Value& properties,
                                             Engine* engine, bool skipDefaults,
                                             EntityItemProperties& defaultEntityProperties) const {
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_TYPE, Vehicle, vehicle, Type, type);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_MASS, Vehicle, vehicle, Mass, mass);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_CENTER_OF_MASS, Vehicle, vehicle, CenterOfMass, centerOfMass);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_WHEELS, Vehicle, vehicle, Wheels, wheels);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_SEATS, Vehicle, vehicle, Seats, seats);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_MAX_STEER_ANGLE, Vehicle, vehicle, MaxSteerAngle, maxSteerAngle);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_MAX_TORQUE, Vehicle, vehicle, MaxTorque, maxTorque);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_MIN_RPM, Vehicle, vehicle, MinRPM, minRPM);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_MAX_RPM, Vehicle, vehicle, MaxRPM, maxRPM);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_GEAR_RATIOS, Vehicle, vehicle, GearRatios, gearRatios);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_REVERSE_GEAR_RATIO, Vehicle, vehicle, ReverseGearRatio, reverseGearRatio);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_FINAL_DRIVE_RATIO, Vehicle, vehicle, FinalDriveRatio, finalDriveRatio);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_MAX_BRAKE_TORQUE, Vehicle, vehicle, MaxBrakeTorque, maxBrakeTorque);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_MAX_HAND_BRAKE_TORQUE, Vehicle, vehicle, MaxHandBrakeTorque, maxHandBrakeTorque);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_DRIVE_TYPE, Vehicle, vehicle, DriveType, driveType);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_SUSPENSION_FREQUENCY, Vehicle, vehicle, SuspensionFrequency, suspensionFrequency);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_SUSPENSION_DAMPING, Vehicle, vehicle, SuspensionDamping, suspensionDamping);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_WHEEL, Vehicle, vehicle, Wheel, wheel);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_AXLE, Vehicle, vehicle, Axle, axle);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_DRIVER, Vehicle, vehicle, Driver, driver);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_INPUT_SEQUENCE, Vehicle, vehicle, InputSequence, inputSequence);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_THROTTLE, Vehicle, vehicle, Throttle, throttle);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_STEERING, Vehicle, vehicle, Steering, steering);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_BRAKE, Vehicle, vehicle, Brake, brake);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_HAND_BRAKE, Vehicle, vehicle, HandBrake, handBrake);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_ENGINE_RPM, Vehicle, vehicle, EngineRPM, engineRPM);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_GEAR, Vehicle, vehicle, Gear, gear);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_STATE_TIME, Vehicle, vehicle, StateTime, stateTime);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_ENVELOPE_VOLUME, Vehicle, vehicle, EnvelopeVolume, envelopeVolume);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_CENTER_OF_BUOYANCY, Vehicle, vehicle, CenterOfBuoyancy, centerOfBuoyancy);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_GAS_DENSITY, Vehicle, vehicle, GasDensity, gasDensity);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_MAX_THRUST, Vehicle, vehicle, MaxThrust, maxThrust);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_MAX_VERTICAL_THRUST, Vehicle, vehicle, MaxVerticalThrust, maxVerticalThrust);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_THRUST_POINT, Vehicle, vehicle, ThrustPoint, thrustPoint);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_DRAG_AREA, Vehicle, vehicle, DragArea, dragArea);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_ANGULAR_DRAG_AREA, Vehicle, vehicle, AngularDragArea, angularDragArea);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_RUDDER_AREA, Vehicle, vehicle, RudderArea, rudderArea);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_ELEVATOR_AREA, Vehicle, vehicle, ElevatorArea, elevatorArea);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_FIN_POINT, Vehicle, vehicle, FinPoint, finPoint);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_MAX_FIN_ANGLE, Vehicle, vehicle, MaxFinAngle, maxFinAngle);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_BALLAST_RATE, Vehicle, vehicle, BallastRate, ballastRate);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_VENT_RATE, Vehicle, vehicle, VentRate, ventRate);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_PITCH, Vehicle, vehicle, Pitch, pitch);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_LIFT, Vehicle, vehicle, Lift, lift);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_TRIM, Vehicle, vehicle, Trim, trim);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_BALLAST, Vehicle, vehicle, Ballast, ballast);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_FILL, Vehicle, vehicle, Fill, fill);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_WING_AREA, Vehicle, vehicle, WingArea, wingArea);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_WING_SPAN, Vehicle, vehicle, WingSpan, wingSpan);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_WING_POINT, Vehicle, vehicle, WingPoint, wingPoint);
    COPY_GROUP_PROPERTY_TO_QSCRIPTVALUE(PROP_VEHICLE_TILT, Vehicle, vehicle, Tilt, tilt);
}

template <typename Value>
void VehiclePropertyGroup::copyFromScriptValue(const Value& object, bool& _defaultSettings) {
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, type, QString, setType);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, mass, float, setMass);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, centerOfMass, vec3, setCenterOfMass);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, wheels, QString, setWheels);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, seats, QString, setSeats);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, maxSteerAngle, float, setMaxSteerAngle);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, maxTorque, float, setMaxTorque);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, minRPM, float, setMinRPM);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, maxRPM, float, setMaxRPM);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, gearRatios, QString, setGearRatios);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, reverseGearRatio, float, setReverseGearRatio);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, finalDriveRatio, float, setFinalDriveRatio);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, maxBrakeTorque, float, setMaxBrakeTorque);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, maxHandBrakeTorque, float, setMaxHandBrakeTorque);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, driveType, QString, setDriveType);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, suspensionFrequency, float, setSuspensionFrequency);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, suspensionDamping, float, setSuspensionDamping);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, wheel, QString, setWheel);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, axle, QString, setAxle);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, driver, QUuid, setDriver);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, inputSequence, quint32, setInputSequence);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, throttle, float, setThrottle);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, steering, float, setSteering);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, brake, float, setBrake);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, handBrake, float, setHandBrake);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, engineRPM, float, setEngineRPM);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, gear, int, setGear);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, stateTime, quint64, setStateTime);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, envelopeVolume, float, setEnvelopeVolume);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, centerOfBuoyancy, vec3, setCenterOfBuoyancy);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, gasDensity, float, setGasDensity);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, maxThrust, float, setMaxThrust);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, maxVerticalThrust, float, setMaxVerticalThrust);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, thrustPoint, vec3, setThrustPoint);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, dragArea, vec3, setDragArea);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, angularDragArea, vec3, setAngularDragArea);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, rudderArea, float, setRudderArea);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, elevatorArea, float, setElevatorArea);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, finPoint, vec3, setFinPoint);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, maxFinAngle, float, setMaxFinAngle);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, ballastRate, float, setBallastRate);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, ventRate, float, setVentRate);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, pitch, float, setPitch);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, lift, float, setLift);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, trim, float, setTrim);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, ballast, float, setBallast);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, fill, float, setFill);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, wingArea, float, setWingArea);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, wingSpan, float, setWingSpan);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, wingPoint, vec3, setWingPoint);
    COPY_GROUP_PROPERTY_FROM_QSCRIPTVALUE(vehicle, tilt, float, setTilt);
}

void VehiclePropertyGroup::merge(const VehiclePropertyGroup& other) {
    COPY_PROPERTY_IF_CHANGED(type);
    COPY_PROPERTY_IF_CHANGED(mass);
    COPY_PROPERTY_IF_CHANGED(centerOfMass);
    COPY_PROPERTY_IF_CHANGED(wheels);
    COPY_PROPERTY_IF_CHANGED(seats);
    COPY_PROPERTY_IF_CHANGED(maxSteerAngle);
    COPY_PROPERTY_IF_CHANGED(maxTorque);
    COPY_PROPERTY_IF_CHANGED(minRPM);
    COPY_PROPERTY_IF_CHANGED(maxRPM);
    COPY_PROPERTY_IF_CHANGED(gearRatios);
    COPY_PROPERTY_IF_CHANGED(reverseGearRatio);
    COPY_PROPERTY_IF_CHANGED(finalDriveRatio);
    COPY_PROPERTY_IF_CHANGED(maxBrakeTorque);
    COPY_PROPERTY_IF_CHANGED(maxHandBrakeTorque);
    COPY_PROPERTY_IF_CHANGED(driveType);
    COPY_PROPERTY_IF_CHANGED(suspensionFrequency);
    COPY_PROPERTY_IF_CHANGED(suspensionDamping);
    COPY_PROPERTY_IF_CHANGED(wheel);
    COPY_PROPERTY_IF_CHANGED(axle);
    COPY_PROPERTY_IF_CHANGED(driver);
    COPY_PROPERTY_IF_CHANGED(inputSequence);
    COPY_PROPERTY_IF_CHANGED(throttle);
    COPY_PROPERTY_IF_CHANGED(steering);
    COPY_PROPERTY_IF_CHANGED(brake);
    COPY_PROPERTY_IF_CHANGED(handBrake);
    COPY_PROPERTY_IF_CHANGED(engineRPM);
    COPY_PROPERTY_IF_CHANGED(gear);
    COPY_PROPERTY_IF_CHANGED(stateTime);
    COPY_PROPERTY_IF_CHANGED(envelopeVolume);
    COPY_PROPERTY_IF_CHANGED(centerOfBuoyancy);
    COPY_PROPERTY_IF_CHANGED(gasDensity);
    COPY_PROPERTY_IF_CHANGED(maxThrust);
    COPY_PROPERTY_IF_CHANGED(maxVerticalThrust);
    COPY_PROPERTY_IF_CHANGED(thrustPoint);
    COPY_PROPERTY_IF_CHANGED(dragArea);
    COPY_PROPERTY_IF_CHANGED(angularDragArea);
    COPY_PROPERTY_IF_CHANGED(rudderArea);
    COPY_PROPERTY_IF_CHANGED(elevatorArea);
    COPY_PROPERTY_IF_CHANGED(finPoint);
    COPY_PROPERTY_IF_CHANGED(maxFinAngle);
    COPY_PROPERTY_IF_CHANGED(ballastRate);
    COPY_PROPERTY_IF_CHANGED(ventRate);
    COPY_PROPERTY_IF_CHANGED(pitch);
    COPY_PROPERTY_IF_CHANGED(lift);
    COPY_PROPERTY_IF_CHANGED(trim);
    COPY_PROPERTY_IF_CHANGED(ballast);
    COPY_PROPERTY_IF_CHANGED(fill);
    COPY_PROPERTY_IF_CHANGED(wingArea);
    COPY_PROPERTY_IF_CHANGED(wingSpan);
    COPY_PROPERTY_IF_CHANGED(wingPoint);
    COPY_PROPERTY_IF_CHANGED(tilt);
}

void VehiclePropertyGroup::debugDump() const {
    qCDebug(entities) << "   VehiclePropertyGroup: ---------------------------------------------";
    qCDebug(entities) << "            _type:" << _type;
    qCDebug(entities) << "            _mass:" << _mass;
    qCDebug(entities) << "            _centerOfMass:" << _centerOfMass;
    qCDebug(entities) << "            _wheels:" << _wheels;
    qCDebug(entities) << "            _seats:" << _seats;
    qCDebug(entities) << "            _maxSteerAngle:" << _maxSteerAngle;
    qCDebug(entities) << "            _maxTorque:" << _maxTorque;
    qCDebug(entities) << "            _minRPM:" << _minRPM;
    qCDebug(entities) << "            _maxRPM:" << _maxRPM;
    qCDebug(entities) << "            _gearRatios:" << _gearRatios;
    qCDebug(entities) << "            _reverseGearRatio:" << _reverseGearRatio;
    qCDebug(entities) << "            _finalDriveRatio:" << _finalDriveRatio;
    qCDebug(entities) << "            _maxBrakeTorque:" << _maxBrakeTorque;
    qCDebug(entities) << "            _maxHandBrakeTorque:" << _maxHandBrakeTorque;
    qCDebug(entities) << "            _driveType:" << _driveType;
    qCDebug(entities) << "            _suspensionFrequency:" << _suspensionFrequency;
    qCDebug(entities) << "            _suspensionDamping:" << _suspensionDamping;
    qCDebug(entities) << "            _wheel:" << _wheel;
    qCDebug(entities) << "            _axle:" << _axle;
    qCDebug(entities) << "            _driver:" << _driver;
    qCDebug(entities) << "            _inputSequence:" << _inputSequence;
    qCDebug(entities) << "            _throttle:" << _throttle;
    qCDebug(entities) << "            _steering:" << _steering;
    qCDebug(entities) << "            _brake:" << _brake;
    qCDebug(entities) << "            _handBrake:" << _handBrake;
    qCDebug(entities) << "            _engineRPM:" << _engineRPM;
    qCDebug(entities) << "            _gear:" << _gear;
    qCDebug(entities) << "            _stateTime:" << _stateTime;
    qCDebug(entities) << "            _envelopeVolume:" << _envelopeVolume;
    qCDebug(entities) << "            _centerOfBuoyancy:" << _centerOfBuoyancy;
    qCDebug(entities) << "            _gasDensity:" << _gasDensity;
    qCDebug(entities) << "            _maxThrust:" << _maxThrust;
    qCDebug(entities) << "            _maxVerticalThrust:" << _maxVerticalThrust;
    qCDebug(entities) << "            _thrustPoint:" << _thrustPoint;
    qCDebug(entities) << "            _dragArea:" << _dragArea;
    qCDebug(entities) << "            _angularDragArea:" << _angularDragArea;
    qCDebug(entities) << "            _rudderArea:" << _rudderArea;
    qCDebug(entities) << "            _elevatorArea:" << _elevatorArea;
    qCDebug(entities) << "            _finPoint:" << _finPoint;
    qCDebug(entities) << "            _maxFinAngle:" << _maxFinAngle;
    qCDebug(entities) << "            _ballastRate:" << _ballastRate;
    qCDebug(entities) << "            _ventRate:" << _ventRate;
    qCDebug(entities) << "            _pitch:" << _pitch;
    qCDebug(entities) << "            _lift:" << _lift;
    qCDebug(entities) << "            _trim:" << _trim;
    qCDebug(entities) << "            _ballast:" << _ballast;
    qCDebug(entities) << "            _fill:" << _fill;
    qCDebug(entities) << "            _wingArea:" << _wingArea;
    qCDebug(entities) << "            _wingSpan:" << _wingSpan;
    qCDebug(entities) << "            _wingPoint:" << _wingPoint;
    qCDebug(entities) << "            _tilt:" << _tilt;
}

void VehiclePropertyGroup::listChangedProperties(QList<QString>& out) {
    if (typeChanged()) {
        out << "vehicle-type";
    }
    if (massChanged()) {
        out << "vehicle-mass";
    }
    if (centerOfMassChanged()) {
        out << "vehicle-centerOfMass";
    }
    if (wheelsChanged()) {
        out << "vehicle-wheels";
    }
    if (seatsChanged()) {
        out << "vehicle-seats";
    }
    if (maxSteerAngleChanged()) {
        out << "vehicle-maxSteerAngle";
    }
    if (maxTorqueChanged()) {
        out << "vehicle-maxTorque";
    }
    if (minRPMChanged()) {
        out << "vehicle-minRPM";
    }
    if (maxRPMChanged()) {
        out << "vehicle-maxRPM";
    }
    if (gearRatiosChanged()) {
        out << "vehicle-gearRatios";
    }
    if (reverseGearRatioChanged()) {
        out << "vehicle-reverseGearRatio";
    }
    if (finalDriveRatioChanged()) {
        out << "vehicle-finalDriveRatio";
    }
    if (maxBrakeTorqueChanged()) {
        out << "vehicle-maxBrakeTorque";
    }
    if (maxHandBrakeTorqueChanged()) {
        out << "vehicle-maxHandBrakeTorque";
    }
    if (driveTypeChanged()) {
        out << "vehicle-driveType";
    }
    if (suspensionFrequencyChanged()) {
        out << "vehicle-suspensionFrequency";
    }
    if (suspensionDampingChanged()) {
        out << "vehicle-suspensionDamping";
    }
    if (wheelChanged()) {
        out << "vehicle-wheel";
    }
    if (axleChanged()) {
        out << "vehicle-axle";
    }
    if (driverChanged()) {
        out << "vehicle-driver";
    }
    if (inputSequenceChanged()) {
        out << "vehicle-inputSequence";
    }
    if (throttleChanged()) {
        out << "vehicle-throttle";
    }
    if (steeringChanged()) {
        out << "vehicle-steering";
    }
    if (brakeChanged()) {
        out << "vehicle-brake";
    }
    if (handBrakeChanged()) {
        out << "vehicle-handBrake";
    }
    if (engineRPMChanged()) {
        out << "vehicle-engineRPM";
    }
    if (gearChanged()) {
        out << "vehicle-gear";
    }
    if (stateTimeChanged()) {
        out << "vehicle-stateTime";
    }
    if (envelopeVolumeChanged()) {
        out << "vehicle-envelopeVolume";
    }
    if (centerOfBuoyancyChanged()) {
        out << "vehicle-centerOfBuoyancy";
    }
    if (gasDensityChanged()) {
        out << "vehicle-gasDensity";
    }
    if (maxThrustChanged()) {
        out << "vehicle-maxThrust";
    }
    if (maxVerticalThrustChanged()) {
        out << "vehicle-maxVerticalThrust";
    }
    if (thrustPointChanged()) {
        out << "vehicle-thrustPoint";
    }
    if (dragAreaChanged()) {
        out << "vehicle-dragArea";
    }
    if (angularDragAreaChanged()) {
        out << "vehicle-angularDragArea";
    }
    if (rudderAreaChanged()) {
        out << "vehicle-rudderArea";
    }
    if (elevatorAreaChanged()) {
        out << "vehicle-elevatorArea";
    }
    if (finPointChanged()) {
        out << "vehicle-finPoint";
    }
    if (maxFinAngleChanged()) {
        out << "vehicle-maxFinAngle";
    }
    if (ballastRateChanged()) {
        out << "vehicle-ballastRate";
    }
    if (ventRateChanged()) {
        out << "vehicle-ventRate";
    }
    if (pitchChanged()) {
        out << "vehicle-pitch";
    }
    if (liftChanged()) {
        out << "vehicle-lift";
    }
    if (trimChanged()) {
        out << "vehicle-trim";
    }
    if (ballastChanged()) {
        out << "vehicle-ballast";
    }
    if (fillChanged()) {
        out << "vehicle-fill";
    }
    if (wingAreaChanged()) {
        out << "vehicle-wingArea";
    }
    if (wingSpanChanged()) {
        out << "vehicle-wingSpan";
    }
    if (wingPointChanged()) {
        out << "vehicle-wingPoint";
    }
    if (tiltChanged()) {
        out << "vehicle-tilt";
    }
}

bool VehiclePropertyGroup::appendToEditPacket(OctreePacketData* packetData,
                                              EntityPropertyFlags& requestedProperties,
                                              EntityPropertyFlags& propertyFlags,
                                              EntityPropertyFlags& propertiesDidntFit,
                                              int& propertyCount,
                                              OctreeElement::AppendState& appendState) const {

    bool successPropertyFits = true;

    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_TYPE, getType());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MASS, getMass());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_CENTER_OF_MASS, getCenterOfMass());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_WHEELS, getWheels());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_SEATS, getSeats());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_STEER_ANGLE, getMaxSteerAngle());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_TORQUE, getMaxTorque());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MIN_RPM, getMinRPM());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_RPM, getMaxRPM());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_GEAR_RATIOS, getGearRatios());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_REVERSE_GEAR_RATIO, getReverseGearRatio());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_FINAL_DRIVE_RATIO, getFinalDriveRatio());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_BRAKE_TORQUE, getMaxBrakeTorque());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_HAND_BRAKE_TORQUE, getMaxHandBrakeTorque());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_DRIVE_TYPE, getDriveType());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_SUSPENSION_FREQUENCY, getSuspensionFrequency());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_SUSPENSION_DAMPING, getSuspensionDamping());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_WHEEL, getWheel());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_AXLE, getAxle());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_DRIVER, getDriver());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_INPUT_SEQUENCE, getInputSequence());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_THROTTLE, getThrottle());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_STEERING, getSteering());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_BRAKE, getBrake());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_HAND_BRAKE, getHandBrake());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_ENGINE_RPM, getEngineRPM());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_GEAR, getGear());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_STATE_TIME, getStateTime());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_ENVELOPE_VOLUME, getEnvelopeVolume());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_CENTER_OF_BUOYANCY, getCenterOfBuoyancy());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_GAS_DENSITY, getGasDensity());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_THRUST, getMaxThrust());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_VERTICAL_THRUST, getMaxVerticalThrust());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_THRUST_POINT, getThrustPoint());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_DRAG_AREA, getDragArea());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_ANGULAR_DRAG_AREA, getAngularDragArea());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_RUDDER_AREA, getRudderArea());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_ELEVATOR_AREA, getElevatorArea());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_FIN_POINT, getFinPoint());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_FIN_ANGLE, getMaxFinAngle());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_BALLAST_RATE, getBallastRate());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_VENT_RATE, getVentRate());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_PITCH, getPitch());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_LIFT, getLift());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_TRIM, getTrim());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_BALLAST, getBallast());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_FILL, getFill());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_WING_AREA, getWingArea());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_WING_SPAN, getWingSpan());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_WING_POINT, getWingPoint());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_TILT, getTilt());

    return true;
}

bool VehiclePropertyGroup::decodeFromEditPacket(EntityPropertyFlags& propertyFlags,
                                                const unsigned char*& dataAt , int& processedBytes, int bytesToRead) {

    int bytesRead = 0;
    bool overwriteLocalData = true;
    bool somethingChanged = false;

    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_TYPE, QString, setType);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_MASS, float, setMass);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_CENTER_OF_MASS, glm::vec3, setCenterOfMass);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_WHEELS, QString, setWheels);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_SEATS, QString, setSeats);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_MAX_STEER_ANGLE, float, setMaxSteerAngle);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_MAX_TORQUE, float, setMaxTorque);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_MIN_RPM, float, setMinRPM);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_MAX_RPM, float, setMaxRPM);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_GEAR_RATIOS, QString, setGearRatios);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_REVERSE_GEAR_RATIO, float, setReverseGearRatio);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_FINAL_DRIVE_RATIO, float, setFinalDriveRatio);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_MAX_BRAKE_TORQUE, float, setMaxBrakeTorque);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_MAX_HAND_BRAKE_TORQUE, float, setMaxHandBrakeTorque);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_DRIVE_TYPE, QString, setDriveType);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_SUSPENSION_FREQUENCY, float, setSuspensionFrequency);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_SUSPENSION_DAMPING, float, setSuspensionDamping);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_WHEEL, QString, setWheel);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_AXLE, QString, setAxle);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_DRIVER, QUuid, setDriver);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_INPUT_SEQUENCE, quint32, setInputSequence);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_THROTTLE, float, setThrottle);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_STEERING, float, setSteering);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_BRAKE, float, setBrake);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_HAND_BRAKE, float, setHandBrake);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_ENGINE_RPM, float, setEngineRPM);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_GEAR, int, setGear);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_STATE_TIME, quint64, setStateTime);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_ENVELOPE_VOLUME, float, setEnvelopeVolume);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_CENTER_OF_BUOYANCY, glm::vec3, setCenterOfBuoyancy);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_GAS_DENSITY, float, setGasDensity);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_MAX_THRUST, float, setMaxThrust);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_MAX_VERTICAL_THRUST, float, setMaxVerticalThrust);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_THRUST_POINT, glm::vec3, setThrustPoint);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_DRAG_AREA, glm::vec3, setDragArea);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_ANGULAR_DRAG_AREA, glm::vec3, setAngularDragArea);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_RUDDER_AREA, float, setRudderArea);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_ELEVATOR_AREA, float, setElevatorArea);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_FIN_POINT, glm::vec3, setFinPoint);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_MAX_FIN_ANGLE, float, setMaxFinAngle);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_BALLAST_RATE, float, setBallastRate);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_VENT_RATE, float, setVentRate);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_PITCH, float, setPitch);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_LIFT, float, setLift);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_TRIM, float, setTrim);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_BALLAST, float, setBallast);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_FILL, float, setFill);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_WING_AREA, float, setWingArea);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_WING_SPAN, float, setWingSpan);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_WING_POINT, glm::vec3, setWingPoint);
    READ_ENTITY_PROPERTY_FROM_EDIT(PROP_VEHICLE_TILT, float, setTilt);

    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_TYPE, Type);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_MASS, Mass);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_CENTER_OF_MASS, CenterOfMass);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_WHEELS, Wheels);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_SEATS, Seats);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_MAX_STEER_ANGLE, MaxSteerAngle);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_MAX_TORQUE, MaxTorque);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_MIN_RPM, MinRPM);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_MAX_RPM, MaxRPM);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_GEAR_RATIOS, GearRatios);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_REVERSE_GEAR_RATIO, ReverseGearRatio);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_FINAL_DRIVE_RATIO, FinalDriveRatio);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_MAX_BRAKE_TORQUE, MaxBrakeTorque);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_MAX_HAND_BRAKE_TORQUE, MaxHandBrakeTorque);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_DRIVE_TYPE, DriveType);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_SUSPENSION_FREQUENCY, SuspensionFrequency);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_SUSPENSION_DAMPING, SuspensionDamping);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_WHEEL, Wheel);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_AXLE, Axle);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_DRIVER, Driver);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_INPUT_SEQUENCE, InputSequence);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_THROTTLE, Throttle);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_STEERING, Steering);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_BRAKE, Brake);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_HAND_BRAKE, HandBrake);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_ENGINE_RPM, EngineRPM);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_GEAR, Gear);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_STATE_TIME, StateTime);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_ENVELOPE_VOLUME, EnvelopeVolume);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_CENTER_OF_BUOYANCY, CenterOfBuoyancy);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_GAS_DENSITY, GasDensity);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_MAX_THRUST, MaxThrust);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_MAX_VERTICAL_THRUST, MaxVerticalThrust);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_THRUST_POINT, ThrustPoint);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_DRAG_AREA, DragArea);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_ANGULAR_DRAG_AREA, AngularDragArea);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_RUDDER_AREA, RudderArea);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_ELEVATOR_AREA, ElevatorArea);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_FIN_POINT, FinPoint);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_MAX_FIN_ANGLE, MaxFinAngle);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_BALLAST_RATE, BallastRate);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_VENT_RATE, VentRate);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_PITCH, Pitch);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_LIFT, Lift);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_TRIM, Trim);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_BALLAST, Ballast);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_FILL, Fill);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_WING_AREA, WingArea);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_WING_SPAN, WingSpan);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_WING_POINT, WingPoint);
    DECODE_GROUP_PROPERTY_HAS_CHANGED(PROP_VEHICLE_TILT, Tilt);

    processedBytes += bytesRead;

    Q_UNUSED(somethingChanged);

    return true;
}

void VehiclePropertyGroup::markAllChanged() {
    _typeChanged = true;
    _massChanged = true;
    _centerOfMassChanged = true;
    _wheelsChanged = true;
    _seatsChanged = true;
    _maxSteerAngleChanged = true;
    _maxTorqueChanged = true;
    _minRPMChanged = true;
    _maxRPMChanged = true;
    _gearRatiosChanged = true;
    _reverseGearRatioChanged = true;
    _finalDriveRatioChanged = true;
    _maxBrakeTorqueChanged = true;
    _maxHandBrakeTorqueChanged = true;
    _driveTypeChanged = true;
    _suspensionFrequencyChanged = true;
    _suspensionDampingChanged = true;
    _wheelChanged = true;
    _axleChanged = true;
    _driverChanged = true;
    _inputSequenceChanged = true;
    _throttleChanged = true;
    _steeringChanged = true;
    _brakeChanged = true;
    _handBrakeChanged = true;
    _engineRPMChanged = true;
    _gearChanged = true;
    _stateTimeChanged = true;
    _envelopeVolumeChanged = true;
    _centerOfBuoyancyChanged = true;
    _gasDensityChanged = true;
    _maxThrustChanged = true;
    _maxVerticalThrustChanged = true;
    _thrustPointChanged = true;
    _dragAreaChanged = true;
    _angularDragAreaChanged = true;
    _rudderAreaChanged = true;
    _elevatorAreaChanged = true;
    _finPointChanged = true;
    _maxFinAngleChanged = true;
    _ballastRateChanged = true;
    _ventRateChanged = true;
    _pitchChanged = true;
    _liftChanged = true;
    _trimChanged = true;
    _ballastChanged = true;
    _fillChanged = true;
    _wingAreaChanged = true;
    _wingSpanChanged = true;
    _wingPointChanged = true;
    _tiltChanged = true;
}

EntityPropertyFlags VehiclePropertyGroup::getChangedProperties() const {
    EntityPropertyFlags changedProperties;

    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_TYPE, type);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_MASS, mass);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_CENTER_OF_MASS, centerOfMass);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_WHEELS, wheels);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_SEATS, seats);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_MAX_STEER_ANGLE, maxSteerAngle);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_MAX_TORQUE, maxTorque);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_MIN_RPM, minRPM);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_MAX_RPM, maxRPM);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_GEAR_RATIOS, gearRatios);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_REVERSE_GEAR_RATIO, reverseGearRatio);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_FINAL_DRIVE_RATIO, finalDriveRatio);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_MAX_BRAKE_TORQUE, maxBrakeTorque);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_MAX_HAND_BRAKE_TORQUE, maxHandBrakeTorque);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_DRIVE_TYPE, driveType);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_SUSPENSION_FREQUENCY, suspensionFrequency);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_SUSPENSION_DAMPING, suspensionDamping);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_WHEEL, wheel);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_AXLE, axle);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_DRIVER, driver);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_INPUT_SEQUENCE, inputSequence);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_THROTTLE, throttle);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_STEERING, steering);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_BRAKE, brake);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_HAND_BRAKE, handBrake);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_ENGINE_RPM, engineRPM);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_GEAR, gear);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_STATE_TIME, stateTime);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_ENVELOPE_VOLUME, envelopeVolume);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_CENTER_OF_BUOYANCY, centerOfBuoyancy);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_GAS_DENSITY, gasDensity);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_MAX_THRUST, maxThrust);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_MAX_VERTICAL_THRUST, maxVerticalThrust);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_THRUST_POINT, thrustPoint);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_DRAG_AREA, dragArea);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_ANGULAR_DRAG_AREA, angularDragArea);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_RUDDER_AREA, rudderArea);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_ELEVATOR_AREA, elevatorArea);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_FIN_POINT, finPoint);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_MAX_FIN_ANGLE, maxFinAngle);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_BALLAST_RATE, ballastRate);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_VENT_RATE, ventRate);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_PITCH, pitch);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_LIFT, lift);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_TRIM, trim);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_BALLAST, ballast);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_FILL, fill);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_WING_AREA, wingArea);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_WING_SPAN, wingSpan);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_WING_POINT, wingPoint);
    CHECK_PROPERTY_CHANGE(PROP_VEHICLE_TILT, tilt);

    return changedProperties;
}

void VehiclePropertyGroup::getProperties(EntityItemProperties& properties) const {
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Type, getType);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Mass, getMass);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, CenterOfMass, getCenterOfMass);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Wheels, getWheels);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Seats, getSeats);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, MaxSteerAngle, getMaxSteerAngle);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, MaxTorque, getMaxTorque);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, MinRPM, getMinRPM);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, MaxRPM, getMaxRPM);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, GearRatios, getGearRatios);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, ReverseGearRatio, getReverseGearRatio);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, FinalDriveRatio, getFinalDriveRatio);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, MaxBrakeTorque, getMaxBrakeTorque);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, MaxHandBrakeTorque, getMaxHandBrakeTorque);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, DriveType, getDriveType);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, SuspensionFrequency, getSuspensionFrequency);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, SuspensionDamping, getSuspensionDamping);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Wheel, getWheel);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Axle, getAxle);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Driver, getDriver);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, InputSequence, getInputSequence);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Throttle, getThrottle);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Steering, getSteering);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Brake, getBrake);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, HandBrake, getHandBrake);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, EngineRPM, getEngineRPM);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Gear, getGear);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, StateTime, getStateTime);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, EnvelopeVolume, getEnvelopeVolume);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, CenterOfBuoyancy, getCenterOfBuoyancy);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, GasDensity, getGasDensity);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, MaxThrust, getMaxThrust);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, MaxVerticalThrust, getMaxVerticalThrust);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, ThrustPoint, getThrustPoint);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, DragArea, getDragArea);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, AngularDragArea, getAngularDragArea);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, RudderArea, getRudderArea);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, ElevatorArea, getElevatorArea);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, FinPoint, getFinPoint);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, MaxFinAngle, getMaxFinAngle);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, BallastRate, getBallastRate);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, VentRate, getVentRate);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Pitch, getPitch);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Lift, getLift);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Trim, getTrim);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Ballast, getBallast);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Fill, getFill);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, WingArea, getWingArea);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, WingSpan, getWingSpan);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, WingPoint, getWingPoint);
    COPY_ENTITY_GROUP_PROPERTY_TO_PROPERTIES(Vehicle, Tilt, getTilt);
}

bool VehiclePropertyGroup::setProperties(const EntityItemProperties& properties) {
    bool somethingChanged = false;

    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Type, type, setType);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Mass, mass, setMass);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, CenterOfMass, centerOfMass, setCenterOfMass);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Wheels, wheels, setWheels);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Seats, seats, setSeats);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, MaxSteerAngle, maxSteerAngle, setMaxSteerAngle);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, MaxTorque, maxTorque, setMaxTorque);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, MinRPM, minRPM, setMinRPM);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, MaxRPM, maxRPM, setMaxRPM);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, GearRatios, gearRatios, setGearRatios);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, ReverseGearRatio, reverseGearRatio, setReverseGearRatio);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, FinalDriveRatio, finalDriveRatio, setFinalDriveRatio);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, MaxBrakeTorque, maxBrakeTorque, setMaxBrakeTorque);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, MaxHandBrakeTorque, maxHandBrakeTorque, setMaxHandBrakeTorque);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, DriveType, driveType, setDriveType);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, SuspensionFrequency, suspensionFrequency, setSuspensionFrequency);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, SuspensionDamping, suspensionDamping, setSuspensionDamping);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Wheel, wheel, setWheel);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Axle, axle, setAxle);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Driver, driver, setDriver);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, InputSequence, inputSequence, setInputSequence);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Throttle, throttle, setThrottle);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Steering, steering, setSteering);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Brake, brake, setBrake);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, HandBrake, handBrake, setHandBrake);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, EngineRPM, engineRPM, setEngineRPM);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Gear, gear, setGear);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, StateTime, stateTime, setStateTime);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, EnvelopeVolume, envelopeVolume, setEnvelopeVolume);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, CenterOfBuoyancy, centerOfBuoyancy, setCenterOfBuoyancy);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, GasDensity, gasDensity, setGasDensity);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, MaxThrust, maxThrust, setMaxThrust);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, MaxVerticalThrust, maxVerticalThrust, setMaxVerticalThrust);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, ThrustPoint, thrustPoint, setThrustPoint);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, DragArea, dragArea, setDragArea);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, AngularDragArea, angularDragArea, setAngularDragArea);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, RudderArea, rudderArea, setRudderArea);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, ElevatorArea, elevatorArea, setElevatorArea);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, FinPoint, finPoint, setFinPoint);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, MaxFinAngle, maxFinAngle, setMaxFinAngle);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, BallastRate, ballastRate, setBallastRate);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, VentRate, ventRate, setVentRate);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Pitch, pitch, setPitch);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Lift, lift, setLift);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Trim, trim, setTrim);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Ballast, ballast, setBallast);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Fill, fill, setFill);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, WingArea, wingArea, setWingArea);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, WingSpan, wingSpan, setWingSpan);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, WingPoint, wingPoint, setWingPoint);
    SET_ENTITY_GROUP_PROPERTY_FROM_PROPERTIES(Vehicle, Tilt, tilt, setTilt);

    return somethingChanged;
}

EntityPropertyFlags VehiclePropertyGroup::getEntityProperties(EncodeBitstreamParams& params) const {
    EntityPropertyFlags requestedProperties;

    requestedProperties += PROP_VEHICLE_TYPE;
    requestedProperties += PROP_VEHICLE_MASS;
    requestedProperties += PROP_VEHICLE_CENTER_OF_MASS;
    requestedProperties += PROP_VEHICLE_WHEELS;
    requestedProperties += PROP_VEHICLE_SEATS;
    requestedProperties += PROP_VEHICLE_MAX_STEER_ANGLE;
    requestedProperties += PROP_VEHICLE_MAX_TORQUE;
    requestedProperties += PROP_VEHICLE_MIN_RPM;
    requestedProperties += PROP_VEHICLE_MAX_RPM;
    requestedProperties += PROP_VEHICLE_GEAR_RATIOS;
    requestedProperties += PROP_VEHICLE_REVERSE_GEAR_RATIO;
    requestedProperties += PROP_VEHICLE_FINAL_DRIVE_RATIO;
    requestedProperties += PROP_VEHICLE_MAX_BRAKE_TORQUE;
    requestedProperties += PROP_VEHICLE_MAX_HAND_BRAKE_TORQUE;
    requestedProperties += PROP_VEHICLE_DRIVE_TYPE;
    requestedProperties += PROP_VEHICLE_SUSPENSION_FREQUENCY;
    requestedProperties += PROP_VEHICLE_SUSPENSION_DAMPING;
    requestedProperties += PROP_VEHICLE_WHEEL;
    requestedProperties += PROP_VEHICLE_AXLE;
    requestedProperties += PROP_VEHICLE_DRIVER;
    requestedProperties += PROP_VEHICLE_INPUT_SEQUENCE;
    requestedProperties += PROP_VEHICLE_THROTTLE;
    requestedProperties += PROP_VEHICLE_STEERING;
    requestedProperties += PROP_VEHICLE_BRAKE;
    requestedProperties += PROP_VEHICLE_HAND_BRAKE;
    requestedProperties += PROP_VEHICLE_ENGINE_RPM;
    requestedProperties += PROP_VEHICLE_GEAR;
    requestedProperties += PROP_VEHICLE_STATE_TIME;
    requestedProperties += PROP_VEHICLE_ENVELOPE_VOLUME;
    requestedProperties += PROP_VEHICLE_CENTER_OF_BUOYANCY;
    requestedProperties += PROP_VEHICLE_GAS_DENSITY;
    requestedProperties += PROP_VEHICLE_MAX_THRUST;
    requestedProperties += PROP_VEHICLE_MAX_VERTICAL_THRUST;
    requestedProperties += PROP_VEHICLE_THRUST_POINT;
    requestedProperties += PROP_VEHICLE_DRAG_AREA;
    requestedProperties += PROP_VEHICLE_ANGULAR_DRAG_AREA;
    requestedProperties += PROP_VEHICLE_RUDDER_AREA;
    requestedProperties += PROP_VEHICLE_ELEVATOR_AREA;
    requestedProperties += PROP_VEHICLE_FIN_POINT;
    requestedProperties += PROP_VEHICLE_MAX_FIN_ANGLE;
    requestedProperties += PROP_VEHICLE_BALLAST_RATE;
    requestedProperties += PROP_VEHICLE_VENT_RATE;
    requestedProperties += PROP_VEHICLE_PITCH;
    requestedProperties += PROP_VEHICLE_LIFT;
    requestedProperties += PROP_VEHICLE_TRIM;
    requestedProperties += PROP_VEHICLE_BALLAST;
    requestedProperties += PROP_VEHICLE_FILL;
    requestedProperties += PROP_VEHICLE_WING_AREA;
    requestedProperties += PROP_VEHICLE_WING_SPAN;
    requestedProperties += PROP_VEHICLE_WING_POINT;
    requestedProperties += PROP_VEHICLE_TILT;

    return requestedProperties;
}

void VehiclePropertyGroup::appendSubclassData(OctreePacketData* packetData, EncodeBitstreamParams& params,
                                              EntityTreeElementExtraEncodeDataPointer entityTreeElementExtraEncodeData,
                                              EntityPropertyFlags& requestedProperties,
                                              EntityPropertyFlags& propertyFlags,
                                              EntityPropertyFlags& propertiesDidntFit,
                                              int& propertyCount,
                                              OctreeElement::AppendState& appendState) const {

    bool successPropertyFits = true;

    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_TYPE, getType());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MASS, getMass());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_CENTER_OF_MASS, getCenterOfMass());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_WHEELS, getWheels());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_SEATS, getSeats());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_STEER_ANGLE, getMaxSteerAngle());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_TORQUE, getMaxTorque());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MIN_RPM, getMinRPM());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_RPM, getMaxRPM());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_GEAR_RATIOS, getGearRatios());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_REVERSE_GEAR_RATIO, getReverseGearRatio());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_FINAL_DRIVE_RATIO, getFinalDriveRatio());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_BRAKE_TORQUE, getMaxBrakeTorque());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_HAND_BRAKE_TORQUE, getMaxHandBrakeTorque());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_DRIVE_TYPE, getDriveType());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_SUSPENSION_FREQUENCY, getSuspensionFrequency());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_SUSPENSION_DAMPING, getSuspensionDamping());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_WHEEL, getWheel());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_AXLE, getAxle());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_DRIVER, getDriver());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_INPUT_SEQUENCE, getInputSequence());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_THROTTLE, getThrottle());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_STEERING, getSteering());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_BRAKE, getBrake());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_HAND_BRAKE, getHandBrake());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_ENGINE_RPM, getEngineRPM());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_GEAR, getGear());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_STATE_TIME, getStateTime());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_ENVELOPE_VOLUME, getEnvelopeVolume());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_CENTER_OF_BUOYANCY, getCenterOfBuoyancy());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_GAS_DENSITY, getGasDensity());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_THRUST, getMaxThrust());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_VERTICAL_THRUST, getMaxVerticalThrust());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_THRUST_POINT, getThrustPoint());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_DRAG_AREA, getDragArea());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_ANGULAR_DRAG_AREA, getAngularDragArea());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_RUDDER_AREA, getRudderArea());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_ELEVATOR_AREA, getElevatorArea());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_FIN_POINT, getFinPoint());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_MAX_FIN_ANGLE, getMaxFinAngle());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_BALLAST_RATE, getBallastRate());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_VENT_RATE, getVentRate());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_PITCH, getPitch());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_LIFT, getLift());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_TRIM, getTrim());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_BALLAST, getBallast());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_FILL, getFill());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_WING_AREA, getWingArea());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_WING_SPAN, getWingSpan());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_WING_POINT, getWingPoint());
    APPEND_ENTITY_PROPERTY(PROP_VEHICLE_TILT, getTilt());
}

int VehiclePropertyGroup::readEntitySubclassDataFromBuffer(const unsigned char* data, int bytesLeftToRead,
                                                           ReadBitstreamToTreeParams& args,
                                                           EntityPropertyFlags& propertyFlags, bool overwriteLocalData,
                                                           bool& somethingChanged) {

    int bytesRead = 0;
    const unsigned char* dataAt = data;

    READ_ENTITY_PROPERTY(PROP_VEHICLE_TYPE, QString, setType);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_MASS, float, setMass);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_CENTER_OF_MASS, glm::vec3, setCenterOfMass);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_WHEELS, QString, setWheels);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_SEATS, QString, setSeats);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_MAX_STEER_ANGLE, float, setMaxSteerAngle);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_MAX_TORQUE, float, setMaxTorque);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_MIN_RPM, float, setMinRPM);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_MAX_RPM, float, setMaxRPM);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_GEAR_RATIOS, QString, setGearRatios);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_REVERSE_GEAR_RATIO, float, setReverseGearRatio);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_FINAL_DRIVE_RATIO, float, setFinalDriveRatio);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_MAX_BRAKE_TORQUE, float, setMaxBrakeTorque);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_MAX_HAND_BRAKE_TORQUE, float, setMaxHandBrakeTorque);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_DRIVE_TYPE, QString, setDriveType);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_SUSPENSION_FREQUENCY, float, setSuspensionFrequency);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_SUSPENSION_DAMPING, float, setSuspensionDamping);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_WHEEL, QString, setWheel);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_AXLE, QString, setAxle);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_DRIVER, QUuid, setDriver);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_INPUT_SEQUENCE, quint32, setInputSequence);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_THROTTLE, float, setThrottle);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_STEERING, float, setSteering);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_BRAKE, float, setBrake);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_HAND_BRAKE, float, setHandBrake);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_ENGINE_RPM, float, setEngineRPM);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_GEAR, int, setGear);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_STATE_TIME, quint64, setStateTime);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_ENVELOPE_VOLUME, float, setEnvelopeVolume);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_CENTER_OF_BUOYANCY, glm::vec3, setCenterOfBuoyancy);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_GAS_DENSITY, float, setGasDensity);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_MAX_THRUST, float, setMaxThrust);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_MAX_VERTICAL_THRUST, float, setMaxVerticalThrust);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_THRUST_POINT, glm::vec3, setThrustPoint);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_DRAG_AREA, glm::vec3, setDragArea);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_ANGULAR_DRAG_AREA, glm::vec3, setAngularDragArea);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_RUDDER_AREA, float, setRudderArea);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_ELEVATOR_AREA, float, setElevatorArea);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_FIN_POINT, glm::vec3, setFinPoint);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_MAX_FIN_ANGLE, float, setMaxFinAngle);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_BALLAST_RATE, float, setBallastRate);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_VENT_RATE, float, setVentRate);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_PITCH, float, setPitch);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_LIFT, float, setLift);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_TRIM, float, setTrim);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_BALLAST, float, setBallast);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_FILL, float, setFill);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_WING_AREA, float, setWingArea);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_WING_SPAN, float, setWingSpan);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_WING_POINT, glm::vec3, setWingPoint);
    READ_ENTITY_PROPERTY(PROP_VEHICLE_TILT, float, setTilt);

    return bytesRead;
}

// The definition's version: a hash of every property that shapes the vehicle, none of the state. The simulation keeps
// its Jolt vehicle while this stands and rebuilds it when it changes (a wheel edited in the Inspector).
uint64_t VehiclePropertyGroup::definitionHash() const {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << _type << _mass << _centerOfMass.x << _centerOfMass.y << _centerOfMass.z << _wheels << _seats
           << _maxSteerAngle << _maxTorque << _minRPM << _maxRPM << _gearRatios << _reverseGearRatio << _finalDriveRatio
           << _maxBrakeTorque << _maxHandBrakeTorque << _driveType << _suspensionFrequency << _suspensionDamping
           << _wheel << _axle
           << _envelopeVolume << _centerOfBuoyancy.x << _centerOfBuoyancy.y << _centerOfBuoyancy.z << _gasDensity
           << _maxThrust << _maxVerticalThrust << _thrustPoint.x << _thrustPoint.y << _thrustPoint.z
           << _dragArea.x << _dragArea.y << _dragArea.z << _angularDragArea.x << _angularDragArea.y << _angularDragArea.z
           << _rudderArea << _elevatorArea << _finPoint.x << _finPoint.y << _finPoint.z << _maxFinAngle
           << _ballastRate << _ventRate
           << _wingArea << _wingSpan << _wingPoint.x << _wingPoint.y << _wingPoint.z;
    QByteArray digest = QCryptographicHash::hash(bytes, QCryptographicHash::Md5);
    uint64_t hash = 0;
    memcpy(&hash, digest.constData(), sizeof(hash));
    return hash == 0 ? 1 : hash;
}

INSTANTIATE_PROPERTY_GROUP_CONVERSIONS(VehiclePropertyGroup)
