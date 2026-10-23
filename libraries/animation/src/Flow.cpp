//
//  Flow.cpp
//
//  Created by Luis Cuenca on 1/21/2019.
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "Flow.h"

#include <GLMHelpers.h>
#include <NumericalConstants.h>

#include "Rig.h"
#include "AnimSkeleton.h"

const std::map<QString, FlowPhysicsSettings> PRESET_FLOW_DATA = { { "hair", FlowPhysicsSettings() },
{ "skirt", FlowPhysicsSettings(true, 0.3f, DEFAULT_GRAVITY, 0.65f, 0.8f, 0.45f, 0.01f, 45.0f) },
{ "breast", FlowPhysicsSettings(true, 0.5f, DEFAULT_GRAVITY, 0.65f, 0.8f, 0.45f, 0.01f, 30.0f) } };

const std::map<QString, FlowCollisionSettings> PRESET_COLLISION_DATA = {
    { "Spine2", FlowCollisionSettings(QUuid(), FlowCollisionType::CollisionSphere, glm::vec3(0.0f, 0.2f, 0.0f), 0.14f) },
    { "LeftArm", FlowCollisionSettings(QUuid(), FlowCollisionType::CollisionSphere, glm::vec3(0.0f, 0.02f, 0.0f), 0.05f) },
    { "RightArm", FlowCollisionSettings(QUuid(), FlowCollisionType::CollisionSphere, glm::vec3(0.0f, 0.02f, 0.0f), 0.05f) },
    { "HeadTop_End", FlowCollisionSettings(QUuid(), FlowCollisionType::CollisionSphere, glm::vec3(0.0f, -0.15f, 0.0f), 0.09f) }
};

// A chain root that moves further than this in one frame has teleported; the chain restarts at rest.
const float FLOW_TELEPORT_DISTANCE = 1.0f;

// The rest direction curled up (toward world up) by `degrees`, for a group whose model hangs limper than wanted.
static glm::vec3 liftDirection(const glm::vec3& direction, float degrees) {
    if (degrees == 0.0f) {
        return direction;
    }
    glm::vec3 axis = glm::cross(direction, Vectors::UP);
    float axisLength = glm::length(axis);
    if (axisLength < EPSILON) {
        return direction;   // already straight up or down: no sideways to curl from
    }
    return glm::angleAxis(glm::radians(degrees), axis / axisLength) * direction;
}

static glm::vec3 closestPointOnSegment(const glm::vec3& a, const glm::vec3& b, const glm::vec3& point) {
    glm::vec3 ab = b - a;
    float length2 = glm::dot(ab, ab);
    if (length2 < EPSILON * EPSILON) {
        return a;
    }
    float t = glm::clamp(glm::dot(point - a, ab) / length2, 0.0f, 1.0f);
    return a + ab * t;
}

// Closest points between the segments [p1, q1] and [p2, q2] (Ericson, Real-Time Collision Detection, 5.1.9).
static void closestPointsBetweenSegments(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2, const glm::vec3& q2,
                                         glm::vec3& c1, glm::vec3& c2) {
    const float TINY = EPSILON * EPSILON;
    glm::vec3 d1 = q1 - p1;
    glm::vec3 d2 = q2 - p2;
    glm::vec3 r = p1 - p2;
    float a = glm::dot(d1, d1);
    float e = glm::dot(d2, d2);
    float f = glm::dot(d2, r);
    float s = 0.0f;
    float t = 0.0f;
    if (a <= TINY && e <= TINY) {
        c1 = p1;
        c2 = p2;
        return;
    }
    if (a <= TINY) {
        t = glm::clamp(f / e, 0.0f, 1.0f);
    } else {
        float c = glm::dot(d1, r);
        if (e <= TINY) {
            s = glm::clamp(-c / a, 0.0f, 1.0f);
        } else {
            float b = glm::dot(d1, d2);
            float denom = a * e - b * b;
            s = denom != 0.0f ? glm::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = glm::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = glm::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

FlowCollider::FlowCollider(int jointIndex, const FlowCollisionSettings& settings, bool isTouch) {
    _jointIndex = jointIndex;
    _endJointIndex = settings._endJointIndex;
    _type = settings._type;
    _radius = _initialRadius = settings._radius;
    _offset = _initialOffset = settings._offset;
    _endOffset = _initialEndOffset = settings._endOffset;
    _entityID = settings._entityID;
    _isTouch = isTouch;
}

bool FlowCollider::pushOut(const glm::vec3& segmentStart, glm::vec3& point, float pointRadius, bool tipOnly) const {
    glm::vec3 onBone, onShape;
    if (_type == FlowCollisionType::CollisionCapsule) {
        closestPointsBetweenSegments(segmentStart, point, _position, _endPosition, onBone, onShape);
        if (tipOnly) {
            onBone = point;
            onShape = closestPointOnSegment(_position, _endPosition, point);
        }
    } else {
        onShape = _position;
        onBone = tipOnly ? point : closestPointOnSegment(segmentStart, point, _position);
    }
    glm::vec3 away = onBone - onShape;
    float distance = glm::length(away);
    float clearance = _radius + pointRadius;
    if (distance >= clearance) {
        return false;
    }
    glm::vec3 normal;
    if (distance > EPSILON) {
        normal = away / distance;
    } else {
        // the bone runs through the shape's axis: push the point straight away from the shape
        glm::vec3 alt = point - onShape;
        float altLength = glm::length(alt);
        normal = altLength > EPSILON ? alt / altLength : Vectors::UP;
    }
    // The contact may be part way along the bone; moving the end point only moves the contact by that
    // fraction, so push the end further (capped at double) and let the length constraint tidy up.
    glm::vec3 bone = point - segmentStart;
    float boneLength2 = glm::dot(bone, bone);
    float fraction = boneLength2 > EPSILON * EPSILON ? glm::clamp(glm::dot(onBone - segmentStart, bone) / boneLength2, 0.5f, 1.0f) : 1.0f;
    point += normal * ((clearance - distance) / fraction);
    return true;
}

void FlowCollisionSystem::addCollisionSphere(int jointIndex, const FlowCollisionSettings& settings, const glm::vec3& position, bool isSelfCollision, bool isTouch) {
    auto collision = FlowCollider(jointIndex, settings, isTouch);
    collision.setPosition(position);
    if (isSelfCollision) {
        if (!isTouch) {
            _selfCollisions.push_back(collision);
        } else {
            _selfTouchCollisions.push_back(collision);
        }
    } else {
        _othersCollisions.push_back(collision);
    }
};

void FlowCollisionSystem::resetCollisions() {
    _allCollisions.clear();
    _othersCollisions.clear();
    _selfTouchCollisions.clear();
    _selfCollisions.clear();
}

bool FlowCollisionSystem::pushOut(const glm::vec3& segmentStart, glm::vec3& point, float pointRadius, bool tipOnly) const {
    bool pushed = false;
    for (const auto& collider : _allCollisions) {
        pushed = collider.pushOut(segmentStart, point, pointRadius, tipOnly) || pushed;
    }
    return pushed;
}

void FlowCollisionSystem::setScale(float scale) {
    _scale = scale;
    for (auto& collider : _selfCollisions) {
        collider._radius = collider._initialRadius * scale;
        collider._offset = collider._initialOffset * scale;
        collider._endOffset = collider._initialEndOffset * scale;
    }
    for (auto& collider : _selfTouchCollisions) {
        collider._radius = collider._initialRadius * scale;
        collider._offset = collider._initialOffset * scale;
        collider._endOffset = collider._initialEndOffset * scale;
    }
};

FlowCollisionSettings FlowCollisionSystem::getCollisionSettingsByJoint(int jointIndex) {
    for (auto &collision : _selfCollisions) {
        if (collision._jointIndex == jointIndex) {
            FlowCollisionSettings settings(collision._entityID, collision._type, collision._initialOffset, collision._initialRadius);
            settings._endJointIndex = collision._endJointIndex;
            settings._endOffset = collision._initialEndOffset;
            return settings;
        }
    }
    return FlowCollisionSettings();
}

void FlowCollisionSystem::setCollisionSettingsByJoint(int jointIndex, const FlowCollisionSettings& settings) {
    for (auto &collision : _selfCollisions) {
        if (collision._jointIndex == jointIndex) {
            collision._type = settings._type;
            collision._endJointIndex = settings._endJointIndex;
            collision._initialRadius = settings._radius;
            collision._initialOffset = settings._offset;
            collision._initialEndOffset = settings._endOffset;
            collision._radius = _scale * settings._radius;
            collision._offset = _scale * settings._offset;
            collision._endOffset = _scale * settings._endOffset;
        }
    }
}

void FlowCollisionSystem::prepareCollisions(const glm::vec3& origin) {
    _allCollisions.clear();
    _allCollisions.insert(_allCollisions.end(), _selfCollisions.begin(), _selfCollisions.end());
    for (auto& collider : _othersCollisions) {
        collider._position -= origin;
        collider._endPosition -= origin;
    }
    _allCollisions.insert(_allCollisions.end(), _othersCollisions.begin(), _othersCollisions.end());
    _allCollisions.insert(_allCollisions.end(), _selfTouchCollisions.begin(), _selfTouchCollisions.end());
    _othersCollisions.clear();
}

FlowJoint::FlowJoint(int jointIndex, int parentIndex, int childIndex, const QString& name, const QString& group, const FlowPhysicsSettings& settings) {
    _index = jointIndex;
    _name = name;
    _group = group;
    _childIndex = childIndex;
    _parentIndex = parentIndex;
    _settings = settings;
};

void FlowJoint::setInitialData(const glm::vec3& initialPosition, const glm::vec3& initialTranslation, const glm::quat& initialRotation, const glm::vec3& parentPosition) {
    _initialPosition = initialPosition;
    _previousPosition = initialPosition;
    _currentPosition = initialPosition;
    _animPosition = initialPosition;
    _initialTranslation = initialTranslation;
    _initialRotation = initialRotation;
    _animRotation = initialRotation;
    _currentRotation = initialRotation;
    _worldRotation = Quaternions::IDENTITY;
    _parentWorldRotation = Quaternions::IDENTITY;
    _length = glm::length(_initialPosition - parentPosition);
}

void FlowJoint::toHelperJoint(const glm::vec3& initialPosition, float length) {
    _initialPosition = _previousPosition = _currentPosition = _animPosition = initialPosition;
    _isHelper = true;
    _length = length;
}

FlowThread::FlowThread(int rootIndex, std::map<int, FlowJoint>* joints) {
    _jointsPointer = joints;
    computeFlowThread(rootIndex);
}

void FlowThread::computeFlowThread(int rootIndex) {
    int parentIndex = rootIndex;
    if (_jointsPointer->size() == 0) {
        return;
    }
    int childIndex = _jointsPointer->at(parentIndex)._childIndex;
    std::vector<int> indexes = { parentIndex };
    for (size_t i = 0; i < _jointsPointer->size(); i++) {
        if (childIndex > -1) {
            indexes.push_back(childIndex);
            childIndex = _jointsPointer->at(childIndex)._childIndex;
        } else {
            break;
        }
    }
    _joints = indexes;
};

void FlowThread::reset() {
    if (_joints.size() == 0) {
        return;
    }
    auto& joints = *_jointsPointer;
    FlowJoint& root = joints[_joints[0]];
    root._currentPosition = root._previousPosition = root._animPosition;
    root._colliding = false;
    glm::quat worldRotation = root._parentWorldRotation * root._animRotation;
    for (size_t i = 1; i < _joints.size(); i++) {
        FlowJoint& parent = joints[_joints[i - 1]];
        FlowJoint& node = joints[_joints[i]];
        glm::vec3 restDirection = liftDirection(glm::normalize(worldRotation * parent._restDirection), node._settings._lift);
        node._currentPosition = node._previousPosition = parent._currentPosition + restDirection * node._length;
        parent._worldRotation = rotationBetween(glm::normalize(worldRotation * parent._restDirection), restDirection) * worldRotation;
        node._colliding = false;
        parent._currentRotation = glm::inverse(worldRotation * glm::inverse(parent._animRotation)) * parent._worldRotation;
        worldRotation = parent._worldRotation * node._animRotation;
    }
    FlowJoint& tip = joints[_joints.back()];
    tip._worldRotation = worldRotation;
    tip._currentRotation = tip._animRotation;
}

void FlowThread::step(float dt, const glm::vec3& rootPosition, const glm::quat& rootParentRotation,
                      const glm::vec3& bodyAcceleration, float scale, const FlowCollisionSystem& collisionSystem) {
    if (_joints.size() < 2) {
        return;
    }
    auto& joints = *_jointsPointer;
    FlowJoint& root = joints[_joints[0]];
    root._previousPosition = root._currentPosition;
    root._currentPosition = rootPosition;
    root._colliding = false;

    const float dt2 = dt * dt;
    // world rotation of the parent of the joint whose rotation is being fitted
    glm::quat grandparentRotation = rootParentRotation;
    for (size_t i = 1; i < _joints.size(); i++) {
        FlowJoint& parent = joints[_joints[i - 1]];
        FlowJoint& node = joints[_joints[i]];
        const FlowPhysicsSettings& settings = node._settings;

        // where the bone would point if the parent kept its animated rotation under the fitted chain above it
        glm::quat parentRestRotation = grandparentRotation * parent._animRotation;
        glm::vec3 restDirection = liftDirection(glm::normalize(parentRestRotation * parent._restDirection), settings._lift);
        if (settings._lift != 0.0f) {
            // the lifted rest is the pose the rotation fit measures from
            parentRestRotation = rotationBetween(glm::normalize(parentRestRotation * parent._restDirection), restDirection) * parentRestRotation;
        }
        float length = node._length;
        glm::vec3 restPosition = parent._currentPosition + restDirection * length;
        glm::vec3 direction = restDirection;

        if (settings._active) {
            glm::vec3 velocity = node._currentPosition - node._previousPosition;
            glm::vec3 gravity(0.0f, settings.gravityAcceleration(), 0.0f);
            // in the avatar's frame its acceleration is felt as a force the other way
            glm::vec3 inertia = -bodyAcceleration * settings.inertiaBlend();
            glm::vec3 next = node._currentPosition
                + velocity * settings.velocityKept(dt)
                + (restPosition - node._currentPosition) * settings.springStep(dt)
                + (gravity + inertia) * dt2;
            node._previousPosition = node._currentPosition;

            // the bone keeps its length
            direction = next - parent._currentPosition;
            float distance = glm::length(direction);
            direction = distance > EPSILON ? direction / distance : restDirection;

            // and stays inside the cone around its rest direction
            float maxAngle = glm::radians(glm::clamp(settings._maxAngle, 0.0f, 180.0f));
            float cosAngle = glm::clamp(glm::dot(direction, restDirection), -1.0f, 1.0f);
            if (cosAngle < cosf(maxAngle)) {
                glm::vec3 axis = glm::cross(restDirection, direction);
                float axisLength = glm::length(axis);
                if (axisLength < EPSILON) {
                    // folded straight back: any perpendicular will do
                    axis = glm::cross(restDirection, fabsf(restDirection.y) < 0.9f ? Vectors::UP : Vectors::RIGHT);
                    axisLength = glm::length(axis);
                }
                direction = glm::angleAxis(maxAngle, axis / axisLength) * restDirection;
            }
            next = parent._currentPosition + direction * length;

            // and out of the body and the hands
            node._colliding = collisionSystem.getActive() && collisionSystem.pushOut(parent._currentPosition, next, settings._radius * scale, settings._tipCollision);
            if (node._colliding) {
                direction = next - parent._currentPosition;
                distance = glm::length(direction);
                direction = distance > EPSILON ? direction / distance : restDirection;
                next = parent._currentPosition + direction * length;
            }
            node._currentPosition = next;
        } else {
            node._previousPosition = node._currentPosition = restPosition;
            node._colliding = false;
        }

        // fit the parent's rotation to where its child ended up
        parent._worldRotation = rotationBetween(restDirection, direction) * parentRestRotation;
        parent._currentRotation = glm::inverse(grandparentRotation) * parent._worldRotation;
        grandparentRotation = parent._worldRotation;
    }
    FlowJoint& tip = joints[_joints.back()];
    tip._worldRotation = grandparentRotation * tip._animRotation;
    tip._currentRotation = tip._animRotation;
}

void Flow::calculateConstraints(const std::shared_ptr<AnimSkeleton>& skeleton,
                                AnimPoseVec& relativePoses, AnimPoseVec& absolutePoses) {
    cleanUp();
    if (!skeleton) {
        return;
    }
    auto flowPrefix = FLOW_JOINT_PREFIX.toUpper();
    auto simPrefix = SIM_JOINT_PREFIX.toUpper();
    std::vector<int> handsIndices;
    _groupSettings.clear();

    for (int i = 0; i < skeleton->getNumJoints(); i++) {
        auto name = skeleton->getJointName(i);
        if (std::find(HAND_COLLISION_JOINTS.begin(), HAND_COLLISION_JOINTS.end(), name) != HAND_COLLISION_JOINTS.end()) {
            handsIndices.push_back(i);
        }
        auto parentIndex = skeleton->getParentIndex(i);
        if (parentIndex == -1) {
            continue;
        }
        auto group = QStringRef(&name, 0, 3).toString().toUpper();
        auto split = name.split("_");
        bool isSimJoint = (group == simPrefix);
        bool isFlowJoint = split.size() > 2 && split[0].toUpper() == flowPrefix;
        if (isFlowJoint || isSimJoint) {
            group = "";
            if (isSimJoint) {
                for (int j = 1; j < name.size() - 1; j++) {
                    bool toFloatSuccess;
                    QStringRef(&name, (int)(name.size() - j), 1).toString().toFloat(&toFloatSuccess);
                    if (!toFloatSuccess && (name.size() - j) > (int)simPrefix.size()) {
                        group = QStringRef(&name, (int)simPrefix.size(), (int)(name.size() - j + 1) - (int)simPrefix.size()).toString();
                        break;
                    }
                }
                if (group.isEmpty()) {
                    group = QStringRef(&name, (int)simPrefix.size(), name.size() - (int)simPrefix.size()).toString();
                }
                qCDebug(animation) << "Sim joint added to flow: " << name;
            } else {
                group = split[1];
            }
            if (!group.isEmpty()) {
                _flowJointKeywords.push_back(group);
                FlowPhysicsSettings jointSettings;
                if (PRESET_FLOW_DATA.find(group) != PRESET_FLOW_DATA.end()) {
                    jointSettings = PRESET_FLOW_DATA.at(group);
                } else {
                    jointSettings = DEFAULT_JOINT_SETTINGS;
                }
                if (_flowJointData.find(i) ==  _flowJointData.end()) {
                    auto flowJoint = FlowJoint(i, parentIndex, -1, name, group, jointSettings);
                    _flowJointData.insert(std::pair<int, FlowJoint>(i, flowJoint));
                }
                updateGroupSettings(group, jointSettings);
            }
        } else {
            if (PRESET_COLLISION_DATA.find(name) != PRESET_COLLISION_DATA.end()) {
                _collisionSystem.addCollisionSphere(i, PRESET_COLLISION_DATA.at(name));
            }
        }
    }

    for (auto &jointData : _flowJointData) {
        int jointIndex = jointData.first;
        glm::vec3 jointPosition, parentPosition, jointTranslation;
        glm::quat jointRotation;
        getJointPositionInWorldFrame(absolutePoses, jointIndex, jointPosition, glm::vec3(0.0f), _entityRotation);
        if (jointIndex >= 0 && jointIndex < (int)relativePoses.size()) {
            jointTranslation = relativePoses[jointIndex].trans();
            jointRotation = relativePoses[jointIndex].rot();
        }
        getJointPositionInWorldFrame(absolutePoses, jointData.second.getParentIndex(), parentPosition, glm::vec3(0.0f), _entityRotation);

        jointData.second.setInitialData(jointPosition, jointTranslation, jointRotation, parentPosition);
    }
    // A joint on top of its parent (Seed-san's hair_F_end) has no length to keep or direction to turn by, and its
    // constraint divides by its length: it stays animated, and its children start threads of their own
    const float SHORTEST_JOINT = 0.0001f;
    for (auto itr = _flowJointData.begin(); itr != _flowJointData.end();) {
        glm::vec3 jointPosition, parentPosition;
        getJointPositionInWorldFrame(absolutePoses, itr->first, jointPosition, _entityPosition, _entityRotation);
        getJointPositionInWorldFrame(absolutePoses, itr->second.getParentIndex(), parentPosition, _entityPosition, _entityRotation);
        if (glm::length(jointPosition - parentPosition) < SHORTEST_JOINT) {
            itr = _flowJointData.erase(itr);
        } else {
            itr++;
        }
    }

    std::vector<int> roots;

    for (auto &joint :_flowJointData) {
        if (_flowJointData.find(joint.second.getParentIndex()) == _flowJointData.end()) {
            joint.second.setAnchored(true);
            roots.push_back(joint.first);
        } else {
            _flowJointData[joint.second.getParentIndex()].setChildIndex(joint.first);
        }
    }
    int extraIndex = -1;
    for (size_t i = 0; i < roots.size(); i++) {
        FlowThread thread = FlowThread(roots[i], &_flowJointData);
        // add threads with at least 2 joints
        if (thread._joints.size() > 0) {
            if (thread._joints.size() == 1) {
                // a lone joint gets an invented tip so it can still swing
                int jointIndex = roots[i];
                auto &joint = _flowJointData[jointIndex];
                glm::vec3 jointPosition = joint.getInitialPosition();
                auto newSettings = joint.getSettings();
                extraIndex = extraIndex > -1 ? extraIndex + 1 : skeleton->getNumJoints();
                joint.setChildIndex(extraIndex);
                auto newJoint = FlowJoint(extraIndex, jointIndex, -1, joint.getName(), joint.getGroup(), newSettings);
                glm::vec3 translation = glm::vec3(0.0f, HELPER_JOINT_LENGTH, 0.0f);
                newJoint.setInitialData(jointPosition + translation, translation, Quaternions::IDENTITY, jointPosition);
                newJoint.toHelperJoint(jointPosition + translation, HELPER_JOINT_LENGTH);
                _flowJointData.insert(std::pair<int, FlowJoint>(extraIndex, newJoint));
                FlowThread newThread = FlowThread(jointIndex, &_flowJointData);
                if (newThread._joints.size() > 1) {
                    _jointThreads.push_back(newThread);
                }
            } else {
                _jointThreads.push_back(thread);
            }
        }
    }

    // each joint's rest direction points at the next joint of its chain, in the joint's own frame
    for (auto& thread : _jointThreads) {
        for (size_t i = 0; i + 1 < thread._joints.size(); i++) {
            FlowJoint& joint = _flowJointData[thread._joints[i]];
            const FlowJoint& child = _flowJointData[thread._joints[i + 1]];
            glm::vec3 toChild = child._initialTranslation;
            joint._restDirection = glm::length(toChild) > EPSILON ? glm::normalize(toChild) : Vectors::UP;
        }
    }

    if (_jointThreads.size() == 0) {
        onCleanup();
    }
    if (handsIndices.size() > 0) {
        FlowCollisionSettings handSettings;
        handSettings._radius = HAND_COLLISION_RADIUS;
        for (size_t i = 0; i < handsIndices.size(); i++) {
            _collisionSystem.addCollisionSphere(handsIndices[i], handSettings, glm::vec3(), true, true);
        }
    }
    _hasBodyMotion = false;
    _initialized = _jointThreads.size() > 0;
}

void Flow::cleanUp() {
    _flowJointData.clear();
    _jointThreads.clear();
    _flowJointKeywords.clear();
    _collisionSystem.resetCollisions();
    _hasBodyMotion = false;
    _initialized = false;
    onCleanup();
 }

void Flow::setTransform(float scale, const glm::vec3& position, const glm::quat& rotation) {
    _scale = scale;
    _entityPosition = position;
    _entityRotation = rotation;
}

void Flow::setScale(float scale) {
    _scale = scale;
    _lastScale = scale;
    _collisionSystem.setScale(scale);
}

void Flow::update(float deltaTime, AnimPoseVec& relativePoses, AnimPoseVec& absolutePoses, const std::vector<bool>& overrideFlags) {
    if (!(_initialized && _active)) {
        return;
    }
    if (_scale != _lastScale) {
        setScale(_scale);
    }
    // this frame's animation: where the chain roots are and which way their parents face
    updateAnchors(relativePoses, absolutePoses);

    // the avatar's own movement, smoothed: a teleport or the first frame starts it at rest
    if (!_hasBodyMotion || deltaTime <= 0.0f || glm::distance(_entityPosition, _lastEntityPosition) > FLOW_TELEPORT_DISTANCE) {
        _bodyVelocity = glm::vec3(0.0f);
        _bodyAcceleration = glm::vec3(0.0f);
        _hasBodyMotion = true;
    } else {
        glm::vec3 velocity = (_entityPosition - _lastEntityPosition) / deltaTime;
        float alpha = 1.0f - expf(-deltaTime / FLOW_BODY_FILTER_TIME);
        glm::vec3 previousVelocity = _bodyVelocity;
        _bodyVelocity += (velocity - _bodyVelocity) * alpha;
        _bodyAcceleration = (_bodyVelocity - previousVelocity) / deltaTime;
        float magnitude = glm::length(_bodyAcceleration);
        if (magnitude > FLOW_MAX_BODY_ACCELERATION) {
            _bodyAcceleration *= FLOW_MAX_BODY_ACCELERATION / magnitude;
        }
    }
    _lastEntityPosition = _entityPosition;

    // one step of the frame's length, or several equal ones for a long frame; a very long frame is cut short
    float frameTime = glm::clamp(deltaTime, 0.0f, FLOW_MAX_STEPS_PER_FRAME * FLOW_STEP);
    int steps = glm::clamp((int)(frameTime / FLOW_STEP + 0.5f), 1, FLOW_MAX_STEPS_PER_FRAME);
    float stepTime = frameTime / (float)steps;
    if (stepTime <= 0.0f) {
        setJoints(relativePoses, overrideFlags);
        updateAbsolutePoses(relativePoses, absolutePoses);
        return;
    }

    for (auto& thread : _jointThreads) {
        if (thread._joints.size() < 2) {
            continue;
        }
        const FlowJoint& root = _flowJointData[thread._joints[0]];
        glm::vec3 rootPosition = root._animPosition;
        glm::quat rootParentRotation = root._parentWorldRotation;
        if (isNaN(rootPosition) || isNaN(rootParentRotation)) {
            continue;
        }
        bool restart = !thread._hasLastRoot || glm::distance(thread._lastRootPosition, rootPosition) > FLOW_TELEPORT_DISTANCE;
        if (!restart) {
            for (int index : thread._joints) {
                if (isNaN(_flowJointData[index]._currentPosition)) {
                    restart = true;
                    break;
                }
            }
        }
        if (restart) {
            thread.reset();
            thread._lastRootPosition = rootPosition;
            thread._lastRootParentRotation = rootParentRotation;
            thread._hasLastRoot = true;
        }
        // the root glides from where it was to where it is across the steps of this frame
        for (int s = 1; s <= steps; s++) {
            float alpha = (float)s / (float)steps;
            glm::vec3 stepPosition = glm::mix(thread._lastRootPosition, rootPosition, alpha);
            glm::quat stepRotation = safeMix(thread._lastRootParentRotation, rootParentRotation, alpha);
            thread.step(stepTime, stepPosition, stepRotation, _bodyAcceleration, _scale, _collisionSystem);
        }
        thread._lastRootPosition = rootPosition;
        thread._lastRootParentRotation = rootParentRotation;
    }

    setJoints(relativePoses, overrideFlags);
    updateAbsolutePoses(relativePoses, absolutePoses);
}

void Flow::updateAnchors(const AnimPoseVec& relativePoses, const AnimPoseVec& absolutePoses) {
    for (auto& thread : _jointThreads) {
        for (size_t i = 0; i < thread._joints.size(); i++) {
            FlowJoint& joint = _flowJointData[thread._joints[i]];
            if (!joint._isHelper) {
                getJointPositionInWorldFrame(absolutePoses, joint._index, joint._animPosition, glm::vec3(0.0f), _entityRotation);
                if (joint._index >= 0 && joint._index < (int)relativePoses.size()) {
                    joint._animRotation = relativePoses[joint._index].rot();
                }
                glm::vec3 parentPosition;
                if (getJointPositionInWorldFrame(absolutePoses, joint._parentIndex, parentPosition, glm::vec3(0.0f), _entityRotation)) {
                    float length = glm::distance(joint._animPosition, parentPosition);
                    if (length > EPSILON) {
                        joint._length = length;
                    }
                }
                if (i == 0) {
                    getJointRotationInWorldFrame(absolutePoses, joint._parentIndex, joint._parentWorldRotation, _entityRotation);
                }
            } else {
                joint._animRotation = Quaternions::IDENTITY;
                joint._length = HELPER_JOINT_LENGTH * _scale;
            }
        }
    }
    auto &selfCollisions = _collisionSystem.getSelfCollisions();
    for (auto &collision : selfCollisions) {
        updateCollisionJoint(collision, absolutePoses);
    }
    auto &selfTouchCollisions = _collisionSystem.getSelfTouchCollisions();
    for (auto &collision : selfTouchCollisions) {
        updateCollisionJoint(collision, absolutePoses);
    }
    _collisionSystem.prepareCollisions(_entityPosition);
}

void Flow::updateAbsolutePoses(const AnimPoseVec& relativePoses, AnimPoseVec& absolutePoses) {
    for (auto &joint : _flowJointData) {
        int index = joint.second.getIndex();
        int parentIndex = joint.second.getParentIndex();
        if (index >= 0 && index < (int)relativePoses.size() &&
            parentIndex >= 0 && parentIndex < (int)absolutePoses.size()) {
            absolutePoses[index] = absolutePoses[parentIndex] * relativePoses[index];
        }
    }
}

void Flow::updateCollisionJoint(FlowCollider& collision, const AnimPoseVec& absolutePoses) {
    glm::quat jointRotation;
    getJointPositionInWorldFrame(absolutePoses, collision._jointIndex, collision._position, glm::vec3(0.0f), _entityRotation);
    getJointRotationInWorldFrame(absolutePoses, collision._jointIndex, jointRotation, _entityRotation);
    collision._position = collision._position + jointRotation * collision._offset;
    if (collision._type == FlowCollisionType::CollisionCapsule && collision._endJointIndex >= 0) {
        glm::quat endRotation;
        getJointPositionInWorldFrame(absolutePoses, collision._endJointIndex, collision._endPosition, glm::vec3(0.0f), _entityRotation);
        getJointRotationInWorldFrame(absolutePoses, collision._endJointIndex, endRotation, _entityRotation);
        collision._endPosition = collision._endPosition + endRotation * collision._endOffset;
    } else {
        collision._endPosition = collision._position;
    }
}

void Flow::setJoints(AnimPoseVec& relativePoses, const std::vector<bool>& overrideFlags) {
    for (auto &thread : _jointThreads) {
        for (int jointIndex : thread._joints) {
            auto &joint = _flowJointData[jointIndex];
            if (jointIndex >= 0 && jointIndex < (int)relativePoses.size() && !overrideFlags[jointIndex]) {
                relativePoses[jointIndex].rot() = joint.getCurrentRotation();
            }
        }
    }
}

void Flow::copyJointRotations(const AnimPoseVec& fromRelativePoses, AnimPoseVec& toRelativePoses) const {
    if (!(_initialized && _active)) {
        return;
    }
    int count = (int)std::min(fromRelativePoses.size(), toRelativePoses.size());
    for (auto &thread : _jointThreads) {
        for (int jointIndex : thread._joints) {
            if (jointIndex >= 0 && jointIndex < count) {
                toRelativePoses[jointIndex].rot() = fromRelativePoses[jointIndex].rot();
            }
        }
    }
}

void Flow::setOthersCollision(const QUuid& otherId, int jointIndex, const glm::vec3& position) {
    FlowCollisionSettings settings;
    settings._entityID = otherId;
    settings._radius = HAND_COLLISION_RADIUS;
    _collisionSystem.addCollisionSphere(jointIndex, settings, position, false, true);
}

void Flow::setPhysicsSettingsForGroup(const QString& group, const FlowPhysicsSettings& settings) {
    for (auto &joint : _flowJointData) {
        if (joint.second.getGroup().toUpper() == group.toUpper()) {
            joint.second.setSettings(settings);
        }
    }
    updateGroupSettings(group, settings);
}

bool Flow::getJointPositionInWorldFrame(const AnimPoseVec& absolutePoses, int jointIndex, glm::vec3& position, glm::vec3 translation, glm::quat rotation) const {
    if (jointIndex >= 0 && jointIndex < (int)absolutePoses.size()) {
        glm::vec3 poseSetTrans = absolutePoses[jointIndex].trans();
        position = (rotation * poseSetTrans) + translation;
        if (!isNaN(position)) {
            return true;
        } else {
            position = glm::vec3(0.0f);
        }
    }
    return false;
}

bool Flow::getJointRotationInWorldFrame(const AnimPoseVec& absolutePoses, int jointIndex, glm::quat& result, const glm::quat& rotation) const {
    if (jointIndex >= 0 && jointIndex < (int)absolutePoses.size()) {
        result = rotation * absolutePoses[jointIndex].rot();
        return true;
    } else {
        return false;
    }
}

void Flow::updateGroupSettings(const QString& group, const FlowPhysicsSettings& settings) {
    if (_groupSettings.find(group) == _groupSettings.end()) {
        _groupSettings.insert(std::pair<QString, FlowPhysicsSettings>(group, settings));
    } else {
        _groupSettings[group] = settings;
    }
}
