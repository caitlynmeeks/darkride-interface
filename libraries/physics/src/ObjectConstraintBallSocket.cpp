//
//  ObjectConstraintBallSocket.cpp
//  libraries/physics/src
//
//  Created by Seth Alves 2017-4-29
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ObjectConstraintBallSocket.h"

#include <Jolt/Physics/Constraints/PointConstraint.h>

#include <LogHandler.h>

#include "QVariantGLM.h"

#include "EntityTree.h"
#include <shared/WireStream.h>
#include "PhysicsLogging.h"


const uint16_t ObjectConstraintBallSocket::constraintVersion = 1;

// A point given relative to the body's origin, as scripts give it, expressed relative to the body's centre of
// mass, which is what Jolt's LocalToBodyCOM constraint space wants.
static JPH::RVec3 toBodyCOMSpace(const JPH::Body* body, const glm::vec3& point) {
    return glmToJoltR(point - joltToGLM(body->GetShape()->GetCenterOfMass()));
}

ObjectConstraintBallSocket::ObjectConstraintBallSocket(const QUuid& id, EntityItemPointer ownerEntity) :
    ObjectConstraint(DYNAMIC_TYPE_BALL_SOCKET, id, ownerEntity),
    _pivotInA(glm::vec3(0.0f)),
    _pivotInB(glm::vec3(0.0f))
{
    #if WANT_DEBUG
    qCDebug(physics) << "ObjectConstraintBallSocket::ObjectConstraintBallSocket";
    #endif
}

ObjectConstraintBallSocket::~ObjectConstraintBallSocket() {
    #if WANT_DEBUG
    qCDebug(physics) << "ObjectConstraintBallSocket::~ObjectConstraintBallSocket";
    #endif
}

std::vector<JPH::BodyID> ObjectConstraintBallSocket::getBodyIDs() {
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

void ObjectConstraintBallSocket::prepareForPhysicsSimulation() {
}

void ObjectConstraintBallSocket::updateBallSocket() {
    JPH::PointConstraint* constraint { nullptr };
    QUuid otherEntityID;
    glm::vec3 pivotInA;
    glm::vec3 pivotInB;

    withReadLock([&]{
        constraint = static_cast<JPH::PointConstraint*>(_constraint.GetPtr());
        otherEntityID = _otherID;
        pivotInA = _pivotInA;
        pivotInB = _pivotInB;
    });

    if (!constraint) {
        return;
    }

    constraint->SetPoint1(JPH::EConstraintSpace::LocalToBodyCOM, toBodyCOMSpace(constraint->GetBody1(), pivotInA));
    if (!otherEntityID.isNull()) {
        constraint->SetPoint2(JPH::EConstraintSpace::LocalToBodyCOM, toBodyCOMSpace(constraint->GetBody2(), pivotInB));
    }
    // against the world, the second point stays where the entity's pivot was when the joint was made
}


JPH::Constraint* ObjectConstraintBallSocket::getConstraint() {
    JPH::Constraint* constraint { nullptr };
    QUuid otherEntityID;
    glm::vec3 pivotInA;
    glm::vec3 pivotInB;

    withReadLock([&]{
        constraint = _constraint.GetPtr();
        pivotInA = _pivotInA;
        otherEntityID = _otherID;
        pivotInB = _pivotInB;
    });
    if (constraint) {
        return constraint;
    }

    static int repeatMessageID = LogHandler::getInstance().newRepeatedMessageID();

    JPH::Body* bodyA = getBody();
    if (!bodyA) {
        HIFI_FCDEBUG_ID(physics(), repeatMessageID, "ObjectConstraintBallSocket::getConstraint -- no bodyA");
        return nullptr;
    }

    JPH::PointConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
    settings.mPoint1 = toBodyCOMSpace(bodyA, pivotInA);

    JPH::Body* bodyB { nullptr };
    if (!otherEntityID.isNull()) {
        // This constraint is between two entities... find the other body.
        bodyB = getOtherBody(otherEntityID);
        if (!bodyB) {
            HIFI_FCDEBUG_ID(physics(), repeatMessageID, "ObjectConstraintBallSocket::getConstraint -- no bodyB");
            return nullptr;
        }

        settings.mPoint2 = toBodyCOMSpace(bodyB, pivotInB);
    } else {
        // This constraint is between an entity and the world-frame. The other body is Jolt's fixed-to-world body,
        // whose own frame is the world frame, and the entity's pivot is held where it is now; otherPivot has no
        // part in this.
        bodyB = &JPH::Body::sFixedToWorld;
        glm::vec3 positionA = joltToGLM(bodyA->GetPosition());
        glm::quat rotationA = joltToGLM(bodyA->GetRotation());
        settings.mPoint2 = glmToJoltR(positionA + rotationA * pivotInA);
    }

    constraint = settings.Create(*bodyA, *bodyB);

    withWriteLock([&]{
        _constraint = constraint;
    });

    // if we don't wake up bodyA, we may not send the dynamicData property over the network
    forceBodyNonStatic();
    activateBody();

    updateBallSocket();

    return constraint;
}


bool ObjectConstraintBallSocket::updateArguments(QVariantMap arguments) {
    glm::vec3 pivotInA;
    QUuid otherEntityID;
    glm::vec3 pivotInB;

    bool needUpdate = false;
    bool somethingChanged = ObjectDynamic::updateArguments(arguments);
    withReadLock([&]{
        bool ok = true;
        pivotInA = EntityDynamicInterface::extractVec3Argument("ball-socket constraint", arguments, "pivot", ok, false);
        if (!ok) {
            pivotInA = _pivotInA;
        }

        ok = true;
        otherEntityID = QUuid(EntityDynamicInterface::extractStringArgument("ball-socket constraint",
                                                                            arguments, "otherEntityID", ok, false));
        if (!ok) {
            otherEntityID = _otherID;
        }

        ok = true;
        pivotInB = EntityDynamicInterface::extractVec3Argument("ball-socket constraint", arguments, "otherPivot", ok, false);
        if (!ok) {
            pivotInB = _pivotInB;
        }

        if (somethingChanged ||
            pivotInA != _pivotInA ||
            otherEntityID != _otherID ||
            pivotInB != _pivotInB) {
            // something changed
            needUpdate = true;
        }
    });

    if (needUpdate) {
        withWriteLock([&] {
            _pivotInA = pivotInA;
            _otherID = otherEntityID;
            _pivotInB = pivotInB;

            _active = true;

            auto ownerEntity = _ownerEntity.lock();
            if (ownerEntity) {
                ownerEntity->setDynamicDataDirty(true);
                ownerEntity->setDynamicDataNeedsTransmit(true);
            }
        });

        updateBallSocket();
    }

    return true;
}

/**jsdoc
 * The <code>"ball-socket"</code> {@link Entities.ActionType|ActionType} connects two entities with a ball and socket joint. 
 * It has arguments in addition to the common {@link Entities.ActionArguments|ActionArguments}:
 *
 * @typedef {object} Entities.ActionArguments-BallSocket
 * @property {Uuid} otherEntityID=null - The ID of the other entity that is connected to the joint. If none is specified
 *     then the entity's <code>pivot</code> is held where it is when the action is created.
 * @property {Vec3} pivot=0,0,0 - The local offset of the joint relative to the entity's position.
 * @property {Vec3} otherPivot=0,0,0 - The local offset of the joint relative to the other entity's position. Not used
 *     when there is no other entity.
 */
QVariantMap ObjectConstraintBallSocket::getArguments() {
    QVariantMap arguments = ObjectDynamic::getArguments();
    withReadLock([&] {
        arguments["pivot"] = vec3ToQMap(_pivotInA);
        arguments["otherEntityID"] = _otherID;
        arguments["otherPivot"] = vec3ToQMap(_pivotInB);
    });
    return arguments;
}

QByteArray ObjectConstraintBallSocket::serialize() const {
    QByteArray serializedConstraintArguments;
    QDataStream dataStream(&serializedConstraintArguments, QIODevice::WriteOnly);
    hifi::stream::pin(dataStream);

    dataStream << DYNAMIC_TYPE_BALL_SOCKET;
    dataStream << getID();
    dataStream << ObjectConstraintBallSocket::constraintVersion;

    withReadLock([&] {
        dataStream << localTimeToServerTime(_expires);
        dataStream << _tag;

        dataStream << _pivotInA;
        dataStream << _otherID;
        dataStream << _pivotInB;
    });

    return serializedConstraintArguments;
}

void ObjectConstraintBallSocket::deserialize(QByteArray serializedArguments) {
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
    if (serializationVersion != ObjectConstraintBallSocket::constraintVersion) {
        assert(false);
        return;
    }

    withWriteLock([&] {
        quint64 serverExpires;
        dataStream >> serverExpires;
        _expires = serverTimeToLocalTime(serverExpires);
        dataStream >> _tag;

        dataStream >> _pivotInA;
        dataStream >> _otherID;
        dataStream >> _pivotInB;

        _active = true;
    });
}
