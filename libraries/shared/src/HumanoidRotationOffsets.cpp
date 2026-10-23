//
//  HumanoidRotationOffsets.cpp
//  libraries/shared/src
//
//  Created on 2026-10-19 for Darkride's avatar support.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "HumanoidRotationOffsets.h"

#include <QHash>

#include <glm/gtx/quaternion.hpp>

#include "StandardSkeleton.h"

namespace {

const float EPSILON = 1.0e-6f;
// More than posing differences, such as a slightly different spine or fingers, but less than a change of axes, which is
// a quarter turn or more
const float SMALLEST_OFFSET = glm::radians(25.0f);

// The rotation that turns direction `from` onto `to`
glm::quat turnOnto(const glm::vec3& from, const glm::vec3& to) {
    return glm::rotation(glm::normalize(from), glm::normalize(to));
}

// The rotation about `axis` that turns `from` onto `to` as seen along the axis; none if either lies along it
glm::quat twistOnto(const glm::vec3& axis, glm::vec3 from, glm::vec3 to) {
    const glm::vec3 unitAxis = glm::normalize(axis);
    from -= glm::dot(from, unitAxis) * unitAxis;
    to -= glm::dot(to, unitAxis) * unitAxis;
    if (glm::length(from) < EPSILON || glm::length(to) < EPSILON) {
        return glm::quat();
    }
    from = glm::normalize(from);
    to = glm::normalize(to);
    float angle = atan2f(glm::dot(glm::cross(from, to), unitAxis), glm::dot(from, to));
    return glm::angleAxis(angle, unitAxis);
}

}

QMap<int, glm::quat> computeHumanoidRotationOffsets(const std::vector<HumanoidRestJoint>& joints) {
    const autorig::StandardSkeleton& standard = autorig::StandardSkeleton::get();
    const int count = (int)joints.size();

    QHash<QString, int> jointsByName;
    std::vector<int> standardIndices(count, -1);
    for (int i = 0; i < count; i++) {
        if (!jointsByName.contains(joints[i].name)) {
            jointsByName.insert(joints[i].name, i);
        }
        standardIndices[i] = standard.indexOf(joints[i].name);
    }
    auto find = [&](const char* name) { return jointsByName.value(name, -1); };

    // Turn the avatar to face +Z (its forward) with +Y up and +X its left, by where its hips, head and legs are
    glm::quat facing;
    const int hips = find("Hips");
    int top = find("Head");
    if (top < 0) {
        top = find("Neck");
    }
    const int leftLeg = find("LeftUpLeg");
    const int rightLeg = find("RightUpLeg");
    if (hips >= 0 && top >= 0 && leftLeg >= 0 && rightLeg >= 0) {
        glm::vec3 up = joints[top].position - joints[hips].position;
        glm::vec3 left = joints[leftLeg].position - joints[rightLeg].position;
        if (glm::length(up) > EPSILON) {
            up = glm::normalize(up);
            left -= glm::dot(left, up) * up;
            if (glm::length(left) > EPSILON) {
                left = glm::normalize(left);
                glm::mat3 avatarAxes(left, up, glm::cross(left, up));
                facing = glm::quat_cast(glm::transpose(avatarAxes));
            }
        }
    }

    // Straighten the bones into the T-pose from the hips out: a joint turns with its parent, and then turns its own bone
    // onto the template's, which turns everything below it too
    std::vector<glm::quat> straightenings(count);
    QMap<int, glm::quat> offsets;
    for (int i = 0; i < count; i++) {
        const int parent = joints[i].parentIndex;
        const glm::quat parentStraightening = (parent >= 0 && parent < i) ? straightenings[parent] : glm::quat();
        glm::quat straightening = parentStraightening;
        const int standardIndex = standardIndices[i];
        // Only the limbs are straightened, as an A-pose differs in them. A spine, neck, head or foot is left as it is:
        // they differ between rigs in proportion more than in pose, so turning them onto the template's tips the avatar
        // (Quaternius's feet are flatter than Mixamo's, and would stand on tiptoe)
        static const QStringList LIMBS { "Arm", "ForeArm", "Hand", "UpLeg", "Leg" };
        const QString& jointName = joints[i].name;
        const bool isLimb = standardIndex >= 0 &&
            (jointName.contains("HandThumb") || jointName.contains("HandIndex") || jointName.contains("HandMiddle") ||
             jointName.contains("HandRing") || jointName.contains("HandPinky") ||
             LIMBS.contains(jointName.mid(jointName.startsWith("Left") ? 4 : 5)));
        if (standardIndex >= 0 && isLimb) {
            // the bone points at the next joint along it that the avatar has
            int target = -1;
            glm::vec3 templateDirection;
            for (int child = standard.boneChild(standardIndex); child >= 0; child = standard.boneChild(child)) {
                target = find(standard.joint(child).name);
                if (target >= 0) {
                    templateDirection = standard.joint(child).position - standard.joint(standardIndex).position;
                    break;
                }
            }
            if (target >= 0) {
                glm::vec3 direction = parentStraightening * facing * (joints[target].position - joints[i].position);
                if (glm::length(direction) > EPSILON && glm::length(templateDirection) > EPSILON) {
                    straightening = turnOnto(direction, templateDirection) * parentStraightening;

                    // a hand also turns about its bone so its palm faces down, by where its index and little fingers are
                    const QString& name = joints[i].name;
                    if (name == "LeftHand" || name == "RightHand") {
                        const int index = jointsByName.value(name + "Index1", -1);
                        const int pinky = jointsByName.value(name + "Pinky1", -1);
                        const int standardIndexFinger = standard.indexOf(name + "Index1");
                        const int standardPinky = standard.indexOf(name + "Pinky1");
                        if (index >= 0 && pinky >= 0 && standardIndexFinger >= 0 && standardPinky >= 0) {
                            glm::vec3 across = straightening * facing * (joints[index].position - joints[pinky].position);
                            glm::vec3 templateAcross = standard.joint(standardIndexFinger).position - standard.joint(standardPinky).position;
                            straightening = twistOnto(templateDirection, across, templateAcross) * straightening;
                        }
                    }
                }
            }
        }
        if (standardIndex >= 0) {
            // the ends of chains (HeadTop_End, LeftToe_End, LeftHandIndex4) carry no mesh and their axes don't matter
            const bool isEnd = standard.boneChild(standardIndex) < 0 && !joints[i].name.endsWith("Eye");
            glm::quat tPoseRotation = straightening * facing * joints[i].rotation;
            glm::quat offset = glm::normalize(glm::inverse(tPoseRotation) * standard.joint(standardIndex).rotation);
            if (!isEnd && 2.0f * acosf(glm::min(fabsf(offset.w), 1.0f)) > SMALLEST_OFFSET) {
                offsets.insert(i, offset);
            }
        }
        straightenings[i] = straightening;
    }
    return offsets;
}
