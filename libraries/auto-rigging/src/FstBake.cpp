//
//  FstBake.cpp
//  libraries/auto-rigging/src
//
//  Created on 2026-10-19 for Darkride's avatar support.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "FstBake.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include <AnimSkeleton.h>
#include <BlendshapeConstants.h>
#include <GLMHelpers.h>
#include <HumanoidJointMap.h>
#include <HumanoidRotationOffsets.h>

// A glTF's morph target names (in its meshes' or primitives' extras), as the loaders read them
QStringList autorig::gltfMorphTargetNames(const QByteArray& data) {
    QJsonObject gltf;
    if (data.startsWith("glTF") && data.size() > 20) {
        const quint32 jsonLength = qFromLittleEndian<quint32>(data.constData() + 12);
        gltf = QJsonDocument::fromJson(data.mid(20, (int)jsonLength)).object();
    } else {
        gltf = QJsonDocument::fromJson(data).object();
    }
    QStringList names;
    for (const QJsonValue& mesh : gltf["meshes"].toArray()) {
        QJsonArray targetNames = mesh.toObject()["extras"].toObject()["targetNames"].toArray();
        if (targetNames.isEmpty()) {
            targetNames = mesh.toObject()["primitives"].toArray().at(0).toObject()["extras"].toObject()["targetNames"].toArray();
        }
        for (const QJsonValue& name : targetNames) {
            if (!names.contains(name.toString())) {
                names << name.toString();
            }
        }
    }
    return names;
}

void autorig::bakeFst(const HFMModel& model, const VRMInfo& vrm, const QStringList& morphTargetNames, QStringList& lines,
                      QStringList& found) {
    // joint names
    QStringList names = model.getJointNames();
    if (!hasMainHumanoidJoints(names)) {
        std::vector<HumanoidJointInfo> joints;
        for (const HFMJoint& joint : model.joints) {
            joints.push_back({ joint.name, joint.parentIndex, joint.isSkeletonJoint, extractTranslation(joint.globalTransform) });
        }
        const QHash<int, QString> humanoidNames = mapHumanoidJoints(joints);
        bool ambiguous = false;
        for (auto itr = humanoidNames.begin(); itr != humanoidNames.end(); itr++) {
            ambiguous = ambiguous || names.count(model.joints[itr.key()].name) > 1;
        }
        if (ambiguous) {
            found << "its joints' names (worked out on each load, as some are used twice)";
        } else if (!humanoidNames.isEmpty()) {
            for (auto itr = humanoidNames.begin(); itr != humanoidNames.end(); itr++) {
                lines << QString("jointMap = %1 = %2").arg(itr.value(), model.joints[itr.key()].name);
                names[itr.key()] = itr.value();
            }
            found << QString("%1 joint names").arg(humanoidNames.size());
        }
    }

    // offsets, as the avatar's skeleton would work them out (AnimSkeleton), by the joints' names in the file
    std::vector<HFMJoint> renamed = model.joints;
    for (int i = 0; i < (int)renamed.size(); i++) {
        renamed[i].name = names[i];
    }
    const AnimSkeleton skeleton(renamed, QMap<int, glm::quat>());
    std::vector<HumanoidRestJoint> restJoints;
    for (int i = 0; i < skeleton.getNumJoints(); i++) {
        const AnimPose& pose = skeleton.getAbsoluteDefaultPose(i);
        restJoints.push_back({ names[i], skeleton.getParentIndex(i), pose.trans(), pose.rot() });
    }
    const QMap<int, glm::quat> offsets = computeHumanoidRotationOffsets(restJoints);
    for (auto itr = offsets.begin(); itr != offsets.end(); itr++) {
        const glm::quat& q = itr.value();
        lines << QString("jointRotationOffset2 = %1 = (%2,%3,%4,%5)").arg(model.joints[itr.key()].name)
                     .arg(q.x, 0, 'f', 6).arg(q.y, 0, 'f', 6).arg(q.z, 0, 'f', 6).arg(q.w, 0, 'f', 6);
    }
    if (!offsets.isEmpty()) {
        found << QString("axis offsets for %1 joints").arg(offsets.size());
    }

    // The face, by ARKit's and Ready Player Me's names. Not a VRM's: its expressions are read from the file when it's
    // worn, by morph target number where the loader gets no names, which an FST's bs lines, by name and read first,
    // would lose (three-vrm-girl keeps its names only where Assimp doesn't look)
    int faceShapes = 0;
    for (int i = 0; i < (int)Blendshapes::BlendshapeCount && !vrm.isVRM(); i++) {
        const BlendshapeSource source = findBlendshapeSource(i, morphTargetNames);
        if (!source.name.isEmpty() && source.name != BLENDSHAPE_NAMES[i]) {
            lines << QString("bs = %1 = %2 = %3").arg(BLENDSHAPE_NAMES[i], source.name).arg(source.weight, 0, 'f', 3);
            faceShapes++;
        }
    }
    if (faceShapes > 0) {
        found << QString("%1 face shapes").arg(faceShapes);
    }

    // a VRM's spring bones and colliders, for Flow
    for (auto itr = model.flowData._physicsConfig.begin(); itr != model.flowData._physicsConfig.end(); itr++) {
        QJsonObject line;
        line[itr.key()] = QJsonObject::fromVariantMap(itr.value().toMap());
        lines << "flowPhysicsData = " + QString::fromUtf8(QJsonDocument(line).toJson(QJsonDocument::Compact));
    }
    for (auto itr = model.flowData._collisionsConfig.begin(); itr != model.flowData._collisionsConfig.end(); itr++) {
        QVariantMap collision = itr.value().toMap();
        // A collider is in its joint's own axes. Worn as the .vrm, they're turned back by the automatic offset; with
        // jointRotationOffset2 in the FST nothing is, so it's turned here
        int jointIndex = -1;
        for (int i = 0; i < (int)model.joints.size() && jointIndex < 0; i++) {
            if (model.joints[i].name == itr.key()) {
                jointIndex = i;
            }
        }
        if (offsets.contains(jointIndex) && collision.contains("offset")) {
            const QVariantMap in = collision["offset"].toMap();
            const glm::vec3 turned = glm::inverse(offsets[jointIndex]) *
                glm::vec3(in["x"].toFloat(), in["y"].toFloat(), in["z"].toFloat());
            QVariantMap out;
            out["x"] = turned.x;
            out["y"] = turned.y;
            out["z"] = turned.z;
            collision["offset"] = out;
        }
        QJsonObject line;
        line[itr.key()] = QJsonObject::fromVariantMap(collision);
        lines << "flowCollisionsData = " + QString::fromUtf8(QJsonDocument(line).toJson(QJsonDocument::Compact));
    }
    if (!model.flowData._physicsConfig.isEmpty()) {
        found << QString("%1 groups of spring bones").arg(model.flowData._physicsConfig.size());
    }
}
