//
//  VehiclePropertyGroup.h
//  libraries/entities/src
//
//  The vehicle property group (PHYSICS.md 4.8, rounds 6 and 7): what makes an entity a vehicle (a wheeled one, or an
//  airship) and the state of its simulation. See the jsdoc below for every property. Generated from a table
//  (scratchpad/gen_vehicle_group.py, round 7's copy) and kept by hand.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_VehiclePropertyGroup_h
#define hifi_VehiclePropertyGroup_h

#include <stdint.h>

#include <glm/glm.hpp>

#include <QtCore/QUuid>
#include <QtScript/QScriptEngine>

#include "PropertyGroup.h"
#include "EntityItemPropertiesMacros.h"

class EntityItemProperties;
class EncodeBitstreamParams;
class OctreePacketData;
class ReadBitstreamToTreeParams;

// The vehicle's kind, as the type property names it.
namespace VehicleType {
    const QString NONE = "none";
    const QString WHEELED = "wheeled";
    const QString WHEEL = "wheel";
    const QString AIRSHIP = "airship";
    const QString AIRCRAFT = "aircraft";
}

/**jsdoc
 * A vehicle is any entity with a <code>vehicle</code> group whose <code>type</code> is not <code>"none"</code> (PHYSICS.md
 * 4.8, rounds 6 and 7). The entity is the chassis or the hull: its own collision shape (a box is best; the physics
 * server always uses a box of the entity's dimensions), its <code>mass</code> and <code>centerOfMass</code>, the wheels
 * hung from it and the seats in it. The names are Unity's where Unity has them (<code>Rigidbody</code>,
 * <code>WheelCollider</code>) so a Unity vehicle carries across, and Jolt's for the engine and transmission, which Unity
 * leaves to scripts. The vehicle is simulated by the physics server when the domain has one, else by the client that
 * owns it; the driver's client predicts its own inputs and is corrected by the server; everyone else runs the model
 * from the last known inputs and blends corrections. The state properties are read-only: the simulator writes them.
 * An airship (<code>type</code> <code>"airship"</code>) is a hull with buoyancy, propellers, fins and ballast; make the
 * entity a moving reference frame too (<code>frame</code> <code>true</code>) and its top is a deck the crew walks on
 * while it flies.
 *
 * @typedef {object} Entities.Vehicle
 * @property {string} type="none" - <code>"none"</code>, <code>"wheeled"</code> (a chassis with wheels),
 *     <code>"wheel"</code> (a child entity that shows one of its parent's wheels: see <code>wheel</code>), or
 *     <code>"airship"</code> (a lighter-than-air hull: see <code>envelopeVolume</code> and the rest).
 * @property {number} mass=0 - The chassis mass in kg (Unity's <code>Rigidbody.mass</code>); <code>0</code> uses the
 *     entity's density and volume as for any entity.
 * @property {Vec3} centerOfMass=0,0,0 - The centre of mass, in the entity's local metres (<code>Rigidbody.centerOfMass</code>).
 *     Low and central keeps a car on its wheels.
 * @property {string} wheels="" - The wheels as a JSON list, each with Unity's <code>WheelCollider</code> names:
 *     <code>name</code>; <code>center</code> [x, y, z] in local metres, the suspension's top (fully compressed) attachment
 *     point; <code>radius</code> and <code>width</code> m; <code>mass</code> kg (default 20); <code>suspensionDistance</code>
 *     m, the travel from <code>center</code> downwards (default 0.2); <code>spring</code> N/m and <code>damper</code> N s/m
 *     (when absent, <code>suspensionFrequency</code> and <code>suspensionDamping</code> below set the spring);
 *     <code>steer</code>, a fraction of <code>maxSteerAngle</code> this wheel turns, negative to counter-steer (default 0);
 *     <code>drive</code>, <code>true</code> when the engine turns it (default from <code>driveType</code>);
 *     <code>brake</code> and <code>handBrake</code>, fractions of the brake torques this wheel takes (defaults 1 and 0).
 * @property {string} seats="" - The seats as a JSON list: <code>name</code>, <code>position</code> [x, y, z] in local
 *     metres (where the avatar's hips go), <code>yaw</code> in degrees about the entity's up (default 0, facing
 *     forward, the entity's +z... the vehicle's forward is its local -z as for every entity), and <code>driver</code>,
 *     <code>true</code> for the one seat whose occupant drives.
 * @property {number} maxSteerAngle=30 - The steering lock, in degrees, for a wheel with <code>steer</code> 1.
 * @property {number} maxTorque=500 - The engine's peak torque, N m (Jolt's <code>VehicleEngineSettings</code>).
 * @property {number} minRPM=1000 - The engine's idle, rpm.
 * @property {number} maxRPM=6000 - The engine's red line, rpm.
 * @property {string} gearRatios="" - The forward gear ratios as a JSON list, first gear first; empty for Jolt's
 *     five-speed default. The automatic gearbox shifts for the driver.
 * @property {number} reverseGearRatio=2.9 - The reverse gear's ratio.
 * @property {number} finalDriveRatio=3.42 - The differential's ratio (the axle, times the transfer box if any).
 * @property {number} maxBrakeTorque=1500 - The foot brake's torque per wheel, N m.
 * @property {number} maxHandBrakeTorque=4000 - The hand brake's torque per wheel with <code>handBrake</code> 1, N m.
 * @property {string} driveType="rear" - Which wheels the engine turns when a wheel does not say: <code>"rear"</code>,
 *     <code>"front"</code> or <code>"all"</code>. Wheels are paired into axles by their <code>center</code> z.
 * @property {number} suspensionFrequency=1.5 - The suspension's natural frequency in Hz, for wheels without a
 *     <code>spring</code>; 1 to 2 Hz is a road car, lower is softer.
 * @property {number} suspensionDamping=0.5 - The suspension's damping ratio for those wheels (1 is critical).
 * @property {string} wheel="" - On a child entity with <code>type</code> <code>"wheel"</code>: the <code>name</code> of
 *     the parent's wheel it shows. Every client places it from its own simulation: it steers, spins and rides the
 *     suspension, and it costs no packets.
 * @property {string} axle="x" - On a wheel entity: which of its own axes is the axle, <code>"x"</code> (a model made
 *     for Unity), <code>"y"</code> (a Cylinder shape entity) or <code>"z"</code>.
 * @property {Uuid} driver=null - <em>State.</em> The session of the avatar in the driver's seat, who steers it;
 *     set by that client when it sits, cleared when it stands.
 * @property {number} inputSequence=0 - <em>State.</em> The sequence number of the driver's last input the simulator
 *     applied; the driver's client reconciles its prediction from there.
 * @property {number} throttle=0 - <em>State.</em> The last applied forward input, -1 (reverse) to 1.
 * @property {number} steering=0 - <em>State.</em> The last applied steering input, -1 (left) to 1 (right).
 * @property {number} brake=0 - <em>State.</em> The last applied brake, 0 to 1.
 * @property {number} handBrake=0 - <em>State.</em> The last applied hand brake, 0 to 1.
 * @property {number} engineRPM=0 - <em>State.</em> The engine's speed.
 * @property {number} gear=0 - <em>State.</em> The gear: 1 and up forward, 0 neutral, -1 reverse.
 * @property {number} stateTime=0 - <em>State.</em> The domain time, in microseconds, that this state and the entity's
 *     position, rotation and velocities in the same update are for.
 * @property {number} envelopeVolume=0 - An airship's envelope, m<sup>3</sup>. Its lift is the volume times
 *     <code>fill</code> times the difference between the air's density at its altitude (the standard atmosphere, sea
 *     level at y = 0) and <code>gasDensity</code>, times gravity, straight up at <code>centerOfBuoyancy</code>; a
 *     1,500 m<sup>3</sup> helium envelope lifts about 1,570 kg at sea level. <code>mass</code> is the hull's without
 *     ballast.
 * @property {Vec3} centerOfBuoyancy=0,0,0 - The envelope's centre in local metres. Above <code>centerOfMass</code> it
 *     keeps the ship upright, as a pendulum.
 * @property {number} gasDensity=0.1786 - The lifting gas, kg/m<sup>3</sup>: helium 0.1786, hydrogen 0.0899.
 * @property {number} maxThrust=0 - The propellers' thrust at full throttle, N, along the ship's forward.
 * @property {number} maxVerticalThrust=0 - The vertical thruster's at full <code>lift</code>, N, along the ship's up.
 * @property {Vec3} thrustPoint=0,0,0 - Where both act, local metres (low on the gondola, it pitches the nose a little).
 * @property {Vec3} dragArea=0,0,0 - The drag coefficient times area per local axis, m<sup>2</sup>, for the drag at
 *     <code>centerOfBuoyancy</code> (half the air's density times speed squared times this); 0,0,0 derives one from the
 *     entity's dimensions.
 * @property {Vec3} angularDragArea=0,0,0 - The same for the spin about each local axis, m<sup>5</sup>; 0,0,0 derives.
 * @property {number} rudderArea=0 - The vertical fin's area, m<sup>2</sup>. The fins are flat plates at
 *     <code>finPoint</code>, lifting with the angle between them and the relative wind: the ship's own sideslip and
 *     incidence (which is what makes the nose follow the velocity) plus the pilot's deflection.
 * @property {number} elevatorArea=0 - The horizontal fin's area, m<sup>2</sup>.
 * @property {Vec3} finPoint=0,0,0 - Where the fins act, local metres: aft of the envelope's centre (+z).
 * @property {number} maxFinAngle=25 - The fins' deflection at full <code>steering</code> or <code>pitch</code>, degrees.
 * @property {number} ballastRate=0 - Ballast dropped per second at full positive <code>trim</code>, kg/s.
 * @property {number} ventRate=0 - The fraction of the envelope vented per second at full negative <code>trim</code>.
 * @property {number} pitch=0 - <em>State.</em> The last applied elevator input, -1 (nose down) to 1 (nose up).
 * @property {number} lift=0 - <em>State.</em> The last applied vertical thruster input, -1 (down) to 1 (up).
 * @property {number} trim=0 - <em>State.</em> The last applied trim, 1 dropping ballast, -1 venting gas.
 * @property {number} ballast=0 - <em>State.</em> The ballast carried now, kg: set it when the ship is made, the
 *     simulator lowers it as the pilot drops ballast.
 * @property {number} fill=1 - <em>State.</em> The fraction of the envelope that is gas: the simulator lowers it as the
 *     pilot vents.
 * @property {number} wingArea=0 - An aircraft's wing, m<sup>2</sup>, both halves (<code>type</code> <code>"aircraft"</code>,
 *     PHYSICS.md round 8: a tilt-thrust craft). The wing lifts with its angle of attack up to the stall at 15 degrees;
 *     <code>maxThrust</code> is its engines' at full throttle, along forward with the nacelles level and along up with
 *     them vertical (<code>tilt</code>), <code>elevatorArea</code>, <code>rudderArea</code> and <code>finPoint</code>
 *     its tail, <code>maxFinAngle</code> its control deflection (the ailerons move 0.4 of it, the rudder 0.5).
 * @property {number} wingSpan=0 - The wing's span, m, tip to tip: with the area it gives the aspect ratio, and the
 *     ailerons act at a quarter of it either side.
 * @property {Vec3} wingPoint=0,0,0 - The wing's aerodynamic centre, local metres (a quarter chord behind the leading
 *     edge; a little aft of <code>centerOfMass</code> for stability).
 * @property {number} tilt=0 - <em>State.</em> The nacelles' tilt, 0 level (forward flight) to 1 vertical (hover):
 *     the pilot's <code>lift</code> input moves it, up while positive and down while negative, at 0.35 a second; set
 *     it to 1 when the craft is made to start in hover.
 */
class VehiclePropertyGroup : public PropertyGroup {
public:
    template <typename Value, typename Engine>
    void copyToScriptValue(const EntityPropertyFlags& desiredProperties, Value& properties,
                                   Engine* engine, bool skipDefaults,
                                   EntityItemProperties& defaultEntityProperties) const;
    template <typename Value>
    void copyFromScriptValue(const Value& object, bool& _defaultSettings);

    void merge(const VehiclePropertyGroup& other);

    virtual void debugDump() const override;
    virtual void listChangedProperties(QList<QString>& out) override;

    virtual bool appendToEditPacket(OctreePacketData* packetData,
                                    EntityPropertyFlags& requestedProperties,
                                    EntityPropertyFlags& propertyFlags,
                                    EntityPropertyFlags& propertiesDidntFit,
                                    int& propertyCount,
                                    OctreeElement::AppendState& appendState) const override;

    virtual bool decodeFromEditPacket(EntityPropertyFlags& propertyFlags,
                                      const unsigned char*& dataAt, int& processedBytes, int bytesToRead) override;
    virtual void markAllChanged() override;
    virtual EntityPropertyFlags getChangedProperties() const override;

    // EntityItem related helpers
    // methods for getting/setting all properties of an entity
    virtual void getProperties(EntityItemProperties& propertiesOut) const override;

    /// returns true if something changed
    virtual bool setProperties(const EntityItemProperties& properties) override;

    virtual EntityPropertyFlags getEntityProperties(EncodeBitstreamParams& params) const override;

    virtual void appendSubclassData(OctreePacketData* packetData, EncodeBitstreamParams& params,
                                    EntityTreeElementExtraEncodeDataPointer entityTreeElementExtraEncodeData,
                                    EntityPropertyFlags& requestedProperties,
                                    EntityPropertyFlags& propertyFlags,
                                    EntityPropertyFlags& propertiesDidntFit,
                                    int& propertyCount,
                                    OctreeElement::AppendState& appendState) const override;

    virtual int readEntitySubclassDataFromBuffer(const unsigned char* data, int bytesLeftToRead,
                                                 ReadBitstreamToTreeParams& args,
                                                 EntityPropertyFlags& propertyFlags, bool overwriteLocalData,
                                                 bool& somethingChanged) override;

    // true for a chassis or a hull: a vehicle the physics simulates (not a wheel, not "none")
    bool isVehicle() const { return _type == VehicleType::WHEELED || _type == VehicleType::AIRSHIP || _type == VehicleType::AIRCRAFT; }
    bool isWheel() const { return _type == VehicleType::WHEEL; }
    bool isAirship() const { return _type == VehicleType::AIRSHIP; }
    bool isAircraft() const { return _type == VehicleType::AIRCRAFT; }
    // the definition (everything but the state) as a version: the simulation rebuilds its vehicle when it changes
    uint64_t definitionHash() const;

    DEFINE_PROPERTY_REF(PROP_VEHICLE_TYPE, Type, type, QString, QString("none"));
    DEFINE_PROPERTY(PROP_VEHICLE_MASS, Mass, mass, float, 0.0f);
    DEFINE_PROPERTY_REF(PROP_VEHICLE_CENTER_OF_MASS, CenterOfMass, centerOfMass, glm::vec3, glm::vec3(0.0f));
    DEFINE_PROPERTY_REF(PROP_VEHICLE_WHEELS, Wheels, wheels, QString, QString(""));
    DEFINE_PROPERTY_REF(PROP_VEHICLE_SEATS, Seats, seats, QString, QString(""));
    DEFINE_PROPERTY(PROP_VEHICLE_MAX_STEER_ANGLE, MaxSteerAngle, maxSteerAngle, float, 30.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_MAX_TORQUE, MaxTorque, maxTorque, float, 500.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_MIN_RPM, MinRPM, minRPM, float, 1000.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_MAX_RPM, MaxRPM, maxRPM, float, 6000.0f);
    DEFINE_PROPERTY_REF(PROP_VEHICLE_GEAR_RATIOS, GearRatios, gearRatios, QString, QString(""));
    DEFINE_PROPERTY(PROP_VEHICLE_REVERSE_GEAR_RATIO, ReverseGearRatio, reverseGearRatio, float, 2.9f);
    DEFINE_PROPERTY(PROP_VEHICLE_FINAL_DRIVE_RATIO, FinalDriveRatio, finalDriveRatio, float, 3.42f);
    DEFINE_PROPERTY(PROP_VEHICLE_MAX_BRAKE_TORQUE, MaxBrakeTorque, maxBrakeTorque, float, 1500.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_MAX_HAND_BRAKE_TORQUE, MaxHandBrakeTorque, maxHandBrakeTorque, float, 4000.0f);
    DEFINE_PROPERTY_REF(PROP_VEHICLE_DRIVE_TYPE, DriveType, driveType, QString, QString("rear"));
    DEFINE_PROPERTY(PROP_VEHICLE_SUSPENSION_FREQUENCY, SuspensionFrequency, suspensionFrequency, float, 1.5f);
    DEFINE_PROPERTY(PROP_VEHICLE_SUSPENSION_DAMPING, SuspensionDamping, suspensionDamping, float, 0.5f);
    DEFINE_PROPERTY_REF(PROP_VEHICLE_WHEEL, Wheel, wheel, QString, QString(""));
    DEFINE_PROPERTY_REF(PROP_VEHICLE_AXLE, Axle, axle, QString, QString("x"));
    DEFINE_PROPERTY_REF(PROP_VEHICLE_DRIVER, Driver, driver, QUuid, QUuid());
    DEFINE_PROPERTY(PROP_VEHICLE_INPUT_SEQUENCE, InputSequence, inputSequence, quint32, 0);
    DEFINE_PROPERTY(PROP_VEHICLE_THROTTLE, Throttle, throttle, float, 0.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_STEERING, Steering, steering, float, 0.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_BRAKE, Brake, brake, float, 0.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_HAND_BRAKE, HandBrake, handBrake, float, 0.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_ENGINE_RPM, EngineRPM, engineRPM, float, 0.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_GEAR, Gear, gear, int, 0);
    DEFINE_PROPERTY(PROP_VEHICLE_STATE_TIME, StateTime, stateTime, quint64, 0);
    DEFINE_PROPERTY(PROP_VEHICLE_ENVELOPE_VOLUME, EnvelopeVolume, envelopeVolume, float, 0.0f);
    DEFINE_PROPERTY_REF(PROP_VEHICLE_CENTER_OF_BUOYANCY, CenterOfBuoyancy, centerOfBuoyancy, glm::vec3, glm::vec3(0.0f));
    DEFINE_PROPERTY(PROP_VEHICLE_GAS_DENSITY, GasDensity, gasDensity, float, 0.1786f);
    DEFINE_PROPERTY(PROP_VEHICLE_MAX_THRUST, MaxThrust, maxThrust, float, 0.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_MAX_VERTICAL_THRUST, MaxVerticalThrust, maxVerticalThrust, float, 0.0f);
    DEFINE_PROPERTY_REF(PROP_VEHICLE_THRUST_POINT, ThrustPoint, thrustPoint, glm::vec3, glm::vec3(0.0f));
    DEFINE_PROPERTY_REF(PROP_VEHICLE_DRAG_AREA, DragArea, dragArea, glm::vec3, glm::vec3(0.0f));
    DEFINE_PROPERTY_REF(PROP_VEHICLE_ANGULAR_DRAG_AREA, AngularDragArea, angularDragArea, glm::vec3, glm::vec3(0.0f));
    DEFINE_PROPERTY(PROP_VEHICLE_RUDDER_AREA, RudderArea, rudderArea, float, 0.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_ELEVATOR_AREA, ElevatorArea, elevatorArea, float, 0.0f);
    DEFINE_PROPERTY_REF(PROP_VEHICLE_FIN_POINT, FinPoint, finPoint, glm::vec3, glm::vec3(0.0f));
    DEFINE_PROPERTY(PROP_VEHICLE_MAX_FIN_ANGLE, MaxFinAngle, maxFinAngle, float, 25.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_BALLAST_RATE, BallastRate, ballastRate, float, 0.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_VENT_RATE, VentRate, ventRate, float, 0.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_PITCH, Pitch, pitch, float, 0.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_LIFT, Lift, lift, float, 0.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_TRIM, Trim, trim, float, 0.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_BALLAST, Ballast, ballast, float, 0.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_FILL, Fill, fill, float, 1.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_WING_AREA, WingArea, wingArea, float, 0.0f);
    DEFINE_PROPERTY(PROP_VEHICLE_WING_SPAN, WingSpan, wingSpan, float, 0.0f);
    DEFINE_PROPERTY_REF(PROP_VEHICLE_WING_POINT, WingPoint, wingPoint, glm::vec3, glm::vec3(0.0f));
    DEFINE_PROPERTY(PROP_VEHICLE_TILT, Tilt, tilt, float, 0.0f);
};

#endif // hifi_VehiclePropertyGroup_h
