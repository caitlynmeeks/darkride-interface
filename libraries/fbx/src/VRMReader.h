//
//  VRMReader.h
//  libraries/fbx/src
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_VRMReader_h
#define hifi_VRMReader_h

#include <vector>

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <BlendshapeConstants.h>
#include <hfm/HFM.h>

// A material's MToon (toon shading) settings, as 1.0's VRMC_materials_mtoon gives them (0.x's converted as three-vrm does)
struct VRMToonMaterial {
    glm::vec3 shadeColor { 0.0f };  // linear
    float shadingShift { 0.0f };
    float shadingToony { 0.9f };
    float giEqualization { 0.9f };
};

// What Darkride uses of a VRM avatar (https://vrm.dev), from the VRM (0.x) or VRMC_vrm and VRMC_springBone (1.0)
// extensions of a glTF binary: which nodes are which humanoid bones, which morph targets make the mouth shapes, blinks
// and eye looks, its spring bones (hair, skirts, ears) and their colliders as Flow's, its node constraints, and how far
// its eyes turn, and its MToon materials' toon shading.
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
    QString title;    // for the log
    QString authors;

    bool isVRM() const { return version >= 0; }
};

// Reads only the JSON chunk of a glTF binary; anything else, or a glTF without a VRM extension, gives !isVRM()
VRMInfo readVRM(const QByteArray& glb);

#endif // hifi_VRMReader_h
