//
//  HumanoidJointMap.cpp
//  libraries/shared/src
//
//  Created on 2026-10-19 for Darkride's avatar support.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "HumanoidJointMap.h"

#include <algorithm>

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

// Finding joints by the shape of the skeleton, with names deciding between candidates, is the approach Godot's BoneMapper
// takes too; this is written for Darkride's skeleton and shares no code with it.

namespace {

// Names are lower-cased before they're matched; "(^|[^a-z])ik([^a-z]|$)" finds ik in "hand_ik.l" but not in "kick"
using pattern = QRegularExpression;

// Joints that aren't part of the body: controls, IK targets, helpers and attachment points
const QRegularExpression CONTROL = pattern("(^|[^a-z])(ik|fk|ctrl|ctl|control|controller|target|pole|mch|org|helper|socket|attach|weapon|prop)([^a-z]|$)");
// Ends of chains (HeadTop_End, Bip01 L Toe0Nub), which are never the first joint of anything
const QRegularExpression END = pattern("(^|[^a-z])(end|nub|tip|top)([^a-z]|$)");
const QRegularExpression TWIST = pattern("twist|roll");

const QRegularExpression LEFT = pattern("left|(^|[^a-z0-9])l([^a-z0-9]|$)");
const QRegularExpression RIGHT = pattern("right|(^|[^a-z0-9])r([^a-z0-9]|$)");

const QRegularExpression HIPS = pattern("hip|pelvis");
const QRegularExpression FOOT = pattern("foot|ankle");
const QRegularExpression NOT_FOOT = pattern("toe|ball|finger|index|thumb|middle|ring|pinky|little");
const QRegularExpression TOE = pattern("toe|ball");
const QRegularExpression HAND = pattern("hand|wrist");
const QRegularExpression NOT_HAND = pattern("thumb|index|middle|ring|pinky|pinkie|little|finger|carpal");
const QRegularExpression NECK = pattern("neck");
const QRegularExpression HEAD = pattern("head");
const QRegularExpression NOT_HEAD = pattern("top|hair|jaw|eye|brow|nose|lip|mouth|ear|cheek|tongue|teeth|face|hat|neck");
// named for the head too: Rocketbox's head has eyelid joints such as "Bip01 LEyeBlinkTop"
const QRegularExpression HEAD_TOP = pattern("head.*(top|end|nub)|(top|end|nub).*head");
const QRegularExpression EYE = pattern("eye");
const QRegularExpression NOT_EYE = pattern("brow|lid|lash|socket|look|aim|blink");
const QRegularExpression METACARPAL = pattern("carpal|meta|palm");
// Second Life's extra Bento spine joints, which sit folded up between the pelvis, torso and chest
const QRegularExpression SECOND_LIFE_SPINE = pattern("^mspine[0-9]$");

struct Finger {
    const char* name;
    QRegularExpression pattern;
};
// 3ds Max's Biped numbers them: Finger0 is the thumb
const Finger FINGERS[] = {
    { "Thumb", pattern("thumb|pollex|finger0") },
    { "Index", pattern("index|pointer|fore|finger1") },
    { "Middle", pattern("middle|finger2") },
    { "Ring", pattern("ring|third|finger3") },
    { "Pinky", pattern("pinky|pinkie|little|finger4") },
};

QStringList standardNames() {
    QStringList names { "Hips", "Spine", "Spine1", "Spine2", "Neck", "Head", "HeadTop_End", "LeftEye", "RightEye" };
    for (const char* side : { "Left", "Right" }) {
        for (const char* joint : { "Shoulder", "Arm", "ForeArm", "Hand", "UpLeg", "Leg", "Foot", "ToeBase", "Toe_End" }) {
            names << QString(side) + joint;
        }
        for (const Finger& finger : FINGERS) {
            for (int n = 1; n <= 4; n++) {
                names << QString(side) + "Hand" + finger.name + QString::number(n);
            }
        }
    }
    return names;
}

const QStringList& STANDARD_NAMES() {
    static const QStringList names = standardNames();
    return names;
}

// Lower case, without a namespace (mixamorig:Hips) or Blender's version of one (mixamorig1_Hips)
QString normalised(const QString& name) {
    static const QRegularExpression MIXAMO_PREFIX("^mixamorig[0-9]*_");
    QString result = name.mid(name.lastIndexOf(':') + 1).toLower();
    result.remove(MIXAMO_PREFIX);
    return result;
}

class Mapper {
public:
    Mapper(const std::vector<HumanoidJointInfo>& joints);

    QHash<int, QString> map();

private:
    bool mapStandardNames();
    bool mapByShape();
    void mapLeg(const QString& side, int legRoot, int foot);
    void mapArm(const QString& side, int armRoot, int hand);
    void mapFingers(const QString& side, int hand);

    bool isSkeleton(int joint) const { return _allSkeleton || _joints[joint].isSkeletonJoint; }
    int parent(int joint) const;
    bool isAncestor(int ancestor, int joint) const;
    int commonAncestor(int a, int b) const;
    std::vector<int> between(int top, int bottom) const;
    std::vector<int> children(int joint) const;
    int side(int joint) const;
    bool matches(int joint, const QRegularExpression& expression) const;
    std::vector<int> candidates(const QRegularExpression& include, const QRegularExpression& exclude, int under = -1) const;
    int shallowest(const std::vector<int>& joints) const;
    std::pair<int, int> leftAndRight(const std::vector<int>& joints) const;
    void assign(const QString& name, int joint);

    const std::vector<HumanoidJointInfo>& _joints;
    std::vector<QString> _names;
    std::vector<int> _depths;
    QSet<QString> _present;     // Darkride's names that joints already have
    QSet<int> _used;            // joints that have one of Darkride's names, or have been given one
    QHash<QString, int> _found;
    bool _allSkeleton { true };  // when none is marked as in the skeleton, as in an animation file with no skinned mesh
};

Mapper::Mapper(const std::vector<HumanoidJointInfo>& joints) : _joints(joints) {
    const int count = (int)joints.size();
    _names.reserve(count);
    _depths.resize(count, 0);
    for (int i = 0; i < count; i++) {
        _names.push_back(normalised(joints[i].name));
        int depth = 0;
        for (int j = parent(i); j >= 0 && depth < count; j = parent(j)) {
            depth++;
        }
        _depths[i] = depth;
        _allSkeleton = _allSkeleton && !joints[i].isSkeletonJoint;
        if (STANDARD_NAMES().contains(joints[i].name)) {
            _present.insert(joints[i].name);
            _used.insert(i);
        }
    }
}

QHash<int, QString> Mapper::map() {
    if (!mapStandardNames() && !mapByShape()) {
        return {};
    }
    QHash<int, QString> result;
    for (auto itr = _found.begin(); itr != _found.end(); itr++) {
        result.insert(itr.value(), itr.key());
    }
    return result;
}

// Darkride's own names, as Mixamo's, behind a namespace or prefix
bool Mapper::mapStandardNames() {
    QHash<QString, int> joints;
    for (int i = 0; i < (int)_joints.size(); i++) {
        if (isSkeleton(i) && !joints.contains(_names[i])) {
            joints.insert(_names[i], i);
        }
    }
    for (const char* name : { "hips", "leftupleg", "rightupleg", "leftarm", "rightarm" }) {
        if (!joints.contains(name)) {
            return false;
        }
    }
    for (const QString& name : STANDARD_NAMES()) {
        auto joint = joints.find(name.toLower());
        if (joint != joints.end()) {
            assign(name, joint.value());
        }
    }
    return true;
}

bool Mapper::mapByShape() {
    // The feet, and the hips above both of them: a joint named like hips, else where the legs meet
    auto feet = leftAndRight(candidates(FOOT, NOT_FOOT));
    if (feet.first < 0 || feet.second < 0) {
        return false;
    }
    const int legRoot = commonAncestor(feet.first, feet.second);
    if (legRoot < 0) {
        return false;
    }
    int hips = legRoot;
    for (int joint = legRoot; joint >= 0; joint = parent(joint)) {
        if (matches(joint, HIPS) && side(joint) == 0) {
            hips = joint;
            break;
        }
    }

    // The hands, which aren't in the legs, and the chest the arms meet at
    std::vector<int> legTops;
    for (int foot : { feet.first, feet.second }) {
        std::vector<int> leg = between(legRoot, foot);
        legTops.push_back(leg.empty() ? foot : leg.front());
    }
    std::vector<int> hands;
    for (int hand : candidates(HAND, NOT_HAND)) {
        if (hand != legTops[0] && hand != legTops[1] && !isAncestor(legTops[0], hand) && !isAncestor(legTops[1], hand)) {
            hands.push_back(hand);
        }
    }
    auto leftAndRightHands = leftAndRight(hands);
    if (leftAndRightHands.first < 0 || leftAndRightHands.second < 0) {
        return false;
    }
    const int armRoot = commonAncestor(leftAndRightHands.first, leftAndRightHands.second);
    // Rigs that split the body into lower and upper halves, as MMD's do, have their hips where the halves meet
    if (armRoot >= 0 && !isAncestor(hips, armRoot)) {
        hips = commonAncestor(legRoot, armRoot);
    }
    if (armRoot < 0 || hips < 0 || !isAncestor(hips, armRoot)) {
        return false;
    }
    // Biped hangs the arms from the neck
    int chest = armRoot;
    int neck = -1;
    if (matches(armRoot, NECK) && isAncestor(hips, parent(armRoot))) {
        neck = armRoot;
        chest = parent(armRoot);
    }

    assign("Hips", hips);
    mapLeg("Left", legRoot, feet.first);
    mapLeg("Right", legRoot, feet.second);
    mapArm("Left", armRoot, leftAndRightHands.first);
    mapArm("Right", armRoot, leftAndRightHands.second);

    // The spine runs from the hips to the chest: Spine at the bottom, Spine2 the chest, Spine1 just below it
    std::vector<int> spine;
    for (int joint : between(hips, chest)) {
        if (!matches(joint, SECOND_LIFE_SPINE)) {
            spine.push_back(joint);
        }
    }
    spine.push_back(chest);
    assign("Spine", spine.front());
    if (spine.size() >= 2) {
        if (spine.size() >= 3) {
            assign("Spine1", spine[spine.size() - 2]);
        }
        assign("Spine2", spine.back());
    }

    std::vector<int> heads;
    for (int head : candidates(HEAD, NOT_HEAD, chest)) {
        if (side(head) == 0) {
            heads.push_back(head);
        }
    }
    const int head = shallowest(heads);
    if (head >= 0) {
        if (neck < 0) {
            std::vector<int> necks = between(chest, head);
            if (!necks.empty()) {
                neck = necks.front();
            }
        }
        assign("Neck", neck);
        assign("Head", head);
        for (int child : children(head)) {
            if (matches(child, HEAD_TOP) && side(child) == 0) {
                assign("HeadTop_End", child);
                break;
            }
        }
        auto eyes = leftAndRight(candidates(EYE, NOT_EYE, head));
        assign("LeftEye", eyes.first);
        assign("RightEye", eyes.second);
    } else {
        assign("Neck", neck);
    }

    mapFingers("Left", leftAndRightHands.first);
    mapFingers("Right", leftAndRightHands.second);
    return true;
}

void Mapper::mapLeg(const QString& side, int legRoot, int foot) {
    std::vector<int> leg;
    for (int joint : between(legRoot, foot)) {
        if (!matches(joint, TWIST)) {
            leg.push_back(joint);
        }
    }
    // a joint between the hips and the thigh, as some motion capture rigs have, is left out
    if (leg.size() >= 2) {
        assign(side + "UpLeg", leg[leg.size() - 2]);
        assign(side + "Leg", leg.back());
    } else if (leg.size() == 1) {
        assign(side + "UpLeg", leg.back());
    }
    assign(side + "Foot", foot);

    int toe = -1;
    for (int child : children(foot)) {
        if (!matches(child, END)) {
            if (matches(child, TOE)) {
                toe = child;
                break;
            }
            if (toe < 0) {
                toe = child;
            }
        }
    }
    if (toe >= 0) {
        assign(side + "ToeBase", toe);
        std::vector<int> toeEnds = children(toe);
        if (!toeEnds.empty()) {
            assign(side + "Toe_End", toeEnds.front());
        }
    }
}

void Mapper::mapArm(const QString& side, int armRoot, int hand) {
    std::vector<int> arm;
    for (int joint : between(armRoot, hand)) {
        if (!matches(joint, TWIST)) {
            arm.push_back(joint);
        }
    }
    // the shoulder (clavicle), the upper arm and the forearm; some rigs have no shoulder
    if (arm.size() >= 3) {
        assign(side + "Shoulder", arm.front());
    }
    if (arm.size() >= 2) {
        assign(side + "Arm", arm[arm.size() - 2]);
    }
    if (!arm.empty()) {
        assign(side + "ForeArm", arm.back());
    }
    assign(side + "Hand", hand);
}

void Mapper::mapFingers(const QString& side, int hand) {
    for (const Finger& finger : FINGERS) {
        int joint = shallowest(candidates(finger.pattern, METACARPAL, hand));
        for (int n = 1; n <= 4 && joint >= 0; n++) {
            assign(side + "Hand" + finger.name + QString::number(n), joint);
            int next = -1;
            for (int child : children(joint)) {
                if (matches(child, finger.pattern)) {
                    next = child;
                    break;
                }
                if (next < 0) {
                    next = child;
                }
            }
            joint = next;
        }
    }

    // A hand with one finger left that has no name the patterns know, as Synty's Finger_01: the middle finger
    const QString middle = side + "HandMiddle1";
    if (!_found.contains(middle) && !_present.contains(middle)) {
        std::vector<int> fingers;
        for (int child : children(hand)) {
            if (!_used.contains(child) && !matches(child, END) && !children(child).empty()) {
                fingers.push_back(child);
            }
        }
        if (fingers.size() == 1) {
            int joint = fingers.front();
            for (int n = 1; n <= 4 && joint >= 0; n++) {
                assign(side + "HandMiddle" + QString::number(n), joint);
                std::vector<int> next = children(joint);
                joint = next.empty() ? -1 : next.front();
            }
        }
    }
}

int Mapper::parent(int joint) const {
    if (joint < 0 || joint >= (int)_joints.size()) {
        return -1;
    }
    int parentIndex = _joints[joint].parentIndex;
    return (parentIndex >= 0 && parentIndex < (int)_joints.size()) ? parentIndex : -1;
}

bool Mapper::isAncestor(int ancestor, int joint) const {
    if (ancestor < 0) {
        return false;
    }
    int steps = 0;
    for (int i = parent(joint); i >= 0 && steps <= (int)_joints.size(); i = parent(i), steps++) {
        if (i == ancestor) {
            return true;
        }
    }
    return false;
}

int Mapper::commonAncestor(int a, int b) const {
    for (int i = a; i >= 0; i = parent(i)) {
        if (i == b || isAncestor(i, b)) {
            return i;
        }
    }
    return -1;
}

// The joints strictly between top and bottom, top's end first; none if top isn't above bottom
std::vector<int> Mapper::between(int top, int bottom) const {
    std::vector<int> joints;
    if (!isAncestor(top, bottom)) {
        return joints;
    }
    for (int i = parent(bottom); i >= 0 && i != top; i = parent(i)) {
        joints.push_back(i);
    }
    std::reverse(joints.begin(), joints.end());
    return joints;
}

// A joint's children in the skeleton, leaving out controls
std::vector<int> Mapper::children(int joint) const {
    std::vector<int> result;
    for (int i = 0; i < (int)_joints.size(); i++) {
        if (parent(i) == joint && isSkeleton(i) && !matches(i, CONTROL)) {
            result.push_back(i);
        }
    }
    return result;
}

// 1 if the name says left, -1 right, 0 neither
int Mapper::side(int joint) const {
    bool left = matches(joint, LEFT);
    bool right = matches(joint, RIGHT);
    return left == right ? 0 : (left ? 1 : -1);
}

bool Mapper::matches(int joint, const QRegularExpression& expression) const {
    return joint >= 0 && joint < (int)_names.size() && expression.match(_names[joint]).hasMatch();
}

std::vector<int> Mapper::candidates(const QRegularExpression& include, const QRegularExpression& exclude, int under) const {
    std::vector<int> result;
    for (int i = 0; i < (int)_joints.size(); i++) {
        if (isSkeleton(i) && matches(i, include) && !matches(i, exclude) && !matches(i, CONTROL) &&
                !matches(i, END) && (under < 0 || isAncestor(under, i))) {
            result.push_back(i);
        }
    }
    return result;
}

int Mapper::shallowest(const std::vector<int>& joints) const {
    int result = -1;
    for (int joint : joints) {
        if (result < 0 || _depths[joint] < _depths[result]) {
            result = joint;
        }
    }
    return result;
}

// The shallowest left and right of the joints, by name; else, if the names don't say, the two shallowest told apart by
// where they are: Darkride's avatars face +Z, so their left is +X
std::pair<int, int> Mapper::leftAndRight(const std::vector<int>& joints) const {
    std::vector<int> left, right, either;
    for (int joint : joints) {
        int jointSide = side(joint);
        (jointSide > 0 ? left : (jointSide < 0 ? right : either)).push_back(joint);
    }
    int leftJoint = shallowest(left);
    int rightJoint = shallowest(right);
    if (leftJoint < 0 && rightJoint < 0) {
        std::sort(either.begin(), either.end(), [&](int a, int b) {
            return _depths[a] != _depths[b] ? _depths[a] < _depths[b] : a < b;
        });
        if (either.size() >= 2 && _depths[either[0]] == _depths[either[1]]) {
            float x0 = _joints[either[0]].position.x;
            float x1 = _joints[either[1]].position.x;
            if (x0 != x1) {
                leftJoint = x0 > x1 ? either[0] : either[1];
                rightJoint = x0 > x1 ? either[1] : either[0];
            }
        }
    }
    return { leftJoint, rightJoint };
}

void Mapper::assign(const QString& name, int joint) {
    if (joint < 0 || _used.contains(joint) || _present.contains(name) || _found.contains(name)) {
        return;
    }
    _found.insert(name, joint);
    _used.insert(joint);
}

}

QHash<int, QString> mapHumanoidJoints(const std::vector<HumanoidJointInfo>& joints) {
    return Mapper(joints).map();
}

bool hasMainHumanoidJoints(const QStringList& jointNames) {
    for (const char* name : { "Hips", "Spine", "Head", "LeftUpLeg", "RightUpLeg", "LeftLeg", "RightLeg", "LeftFoot", "RightFoot",
                              "LeftArm", "RightArm", "LeftForeArm", "RightForeArm", "LeftHand", "RightHand" }) {
        if (!jointNames.contains(name)) {
            return false;
        }
    }
    return true;
}
