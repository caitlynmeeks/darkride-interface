//
//  ObjectConstraintConeTwist.cpp
//  libraries/physics/src
//
//  Created by Seth Alves 2017-4-29
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ObjectConstraintConeTwist.h"

#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>

#include <LogHandler.h>

#include "QVariantGLM.h"

#include "EntityTree.h"
#include <shared/WireStream.h>
#include "PhysicsLogging.h"

const uint16_t CONE_TWIST_VERSION_WITH_UNUSED_PAREMETERS = 1;
const uint16_t ObjectConstraintConeTwist::constraintVersion = 2;
const glm::vec3 DEFAULT_CONE_TWIST_AXIS(1.0f, 0.0f, 0.0f);
// The joint frame, as Bullet built it, is the default axis turned onto the given one; its X is the twist axis.
const glm::vec3 CONE_TWIST_FRAME_Z(0.0f, 0.0f, 1.0f);

// A point given relative to the body's origin, as scripts give it, expressed relative to the body's centre of
// mass, which is what Jolt's LocalToBodyCOM constraint space wants.
static JPH::RVec3 toBodyCOMSpace(const JPH::Body* body, const glm::vec3& point) {
    return glmToJoltR(point - joltToGLM(body->GetShape()->GetCenterOfMass()));
}

// Bullet's spans were half angles without an upper bound: anything from pi up meant no limit, and a negative twist
// span no twist limit. Jolt wants half cone angles in [0, pi] and twist angles in [-pi, pi], where pi means free.
static float swingHalfAngleForJolt(float span) {
    return glm::clamp(span, 0.0f, PI);
}

static float twistHalfAngleForJolt(float span) {
    return span < 0.0f ? PI : glm::min(span, PI);
}

ObjectConstraintConeTwist::ObjectConstraintConeTwist(const QUuid& id, EntityItemPointer ownerEntity) :
    ObjectConstraint(DYNAMIC_TYPE_CONE_TWIST, id, ownerEntity),
    _axisInA(DEFAULT_CONE_TWIST_AXIS),
    _axisInB(DEFAULT_CONE_TWIST_AXIS)
{
    #if WANT_DEBUG
    qCDebug(physics) << "ObjectConstraintConeTwist::ObjectConstraintConeTwist";
    #endif
}

ObjectConstraintConeTwist::~ObjectConstraintConeTwist() {
    #if WANT_DEBUG
    qCDebug(physics) << "ObjectConstraintConeTwist::~ObjectConstraintConeTwist";
    #endif
}

std::vector<JPH::BodyID> ObjectConstraintConeTwist::getBodyIDs() {
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

void ObjectConstraintConeTwist::prepareForPhysicsSimulation() {
}

void ObjectConstraintConeTwist::updateConeTwist() {
    JPH::SwingTwistConstraint* constraint { nullptr };
    float swingSpan1;
    float swingSpan2;
    float twistSpan;

    withReadLock([&]{
        constraint = static_cast<JPH::SwingTwistConstraint*>(_constraint.GetPtr());
        swingSpan1 = _swingSpan1;
        swingSpan2 = _swingSpan2;
        twistSpan = _twistSpan;
    });

    if (!constraint) {
        return;
    }

    // swingSpan1 bounds the swing about the frame's Z (the twist axis leaning toward the frame's Y, Jolt's normal
    // axis); swingSpan2 the swing about the frame's Y (leaning toward the frame's Z, Jolt's plane axis).
    constraint->SetNormalHalfConeAngle(swingHalfAngleForJolt(swingSpan1));
    constraint->SetPlaneHalfConeAngle(swingHalfAngleForJolt(swingSpan2));
    float twist = twistHalfAngleForJolt(twistSpan);
    constraint->SetTwistMinAngle(-twist);
    constraint->SetTwistMaxAngle(twist);
}


JPH::Constraint* ObjectConstraintConeTwist::getConstraint() {
    JPH::Constraint* constraint { nullptr };
    QUuid otherEntityID;
    glm::vec3 pivotInA;
    glm::vec3 axisInA;
    glm::vec3 pivotInB;
    glm::vec3 axisInB;
    float swingSpan1;
    float swingSpan2;
    float twistSpan;

    withReadLock([&]{
        constraint = _constraint.GetPtr();
        pivotInA = _pivotInA;
        axisInA = _axisInA;
        otherEntityID = _otherID;
        pivotInB = _pivotInB;
        axisInB = _axisInB;
        swingSpan1 = _swingSpan1;
        swingSpan2 = _swingSpan2;
        twistSpan = _twistSpan;
    });
    if (constraint) {
        return constraint;
    }

    static int repeatMessageID = LogHandler::getInstance().newRepeatedMessageID();

    JPH::Body* bodyA = getBody();
    if (!bodyA) {
        HIFI_FCDEBUG_ID(physics(), repeatMessageID, "ObjectConstraintConeTwist::getConstraint -- no bodyA");
        return nullptr;
    }

    if (glm::length(axisInA) < FLT_EPSILON) {
        qCWarning(physics) << "cone-twist axis cannot be a zero vector";
        axisInA = DEFAULT_CONE_TWIST_AXIS;
    } else {
        axisInA = glm::normalize(axisInA);
    }

    // Jolt's constraint frame is made the same as Bullet's: the twist axis is the frame's X (the given axis) and
    // the plane axis the frame's Z, which makes Jolt's normal axis the frame's Y. The joint is at rest, with no
    // swing and no twist, when the two frames align.
    JPH::SwingTwistConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
    settings.mSwingType = JPH::ESwingType::Cone; // an elliptical cone, as Bullet's
    glm::quat rotA = glm::rotation(DEFAULT_CONE_TWIST_AXIS, axisInA);
    settings.mPosition1 = toBodyCOMSpace(bodyA, pivotInA);
    settings.mTwistAxis1 = glmToJolt(axisInA);
    settings.mPlaneAxis1 = glmToJolt(rotA * CONE_TWIST_FRAME_Z);

    JPH::Body* bodyB { nullptr };
    if (!otherEntityID.isNull()) {
        // This coneTwist is between two entities... find the other body.
        bodyB = getOtherBody(otherEntityID);
        if (!bodyB) {
            HIFI_FCDEBUG_ID(physics(), repeatMessageID, "ObjectConstraintConeTwist::getConstraint -- no bodyB");
            return nullptr;
        }

        if (glm::length(axisInB) < FLT_EPSILON) {
            qCWarning(physics) << "cone-twist axis cannot be a zero vector";
            axisInB = DEFAULT_CONE_TWIST_AXIS;
        } else {
            axisInB = glm::normalize(axisInB);
        }

        glm::quat rotB = glm::rotation(DEFAULT_CONE_TWIST_AXIS, axisInB);
        settings.mPosition2 = toBodyCOMSpace(bodyB, pivotInB);
        settings.mTwistAxis2 = glmToJolt(axisInB);
        settings.mPlaneAxis2 = glmToJolt(rotB * CONE_TWIST_FRAME_Z);
    } else {
        // This coneTwist is between an entity and the world-frame. The other body is Jolt's fixed-to-world body,
        // whose own frame is the world frame. As with Bullet, the world side gets the entity's frame as a
        // world-space frame, so swing and twist are measured from the entity's identity rotation; its pivot,
        // though, is where the entity's pivot is now (Bullet left it at the world origin).
        bodyB = &JPH::Body::sFixedToWorld;
        glm::vec3 positionA = joltToGLM(bodyA->GetPosition());
        glm::quat rotationA = joltToGLM(bodyA->GetRotation());
        settings.mPosition2 = glmToJoltR(positionA + rotationA * pivotInA);
        settings.mTwistAxis2 = settings.mTwistAxis1;
        settings.mPlaneAxis2 = settings.mPlaneAxis1;
    }
    settings.mNormalHalfConeAngle = swingHalfAngleForJolt(swingSpan1);
    settings.mPlaneHalfConeAngle = swingHalfAngleForJolt(swingSpan2);
    float twist = twistHalfAngleForJolt(twistSpan);
    settings.mTwistMinAngle = -twist;
    settings.mTwistMaxAngle = twist;

    constraint = settings.Create(*bodyA, *bodyB);

    withWriteLock([&]{
        _constraint = constraint;
    });

    // if we don't wake up bodyA, we may not send the dynamicData property over the network
    forceBodyNonStatic();
    activateBody();

    updateConeTwist();

    return constraint;
}


bool ObjectConstraintConeTwist::updateArguments(QVariantMap arguments) {
    glm::vec3 pivotInA;
    glm::vec3 axisInA;
    QUuid otherEntityID;
    glm::vec3 pivotInB;
    glm::vec3 axisInB;
    float swingSpan1;
    float swingSpan2;
    float twistSpan;

    bool needUpdate = false;
    bool somethingChanged = ObjectDynamic::updateArguments(arguments);
    withReadLock([&]{
        bool ok = true;
        pivotInA = EntityDynamicInterface::extractVec3Argument("coneTwist constraint", arguments, "pivot", ok, false);
        if (!ok) {
            pivotInA = _pivotInA;
        }

        ok = true;
        axisInA = EntityDynamicInterface::extractVec3Argument("coneTwist constraint", arguments, "axis", ok, false);
        if (!ok) {
            axisInA = _axisInA;
        }

        ok = true;
        otherEntityID = QUuid(EntityDynamicInterface::extractStringArgument("coneTwist constraint",
                                                                            arguments, "otherEntityID", ok, false));
        if (!ok) {
            otherEntityID = _otherID;
        }

        ok = true;
        pivotInB = EntityDynamicInterface::extractVec3Argument("coneTwist constraint", arguments, "otherPivot", ok, false);
        if (!ok) {
            pivotInB = _pivotInB;
        }

        ok = true;
        axisInB = EntityDynamicInterface::extractVec3Argument("coneTwist constraint", arguments, "otherAxis", ok, false);
        if (!ok) {
            axisInB = _axisInB;
        }

        ok = true;
        swingSpan1 = EntityDynamicInterface::extractFloatArgument("coneTwist constraint", arguments, "swingSpan1", ok, false);
        if (!ok) {
            swingSpan1 = _swingSpan1;
        }

        ok = true;
        swingSpan2 = EntityDynamicInterface::extractFloatArgument("coneTwist constraint", arguments, "swingSpan2", ok, false);
        if (!ok) {
            swingSpan2 = _swingSpan2;
        }

        ok = true;
        twistSpan = EntityDynamicInterface::extractFloatArgument("coneTwist constraint", arguments, "twistSpan", ok, false);
        if (!ok) {
            twistSpan = _twistSpan;
        }

        if (somethingChanged ||
            pivotInA != _pivotInA ||
            axisInA != _axisInA ||
            otherEntityID != _otherID ||
            pivotInB != _pivotInB ||
            axisInB != _axisInB ||
            swingSpan1 != _swingSpan1 ||
            swingSpan2 != _swingSpan2 ||
            twistSpan != _twistSpan) {
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
            _swingSpan1 = swingSpan1;
            _swingSpan2 = swingSpan2;
            _twistSpan = twistSpan;

            _active = true;

            auto ownerEntity = _ownerEntity.lock();
            if (ownerEntity) {
                ownerEntity->setDynamicDataDirty(true);
                ownerEntity->setDynamicDataNeedsTransmit(true);
            }
        });

        updateConeTwist();
    }

    return true;
}

/**jsdoc
 * The <code>"cone-twist"</code> {@link Entities.ActionType|ActionType} connects two entities with a joint that can move 
 * through a cone and can twist.
 * It has arguments in addition to the common {@link Entities.ActionArguments|ActionArguments}:
 *
 * @typedef {object} Entities.ActionArguments-ConeTwist
 * @property {Uuid} otherEntityID=null - The ID of the other entity that is connected to the joint.
 * @property {Vec3} pivot=0,0,0 - The local offset of the joint relative to the entity's position.
 * @property {Vec3} axis=1,0,0 - The axis of the entity that moves through the cone. Must be a non-zero vector.
 * @property {Vec3} otherPivot=0,0,0 - The local offset of the joint relative to the other entity's position.
 * @property {Vec3} otherAxis=1,0,0 - The axis of the other entity that moves through the cone. Must be a non-zero vector.
 * @property {number} swingSpan1=2*Math.PI - The angle through which the joint can move in one axis of the cone, in radians.
 *     Spans of <code>Math.PI</code> or more leave that movement free.
 * @property {number} swingSpan2=2*Math.PI - The angle through which the joint can move in the other axis of the cone, in 
 *     radians.
 * @property {number} twistSpan=2*Math.PI - The angle through with the joint can twist, in radians.
 */
QVariantMap ObjectConstraintConeTwist::getArguments() {
    QVariantMap arguments = ObjectDynamic::getArguments();
    withReadLock([&] {
        arguments["pivot"] = vec3ToQMap(_pivotInA);
        arguments["axis"] = vec3ToQMap(_axisInA);
        arguments["otherEntityID"] = _otherID;
        arguments["otherPivot"] = vec3ToQMap(_pivotInB);
        arguments["otherAxis"] = vec3ToQMap(_axisInB);
        arguments["swingSpan1"] = _swingSpan1;
        arguments["swingSpan2"] = _swingSpan2;
        arguments["twistSpan"] = _twistSpan;
    });
    return arguments;
}

QByteArray ObjectConstraintConeTwist::serialize() const {
    QByteArray serializedConstraintArguments;
    QDataStream dataStream(&serializedConstraintArguments, QIODevice::WriteOnly);
    hifi::stream::pin(dataStream);

    dataStream << DYNAMIC_TYPE_CONE_TWIST;
    dataStream << getID();
    dataStream << ObjectConstraintConeTwist::constraintVersion;

    withReadLock([&] {
        dataStream << localTimeToServerTime(_expires);
        dataStream << _tag;

        dataStream << _pivotInA;
        dataStream << _axisInA;
        dataStream << _otherID;
        dataStream << _pivotInB;
        dataStream << _axisInB;
        dataStream << _swingSpan1;
        dataStream << _swingSpan2;
        dataStream << _twistSpan;
    });

    return serializedConstraintArguments;
}

void ObjectConstraintConeTwist::deserialize(QByteArray serializedArguments) {
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
    if (serializationVersion > ObjectConstraintConeTwist::constraintVersion) {
        assert(false);
        return;
    }

    withWriteLock([&] {
        quint64 serverExpires;
        dataStream >> serverExpires;
        _expires = serverTimeToLocalTime(serverExpires);
        dataStream >> _tag;

        dataStream >> _pivotInA;
        dataStream >> _axisInA;
        dataStream >> _otherID;
        dataStream >> _pivotInB;
        dataStream >> _axisInB;
        dataStream >> _swingSpan1;
        dataStream >> _swingSpan2;
        dataStream >> _twistSpan;
        if (serializationVersion == CONE_TWIST_VERSION_WITH_UNUSED_PAREMETERS) {
            float softness, biasFactor, relaxationFactor;
            dataStream >> softness;
            dataStream >> biasFactor;
            dataStream >> relaxationFactor;
        }

        _active = true;
    });
}
