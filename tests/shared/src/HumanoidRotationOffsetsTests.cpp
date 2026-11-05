//
//  HumanoidRotationOffsetsTests.cpp
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "HumanoidRotationOffsetsTests.h"

#include <glm/gtx/quaternion.hpp>

#include <HumanoidRotationOffsets.h>
#include <StandardSkeleton.h>

QTEST_MAIN(HumanoidRotationOffsetsTests)

namespace {

// The default avatar's skeleton in its T-pose, as its rest joints
std::vector<HumanoidRestJoint> defaultAvatar() {
    const autorig::StandardSkeleton& standard = autorig::StandardSkeleton::get();
    std::vector<HumanoidRestJoint> joints;
    for (int i = 0; i < standard.size(); i++) {
        const autorig::TemplateJoint& joint = standard.joint(i);
        joints.push_back({ joint.name, standard.parentIndex(i), joint.position, joint.rotation });
    }
    return joints;
}

bool isBelow(const std::vector<HumanoidRestJoint>& joints, int joint, int ancestor) {
    for (int i = joint; i >= 0; i = joints[i].parentIndex) {
        if (i == ancestor) {
            return true;
        }
    }
    return false;
}

// a joint and everything below it turned about the joint
void turnFrom(std::vector<HumanoidRestJoint>& joints, const QString& name, const glm::quat& turn) {
    int top = -1;
    for (int i = 0; i < (int)joints.size(); i++) {
        top = joints[i].name == name ? i : top;
    }
    const glm::vec3 pivot = joints[top].position;
    for (int i = 0; i < (int)joints.size(); i++) {
        if (isBelow(joints, i, top)) {
            joints[i].position = pivot + turn * (joints[i].position - pivot);
            joints[i].rotation = turn * joints[i].rotation;
        }
    }
}


float angleOf(const glm::quat& rotation) {
    return 2.0f * acosf(glm::min(fabsf(glm::normalize(rotation).w), 1.0f));
}

// within a few degrees: the avatar is first turned to face +Z by its hips, head and legs, which on the default avatar's
// slightly leaning spine is a degree or two from none
bool sameRotation(const glm::quat& a, const glm::quat& b) {
    return angleOf(glm::inverse(a) * b) < glm::radians(5.0f);
}

}

// The skeleton animations are made for needs no offsets
void HumanoidRotationOffsetsTests::testDefaultAvatarTPose() {
    const QMap<int, glm::quat> offsets = computeHumanoidRotationOffsets(defaultAvatar());
    QVERIFY2(offsets.isEmpty(), qPrintable(QString("%1 offsets").arg(offsets.size())));
}

// Nor does it in an A-pose, its arms 45 degrees down: the bones are straightened into the T-pose first
void HumanoidRotationOffsetsTests::testDefaultAvatarAPose() {
    std::vector<HumanoidRestJoint> joints = defaultAvatar();
    // the left arm is at +X: down is a turn about -Z there, and about +Z for the right arm
    turnFrom(joints, "LeftArm", glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 0.0f, -1.0f)));
    turnFrom(joints, "RightArm", glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
    const QMap<int, glm::quat> offsets = computeHumanoidRotationOffsets(joints);
    QStringList offset;
    for (auto itr = offsets.begin(); itr != offsets.end(); itr++) {
        offset << joints[itr.key()].name;
    }
    QVERIFY2(offsets.isEmpty(), qPrintable("offsets for " + offset.join(", ")));
}

// A neck that rests further forward than the default avatar's, as Ready Player Me's does (26 degrees), is a pose, not
// other axes: no offset (F1a)
void HumanoidRotationOffsetsTests::testPosedNeck() {
    std::vector<HumanoidRestJoint> joints = defaultAvatar();
    turnFrom(joints, "Neck", glm::angleAxis(glm::radians(30.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    const QMap<int, glm::quat> offsets = computeHumanoidRotationOffsets(joints);
    QVERIFY2(offsets.isEmpty(), qPrintable(QString("%1 offsets").arg(offsets.size())));
}

// Joints with other axes, as Blender's (each joint's axes a quarter turn from Mixamo's) get the offset that undoes them:
// every joint but the ends of chains, which carry no mesh
void HumanoidRotationOffsetsTests::testBlenderAxes() {
    const glm::quat axes = glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    std::vector<HumanoidRestJoint> joints = defaultAvatar();
    for (HumanoidRestJoint& joint : joints) {
        joint.rotation = joint.rotation * axes;
    }
    const QMap<int, glm::quat> offsets = computeHumanoidRotationOffsets(joints);
    const autorig::StandardSkeleton& standard = autorig::StandardSkeleton::get();
    for (int i = 0; i < (int)joints.size(); i++) {
        const bool isEnd = standard.boneChild(i) < 0 && !joints[i].name.endsWith("Eye");
        if (isEnd) {
            QVERIFY2(!offsets.contains(i), qPrintable(joints[i].name + " has an offset"));
        } else {
            QVERIFY2(offsets.contains(i), qPrintable(joints[i].name + " has no offset"));
            QVERIFY2(sameRotation(offsets[i], glm::inverse(axes)), qPrintable(QString("%1's offset is %2 degrees from the quarter turn back")
                     .arg(joints[i].name).arg(glm::degrees(angleOf(glm::inverse(offsets[i]) * glm::inverse(axes))))));
        }
    }
}

// A rig facing -Z with its joints' axes the model's own, as a VRM 0.x's: the hips are turned round, and the rest follow
void HumanoidRotationOffsetsTests::testFacingMinusZ() {
    const glm::quat aboutFace = glm::angleAxis(glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    std::vector<HumanoidRestJoint> joints = defaultAvatar();
    for (HumanoidRestJoint& joint : joints) {
        joint.position = aboutFace * joint.position;
        joint.rotation = glm::quat();
    }
    const QMap<int, glm::quat> offsets = computeHumanoidRotationOffsets(joints);
    const int hips = autorig::StandardSkeleton::get().indexOf("Hips");
    QVERIFY(offsets.contains(hips));
    QVERIFY2(angleOf(offsets[hips]) > glm::radians(178.0f), qPrintable(QString("the hips turn %1 degrees")
                                                                       .arg(glm::degrees(angleOf(offsets[hips])))));
    QVERIFY(fabsf(glm::axis(offsets[hips]).y) > 0.99f);
}
