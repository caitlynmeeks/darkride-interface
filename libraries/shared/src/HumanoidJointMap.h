//
//  HumanoidJointMap.h
//  libraries/shared/src
//
//  Created on 2026-10-19 for Darkride's avatar support.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_HumanoidJointMap_h
#define hifi_HumanoidJointMap_h

#include <vector>

#include <QHash>
#include <QString>
#include <QStringList>

#include <glm/glm.hpp>

// What the mapping needs to know of a joint
struct HumanoidJointInfo {
    QString name;
    int parentIndex { -1 };
    bool isSkeletonJoint { true };
    glm::vec3 position;  // in the model, which faces +Z
};

// Works out which of a model's joints are Darkride's humanoid joints (Hips, Spine, LeftArm, LeftHandIndex1...) when the
// skeleton names them otherwise: Mixamo's with a prefix (mixamorig:Hips), Unreal's, Unity humanoids', 3ds Max Biped's,
// Second Life's and OpenSim's, VRoid's, Synty's and the like. The shape of the skeleton decides most of it (the hips are
// above both feet, an arm is what lies between the chest and a hand) and names decide between candidates.
//
// Returns the indices of the joints to rename and Darkride's names for them, leaving out any name a joint already has and
// any joint that already has one of Darkride's names. By index rather than name, as an FST's jointMap is, because some
// rigs use the same name on both sides (Synty's fingers). Empty if the model doesn't look like a humanoid.
QHash<int, QString> mapHumanoidJoints(const std::vector<HumanoidJointInfo>& joints);

// Whether the joints, as named, have all of Darkride's main humanoid joints (hips, spine, head, legs and arms)
bool hasMainHumanoidJoints(const QStringList& jointNames);

#endif // hifi_HumanoidJointMap_h
