//
//  HumanoidJointMapTests.cpp
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "HumanoidJointMapTests.h"

#include <functional>

#include <HumanoidJointMap.h>
#include <StandardSkeleton.h>

QTEST_MAIN(HumanoidJointMapTests)

namespace {

// A joint list as a model file gives it: names, parents and positions only. Each joint has a key of its own, as rigs can
// use one name on both sides (Synty's fingers); the template's joints are keyed by Darkride's names.
class Rig {
public:
    // The default avatar's skeleton (Mixamo's T-pose, facing +Z, its left at +X), each joint renamed; an empty name
    // leaves the joint out and hangs its children from its nearest ancestor that's kept. Facing -Z turns it round.
    static Rig fromTemplate(const std::function<QString(const QString&)>& rename, bool facingMinusZ = false) {
        const autorig::StandardSkeleton& standard = autorig::StandardSkeleton::get();
        Rig rig;
        QHash<QString, QString> keptAncestors;
        for (int i = 0; i < standard.size(); i++) {
            const QString key = standard.joint(i).name;
            const QString parent = standard.parentIndex(i) >= 0 ? keptAncestors.value(standard.joint(standard.parentIndex(i)).name) : QString();
            const QString name = rename(key);
            glm::vec3 position = standard.joint(i).position;
            if (facingMinusZ) {
                position = glm::vec3(-position.x, position.y, -position.z);
            }
            if (name.isEmpty()) {
                keptAncestors.insert(key, parent);
                continue;
            }
            keptAncestors.insert(key, key);
            rig.add(key, name, parent, position);
        }
        return rig;
    }

    void add(const QString& key, const QString& name, const QString& parentKey, const glm::vec3& position, bool skeleton = true) {
        _joints.push_back({ key, name, parentKey, position, skeleton });
    }
    // a joint added halfway between two, which then hangs from it
    void insertBetween(const QString& key, const QString& name, const QString& parentKey, const QString& childKey) {
        add(key, name, parentKey, 0.5f * (position(parentKey) + position(childKey)));
        setParent(childKey, key);
    }
    void setParent(const QString& key, const QString& parentKey) {
        for (Joint& joint : _joints) {
            if (joint.key == key) {
                joint.parentKey = parentKey;
            }
        }
    }
    glm::vec3 position(const QString& key) const {
        for (const Joint& joint : _joints) {
            if (joint.key == key) {
                return joint.position;
            }
        }
        return glm::vec3();
    }

    // Darkride's name: the key of the joint mapHumanoidJoints gives it to
    QHash<QString, QString> map() const {
        std::vector<HumanoidJointInfo> joints;
        for (const Joint& joint : _joints) {
            int parent = -1;
            for (int i = 0; i < (int)_joints.size(); i++) {
                if (_joints[i].key == joint.parentKey) {
                    parent = i;
                }
            }
            joints.push_back({ joint.name, parent, joint.skeleton, joint.position });
        }
        QHash<QString, QString> result;
        const QHash<int, QString> mapped = mapHumanoidJoints(joints);
        for (auto itr = mapped.begin(); itr != mapped.end(); itr++) {
            result.insert(itr.value(), _joints[itr.key()].key);
        }
        return result;
    }

private:
    struct Joint {
        QString key;
        QString name;
        QString parentKey;
        glm::vec3 position;
        bool skeleton;
    };
    std::vector<Joint> _joints;
};

// "Left"/"Right" and the rest of a Darkride name, for renaming by side
bool splitSide(const QString& name, QString& side, QString& rest) {
    for (const char* prefix : { "Left", "Right" }) {
        if (name.startsWith(prefix)) {
            side = prefix;
            rest = name.mid(side.size());
            return true;
        }
    }
    return false;
}

// the finger and its number in a Darkride name (LeftHandIndex2: Index, 2)
bool splitFinger(const QString& rest, QString& finger, int& number) {
    static const QRegularExpression FINGER("^Hand(Thumb|Index|Middle|Ring|Pinky)([1-4])$");
    QRegularExpressionMatch match = FINGER.match(rest);
    if (!match.hasMatch()) {
        return false;
    }
    finger = match.captured(1);
    number = match.captured(2).toInt();
    return true;
}

// Each expected Darkride name maps to the joint of that key, and nothing else is mapped but what `others` allows
void verifyMapping(const QHash<QString, QString>& mapped, const QHash<QString, QString>& expected, int expectedCount = -1) {
    for (auto itr = expected.begin(); itr != expected.end(); itr++) {
        QVERIFY2(mapped.value(itr.key()) == itr.value(),
                 qPrintable(QString("%1 is %2, not %3").arg(itr.key(), mapped.value(itr.key(), "nothing"), itr.value())));
    }
    if (expectedCount >= 0) {
        QVERIFY2(mapped.size() == expectedCount, qPrintable(QString("%1 joints mapped, not %2").arg(mapped.size()).arg(expectedCount)));
    }
}

const QHash<QString, QString> MAIN_JOINTS_BY_KEY {
    { "Hips", "Hips" }, { "Spine", "Spine" }, { "Spine2", "Spine2" }, { "Neck", "Neck" }, { "Head", "Head" },
    { "LeftUpLeg", "LeftUpLeg" }, { "LeftLeg", "LeftLeg" }, { "LeftFoot", "LeftFoot" }, { "LeftToeBase", "LeftToeBase" },
    { "RightUpLeg", "RightUpLeg" }, { "RightLeg", "RightLeg" }, { "RightFoot", "RightFoot" },
    { "LeftShoulder", "LeftShoulder" }, { "LeftArm", "LeftArm" }, { "LeftForeArm", "LeftForeArm" }, { "LeftHand", "LeftHand" },
    { "RightShoulder", "RightShoulder" }, { "RightArm", "RightArm" }, { "RightForeArm", "RightForeArm" }, { "RightHand", "RightHand" },
    { "LeftHandThumb1", "LeftHandThumb1" }, { "LeftHandIndex1", "LeftHandIndex1" }, { "RightHandPinky2", "RightHandPinky2" },
};

}

// Mixamo's own names: nothing to do
void HumanoidJointMapTests::testMixamo() {
    const Rig rig = Rig::fromTemplate([](const QString& name) { return name; });
    QVERIFY(rig.map().isEmpty());
}

// Mixamo's names behind a namespace (mixamorig:Hips) or Blender's version of one (mixamorig1_Hips): every joint
void HumanoidJointMapTests::testMixamoPrefixed() {
    for (const QString& prefix : { QString("mixamorig:"), QString("mixamorig1_") }) {
        const Rig rig = Rig::fromTemplate([&](const QString& name) { return prefix + name; });
        const QHash<QString, QString> mapped = rig.map();
        verifyMapping(mapped, MAIN_JOINTS_BY_KEY, autorig::StandardSkeleton::get().size());
    }
}

// Unreal's mannequin: pelvis, spine_01 to _03, clavicle_l, upperarm_l, thigh_l, calf_l, ball_l, twist bones beside the
// limbs, and IK joints, which are left out
void HumanoidJointMapTests::testUnrealMannequin() {
    Rig rig = Rig::fromTemplate([](const QString& name) -> QString {
        static const QHash<QString, QString> BODY {
            { "Hips", "pelvis" }, { "Spine", "spine_01" }, { "Spine1", "spine_02" }, { "Spine2", "spine_03" },
            { "Neck", "neck_01" }, { "Head", "head" },
        };
        static const QHash<QString, QString> LIMBS {
            { "Shoulder", "clavicle" }, { "Arm", "upperarm" }, { "ForeArm", "lowerarm" }, { "Hand", "hand" },
            { "UpLeg", "thigh" }, { "Leg", "calf" }, { "Foot", "foot" }, { "ToeBase", "ball" },
        };
        if (BODY.contains(name)) {
            return BODY.value(name);
        }
        QString side, rest, finger;
        int number;
        if (!splitSide(name, side, rest)) {
            return QString();
        }
        const QString suffix = side == "Left" ? "_l" : "_r";
        if (LIMBS.contains(rest)) {
            return LIMBS.value(rest) + suffix;
        }
        if (splitFinger(rest, finger, number) && number <= 3) {
            return finger.toLower() + "_0" + QString::number(number) + suffix;
        }
        return QString();
    });
    rig.add("root", "root", QString(), glm::vec3());
    rig.setParent("Hips", "root");
    for (const QString& side : { QString("l"), QString("r") }) {
        const QString Side = side == "l" ? "Left" : "Right";
        rig.add("upperarm_twist_" + side, "upperarm_twist_01_" + side, Side + "Arm", rig.position(Side + "Arm"));
        rig.add("lowerarm_twist_" + side, "lowerarm_twist_01_" + side, Side + "ForeArm", rig.position(Side + "Hand"));
        rig.add("thigh_twist_" + side, "thigh_twist_01_" + side, Side + "UpLeg", rig.position(Side + "UpLeg"));
        rig.add("ik_foot_" + side, "ik_foot_" + side, "root", rig.position(Side + "Foot"));
        rig.add("ik_hand_" + side, "ik_hand_" + side, "root", rig.position(Side + "Hand"));
    }
    const QHash<QString, QString> mapped = rig.map();
    verifyMapping(mapped, MAIN_JOINTS_BY_KEY);
    QCOMPARE(mapped.value("Spine1"), QString("Spine1"));
    for (const QString& key : mapped) {
        QVERIFY2(!key.contains("twist") && !key.startsWith("ik_") && key != "root", qPrintable(key + " mapped"));
    }
}

// Unity humanoid names: Darkride's where they're the same (Hips, LeftHand), so those aren't in the result
void HumanoidJointMapTests::testUnityHumanoid() {
    const Rig rig = Rig::fromTemplate([](const QString& name) -> QString {
        static const QHash<QString, QString> NAMES {
            { "Spine1", "Chest" }, { "Spine2", "UpperChest" }, { "HeadTop_End", "" },
            { "LeftArm", "LeftUpperArm" }, { "LeftForeArm", "LeftLowerArm" }, { "LeftUpLeg", "LeftUpperLeg" },
            { "LeftLeg", "LeftLowerLeg" }, { "LeftToeBase", "LeftToes" }, { "LeftToe_End", "" },
            { "RightArm", "RightUpperArm" }, { "RightForeArm", "RightLowerArm" }, { "RightUpLeg", "RightUpperLeg" },
            { "RightLeg", "RightLowerLeg" }, { "RightToeBase", "RightToes" }, { "RightToe_End", "" },
        };
        if (NAMES.contains(name)) {
            return NAMES.value(name);
        }
        QString side, rest, finger;
        int number;
        if (splitSide(name, side, rest) && splitFinger(rest, finger, number)) {
            static const char* PHALANGES[] = { "Proximal", "Intermediate", "Distal" };
            return number <= 3 ? side + (finger == "Pinky" ? "Little" : finger) + PHALANGES[number - 1] : QString();
        }
        return name;
    });
    const QHash<QString, QString> mapped = rig.map();
    verifyMapping(mapped, {
        { "Spine1", "Spine1" }, { "Spine2", "Spine2" }, { "LeftArm", "LeftArm" }, { "LeftForeArm", "LeftForeArm" },
        { "LeftUpLeg", "LeftUpLeg" }, { "LeftLeg", "LeftLeg" }, { "LeftToeBase", "LeftToeBase" },
        { "RightArm", "RightArm" }, { "RightLeg", "RightLeg" }, { "LeftHandThumb1", "LeftHandThumb1" },
        { "LeftHandIndex3", "LeftHandIndex3" }, { "RightHandPinky1", "RightHandPinky1" },
    });
    for (const QString& present : { "Hips", "Spine", "Neck", "Head", "LeftHand", "LeftFoot", "LeftShoulder", "LeftEye" }) {
        QVERIFY2(!mapped.contains(present), qPrintable(QString(present) + " mapped though it has the name"));
    }
}

// 3ds Max's Biped: Bip01 Pelvis, the thighs hanging from the spine, the clavicles from the neck, fingers by number
void HumanoidJointMapTests::testBiped() {
    Rig rig = Rig::fromTemplate([](const QString& name) -> QString {
        static const QHash<QString, QString> BODY {
            { "Hips", "Bip01 Pelvis" }, { "Spine", "Bip01 Spine" }, { "Spine1", "Bip01 Spine1" }, { "Spine2", "Bip01 Spine2" },
            { "Neck", "Bip01 Neck" }, { "Head", "Bip01 Head" }, { "HeadTop_End", "Bip01 HeadNub" },
        };
        static const QHash<QString, QString> LIMBS {
            { "Shoulder", "Clavicle" }, { "Arm", "UpperArm" }, { "ForeArm", "Forearm" }, { "Hand", "Hand" },
            { "UpLeg", "Thigh" }, { "Leg", "Calf" }, { "Foot", "Foot" }, { "ToeBase", "Toe0" }, { "Toe_End", "Toe0Nub" },
        };
        static const QHash<QString, int> FINGERS { { "Thumb", 0 }, { "Index", 1 }, { "Middle", 2 }, { "Ring", 3 }, { "Pinky", 4 } };
        if (BODY.contains(name)) {
            return BODY.value(name);
        }
        QString side, rest, finger;
        int number;
        if (!splitSide(name, side, rest)) {
            return QString();
        }
        const QString prefix = side == "Left" ? "Bip01 L " : "Bip01 R ";
        if (LIMBS.contains(rest)) {
            return prefix + LIMBS.value(rest);
        }
        if (splitFinger(rest, finger, number)) {
            const QString base = prefix + "Finger" + QString::number(FINGERS.value(finger));
            return number == 1 ? base : number == 4 ? base + "Nub" : base + QString::number(number - 1);
        }
        return QString();
    });
    rig.add("Bip01", "Bip01", QString(), glm::vec3());
    rig.setParent("Hips", "Bip01");
    for (const QString& side : { QString("Left"), QString("Right") }) {
        rig.setParent(side + "UpLeg", "Spine");
        rig.setParent(side + "Shoulder", "Neck");
    }
    const QHash<QString, QString> mapped = rig.map();
    QHash<QString, QString> expected = MAIN_JOINTS_BY_KEY;
    expected.insert("Spine1", "Spine1");
    expected.insert("HeadTop_End", "HeadTop_End");
    expected.insert("LeftHandThumb3", "LeftHandThumb3");
    expected.insert("RightHandMiddle2", "RightHandMiddle2");
    verifyMapping(mapped, expected);
}

// Second Life's Bento skeleton: mPelvis, mTorso, mChest, with the extra mSpine1 to 4 folded between them
void HumanoidJointMapTests::testSecondLifeBento() {
    Rig rig = Rig::fromTemplate([](const QString& name) -> QString {
        static const QHash<QString, QString> BODY {
            { "Hips", "mPelvis" }, { "Spine", "mTorso" }, { "Spine2", "mChest" }, { "Neck", "mNeck" }, { "Head", "mHead" },
            { "HeadTop_End", "mSkull" },
        };
        static const QHash<QString, QString> LIMBS {
            { "Shoulder", "mCollar" }, { "Arm", "mShoulder" }, { "ForeArm", "mElbow" }, { "Hand", "mWrist" }, { "UpLeg", "mHip" },
            { "Leg", "mKnee" }, { "Foot", "mAnkle" }, { "ToeBase", "mFoot" }, { "Toe_End", "mToe" }, { "Eye", "mEye" },
        };
        if (BODY.contains(name)) {
            return BODY.value(name);
        }
        QString side, rest, finger;
        int number;
        if (!splitSide(name, side, rest)) {
            return QString();
        }
        if (LIMBS.contains(rest)) {
            return LIMBS.value(rest) + side;
        }
        if (splitFinger(rest, finger, number) && number <= 3) {
            return "mHand" + finger + QString::number(number) + side;
        }
        return QString();
    });
    rig.insertBetween("mSpine1", "mSpine1", "Hips", "Spine");
    rig.insertBetween("mSpine2", "mSpine2", "mSpine1", "Spine");
    rig.insertBetween("mSpine3", "mSpine3", "Spine", "Spine2");
    rig.insertBetween("mSpine4", "mSpine4", "mSpine3", "Spine2");
    const QHash<QString, QString> mapped = rig.map();
    QHash<QString, QString> expected = MAIN_JOINTS_BY_KEY;
    expected.insert("LeftEye", "LeftEye");
    expected.insert("RightHandIndex3", "RightHandIndex3");
    verifyMapping(mapped, expected);
    QVERIFY(!mapped.contains("Spine1"));
    for (const QString& key : mapped) {
        QVERIFY2(!key.startsWith("mSpine"), qPrintable(key + " mapped"));
    }
}

// VRoid's J_Bip_ names, with its hair and other secondary joints, which are left out
void HumanoidJointMapTests::testVRoid() {
    Rig rig = Rig::fromTemplate([](const QString& name) -> QString {
        static const QHash<QString, QString> BODY {
            { "Hips", "J_Bip_C_Hips" }, { "Spine", "J_Bip_C_Spine" }, { "Spine1", "J_Bip_C_Chest" }, { "Spine2", "J_Bip_C_UpperChest" },
            { "Neck", "J_Bip_C_Neck" }, { "Head", "J_Bip_C_Head" },
        };
        static const QHash<QString, QString> LIMBS {
            { "Shoulder", "Shoulder" }, { "Arm", "UpperArm" }, { "ForeArm", "LowerArm" }, { "Hand", "Hand" }, { "UpLeg", "UpperLeg" },
            { "Leg", "LowerLeg" }, { "Foot", "Foot" }, { "ToeBase", "ToeBase" },
        };
        if (BODY.contains(name)) {
            return BODY.value(name);
        }
        QString side, rest, finger;
        int number;
        if (!splitSide(name, side, rest)) {
            return QString();
        }
        const QString letter = side.left(1);
        if (LIMBS.contains(rest)) {
            return "J_Bip_" + letter + "_" + LIMBS.value(rest);
        }
        if (rest == "Eye") {
            return "J_Adj_" + letter + "_FaceEye";
        }
        if (splitFinger(rest, finger, number) && number <= 3) {
            return "J_Bip_" + letter + "_" + (finger == "Pinky" ? "Little" : finger) + QString::number(number);
        }
        return QString();
    });
    rig.add("Root", "Root", QString(), glm::vec3());
    rig.setParent("Hips", "Root");
    rig.add("hair", "J_Sec_Hair1_01", "Head", rig.position("Head") + glm::vec3(0.0f, 15.0f, 0.0f));
    rig.add("hair2", "J_Sec_Hair2_01", "hair", rig.position("Head") + glm::vec3(0.0f, 10.0f, -5.0f));
    rig.add("bust", "J_Sec_L_Bust1", "Spine2", rig.position("Spine2") + glm::vec3(5.0f, 0.0f, 8.0f));
    const QHash<QString, QString> mapped = rig.map();
    QHash<QString, QString> expected = MAIN_JOINTS_BY_KEY;
    expected.insert("Spine1", "Spine1");
    expected.insert("LeftEye", "LeftEye");
    expected.insert("RightEye", "RightEye");
    verifyMapping(mapped, expected);
    for (const QString& key : { "hair", "hair2", "bust", "Root" }) {
        QVERIFY2(!mapped.values().contains(key), qPrintable(QString(key) + " mapped"));
    }
}

// Synty's characters: Hips, Neck and Head as Darkride names them, Shoulder_L for the upper arm, and fingers named the same
// on both hands, one of them with no name the patterns know (Finger_01, the middle finger)
void HumanoidJointMapTests::testSynty() {
    const Rig rig = Rig::fromTemplate([](const QString& name) -> QString {
        static const QHash<QString, QString> BODY {
            { "Hips", "Hips" }, { "Spine", "Spine_01" }, { "Spine1", "Spine_02" }, { "Spine2", "Spine_03" }, { "Neck", "Neck" },
            { "Head", "Head" },
        };
        static const QHash<QString, QString> LIMBS {
            { "Shoulder", "Clavicle" }, { "Arm", "Shoulder" }, { "ForeArm", "Elbow" }, { "Hand", "Hand" }, { "UpLeg", "UpperLeg" },
            { "Leg", "LowerLeg" }, { "Foot", "Ankle" }, { "ToeBase", "Ball" }, { "Toe_End", "Toes" },
        };
        if (BODY.contains(name)) {
            return BODY.value(name);
        }
        QString side, rest, finger;
        int number;
        if (!splitSide(name, side, rest)) {
            return QString();
        }
        if (LIMBS.contains(rest)) {
            return LIMBS.value(rest) + "_" + side.left(1);
        }
        if (splitFinger(rest, finger, number) && number <= 3) {
            // the same names on both hands
            if (finger == "Thumb") {
                return "Thumb_0" + QString::number(number);
            } else if (finger == "Index") {
                return "IndexFinger_0" + QString::number(number);
            } else if (finger == "Middle") {
                return "Finger_0" + QString::number(number);
            }
        }
        return QString();
    });
    const QHash<QString, QString> mapped = rig.map();
    verifyMapping(mapped, {
        { "Spine", "Spine" }, { "Spine1", "Spine1" }, { "Spine2", "Spine2" }, { "LeftShoulder", "LeftShoulder" },
        { "LeftArm", "LeftArm" }, { "LeftForeArm", "LeftForeArm" }, { "LeftHand", "LeftHand" }, { "RightHand", "RightHand" },
        { "LeftUpLeg", "LeftUpLeg" }, { "LeftLeg", "LeftLeg" }, { "LeftFoot", "LeftFoot" }, { "LeftToeBase", "LeftToeBase" },
        { "LeftHandThumb1", "LeftHandThumb1" }, { "RightHandThumb1", "RightHandThumb1" },
        { "LeftHandIndex2", "LeftHandIndex2" }, { "LeftHandMiddle1", "LeftHandMiddle1" }, { "RightHandMiddle3", "RightHandMiddle3" },
    });
    for (const QString& present : { "Hips", "Neck", "Head" }) {
        QVERIFY(!mapped.contains(present));
    }
}

// An MMD model with English names: the body split into a lower and an upper half under "center", which is the hips,
// an arm twist joint in the arm, and IK joints at the root, which are left out
void HumanoidJointMapTests::testMMD() {
    Rig rig = Rig::fromTemplate([](const QString& name) -> QString {
        static const QHash<QString, QString> BODY {
            { "Hips", "lower body" }, { "Spine", "upper body" }, { "Spine2", "upper body2" }, { "Neck", "neck" }, { "Head", "head" },
        };
        static const QHash<QString, QString> LIMBS {
            { "Shoulder", "shoulder" }, { "Arm", "arm" }, { "ForeArm", "elbow" }, { "Hand", "wrist" }, { "UpLeg", "leg" },
            { "Leg", "knee" }, { "Foot", "ankle" }, { "ToeBase", "toe" }, { "Eye", "eye" },
        };
        if (BODY.contains(name)) {
            return BODY.value(name);
        }
        QString side, rest, finger;
        int number;
        if (!splitSide(name, side, rest)) {
            return QString();
        }
        const QString suffix = "_" + side.left(1);
        if (LIMBS.contains(rest)) {
            return LIMBS.value(rest) + suffix;
        }
        if (splitFinger(rest, finger, number) && number <= 3) {
            // its thumbs count from 0
            return (finger == "Pinky" ? QString("little") : finger.toLower()) +
                   QString::number(finger == "Thumb" ? number - 1 : number) + suffix;
        }
        return QString();
    });
    rig.add("center", "center", QString(), rig.position("Hips"));
    rig.setParent("Hips", "center");
    rig.setParent("Spine", "center");
    for (const QString& side : { QString("Left"), QString("Right") }) {
        const QString suffix = side == "Left" ? "_L" : "_R";
        rig.insertBetween("arm twist" + suffix, "arm twist" + suffix, side + "Arm", side + "ForeArm");
        rig.add("leg IK" + suffix, "leg IK" + suffix, "center", rig.position(side + "Foot"));
        rig.add("toe IK" + suffix, "toe IK" + suffix, "leg IK" + suffix, rig.position(side + "ToeBase"));
    }
    const QHash<QString, QString> mapped = rig.map();
    QHash<QString, QString> expected = MAIN_JOINTS_BY_KEY;
    expected.insert("Hips", "center");
    expected.insert("LeftEye", "LeftEye");
    expected.insert("LeftHandThumb1", "LeftHandThumb1");
    expected.insert("RightHandThumb3", "RightHandThumb3");
    verifyMapping(mapped, expected);
    for (const QString& key : mapped) {
        QVERIFY2(!key.contains("twist") && !key.contains("IK"), qPrintable(key + " mapped"));
    }
}

// Four legs and no hands is no humanoid
void HumanoidJointMapTests::testQuadruped() {
    Rig rig;
    rig.add("Root", "Root", QString(), glm::vec3(0.0f, 0.0f, 0.0f));
    rig.add("Pelvis", "Pelvis", "Root", glm::vec3(0.0f, 60.0f, -40.0f));
    rig.add("Spine1", "Spine1", "Pelvis", glm::vec3(0.0f, 62.0f, -10.0f));
    rig.add("Spine2", "Spine2", "Spine1", glm::vec3(0.0f, 64.0f, 20.0f));
    rig.add("Neck", "Neck", "Spine2", glm::vec3(0.0f, 75.0f, 40.0f));
    rig.add("Head", "Head", "Neck", glm::vec3(0.0f, 85.0f, 55.0f));
    rig.add("Tail1", "Tail1", "Pelvis", glm::vec3(0.0f, 62.0f, -55.0f));
    for (const QString& side : { QString("L"), QString("R") }) {
        const float x = side == "L" ? 10.0f : -10.0f;
        for (const QString& end : { QString("Front"), QString("Back") }) {
            const QString parent = end == "Front" ? "Spine2" : "Pelvis";
            const float z = end == "Front" ? 25.0f : -40.0f;
            rig.add(end + "Leg_" + side, end + "Leg_" + side, parent, glm::vec3(x, 55.0f, z));
            rig.add(end + "Knee_" + side, end + "Knee_" + side, end + "Leg_" + side, glm::vec3(x, 30.0f, z));
            rig.add(end + "Foot_" + side, end + "Foot_" + side, end + "Knee_" + side, glm::vec3(x, 5.0f, z));
            rig.add(end + "Toe_" + side, end + "Toe_" + side, end + "Foot_" + side, glm::vec3(x, 0.0f, z + 5.0f));
        }
    }
    QVERIFY(rig.map().isEmpty());
}

// A building's and a desk lamp's hierarchy, as a model entity has, is no humanoid
void HumanoidJointMapTests::testScenery() {
    Rig rig;
    rig.add("Building", "Building", QString(), glm::vec3(), false);
    rig.add("Floor1", "Floor1", "Building", glm::vec3(0.0f, 0.0f, 0.0f), false);
    rig.add("Door", "Door", "Floor1", glm::vec3(0.0f, 100.0f, 500.0f), false);
    rig.add("Handle", "Door_Handle", "Door", glm::vec3(40.0f, 100.0f, 505.0f), false);
    rig.add("WindowL", "Window_L", "Floor1", glm::vec3(300.0f, 150.0f, 500.0f), false);
    rig.add("WindowR", "Window_R", "Floor1", glm::vec3(-300.0f, 150.0f, 500.0f), false);
    rig.add("Lamp", "Desk_Lamp", "Floor1", glm::vec3(0.0f, 75.0f, 0.0f), false);
    rig.add("Arm1", "Lamp_Arm.001", "Lamp", glm::vec3(0.0f, 100.0f, 0.0f), false);
    rig.add("Arm2", "Lamp_Arm.002", "Arm1", glm::vec3(0.0f, 120.0f, 20.0f), false);
    rig.add("Head", "Lamp_Head", "Arm2", glm::vec3(0.0f, 115.0f, 35.0f), false);
    rig.add("Bulb", "Bulb", "Head", glm::vec3(0.0f, 110.0f, 38.0f), false);
    QVERIFY(rig.map().isEmpty());
}

// A rig whose joints don't say which side they're on, told apart by position: facing -Z (as VRM 0.x's do) its left is at
// -X, which taking +Z for granted swapped (F11)
void HumanoidJointMapTests::testFacingMinusZ() {
    auto unsided = [](const QString& name) -> QString {
        static const QHash<QString, QString> NAMES {
            { "Hips", "pelvis" }, { "Spine", "spine" }, { "Spine1", "spine.001" }, { "Spine2", "chest" }, { "Neck", "neck" },
            { "Head", "head" },
        };
        static const QHash<QString, QString> LIMBS {
            { "Shoulder", "shoulder" }, { "Arm", "upper_arm" }, { "ForeArm", "forearm" }, { "Hand", "hand" }, { "UpLeg", "thigh" },
            { "Leg", "shin" }, { "Foot", "foot" }, { "ToeBase", "toe" },
        };
        if (NAMES.contains(name)) {
            return NAMES.value(name);
        }
        QString side, rest;
        return splitSide(name, side, rest) ? LIMBS.value(rest) : QString();
    };
    for (bool facingMinusZ : { false, true }) {
        const QHash<QString, QString> mapped = Rig::fromTemplate(unsided, facingMinusZ).map();
        verifyMapping(mapped, {
            { "LeftFoot", "LeftFoot" }, { "RightFoot", "RightFoot" }, { "LeftUpLeg", "LeftUpLeg" }, { "LeftToeBase", "LeftToeBase" },
            { "LeftHand", "LeftHand" }, { "RightHand", "RightHand" }, { "LeftArm", "LeftArm" }, { "RightShoulder", "RightShoulder" },
        });
    }
}
