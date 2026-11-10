//
//  FstBake.cpp
//  libraries/auto-rigging/src
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "FstBake.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <AnimSkeleton.h>
#include <BlendshapeConstants.h>
#include <FSTReader.h>
#include <GLMHelpers.h>
#include <HumanoidJointMap.h>
#include <HumanoidRotationOffsets.h>

QStringList autorig::morphTargetNames(const HFMModel& model) {
    QSet<int> engineShapes;
    for (const hfm::Mesh& mesh : model.meshes) {
        for (int b = 0; b < mesh.blendshapes.size() && b < (int)Blendshapes::BlendshapeCount; b++) {
            if (!mesh.blendshapes[b].indices.isEmpty()) {
                engineShapes.insert(b);
            }
        }
    }
    QStringList names;
    for (int b = 0; b < (int)Blendshapes::BlendshapeCount; b++) {
        if (engineShapes.contains(b)) {
            names << BLENDSHAPE_NAMES[b];
        }
    }
    return names + model.ownBlendshapeNames;
}

QStringList autorig::bakeFaceLines(const QStringList& morphTargetNames, int* count) {
    QStringList lines;
    int shapes = 0;
    for (int i = 0; i < (int)Blendshapes::BlendshapeCount; i++) {
        const BlendshapeSource source = findBlendshapeSource(i, morphTargetNames);
        // a morph target whose name an .fst line can't hold is left to the load, which finds it the same way
        if (source.name.isEmpty() || source.name == BLENDSHAPE_NAMES[i] || !FSTReader::isLineValue(source.name)) {
            continue;
        }
        lines << QString("bs = %1 = %2 = %3").arg(BLENDSHAPE_NAMES[i], source.name).arg(source.weight, 0, 'f', 3);
        if (source.proceduralGain != 1.0f) {
            lines << QString("bsGain = %1 = %2").arg(BLENDSHAPE_NAMES[i]).arg(source.proceduralGain, 0, 'f', 3);
        }
        shapes++;
    }
    if (count) {
        *count = shapes;
    }
    return lines;
}

void autorig::bakeFst(const HFMModel& model, bool isVRM, QStringList& lines, QStringList& found) {
    // joint names
    QStringList names = model.getJointNames();
    if (!hasMainHumanoidJoints(names)) {
        std::vector<HumanoidJointInfo> joints;
        for (const HFMJoint& joint : model.joints) {
            joints.push_back({ joint.name, joint.parentIndex, joint.isSkeletonJoint, extractTranslation(joint.globalTransform) });
        }
        const QHash<int, QString> humanoidNames = mapHumanoidJoints(joints);
        bool ambiguous = false;
        bool unwritable = false;
        for (auto itr = humanoidNames.begin(); itr != humanoidNames.end(); itr++) {
            ambiguous = ambiguous || names.count(model.joints[itr.key()].name) > 1;
            unwritable = unwritable || !FSTReader::isLineValue(model.joints[itr.key()].name);
        }
        if (ambiguous) {
            found << "its joints' names (worked out on each load, as some are used twice)";
        } else if (unwritable) {
            found << "its joints' names (worked out on each load, as an .fst line can't hold some of them)";
        } else if (!humanoidNames.isEmpty()) {
            for (auto itr = humanoidNames.begin(); itr != humanoidNames.end(); itr++) {
                lines << QString("jointMap = %1 = %2").arg(itr.value(), model.joints[itr.key()].name);
                names[itr.key()] = itr.value();
            }
            found << QString("%1 joint names").arg(humanoidNames.size());
        }
    }

    // offsets, as the model baker works them out for a model worn without an .fst, from the joints' default poses as the
    // rig builds them (AnimSkeleton); by the joints' names in the file, as jointRotationOffset2 takes them
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
    int offsetLines = 0;
    for (auto itr = offsets.begin(); itr != offsets.end(); itr++) {
        if (!FSTReader::isLineValue(model.joints[itr.key()].name)) {
            continue;
        }
        offsetLines++;
        const glm::quat& q = itr.value();
        lines << QString("jointRotationOffset2 = %1 = (%2,%3,%4,%5)").arg(model.joints[itr.key()].name)
                     .arg(q.x, 0, 'f', 6).arg(q.y, 0, 'f', 6).arg(q.z, 0, 'f', 6).arg(q.w, 0, 'f', 6);
    }
    if (offsetLines > 0) {
        found << QString("axis offsets for %1 joints").arg(offsetLines);
    }

    // The face, by ARKit's and Ready Player Me's names. Not a VRM's: its expressions are read from the file when it's
    // worn, by morph target number where the loader gets no names, which an .fst's bs lines, by name and read first,
    // would lose (three-vrm-girl keeps its names only where Assimp doesn't look)
    if (!isVRM) {
        int faceShapes = 0;
        lines << bakeFaceLines(morphTargetNames(model), &faceShapes);
        if (faceShapes > 0) {
            found << QString("%1 face shapes").arg(faceShapes);
        }
    }

    // a VRM's spring bones and colliders, for Flow
    for (auto itr = model.flowData._physicsConfig.begin(); itr != model.flowData._physicsConfig.end(); itr++) {
        QJsonObject line;
        line[itr.key()] = QJsonObject::fromVariantMap(itr.value().toMap());
        lines << "flowPhysicsData = " + FSTReader::jsonLineValue(QJsonDocument(line));
    }
    for (auto itr = model.flowData._collisionsConfig.begin(); itr != model.flowData._collisionsConfig.end(); itr++) {
        QVariantMap collision = itr.value().toMap();
        // A collider is in its joint's own axes, and an .fst's in the rig's, which the joint's offset turns (as the model
        // baker turns a model's own when it works the offsets out)
        const int jointIndex = names.indexOf(itr.key()) >= 0 ? names.indexOf(itr.key()) : model.getJointNames().indexOf(itr.key());
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
        lines << "flowCollisionsData = " + FSTReader::jsonLineValue(QJsonDocument(line));
    }
    if (!model.flowData._physicsConfig.isEmpty()) {
        found << QString("%1 groups of spring bones").arg(model.flowData._physicsConfig.size());
    }
}
