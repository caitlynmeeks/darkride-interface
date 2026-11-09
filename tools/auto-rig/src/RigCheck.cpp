//
//  RigCheck.cpp
//  tools/auto-rig/src
//
//  Created on 2026-10-12 for Darkride's avatar rigger (AUTO-RIGGING.md in the project root).
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "RigCheck.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include <QFile>

#include <FBXSerializer.h>
#include <GLMHelpers.h>

#include <StandardSkeleton.h>

static int standardIndexOf(const QString& name) {
    const autorig::StandardSkeleton& standard = autorig::StandardSkeleton::get();
    const QString wanted = name.section(':', -1).toLower();
    for (int i = 0; i < standard.size(); i++) {
        if (QString(standard.joint(i).name).toLower() == wanted) {
            return i;
        }
    }
    return -1;
}

KeyRig readKeyRig(const HFMModel& key, float scale) {
    KeyRig rig;
    const int count = (int)key.joints.size();
    for (const HFMJoint& joint : key.joints) {
        rig.names.push_back(joint.name);
        rig.parents.push_back(joint.parentIndex);
        rig.positions.push_back(extractTranslation(joint.globalTransform) * scale);
    }
    rig.standard.assign(count, -1);
    for (int j = 0; j < count; j++) {
        for (int a = j; a >= 0; a = rig.parents[a]) {
            const int s = standardIndexOf(rig.names[a]);
            if (s >= 0) {
                rig.standard[j] = s;
                break;
            }
        }
    }
    // the weights, vertex by vertex in the order autorig::gatherBody lays the vertices out
    std::map<std::pair<uint32_t, uint32_t>, bool> done;
    for (const hfm::Shape& shape : key.shapes) {
        if (shape.mesh >= key.meshes.size()) {
            continue;
        }
        const hfm::Mesh& mesh = key.meshes[shape.mesh];
        if (shape.meshPart >= mesh.parts.size()) {
            continue;
        }
        const auto meshAndJoint = std::make_pair(shape.mesh, shape.joint);
        if (done.count(meshAndJoint)) {
            continue;
        }
        done[meshAndJoint] = true;
        const int perVertex = mesh.clusterWeightsPerVertex;
        const hfm::SkinDeformer* deformer =
            shape.skinDeformer < key.skinDeformers.size() ? &key.skinDeformers[shape.skinDeformer] : nullptr;
        for (int v = 0; v < mesh.vertices.size(); v++) {
            std::vector<std::pair<int, float>> weights;
            if (deformer && perVertex > 0) {
                for (int i = 0; i < perVertex; i++) {
                    const size_t at = (size_t)v * perVertex + i;
                    if (at >= mesh.clusterWeights.size() || at >= mesh.clusterIndices.size()) {
                        break;
                    }
                    const float weight = mesh.clusterWeights[at] / 65535.0f;
                    const uint16_t cluster = mesh.clusterIndices[at];
                    if (weight > 0.0f && cluster < deformer->clusters.size()) {
                        weights.push_back({ (int)deformer->clusters[cluster].jointIndex, weight });
                    }
                }
            }
            if (weights.empty()) {
                weights.push_back({ (int)shape.joint, 1.0f });  // an unskinned mesh moves with its node
            }
            rig.weights.push_back(weights);
        }
    }
    return rig;
}

void compareWeights(const autorig::Result& ours, const KeyRig& key) {
    if (key.weights.size() != ours.weights.size()) {
        fprintf(stdout, "weights: the answer key has %zu vertices, the body %zu: not compared\n", key.weights.size(),
                ours.weights.size());
        return;
    }
    const autorig::StandardSkeleton& standard = autorig::StandardSkeleton::get();
    const int hips = standard.indexOf("Hips");
    const size_t count = ours.weights.size();
    std::vector<float> a(standard.size()), b(standard.size());
    std::vector<double> regionSum(standard.size(), 0.0);
    std::vector<size_t> regionCount(standard.size(), 0);
    double total = 0.0;
    size_t same = 0;
    for (size_t v = 0; v < count; v++) {
        std::fill(a.begin(), a.end(), 0.0f);
        std::fill(b.begin(), b.end(), 0.0f);
        for (int k = 0; k < 4; k++) {
            a[ours.weights[v].joints[k]] += ours.weights[v].weights[k];
        }
        float keySum = 0.0f;
        for (const auto& w : key.weights[v]) {
            const int s = key.standard[w.first];
            b[s >= 0 ? s : hips] += w.second;
            keySum += w.second;
        }
        for (float& value : b) {
            value /= std::max(keySum, 1.0e-6f);
        }
        double l1 = 0.0;
        for (int j = 0; j < standard.size(); j++) {
            l1 += std::fabs(a[j] - b[j]);
        }
        total += l1;
        const int mainOurs = (int)(std::max_element(a.begin(), a.end()) - a.begin());
        const int mainKey = (int)(std::max_element(b.begin(), b.end()) - b.begin());
        if (mainOurs == mainKey) {
            same++;
        }
        regionSum[mainKey] += l1;
        regionCount[mainKey]++;
    }
    fprintf(stdout, "weights: mean L1 %.3f (0 the same, 2 nothing shared); the same main joint on %.1f%% of vertices\n",
            total / count, 100.0 * same / count);
    std::vector<std::pair<double, int>> regions;
    for (int j = 0; j < standard.size(); j++) {
        if (regionCount[j] >= 50) {
            regions.push_back({ regionSum[j] / regionCount[j], j });
        }
    }
    std::sort(regions.begin(), regions.end(), [](const std::pair<double, int>& x, const std::pair<double, int>& y) {
        return x.first > y.first;
    });
    QStringList worst;
    for (size_t i = 0; i < std::min<size_t>(6, regions.size()); i++) {
        worst << QString("%1 %2 (%3)").arg(standard.joint(regions[i].second).name).arg(regions[i].first, 0, 'f', 2)
                     .arg(regionCount[regions[i].second]);
    }
    fprintf(stdout, "  least alike where the key's main joint is: %s\n", qPrintable(worst.join(", ")));
}

namespace {
struct TestPose {
    const char* joint;
    glm::vec3 axis;
    float degrees;
    const char* label;
};
// the character stands in a T-pose facing +Z, its left along +X
const TestPose TEST_POSES[] = {
    { "LeftForeArm", { 0.0f, 1.0f, 0.0f }, -90.0f, "left elbow bent 90" },
    { "RightForeArm", { 0.0f, 1.0f, 0.0f }, 90.0f, "right elbow bent 90" },
    { "LeftArm", { 0.0f, 0.0f, 1.0f }, 60.0f, "left arm raised 60" },
    { "LeftHand", { 0.0f, 0.0f, 1.0f }, -60.0f, "left wrist bent 60" },
    { "LeftLeg", { 1.0f, 0.0f, 0.0f }, 90.0f, "left knee bent 90" },
    { "LeftUpLeg", { 1.0f, 0.0f, 0.0f }, -60.0f, "left hip forward 60" },
    { "Spine1", { 1.0f, 0.0f, 0.0f }, 30.0f, "spine bent forward 30" },
    { "Neck", { 0.0f, 1.0f, 0.0f }, 60.0f, "head turned 60" },
    // at the head joint itself, so the jaw line between neck and head bends (a nod opened Chloe's mouth, 10-13)
    { "Head", { 1.0f, 0.0f, 0.0f }, 25.0f, "head nodded forward 25" },
    { "Head", { 1.0f, 0.0f, 0.0f }, -25.0f, "head tilted back 25" },
};

// Every joint at or below a joint, given each joint's parent.
std::vector<bool> subtree(const std::vector<int>& parents, int top) {
    std::vector<bool> inside(parents.size(), false);
    for (size_t j = 0; j < parents.size(); j++) {
        for (int a = (int)j; a >= 0; a = parents[a]) {
            if (a == top) {
                inside[j] = true;
                break;
            }
        }
    }
    return inside;
}
}

void compareDeformation(const autorig::Body& body, const autorig::Result& ours, const KeyRig& key) {
    if (key.weights.size() != body.vertices.size() || ours.weights.size() != body.vertices.size()) {
        return;
    }
    const autorig::StandardSkeleton& standard = autorig::StandardSkeleton::get();
    std::vector<int> ourParents;
    for (const autorig::RiggedJoint& joint : ours.joints) {
        ourParents.push_back(joint.parent);
    }
    fprintf(stdout, "deformation, both rigs turned the same way (cm apart over the vertices that move):\n");
    for (const TestPose& pose : TEST_POSES) {
        const int ourJoint = standard.indexOf(pose.joint);
        int keyJoint = -1;
        for (size_t j = 0; j < key.names.size() && keyJoint < 0; j++) {
            if (standardIndexOf(key.names[j]) == ourJoint) {
                keyJoint = (int)j;
            }
        }
        if (ourJoint < 0 || keyJoint < 0) {
            fprintf(stdout, "  %-24s not in the answer key\n", pose.label);
            continue;
        }
        const std::vector<bool> inOurs = subtree(ourParents, ourJoint);
        const std::vector<bool> inKey = subtree(key.parents, keyJoint);
        const glm::quat turn = glm::angleAxis(glm::radians(pose.degrees), glm::normalize(pose.axis));
        const glm::vec3 ourPivot = ours.joints[ourJoint].position;
        const glm::vec3 keyPivot = key.positions[keyJoint];
        double sum2 = 0.0;
        float worst = 0.0f;
        size_t moved = 0;
        for (size_t v = 0; v < body.vertices.size(); v++) {
            const glm::vec3& p = body.vertices[v];
            float ourShare = 0.0f;
            for (int k = 0; k < 4; k++) {
                if (inOurs[ours.weights[v].joints[k]]) {
                    ourShare += ours.weights[v].weights[k];
                }
            }
            float keyShare = 0.0f;
            for (const auto& w : key.weights[v]) {
                if (w.first >= 0 && w.first < (int)inKey.size() && inKey[w.first]) {
                    keyShare += w.second;
                }
            }
            const glm::vec3 ourMoved = p + ourShare * (turn * (p - ourPivot) + ourPivot - p);
            const glm::vec3 keyMoved = p + keyShare * (turn * (p - keyPivot) + keyPivot - p);
            if (glm::distance(ourMoved, p) > 0.001f || glm::distance(keyMoved, p) > 0.001f) {
                const float apart = glm::distance(ourMoved, keyMoved);
                sum2 += (double)apart * apart;
                worst = std::max(worst, apart);
                moved++;
            }
        }
        fprintf(stdout, "  %-24s %7zu vertices move, %5.2f cm apart (root mean square), %5.1f cm at worst\n", pose.label,
                moved, moved ? std::sqrt(sum2 / moved) * 100.0 : 0.0, worst * 100.0f);
    }
}

bool checkWritten(const QString& fbxPath, const autorig::Body& body, const autorig::Result& ours) {
    QFile file(fbxPath);
    if (!file.open(QIODevice::ReadOnly)) {
        fprintf(stdout, "read back: cannot open %s\n", qPrintable(fbxPath));
        return false;
    }
    HFMModel::Pointer model = FBXSerializer().read(file.readAll(), hifi::VariantMultiHash(), QUrl::fromLocalFile(fbxPath));
    if (!model) {
        fprintf(stdout, "read back: the engine could not read it\n");
        return false;
    }
    QStringList problems;
    int roots = 0;
    for (const HFMJoint& joint : model->joints) {
        if (joint.parentIndex == -1 && joint.isSkeletonJoint) {
            roots++;
        }
    }
    if (roots != 1) {
        problems << QString("%1 skeleton roots (the Avatar Doctor wants one)").arg(roots);
    }
    if (model->joints.size() > 256) {
        problems << QString("%1 joints (the Avatar Doctor allows 256)").arg(model->joints.size());
    }
    // the reader leaves the model's name table to a later loading step, so match the names here
    std::map<QString, int> byName;
    for (int j = 0; j < (int)model->joints.size(); j++) {
        byName.emplace(model->joints[j].name, j);
    }
    auto indexOf = [&](const QString& name) {
        auto found = byName.find(name);
        return found == byName.end() ? -1 : found->second;
    };
    float worstPlace = 0.0f, worstAngle = 0.0f;
    QString worstPlaceJoint, worstAngleJoint;
    for (const autorig::RiggedJoint& joint : ours.joints) {
        const int index = indexOf(joint.name);
        if (index < 0) {
            problems << joint.name + " is missing";
            continue;
        }
        const glm::mat4& global = model->joints[index].globalTransform;
        const float place = glm::distance(extractTranslation(global), joint.position);
        const glm::quat read = glm::normalize(extractRotation(global));
        const float angle = glm::degrees(2.0f * std::acos(std::min(1.0f, std::fabs(glm::dot(read, joint.rotation)))));
        if (place > worstPlace) {
            worstPlace = place;
            worstPlaceJoint = joint.name;
        }
        if (angle > worstAngle) {
            worstAngle = angle;
            worstAngleJoint = joint.name;
        }
    }
    const int hips = indexOf("Hips");
    if (hips >= 0 && extractTranslation(model->joints[hips].globalTransform).y < 0.1f) {
        problems << "the hips are on the ground";
    }
    size_t vertices = 0;
    for (const hfm::Mesh& mesh : model->meshes) {
        vertices += mesh.vertices.size();
    }
    fprintf(stdout, "read back: %zu joints, %d skeleton root, %zu meshes, %zu vertices (the body had %zu), %zu skins; "
                    "joints within %.2f mm (%s) and %.3f degrees (%s)\n",
            model->joints.size(), roots, model->meshes.size(), vertices, body.vertices.size(), model->skinDeformers.size(),
            worstPlace * 1000.0f, qPrintable(worstPlaceJoint), worstAngle, qPrintable(worstAngleJoint));
    if (worstPlace > 0.001f) {
        problems << QString("joints moved by up to %1 mm").arg(worstPlace * 1000.0f, 0, 'f', 2);
    }
    if (worstAngle > 0.1f) {
        problems << QString("joint axes turned by up to %1 degrees").arg(worstAngle, 0, 'f', 2);
    }
    for (const QString& problem : problems) {
        fprintf(stdout, "  PROBLEM: %s\n", qPrintable(problem));
    }
    return problems.isEmpty();
}
