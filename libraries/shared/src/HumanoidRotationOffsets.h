//
//  HumanoidRotationOffsets.h
//  libraries/shared/src
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_HumanoidRotationOffsets_h
#define hifi_HumanoidRotationOffsets_h

#include <vector>

#include <QMap>
#include <QString>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// What the offsets need to know of a joint in an avatar's default pose
struct HumanoidRestJoint {
    QString name;  // Darkride's (Hips, LeftArm...) for the joints that have them
    int parentIndex { -1 };
    glm::vec3 position;  // absolute
    glm::quat rotation;  // absolute
};

// Animations give each joint the absolute axes Mixamo's skeleton has (AnimClip.cpp), so an avatar whose joints have other
// axes, as Blender's, Unreal's and VRM's do, is twisted by them. For each such joint this is the rotation to put after the
// joint's own in its default pose (as an FST's jointRotationOffset2 does) so that it has Mixamo's axes there.
//
// The avatar is first turned to face +Z standing up (VRM 0.x avatars face -Z) and its bones straightened into Mixamo's
// T-pose (StandardSkeleton), so an A-pose doesn't count as other axes. Joints within a small angle of Mixamo's axes are
// left out, so avatars that work today are untouched; joints are parents first, as in AnimSkeleton.
QMap<int, glm::quat> computeHumanoidRotationOffsets(const std::vector<HumanoidRestJoint>& joints);

#endif // hifi_HumanoidRotationOffsets_h
