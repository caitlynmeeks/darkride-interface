//
//  ObjectConstraintHinge.cpp
//  libraries/physics/src
//
//  Created by Seth Alves 2017-4-11
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ObjectConstraintHinge.h"

#include <Jolt/Physics/Constraints/HingeConstraint.h>

#include <LogHandler.h>

#include "QVariantGLM.h"

#include "EntityTree.h"
#include <shared/WireStream.h>
#include "PhysicsLogging.h"


const uint16_t HINGE_VERSION_WITH_UNUSED_PAREMETERS = 1;
const uint16_t ObjectConstraintHinge::constraintVersion = 2;
const glm::vec3 DEFAULT_HINGE_AXIS(1.0f, 0.0f, 0.0f);

// A point given relative to the body's origin, as scripts give it, expressed relative to the body's centre of
// mass, which is what Jolt's LocalToBodyCOM constraint space wants.
static JPH::RVec3 toBodyCOMSpace(const JPH::Body* body, const glm::vec3& point) {
    return glmToJoltR(point - joltToGLM(body->GetShape()->GetCenterOfMass()));
}

// Bullet accepted any [low, high] arc: a lower bound above the upper one, or a span of a full turn or more, meant
// no limit. Jolt wants the lower limit in [-pi, 0] and the upper in [0, pi], with [-pi, pi] meaning no limit, so an
// arc that does not include the rest angle is widened to include it.
static void hingeLimitsForJolt(float low, float high, float& limitsMin, float& limitsMax) {
    if (low > high || high - low >= TWO_PI) {
        limitsMin = -PI;
        limitsMax = PI;
        return;
    }
    limitsMin = glm::clamp(low, -PI, 0.0f);
    limitsMax = glm::clamp(high, 0.0f, PI);
}

ObjectConstraintHinge::ObjectConstraintHinge(const QUuid& id, EntityItemPointer ownerEntity) :
    ObjectConstraint(DYNAMIC_TYPE_HINGE, id, ownerEntity),
    _axisInA(DEFAULT_HINGE_AXIS),
    _axisInB(DEFAULT_HINGE_AXIS)
{
    #if WANT_DEBUG
    qCDebug(physics) << "ObjectConstraintHinge::ObjectConstraintHinge";
    #endif
}

ObjectConstraintHinge::~ObjectConstraintHinge() {
    #if WANT_DEBUG
    qCDebug(physics) << "ObjectConstraintHinge::~ObjectConstraintHinge";
    #endif
}

std::vector<JPH::BodyID> ObjectConstraintHinge::getBodyIDs() {
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

void ObjectConstraintHinge::prepareForPhysicsSimulation() {
}

void ObjectConstraintHinge::updateHinge() {
    JPH::HingeConstraint* constraint { nullptr };
    float low;
    float high;

    withReadLock([&]{
        constraint = static_cast<JPH::HingeConstraint*>(_constraint.GetPtr());
        low = _low;
        high = _high;
    });

    if (!constraint) {
        return;
    }

    float limitsMin, limitsMax;
    hingeLimitsForJolt(low, high, limitsMin, limitsMax);
    constraint->SetLimits(limitsMin, limitsMax);
}


JPH::Constraint* ObjectConstraintHinge::getConstraint() {
    JPH::Constraint* constraint { nullptr };
    QUuid otherEntityID;
    glm::vec3 pivotInA;
    glm::vec3 axisInA;
    glm::vec3 pivotInB;
    glm::vec3 axisInB;
    float low;
    float high;

    withReadLock([&]{
        constraint = _constraint.GetPtr();
        pivotInA = _pivotInA;
        axisInA = _axisInA;
        otherEntityID = _otherID;
        pivotInB = _pivotInB;
        axisInB = _axisInB;
        low = _low;
        high = _high;
    });
    if (constraint) {
        return constraint;
    }

    static int repeatMessageID = LogHandler::getInstance().newRepeatedMessageID();

    JPH::Body* bodyA = getBody();
    if (!bodyA) {
        HIFI_FCDEBUG_ID(physics(), repeatMessageID, "ObjectConstraintHinge::getConstraint -- no bodyA");
        return nullptr;
    }

    if (glm::length(axisInA) < FLT_EPSILON) {
        qCWarning(physics) << "hinge axis cannot be a zero vector";
        axisInA = DEFAULT_HINGE_AXIS;
    } else {
        axisInA = glm::normalize(axisInA);
    }

    // The frames are the ones Bullet built: each body's frame is its pivot, its hinge axis and a normal
    // perpendicular to that axis, and the second frame's normal is the first one's carried over by the shortest
    // rotation between the two hinge axes. The hinge is at angle 0, from which low and high are measured, when the
    // two normals align; which perpendicular serves as the first normal makes no difference to that.
    JPH::HingeConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
    settings.mPoint1 = toBodyCOMSpace(bodyA, pivotInA);
    settings.mHingeAxis1 = glmToJolt(axisInA);
    settings.mNormalAxis1 = settings.mHingeAxis1.GetNormalizedPerpendicular();
    glm::vec3 normalInA = joltToGLM(settings.mNormalAxis1);

    JPH::Body* bodyB { nullptr };
    if (!otherEntityID.isNull()) {
        // This hinge is between two entities... find the other body.
        bodyB = getOtherBody(otherEntityID);
        if (!bodyB) {
            HIFI_FCDEBUG_ID(physics(), repeatMessageID, "ObjectConstraintHinge::getConstraint -- no bodyB");
            return nullptr;
        }

        if (glm::length(axisInB) < FLT_EPSILON) {
            qCWarning(physics) << "hinge axis cannot be a zero vector";
            axisInB = DEFAULT_HINGE_AXIS;
        } else {
            axisInB = glm::normalize(axisInB);
        }

        settings.mPoint2 = toBodyCOMSpace(bodyB, pivotInB);
        settings.mHingeAxis2 = glmToJolt(axisInB);
        settings.mNormalAxis2 = glmToJolt(glm::rotation(axisInA, axisInB) * normalInA);
    } else {
        // This hinge is between an entity and the world-frame. The other body is Jolt's fixed-to-world body, whose
        // own frame is the world frame, so its side of the joint is the entity's pivot and axis where they are now.
        bodyB = &JPH::Body::sFixedToWorld;
        glm::vec3 positionA = joltToGLM(bodyA->GetPosition());
        glm::quat rotationA = joltToGLM(bodyA->GetRotation());
        glm::vec3 axisInWorld = rotationA * axisInA;

        settings.mPoint2 = glmToJoltR(positionA + rotationA * pivotInA);
        settings.mHingeAxis2 = glmToJolt(axisInWorld);
        settings.mNormalAxis2 = glmToJolt(glm::rotation(axisInA, axisInWorld) * normalInA);
    }
    hingeLimitsForJolt(low, high, settings.mLimitsMin, settings.mLimitsMax);

    constraint = settings.Create(*bodyA, *bodyB);

    withWriteLock([&]{
        _constraint = constraint;
    });

    // if we don't wake up bodyA, we may not send the dynamicData property over the network
    forceBodyNonStatic();
    activateBody();

    updateHinge();

    return constraint;
}


bool ObjectConstraintHinge::updateArguments(QVariantMap arguments) {
    glm::vec3 pivotInA;
    glm::vec3 axisInA;
    QUuid otherEntityID;
    glm::vec3 pivotInB;
    glm::vec3 axisInB;
    float low;
    float high;

    bool needUpdate = false;
    bool somethingChanged = ObjectDynamic::updateArguments(arguments);
    withReadLock([&]{
        bool ok = true;
        pivotInA = EntityDynamicInterface::extractVec3Argument("hinge constraint", arguments, "pivot", ok, false);
        if (!ok) {
            pivotInA = _pivotInA;
        }

        ok = true;
        axisInA = EntityDynamicInterface::extractVec3Argument("hinge constraint", arguments, "axis", ok, false);
        if (!ok) {
            axisInA = _axisInA;
        }

        ok = true;
        otherEntityID = QUuid(EntityDynamicInterface::extractStringArgument("hinge constraint",
                                                                            arguments, "otherEntityID", ok, false));
        if (!ok) {
            otherEntityID = _otherID;
        }

        ok = true;
        pivotInB = EntityDynamicInterface::extractVec3Argument("hinge constraint", arguments, "otherPivot", ok, false);
        if (!ok) {
            pivotInB = _pivotInB;
        }

        ok = true;
        axisInB = EntityDynamicInterface::extractVec3Argument("hinge constraint", arguments, "otherAxis", ok, false);
        if (!ok) {
            axisInB = _axisInB;
        }

        ok = true;
        low = EntityDynamicInterface::extractFloatArgument("hinge constraint", arguments, "low", ok, false);
        if (!ok) {
            low = _low;
        }

        ok = true;
        high = EntityDynamicInterface::extractFloatArgument("hinge constraint", arguments, "high", ok, false);
        if (!ok) {
            high = _high;
        }

        if (somethingChanged ||
            pivotInA != _pivotInA ||
            axisInA != _axisInA ||
            otherEntityID != _otherID ||
            pivotInB != _pivotInB ||
            axisInB != _axisInB ||
            low != _low ||
            high != _high) {
            // something changed
            needUpdate = true;
        }
    });

    if (needUpdate) {
        withWriteLock([&] {
            _pivotInA = pivotInA;
            _axisInA = axisInA;
            _otherID = otherEntityID;
            _pivotInB = pivotInB;
            _axisInB = axisInB;
            _low = low;
            _high = high;

            _active = true;

            auto ownerEntity = _ownerEntity.lock();
            if (ownerEntity) {
                ownerEntity->setDynamicDataDirty(true);
                ownerEntity->setDynamicDataNeedsTransmit(true);
            }
        });

        updateHinge();
    }

    return true;
}

/**jsdoc
 * The <code>"hinge"</code> {@link Entities.ActionType|ActionType} lets an entity pivot about an axis or connects two entities
 * with a hinge joint.
 * It has arguments in addition to the common {@link Entities.ActionArguments|ActionArguments}:
 *
 * @typedef {object} Entities.ActionArguments-Hinge
 * @property {Uuid} otherEntityID=null - The ID of the other entity that is connected to the joint, if any. If none is
 *     specified then the first entity simply pivots about its specified <code>axis</code>.
 * @property {Vec3} pivot=0,0,0 - The local offset of the joint relative to the entity's position.
 * @property {Vec3} axis=1,0,0 - The axis of the entity that it pivots about. Must be a non-zero vector.
 * @property {Vec3} otherPivot=0,0,0 - The local offset of the joint relative to the other entity's position.
 * @property {Vec3} otherAxis=1,0,0 - The axis of the other entity that it pivots about. Must be a non-zero vector.
 * @property {number} low=-2*Math.PI - The most negative angle that the hinge can take, in radians. Limits are enforced
 *     between <code>-Math.PI</code> and <code>Math.PI</code> and always include the rest angle <code>0</code>; a range
 *     of a full turn or more, or <code>low</code> above <code>high</code>, leaves the hinge free.
 * @property {number} high=2*Math.PI - The most positive angle that the hinge can take, in radians.
 * @property {number} angle=0 - The current angle of the hinge, in radians, range <code>-Math.PI</code> &ndash; 
 *     <code>Math.PI</code>. <em>Read-only.</em>
 */
QVariantMap ObjectConstraintHinge::getArguments() {
    QVariantMap arguments = ObjectDynamic::getArguments();
    withReadLock([&] {
        arguments["pivot"] = vec3ToQMap(_pivotInA);
        arguments["axis"] = vec3ToQMap(_axisInA);
        arguments["otherEntityID"] = _otherID;
        arguments["otherPivot"] = vec3ToQMap(_pivotInB);
        arguments["otherAxis"] = vec3ToQMap(_axisInB);
        arguments["low"] = _low;
        arguments["high"] = _high;
        if (_constraint) {
            arguments["angle"] = static_cast<JPH::HingeConstraint*>(_constraint.GetPtr())->GetCurrentAngle(); // [-PI,PI]
        } else {
            arguments["angle"] = 0.0f;
        }
    });
    return arguments;
}

QByteArray ObjectConstraintHinge::serialize() const {
    QByteArray serializedConstraintArguments;
    QDataStream dataStream(&serializedConstraintArguments, QIODevice::WriteOnly);
    hifi::stream::pin(dataStream);

    dataStream << DYNAMIC_TYPE_HINGE;
    dataStream << getID();
    dataStream << ObjectConstraintHinge::constraintVersion;

    withReadLock([&] {
        dataStream << _pivotInA;
        dataStream << _axisInA;
        dataStream << _otherID;
        dataStream << _pivotInB;
        dataStream << _axisInB;
        dataStream << _low;
        dataStream << _high;

        dataStream << localTimeToServerTime(_expires);
        dataStream << _tag;
    });

    return serializedConstraintArguments;
}

void ObjectConstraintHinge::deserialize(QByteArray serializedArguments) {
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
    if (serializationVersion > ObjectConstraintHinge::constraintVersion) {
        assert(false);
        return;
    }

    withWriteLock([&] {
        dataStream >> _pivotInA;
        dataStream >> _axisInA;
        dataStream >> _otherID;
        dataStream >> _pivotInB;
        dataStream >> _axisInB;
        dataStream >> _low;
        dataStream >> _high;
        if (serializationVersion == HINGE_VERSION_WITH_UNUSED_PAREMETERS) {
            float softness, biasFactor, relaxationFactor;
            dataStream >> softness;
            dataStream >> biasFactor;
            dataStream >> relaxationFactor;
        }

        quint64 serverExpires;
        dataStream >> serverExpires;
        _expires = serverTimeToLocalTime(serverExpires);

        dataStream >> _tag;

        _active = true;
    });
}
