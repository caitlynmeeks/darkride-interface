//
//  ObjectConstraintSlider.cpp
//  libraries/physics/src
//
//  Created by Seth Alves 2017-4-23
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ObjectConstraintSlider.h"

#include <cfloat>

#include <Jolt/Physics/Constraints/SliderConstraint.h>

#include <LogHandler.h>

#include "QVariantGLM.h"

#include "EntityTree.h"
#include <shared/WireStream.h>
#include "PhysicsLogging.h"


const uint16_t ObjectConstraintSlider::constraintVersion = 1;
const glm::vec3 DEFAULT_SLIDER_AXIS(1.0f, 0.0f, 0.0f);

// A point given relative to the body's origin, as scripts give it, expressed relative to the body's centre of
// mass, which is what Jolt's LocalToBodyCOM constraint space wants.
static JPH::RVec3 toBodyCOMSpace(const JPH::Body* body, const glm::vec3& point) {
    return glmToJoltR(point - joltToGLM(body->GetShape()->GetCenterOfMass()));
}

// Bullet accepted any [low, high], with low above high meaning no limit. Jolt wants the lower limit at or below 0
// and the upper at or above 0 (0 being where the two points coincide), with -FLT_MAX / FLT_MAX meaning no limit,
// so a range that does not include 0 is widened to include it.
static void sliderLimitsForJolt(float low, float high, float& limitsMin, float& limitsMax) {
    if (low > high) {
        limitsMin = -FLT_MAX;
        limitsMax = FLT_MAX;
        return;
    }
    limitsMin = glm::min(low, 0.0f);
    limitsMax = glm::max(high, 0.0f);
}

ObjectConstraintSlider::ObjectConstraintSlider(const QUuid& id, EntityItemPointer ownerEntity) :
    ObjectConstraint(DYNAMIC_TYPE_SLIDER, id, ownerEntity),
    _axisInA(DEFAULT_SLIDER_AXIS),
    _axisInB(DEFAULT_SLIDER_AXIS)
{
}

ObjectConstraintSlider::~ObjectConstraintSlider() {
}

std::vector<JPH::BodyID> ObjectConstraintSlider::getBodyIDs() {
    std::vector<JPH::BodyID> result = ObjectDynamic::getBodyIDs();
    QUuid otherEntityID;
    withReadLock([&]{
        otherEntityID = _otherID;
    });
    if (!otherEntityID.isNull()) {
        JPH::Body* otherBody = getOtherBody(otherEntityID);
        if (otherBody) {
            result.push_back(otherBody->GetID());
        }
    }
    return result;
}

void ObjectConstraintSlider::prepareForPhysicsSimulation() {
}

void ObjectConstraintSlider::updateSlider() {
    JPH::SliderConstraint* constraint { nullptr };
    float linearLow;
    float linearHigh;

    withReadLock([&]{
        constraint = static_cast<JPH::SliderConstraint*>(_constraint.GetPtr());
        linearLow = _linearLow;
        linearHigh = _linearHigh;
    });

    if (!constraint) {
        return;
    }

    // Jolt's slider does not turn about its axis, so _angularLow and _angularHigh have nothing to set.
    float limitsMin, limitsMax;
    sliderLimitsForJolt(linearLow, linearHigh, limitsMin, limitsMax);
    constraint->SetLimits(limitsMin, limitsMax);
}


JPH::Constraint* ObjectConstraintSlider::getConstraint() {
    JPH::Constraint* constraint { nullptr };
    QUuid otherEntityID;
    glm::vec3 pointInA;
    glm::vec3 axisInA;
    glm::vec3 pointInB;
    glm::vec3 axisInB;
    float linearLow;
    float linearHigh;

    withReadLock([&]{
        constraint = _constraint.GetPtr();
        pointInA = _pointInA;
        axisInA = _axisInA;
        otherEntityID = _otherID;
        pointInB = _pointInB;
        axisInB = _axisInB;
        linearLow = _linearLow;
        linearHigh = _linearHigh;
    });
    if (constraint) {
        return constraint;
    }

    static int repeatMessageID = LogHandler::getInstance().newRepeatedMessageID();

    JPH::Body* bodyA = getBody();
    if (!bodyA) {
        HIFI_FCDEBUG_ID(physics(), repeatMessageID, "ObjectConstraintSlider::getConstraint -- no bodyA");
        return nullptr;
    }

    if (glm::length(axisInA) < FLT_EPSILON) {
        qCWarning(physics) << "slider axis cannot be a zero vector";
        axisInA = DEFAULT_SLIDER_AXIS;
    } else {
        axisInA = glm::normalize(axisInA);
    }

    JPH::SliderConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
    glm::vec3 normalInA = joltToGLM(glmToJolt(axisInA).GetNormalizedPerpendicular());
    glm::vec3 positionA = joltToGLM(bodyA->GetPosition());
    glm::quat rotationA = joltToGLM(bodyA->GetRotation());

    JPH::Body* body1 { nullptr };
    JPH::Body* body2 { nullptr };
    if (!otherEntityID.isNull()) {
        // This slider is between two entities... find the other body. As with Bullet this entity is the first body
        // and the other the second, so the slider position is the other point's offset along the axis from this
        // entity's point.
        JPH::Body* bodyB = getOtherBody(otherEntityID);
        if (!bodyB) {
            HIFI_FCDEBUG_ID(physics(), repeatMessageID, "ObjectConstraintSlider::getConstraint -- no bodyB");
            return nullptr;
        }

        if (glm::length(axisInB) < FLT_EPSILON) {
            qCWarning(physics) << "slider axis cannot be a zero vector";
            axisInB = DEFAULT_SLIDER_AXIS;
        } else {
            axisInB = glm::normalize(axisInB);
        }

        // Jolt's slider allows no rotation about its axis (Bullet's did, within the angular limits), so the bodies
        // are held at the rotation where the two frames' normals align. The second normal is taken from where the
        // bodies are now: they are only turned to align their axes, as Bullet turned them, and keep their present
        // rotation about it.
        glm::quat rotationB = joltToGLM(bodyB->GetRotation());
        glm::vec3 axisAInWorld = rotationA * axisInA;
        glm::vec3 axisBInWorld = rotationB * axisInB;
        glm::vec3 normalInB = glm::inverse(rotationB) *
            (glm::rotation(axisAInWorld, axisBInWorld) * (rotationA * normalInA));

        body1 = bodyA;
        body2 = bodyB;
        settings.mPoint1 = toBodyCOMSpace(bodyA, pointInA);
        settings.mSliderAxis1 = glmToJolt(axisInA);
        settings.mNormalAxis1 = glmToJolt(normalInA);
        settings.mPoint2 = toBodyCOMSpace(bodyB, pointInB);
        settings.mSliderAxis2 = glmToJolt(axisInB);
        settings.mNormalAxis2 = glmToJolt(normalInB);
    } else {
        // This slider is between an entity and the world-frame. As with Bullet the world is the first body and the
        // entity the second, so the slider position is the entity's offset along the axis from where it started.
        // The world side is Jolt's fixed-to-world body, whose own frame is the world frame; it gets the entity's
        // point and axis where they are now.
        body1 = &JPH::Body::sFixedToWorld;
        body2 = bodyA;
        settings.mPoint1 = glmToJoltR(positionA + rotationA * pointInA);
        settings.mSliderAxis1 = glmToJolt(rotationA * axisInA);
        settings.mNormalAxis1 = glmToJolt(rotationA * normalInA);
        settings.mPoint2 = toBodyCOMSpace(bodyA, pointInA);
        settings.mSliderAxis2 = glmToJolt(axisInA);
        settings.mNormalAxis2 = glmToJolt(normalInA);
    }
    sliderLimitsForJolt(linearLow, linearHigh, settings.mLimitsMin, settings.mLimitsMax);

    constraint = settings.Create(*body1, *body2);

    withWriteLock([&]{
        _constraint = constraint;
    });

    // if we don't wake up bodyA, we may not send the dynamicData property over the network
    forceBodyNonStatic();
    activateBody();

    updateSlider();

    return constraint;
}


bool ObjectConstraintSlider::updateArguments(QVariantMap arguments) {
    glm::vec3 pointInA;
    glm::vec3 axisInA;
    QUuid otherEntityID;
    glm::vec3 pointInB;
    glm::vec3 axisInB;
    float linearLow;
    float linearHigh;
    float angularLow;
    float angularHigh;

    bool needUpdate = false;
    bool somethingChanged = ObjectDynamic::updateArguments(arguments);
    withReadLock([&]{
        bool ok = true;
        pointInA = EntityDynamicInterface::extractVec3Argument("slider constraint", arguments, "point", ok, false);
        if (!ok) {
            pointInA = _pointInA;
        }

        ok = true;
        axisInA = EntityDynamicInterface::extractVec3Argument("slider constraint", arguments, "axis", ok, false);
        if (!ok) {
            axisInA = _axisInA;
        }

        ok = true;
        otherEntityID = QUuid(EntityDynamicInterface::extractStringArgument("slider constraint",
                                                                            arguments, "otherEntityID", ok, false));
        if (!ok) {
            otherEntityID = _otherID;
        }

        ok = true;
        pointInB = EntityDynamicInterface::extractVec3Argument("slider constraint", arguments, "otherPoint", ok, false);
        if (!ok) {
            pointInB = _pointInB;
        }

        ok = true;
        axisInB = EntityDynamicInterface::extractVec3Argument("slider constraint", arguments, "otherAxis", ok, false);
        if (!ok) {
            axisInB = _axisInB;
        }

        ok = true;
        linearLow = EntityDynamicInterface::extractFloatArgument("slider constraint", arguments, "linearLow", ok, false);
        if (!ok) {
            linearLow = _linearLow;
        }

        ok = true;
        linearHigh = EntityDynamicInterface::extractFloatArgument("slider constraint", arguments, "linearHigh", ok, false);
        if (!ok) {
            linearHigh = _linearHigh;
        }

        ok = true;
        angularLow = EntityDynamicInterface::extractFloatArgument("slider constraint", arguments, "angularLow", ok, false);
        if (!ok) {
            angularLow = _angularLow;
        }

        ok = true;
        angularHigh = EntityDynamicInterface::extractFloatArgument("slider constraint", arguments, "angularHigh", ok, false);
        if (!ok) {
            angularHigh = _angularHigh;
        }

        if (somethingChanged ||
            pointInA != _pointInA ||
            axisInA != _axisInA ||
            otherEntityID != _otherID ||
            pointInB != _pointInB ||
            axisInB != _axisInB ||
            linearLow != _linearLow ||
            linearHigh != _linearHigh ||
            angularLow != _angularLow ||
            angularHigh != _angularHigh) {
            // something changed
            needUpdate = true;
        }
    });

    if (needUpdate) {
        withWriteLock([&] {
            _pointInA = pointInA;
            _axisInA = axisInA;
            _otherID = otherEntityID;
            _pointInB = pointInB;
            _axisInB = axisInB;
            _linearLow = linearLow;
            _linearHigh = linearHigh;
            _angularLow = angularLow;
            _angularHigh = angularHigh;

            _active = true;

            auto ownerEntity = _ownerEntity.lock();
            if (ownerEntity) {
                ownerEntity->setDynamicDataDirty(true);
                ownerEntity->setDynamicDataNeedsTransmit(true);
            }
        });

        updateSlider();
    }

    return true;
}

/**jsdoc
 * The <code>"slider"</code> {@link Entities.ActionType|ActionType} lets an entity slide along an axis, or connects two
 * entities that slide along a shared axis.
 * It has arguments in addition to the common {@link Entities.ActionArguments|ActionArguments}:
 *
 * @typedef {object} Entities.ActionArguments-Slider
 * @property {Uuid} otherEntityID=null - The ID of the other entity that is connected to the joint, if any. If none is
 *     specified then the first entity simply slides along its specified <code>axis</code>.
 * @property {Vec3} point=0,0,0 - The local position of a point in the entity that slides along the axis.
 * @property {Vec3} axis=1,0,0 - The axis of the entity that slides along the joint. Must be a non-zero vector.
 * @property {Vec3} otherPoint=0,0,0 - The local position of a point in the other entity that slides along the axis.
 * @property {Vec3} otherAxis=1,0,0 - The axis of the other entity that slides along the joint. Must be a non-zero vector.
 * @property {number} linearLow=1.17e-38 - The most negative linear offset from the entity's initial point that the entity can 
 *     have along the slider. Limits always include the initial point <code>0</code>; <code>linearLow</code> above
 *     <code>linearHigh</code> leaves the slider free.
 * @property {number} linearHigh=3.40e+38 - The most positive linear offset from the entity's initial point that the entity can 
 *     have along the slider. 
 * @property {number} angularLow=-2*Math.PI - The most negative angle that the entity can rotate about the axis if the action 
 *     involves only one entity, otherwise the most negative angle the rotation can be between the two entities. In radians.
 *     <em>Not enforced: the joint does not rotate about its axis. The value is stored and reported only.</em>
 * @property {number} angularHigh=Math.PI - The most positive angle that the entity can rotate about the axis if the action 
 *     involves only one entity, otherwise the most positive angle the rotation can be between the two entities. In radians.
 *     <em>Not enforced: the joint does not rotate about its axis. The value is stored and reported only.</em>
 * @property {number} linearPosition=0 - The current linear offset the entity is from its initial point if the action involves 
 *     only one entity, otherwise the linear offset between the two entities' action points. <em>Read-only.</em>
 * @property {number} angularPosition=0 - The current angular offset of the entity from its initial rotation if the action 
 *     involves only one entity, otherwise the angular offset between the two entities. In radians. Always <code>0</code>,
 *     as the joint does not rotate about its axis. <em>Read-only.</em>
 */
QVariantMap ObjectConstraintSlider::getArguments() {
    QVariantMap arguments = ObjectDynamic::getArguments();
    withReadLock([&] {
        arguments["point"] = vec3ToQMap(_pointInA);
        arguments["axis"] = vec3ToQMap(_axisInA);
        arguments["otherEntityID"] = _otherID;
        arguments["otherPoint"] = vec3ToQMap(_pointInB);
        arguments["otherAxis"] = vec3ToQMap(_axisInB);
        arguments["linearLow"] = _linearLow;
        arguments["linearHigh"] = _linearHigh;
        arguments["angularLow"] = _angularLow;
        arguments["angularHigh"] = _angularHigh;
        if (_constraint) {
            arguments["linearPosition"] = static_cast<JPH::SliderConstraint*>(_constraint.GetPtr())->GetCurrentPosition();
        } else {
            arguments["linearPosition"] = 0.0f;
        }
        // Jolt's slider holds the rotation about its axis fixed
        arguments["angularPosition"] = 0.0f;
    });
    return arguments;
}

QByteArray ObjectConstraintSlider::serialize() const {
    QByteArray serializedConstraintArguments;
    QDataStream dataStream(&serializedConstraintArguments, QIODevice::WriteOnly);
    hifi::stream::pin(dataStream);

    dataStream << DYNAMIC_TYPE_SLIDER;
    dataStream << getID();
    dataStream << ObjectConstraintSlider::constraintVersion;

    withReadLock([&] {
        dataStream << localTimeToServerTime(_expires);
        dataStream << _tag;

        dataStream << _pointInA;
        dataStream << _axisInA;
        dataStream << _otherID;
        dataStream << _pointInB;
        dataStream << _axisInB;
        dataStream << _linearLow;
        dataStream << _linearHigh;
        dataStream << _angularLow;
        dataStream << _angularHigh;
    });

    return serializedConstraintArguments;
}

void ObjectConstraintSlider::deserialize(QByteArray serializedArguments) {
    QDataStream dataStream(serializedArguments);
    hifi::stream::pin(dataStream);

    EntityDynamicType type;
    dataStream >> type;
    assert(type == getType());

    QUuid id;
    dataStream >> id;
    assert(id == getID());

    uint16_t serializationVersion;
    dataStream >> serializationVersion;
    if (serializationVersion != ObjectConstraintSlider::constraintVersion) {
        assert(false);
        return;
    }

    withWriteLock([&] {
        quint64 serverExpires;
        dataStream >> serverExpires;
        _expires = serverTimeToLocalTime(serverExpires);
        dataStream >> _tag;

        dataStream >> _pointInA;
        dataStream >> _axisInA;
        dataStream >> _otherID;
        dataStream >> _pointInB;
        dataStream >> _axisInB;
        dataStream >> _linearLow;
        dataStream >> _linearHigh;
        dataStream >> _angularLow;
        dataStream >> _angularHigh;

        _active = true;
    });
}
