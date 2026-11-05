//
//  ObjectActionTractor.cpp
//  libraries/physics/src
//
//  Created by Seth Alves 2015-5-8
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ObjectActionTractor.h"

#include <glm/gtc/quaternion.hpp>

#include <EntityItem.h>
#include <PhysicsHelpers.h>
#include <QVariantGLM.h>

#include <shared/WireStream.h>
#include "PhysicsLogging.h"

const float TRACTOR_MAX_SPEED = 10.0f;
const float MAX_TRACTOR_TIMESCALE = 600.0f; // 10 min is a long time

// Bullet let a body fall asleep below a per-body speed, which the engine set by motion type; Jolt has one world-wide
// threshold instead. The action keeps Bullet's numbers to decide when the motion it asks for has to wake the body.
const float BULLET_DEFAULT_LINEAR_SLEEPING_THRESHOLD = 0.8f; // m/s, what a static body kept
const float BULLET_DEFAULT_ANGULAR_SLEEPING_THRESHOLD = 1.0f; // rad/s

static float linearSleepingThreshold(const JPH::Body* body) {
    if (body->IsDynamic()) {
        return DYNAMIC_LINEAR_SPEED_THRESHOLD;
    }
    if (body->IsKinematic()) {
        return KINEMATIC_LINEAR_SPEED_THRESHOLD;
    }
    return BULLET_DEFAULT_LINEAR_SLEEPING_THRESHOLD;
}

static float angularSleepingThreshold(const JPH::Body* body) {
    if (body->IsDynamic()) {
        return DYNAMIC_ANGULAR_SPEED_THRESHOLD;
    }
    if (body->IsKinematic()) {
        return KINEMATIC_ANGULAR_SPEED_THRESHOLD;
    }
    return BULLET_DEFAULT_ANGULAR_SLEEPING_THRESHOLD;
}

const uint16_t ObjectActionTractor::tractorVersion = 1;


ObjectActionTractor::ObjectActionTractor(const QUuid& id, EntityItemPointer ownerEntity) :
    ObjectAction(DYNAMIC_TYPE_TRACTOR, id, ownerEntity),
    _positionalTarget(0.0f),
    _desiredPositionalTarget(0.0f),
    _linearTimeScale(FLT_MAX),
    _positionalTargetSet(false),
    _rotationalTarget(),
    _desiredRotationalTarget(),
    _angularTimeScale(FLT_MAX),
    _rotationalTargetSet(true),
    _linearVelocityTarget(0.0f)
{
#if WANT_DEBUG
    qCDebug(physics) << "ObjectActionTractor::ObjectActionTractor";
#endif
}

ObjectActionTractor::~ObjectActionTractor() {
#if WANT_DEBUG
    qCDebug(physics) << "ObjectActionTractor::~ObjectActionTractor";
#endif
}

bool ObjectActionTractor::getTarget(float deltaTimeStep, glm::quat& rotation, glm::vec3& position,
                                    glm::vec3& linearVelocity, glm::vec3& angularVelocity,
                                    float& linearTimeScale, float& angularTimeScale) {
    SpatiallyNestablePointer other = getOther();
    return resultWithReadLock<bool>([&]{
        linearTimeScale = _linearTimeScale;
        angularTimeScale = _angularTimeScale;

        if (!_otherID.isNull()) {
            bool otherIsReady { true };
            if (other && other->getNestableType() == NestableType::Entity) {
                EntityItemPointer otherEntity = std::static_pointer_cast<EntityItem>(other);
                otherIsReady = otherEntity->isReadyToComputeShape();
            }
            if (other && otherIsReady) {
                bool success;
                glm::vec3 otherWorldPosition = other->getJointWorldPosition(_otherJointIndex, success);
                if (!success) {
                    linearTimeScale = FLT_MAX;
                    angularTimeScale = FLT_MAX;
                    return false;
                }
                glm::quat otherWorldOrientation = other->getWorldOrientation(_otherJointIndex, success);
                if (!success) {
                    linearTimeScale = FLT_MAX;
                    angularTimeScale = FLT_MAX;
                    return false;
                }
                rotation = otherWorldOrientation * _desiredRotationalTarget;
                position = otherWorldOrientation * _desiredPositionalTarget + otherWorldPosition;
            } else {
                // we should have an "other" but can't find it, or its collision shape isn't loaded,
                // so disable the tractor.
                linearTimeScale = FLT_MAX;
                angularTimeScale = FLT_MAX;
                return false;
            }
        } else {
            rotation = _desiredRotationalTarget;
            position = _desiredPositionalTarget;
        }
        linearVelocity = glm::vec3();
        angularVelocity = glm::vec3();
        return true;
    });
}

bool ObjectActionTractor::prepareForTractorUpdate(float deltaTimeStep) {
    auto ownerEntity = _ownerEntity.lock();
    if (!ownerEntity) {
        return false;
    }

    bool doLinearTraction = _positionalTargetSet && (_linearTimeScale < MAX_TRACTOR_TIMESCALE);
    bool doAngularTraction = _rotationalTargetSet && (_angularTimeScale < MAX_TRACTOR_TIMESCALE);
    if (!doLinearTraction && !doAngularTraction) {
        // nothing to do
        return false;
    }

    glm::quat rotation;
    glm::vec3 position;
    glm::vec3 angularVelocity;

    int linearTractorCount = 0;
    int angularTractorCount = 0;

    QList<EntityDynamicPointer> tractorDerivedActions;
    tractorDerivedActions.append(ownerEntity->getActionsOfType(DYNAMIC_TYPE_TRACTOR));
    tractorDerivedActions.append(ownerEntity->getActionsOfType(DYNAMIC_TYPE_FAR_GRAB));
    tractorDerivedActions.append(ownerEntity->getActionsOfType(DYNAMIC_TYPE_HOLD));

    foreach (EntityDynamicPointer action, tractorDerivedActions) {
        std::shared_ptr<ObjectActionTractor> tractorAction = std::static_pointer_cast<ObjectActionTractor>(action);
        glm::quat rotationForAction;
        glm::vec3 positionForAction;
        glm::vec3 linearVelocityForAction;
        glm::vec3 angularVelocityForAction;
        float linearTimeScale;
        float angularTimeScale;
        bool success = tractorAction->getTarget(deltaTimeStep,
                                                rotationForAction, positionForAction,
                                                linearVelocityForAction, angularVelocityForAction,
                                                linearTimeScale, angularTimeScale);
        if (success) {
            if (angularTimeScale < MAX_TRACTOR_TIMESCALE) {
                angularTractorCount++;
                angularVelocity += angularVelocityForAction;
                if (tractorAction.get() == this) {
                    // only use the rotation for this action
                    rotation = rotationForAction;
                }
            }

            if (linearTimeScale < MAX_TRACTOR_TIMESCALE) {
                linearTractorCount++;
                position += positionForAction;
            }
        } else {
            return false; // we don't have both entities loaded, so don't do anything
        }
    }

    if (angularTractorCount > 0 || linearTractorCount > 0) {
        withWriteLock([&]{
            if (doLinearTraction && linearTractorCount > 0) {
                position /= linearTractorCount;
                _lastPositionTarget = _positionalTarget;
                _positionalTarget = position;
                if (deltaTimeStep > EPSILON) {
                    if (_havePositionTargetHistory) {
                        // blend the new velocity with the old (low-pass filter)
                        glm::vec3 newVelocity = (1.0f / deltaTimeStep) * (_positionalTarget - _lastPositionTarget);
                        const float blend = 0.25f;
                        _linearVelocityTarget = (1.0f - blend) * _linearVelocityTarget + blend * newVelocity;
                    } else {
                        _havePositionTargetHistory = true;
                    }
                }
                _active = true;
            }
            if (doAngularTraction && angularTractorCount > 0) {
                angularVelocity /= angularTractorCount;
                _rotationalTarget = rotation;
                _angularVelocityTarget = angularVelocity;
                _active = true;
            }
        });
    }

    return true;
}


void ObjectActionTractor::updateActionWorker(float deltaTimeStep) {
    if (!prepareForTractorUpdate(deltaTimeStep)) {
        return;
    }

    withReadLock([&]{
        auto ownerEntity = _ownerEntity.lock();
        if (!ownerEntity) {
            return;
        }

        void* physicsInfo = ownerEntity->getPhysicsInfo();
        if (!physicsInfo) {
            return;
        }
        ObjectMotionState* motionState = static_cast<ObjectMotionState*>(physicsInfo);
        JPH::Body* body = motionState->getBody();
        if (!body) {
            qCDebug(physics) << "ObjectActionTractor::updateActionWorker no body";
            return;
        }

        // the body's origin and orientation in the simulation frame (what Bullet called the center-of-mass transform)
        glm::vec3 bodyPosition;
        glm::quat bodyRotation;
        motionState->getBodyTransform(bodyPosition, bodyRotation);

        if (_linearTimeScale < MAX_TRACTOR_TIMESCALE) {
            glm::vec3 offsetVelocity(0.0f);
            glm::vec3 offset = bodyPosition - _positionalTarget;
            float offsetLength = glm::length(offset);
            if (offsetLength > FLT_EPSILON) {
                float speed = glm::min(offsetLength / _linearTimeScale, TRACTOR_MAX_SPEED);
                offsetVelocity = (-speed / offsetLength) * offset;
                if (speed > linearSleepingThreshold(body)) {
                    forceBodyNonStatic();
                    activateBody();
                }
            }
            // this action is aggresively critically damped and defeats the current velocity
            motionState->setBodyLinearVelocity(_linearVelocityTarget + offsetVelocity);
        }

        if (_angularTimeScale < MAX_TRACTOR_TIMESCALE) {
            glm::vec3 targetVelocity(0.0f);

            float alignmentDot = glm::dot(bodyRotation, _rotationalTarget);
            const float ALMOST_ONE = 0.99999f;
            if (glm::abs(alignmentDot) < ALMOST_ONE) {
                glm::quat target = _rotationalTarget;
                if (alignmentDot < 0.0f) {
                    target = -target;
                }
                // if dQ is the incremental rotation that gets an object from Q0 to Q1 then:
                //
                //      Q1 = dQ * Q0
                //
                // solving for dQ gives:
                //
                //      dQ = Q1 * Q0^
                //
                // (the conjugate is the inverse of a unit quaternion, and glm::angle/axis are Bullet's getAngle/getAxis)
                glm::quat deltaQ = target * glm::conjugate(bodyRotation);
                float speed = glm::angle(deltaQ) / _angularTimeScale;
                targetVelocity = speed * glm::axis(deltaQ);
                if (speed > angularSleepingThreshold(body)) {
                    activateBody();
                }
            }
            // this action is aggresively critically damped and defeats the current velocity
            motionState->setBodyAngularVelocity(targetVelocity);
        }
    });
}

const float MIN_TIMESCALE = 0.1f;


bool ObjectActionTractor::updateArguments(QVariantMap arguments) {
    glm::vec3 positionalTarget;
    float linearTimeScale;
    glm::quat rotationalTarget;
    float angularTimeScale;
    QUuid otherID;
    int otherJointIndex;

    bool needUpdate = false;
    bool somethingChanged = ObjectDynamic::updateArguments(arguments);
    withReadLock([&]{
        // targets are required, tractor-constants are optional
        bool ok = true;
        positionalTarget = EntityDynamicInterface::extractVec3Argument("tractor action", arguments, "targetPosition", ok, false);
        if (ok) {
            _positionalTargetSet = true;
        } else {
            positionalTarget = _desiredPositionalTarget;
        }
        ok = true;
        linearTimeScale = EntityDynamicInterface::extractFloatArgument("tractor action", arguments, "linearTimeScale", ok, false);
        if (!ok || linearTimeScale <= 0.0f) {
            linearTimeScale = _linearTimeScale;
        }

        ok = true;
        rotationalTarget = EntityDynamicInterface::extractQuatArgument("tractor action", arguments, "targetRotation", ok, false);
        if (ok) {
            _rotationalTargetSet = true;
        } else {
            rotationalTarget = _desiredRotationalTarget;
        }

        ok = true;
        angularTimeScale =
            EntityDynamicInterface::extractFloatArgument("tractor action", arguments, "angularTimeScale", ok, false);
        if (!ok) {
            angularTimeScale = _angularTimeScale;
        }

        ok = true;
        otherID = QUuid(EntityDynamicInterface::extractStringArgument("tractor action", arguments, "otherID", ok, false));
        if (!ok) {
            otherID = _otherID;
        }

        ok = true;
        otherJointIndex = EntityDynamicInterface::extractIntegerArgument("tractor action", arguments,
                                                                         "otherJointIndex", ok, false);
        if (!ok) {
            otherJointIndex = _otherJointIndex;
        }

        if (somethingChanged ||
            positionalTarget != _desiredPositionalTarget ||
            linearTimeScale != _linearTimeScale ||
            rotationalTarget != _desiredRotationalTarget ||
            angularTimeScale != _angularTimeScale ||
            otherID != _otherID ||
            otherJointIndex != _otherJointIndex) {
            // something changed
            needUpdate = true;
        }
    });

    if (needUpdate) {
        withWriteLock([&] {
            _desiredPositionalTarget = positionalTarget;
            _linearTimeScale = glm::max(MIN_TIMESCALE, glm::abs(linearTimeScale));
            _desiredRotationalTarget = rotationalTarget;
            _angularTimeScale = glm::max(MIN_TIMESCALE, glm::abs(angularTimeScale));
            _otherID = otherID;
            _otherJointIndex = otherJointIndex;
            _active = true;

            auto ownerEntity = _ownerEntity.lock();
            if (ownerEntity) {
                ownerEntity->setDynamicDataDirty(true);
                ownerEntity->setDynamicDataNeedsTransmit(true);
            }
        });
        activateBody();
    }

    return true;
}

/**jsdoc
 * The <code>"tractor"</code> {@link Entities.ActionType|ActionType} moves and rotates an entity to a target position and
 * orientation, optionally relative to another entity.
 * It has arguments in addition to the common {@link Entities.ActionArguments|ActionArguments}:
 *
 * @typedef {object} Entities.ActionArguments-Tractor
 * @property {Uuid} otherID=null - If an entity ID, the <code>targetPosition</code> and <code>targetRotation</code> are
 *     relative to the entity's position and rotation.
 * @property {Uuid} otherJointIndex=null - If a joint index in the <code>otherID</code> entity, the <code>targetPosition</code> 
 *     and <code>targetRotation</code> are relative to the entity joint's position and rotation.
 * @property {Vec3} targetPosition=0,0,0 - The target position.
 * @property {Quat} targetRotation=0,0,0,1 - The target rotation.
 * @property {number} linearTimeScale=3.4e+38 - Controls how long it takes for the entity's position to catch up with the
 *     target position. The value is the time for the action to catch up to 1/e = 0.368 of the target value, where the action
 *     is applied using an exponential decay.
 * @property {number} angularTimeScale=3.4e+38 - Controls how long it takes for the entity's orientation to catch up with the
 *     target orientation. The value is the time for the action to catch up to 1/e = 0.368 of the target value, where the
 *     action is applied using an exponential decay.
 */
QVariantMap ObjectActionTractor::getArguments() {
    QVariantMap arguments = ObjectDynamic::getArguments();
    withReadLock([&] {
        arguments["linearTimeScale"] = _linearTimeScale;
        arguments["targetPosition"] = vec3ToQMap(_desiredPositionalTarget);

        arguments["targetRotation"] = quatToQMap(_desiredRotationalTarget);
        arguments["angularTimeScale"] = _angularTimeScale;

        arguments["otherID"] = _otherID;
        arguments["otherJointIndex"] = _otherJointIndex;
    });
    return arguments;
}

void ObjectActionTractor::serializeParameters(QDataStream& dataStream) const {
    withReadLock([&] {
        dataStream << _desiredPositionalTarget;
        dataStream << _linearTimeScale;
        dataStream << _positionalTargetSet;
        dataStream << _desiredRotationalTarget;
        dataStream << _angularTimeScale;
        dataStream << _rotationalTargetSet;
        dataStream << localTimeToServerTime(_expires);
        dataStream << _tag;
        dataStream << _otherID;
        dataStream << _otherJointIndex;
    });
}

QByteArray ObjectActionTractor::serialize() const {
    QByteArray serializedActionArguments;
    QDataStream dataStream(&serializedActionArguments, QIODevice::WriteOnly);
    hifi::stream::pin(dataStream);

    dataStream << DYNAMIC_TYPE_TRACTOR;
    dataStream << getID();
    dataStream << ObjectActionTractor::tractorVersion;

    serializeParameters(dataStream);

    return serializedActionArguments;
}

void ObjectActionTractor::deserializeParameters(QByteArray serializedArguments, QDataStream& dataStream) {
    withWriteLock([&] {
        dataStream >> _desiredPositionalTarget;
        dataStream >> _linearTimeScale;
        dataStream >> _positionalTargetSet;

        dataStream >> _desiredRotationalTarget;
        dataStream >> _angularTimeScale;
        dataStream >> _rotationalTargetSet;

        quint64 serverExpires;
        dataStream >> serverExpires;
        _expires = serverTimeToLocalTime(serverExpires);

        dataStream >> _tag;

        dataStream >> _otherID;
        dataStream >> _otherJointIndex;

        _active = true;
    });
}

void ObjectActionTractor::deserialize(QByteArray serializedArguments) {
    QDataStream dataStream(serializedArguments);
    hifi::stream::pin(dataStream);

    EntityDynamicType type;
    dataStream >> type;
    assert(type == getType() || type == DYNAMIC_TYPE_SPRING);

    QUuid id;
    dataStream >> id;
    assert(id == getID());

    uint16_t serializationVersion;
    dataStream >> serializationVersion;
    if (serializationVersion != ObjectActionTractor::tractorVersion) {
        assert(false);
        return;
    }

    deserializeParameters(serializedArguments, dataStream);
}
