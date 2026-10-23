//
//  VRMReader.h
//  libraries/fbx/src
//
//  Created on 2026-10-19 for Darkride's avatar support.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_VRMReader_h
#define hifi_VRMReader_h

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <BlendshapeConstants.h>
#include <hfm/HFM.h>

// What Darkride uses of a VRM avatar (https://vrm.dev), from the VRM (0.x) or VRMC_vrm and VRMC_springBone (1.0)
// extensions of a glTF binary (.vrm, or a .glb that kept them): which nodes are which humanoid bones, which morph targets
// make the mouth shapes and blinks, its spring bones (hair, skirts, ears) and their colliders as Flow's, and who made it
// and on what terms.
// A material's MToon (toon shading) settings, as 1.0's VRMC_materials_mtoon gives them (0.x's converted as three-vrm does)
struct VRMToonMaterial {
    glm::vec3 shadeColor { 0.0f };  // linear
    float shadingShift { 0.0f };
    float shadingToony { 0.9f };
    float giEqualization { 0.9f };
};

struct VRMInfo {
    int version { -1 };  // 0 for 0.x, 1 for 1.0; -1 if it isn't a VRM
    QHash<QString, QString> jointNames;  // a node's name: Darkride's name for its joint
    QHash<int, QVector<BlendshapeSource>> blendshapes;  // a Blendshapes value: the morph targets to blend for it
    // Flow finds its joints by name (flow_<group>_...), so each spring's joints are renamed into a group of their own,
    // with that group's physics; and the colliders become its collision spheres, as an FST's flow data would give them
    QHash<QString, QString> flowJointNames;  // a node's name: its name for Flow
    QVariantMap flowPhysics;                  // a Flow group: its stiffness, damping, gravity and radius
    QVariantMap flowCollisions;               // a joint (by the name it ends up with): a sphere's radius and offset
    std::vector<hfm::NodeConstraint> nodeConstraints;  // 1.0's VRMC_node_constraint, by the joints' final names
    // How far its eyes turn as the gaze does (lookAt): the gaze's turn times eyeLookScale, up to eyeLookMaxAngle
    // (degrees); a scale of 0 where its eyes look by expression rather than by turning their bones
    float eyeLookScale { 1.0f };
    float eyeLookMaxAngle { 25.0f };
    float eyeLookExpressionScale { 0.0f };  // where they look by expression: how much of one per degree of gaze
    // its MToon materials' toon shading, by the material's name, and by its number for those whose names aren't theirs alone
    QHash<QString, VRMToonMaterial> toonMaterialsByName;
    QHash<int, VRMToonMaterial> toonMaterials;
    QString title;
    QString authors;
    QString licence;
    bool onlyAuthorMayWear { false };    // only its author may use it as an avatar
    bool redistributionAllowed { true };  // others may share copies of it

    bool isVRM() const { return version >= 0; }
};

VRMInfo readVRM(const QByteArray& glb);

#endif // hifi_VRMReader_h
