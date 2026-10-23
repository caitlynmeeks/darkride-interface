//
//  VRMReader.cpp
//  libraries/fbx/src
//
//  Created on 2026-10-19 for Darkride's avatar support.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "VRMReader.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QtEndian>

#include <cmath>
#include <functional>

#include <glm/glm.hpp>

namespace {

// VRM's humanoid bones (1.0's names; 0.x's are the same but for the thumb's), Darkride's names for them
const QHash<QString, QString>& BONE_NAMES() {
    static const QHash<QString, QString> names = [] {
        QHash<QString, QString> result {
            { "hips", "Hips" }, { "spine", "Spine" }, { "neck", "Neck" }, { "head", "Head" },
            { "leftEye", "LeftEye" }, { "rightEye", "RightEye" },
        };
        for (const char* side : { "left", "right" }) {
            const QString vrm(side);
            const QString darkride = vrm == "left" ? "Left" : "Right";
            result.insert(vrm + "UpperLeg", darkride + "UpLeg");
            result.insert(vrm + "LowerLeg", darkride + "Leg");
            result.insert(vrm + "Foot", darkride + "Foot");
            result.insert(vrm + "Toes", darkride + "ToeBase");
            result.insert(vrm + "Shoulder", darkride + "Shoulder");
            result.insert(vrm + "UpperArm", darkride + "Arm");
            result.insert(vrm + "LowerArm", darkride + "ForeArm");
            result.insert(vrm + "Hand", darkride + "Hand");
            // 1.0's thumb starts at its metacarpal, 0.x's at its "proximal", which is the same bone
            result.insert(vrm + "ThumbMetacarpal", darkride + "HandThumb1");
            result.insert(vrm + "ThumbProximal", darkride + "HandThumb2");
            result.insert(vrm + "ThumbDistal", darkride + "HandThumb3");
            for (const auto& finger : { std::make_pair("Index", "Index"), std::make_pair("Middle", "Middle"),
                                        std::make_pair("Ring", "Ring"), std::make_pair("Little", "Pinky") }) {
                result.insert(vrm + finger.first + "Proximal", darkride + "Hand" + finger.second + "1");
                result.insert(vrm + finger.first + "Intermediate", darkride + "Hand" + finger.second + "2");
                result.insert(vrm + finger.first + "Distal", darkride + "Hand" + finger.second + "3");
            }
        }
        return result;
    }();
    return names;
}

// 0.x's expression presets by 1.0's names
QString presetName(const QString& name) {
    static const QHash<QString, QString> PRESETS_0 {
        { "a", "aa" }, { "i", "ih" }, { "u", "ou" }, { "e", "ee" }, { "o", "oh" },
        { "blink_l", "blinkLeft" }, { "blink_r", "blinkRight" },
        { "joy", "happy" }, { "sorrow", "sad" }, { "fun", "relaxed" },
    };
    return PRESETS_0.value(name.toLower(), name);
}

}

VRMInfo readVRM(const QByteArray& glb) {
    VRMInfo info;
    const int HEADER = 12;
    const int CHUNK_HEADER = 8;
    if (glb.size() < HEADER + CHUNK_HEADER || !glb.startsWith("glTF") || glb.mid(HEADER + 4, 4) != "JSON") {
        return info;
    }
    const quint32 jsonLength = qFromLittleEndian<quint32>(glb.constData() + HEADER);
    if (jsonLength > (quint32)(glb.size() - HEADER - CHUNK_HEADER)) {
        return info;
    }
    const QJsonObject gltf = QJsonDocument::fromJson(glb.mid(HEADER + CHUNK_HEADER, (int)jsonLength)).object();
    const QJsonObject extensions = gltf["extensions"].toObject();
    const QJsonObject vrm0 = extensions["VRM"].toObject();
    const QJsonObject vrm1 = extensions["VRMC_vrm"].toObject();
    if (!vrm1.isEmpty()) {
        info.version = 1;
    } else if (!vrm0.isEmpty()) {
        info.version = 0;
    } else {
        return info;
    }

    const QJsonArray nodes = gltf["nodes"].toArray();
    const QJsonArray meshes = gltf["meshes"].toArray();
    auto targetName = [&](int mesh, int index) {
        const QJsonObject meshObject = meshes.at(mesh).toObject();
        QJsonArray names = meshObject["extras"].toObject()["targetNames"].toArray();
        if (names.isEmpty()) {
            names = meshObject["primitives"].toArray().at(0).toObject()["extras"].toObject()["targetNames"].toArray();
        }
        return names.at(index).toString();
    };

    // humanoid bones
    QHash<QString, int> bones;
    if (info.version == 1) {
        const QJsonObject humanBones = vrm1["humanoid"].toObject()["humanBones"].toObject();
        for (auto itr = humanBones.begin(); itr != humanBones.end(); itr++) {
            bones.insert(itr.key(), itr.value().toObject()["node"].toInt(-1));
        }
    } else {
        for (const QJsonValue& bone : vrm0["humanoid"].toObject()["humanBones"].toArray()) {
            bones.insert(bone.toObject()["bone"].toString(), bone.toObject()["node"].toInt(-1));
        }
        // 0.x's thumb bones are proximal, intermediate and distal; 1.0's, the same bones, metacarpal, proximal and distal
        for (const char* side : { "left", "right" }) {
            const QString thumb = QString(side) + "Thumb";
            const int proximal = bones.contains(thumb + "Proximal") ? bones.take(thumb + "Proximal") : -1;
            const int intermediate = bones.contains(thumb + "Intermediate") ? bones.take(thumb + "Intermediate") : -1;
            if (proximal >= 0) {
                bones.insert(thumb + "Metacarpal", proximal);
            }
            if (intermediate >= 0) {
                bones.insert(thumb + "Proximal", intermediate);
            }
        }
    }
    for (auto itr = bones.begin(); itr != bones.end(); itr++) {
        QString name = BONE_NAMES().value(itr.key());
        // the chest is the top of the spine (Spine2), where the arms and neck join, unless there's an upper chest
        if (itr.key() == "chest") {
            name = bones.contains("upperChest") ? "Spine1" : "Spine2";
        } else if (itr.key() == "upperChest") {
            name = "Spine2";
        }
        const QString nodeName = nodes.at(itr.value()).toObject()["name"].toString();
        if (!name.isEmpty() && !nodeName.isEmpty()) {
            info.jointNames.insert(nodeName, name);
        }
    }

    // expressions, by 1.0's preset names
    QHash<QString, QVector<BlendshapeSource>> expressions;
    if (info.version == 1) {
        const QJsonObject presets = vrm1["expressions"].toObject()["preset"].toObject();
        for (auto itr = presets.begin(); itr != presets.end(); itr++) {
            for (const QJsonValue& bind : itr.value().toObject()["morphTargetBinds"].toArray()) {
                const int mesh = nodes.at(bind.toObject()["node"].toInt(-1)).toObject()["mesh"].toInt(-1);
                const int index = bind.toObject()["index"].toInt(-1);
                const QString name = targetName(mesh, index);
                if (!name.isEmpty() || index >= 0) {
                    expressions[itr.key()].append({ name, (float)bind.toObject()["weight"].toDouble(1.0),
                                                    meshes.at(mesh).toObject()["name"].toString(), index });
                }
            }
        }
    } else {
        for (const QJsonValue& group : vrm0["blendShapeMaster"].toObject()["blendShapeGroups"].toArray()) {
            const QString preset = presetName(group.toObject()["presetName"].toString());
            for (const QJsonValue& bind : group.toObject()["binds"].toArray()) {
                const int mesh = bind.toObject()["mesh"].toInt(-1);
                const int index = bind.toObject()["index"].toInt(-1);
                const QString name = targetName(mesh, index);
                if (!name.isEmpty() || index >= 0) {
                    expressions[preset].append({ name, (float)bind.toObject()["weight"].toDouble(100.0) / 100.0f,
                                                 meshes.at(mesh).toObject()["name"].toString(), index });
                }
            }
        }
    }
    auto use = [&](Blendshapes blendshape, const QString& preset, float weight) {
        for (BlendshapeSource source : expressions.value(preset)) {
            source.weight *= weight;
            info.blendshapes[(int)blendshape].append(source);
        }
    };
    // What voice drives. Head.cpp opens JawOpen to 0.35 at the loudest and nearer 0.1 in talking, as High Fidelity's
    // avatars' jaw shapes were made for, so the "aa" viseme, a mouth wide open at 1, is driven half as hard again (three
    // times was too much in talking); and the "ou" less, as with the jaw barely open it pursed the lips oddly (Seed-san)
    use(Blendshapes::JawOpen, "aa", 1.5f);
    use(Blendshapes::LipsFunnel, "ou", 0.5f);
    if (expressions.contains("blinkLeft") && expressions.contains("blinkRight")) {
        use(Blendshapes::EyeBlink_L, "blinkLeft", 1.0f);
        use(Blendshapes::EyeBlink_R, "blinkRight", 1.0f);
    } else {
        // both eyes in one, so half each: blinks close both together
        use(Blendshapes::EyeBlink_L, "blink", 0.5f);
        use(Blendshapes::EyeBlink_R, "blink", 0.5f);
    }
    // Eyes that look by expression: up and down for both eyes at once, so half on each eye's; looking to its left is the
    // left eye's out and the right eye's in
    use(Blendshapes::EyeUp_L, "lookUp", 0.5f);
    use(Blendshapes::EyeUp_R, "lookUp", 0.5f);
    use(Blendshapes::EyeDown_L, "lookDown", 0.5f);
    use(Blendshapes::EyeDown_R, "lookDown", 0.5f);
    use(Blendshapes::EyeOut_L, "lookLeft", 0.5f);
    use(Blendshapes::EyeIn_R, "lookLeft", 0.5f);
    use(Blendshapes::EyeIn_L, "lookRight", 0.5f);
    use(Blendshapes::EyeOut_R, "lookRight", 0.5f);

    // Spring bones as Flow threads. VRM moves a joint's tail each frame by (1 - drag) of its last move, plus gravity
    // power and stiffness (towards its rest direction) times the frame's time; Flow (Flow.h, per 1/60 s step) by damping
    // times its last move, gravity times delta squared (0.55^2, Flow's default delta) per step squared, and, towards
    // its rest, stiffness of the way there. At 60 frames a second, and taking a hair joint as about 8 cm long:
    const float FRAME = 1.0f / 60.0f;
    const float DELTA_SQUARED = 0.55f * 0.55f;
    const float JOINT_LENGTH = 0.08f;
    auto nodeName = [&](int node) { return nodes.at(node).toObject()["name"].toString(); };
    auto children = [&](int node) {
        QVector<int> result;
        for (const QJsonValue& child : nodes.at(node).toObject()["children"].toArray()) {
            result.append(child.toInt());
        }
        return result;
    };
    int groupCount = 0;
    auto addSpring = [&](const QVector<int>& springNodes, float stiffness, float drag, float gravityPower, float gravityY, float radius) {
        const QString group = QString("vrm%1").arg(groupCount);
        int added = 0;
        for (int node : springNodes) {
            const QString name = nodeName(node);
            if (!name.isEmpty() && !info.jointNames.contains(name) && !info.flowJointNames.contains(name)) {
                info.flowJointNames.insert(name, "flow_" + group + "_" + name);
                added++;
            }
        }
        if (added == 0) {
            return;
        }
        groupCount++;
        const float pull = glm::clamp(stiffness * FRAME / JOINT_LENGTH, 0.0f, 1.0f);
        QVariantMap physics;
        physics["active"] = true;
        physics["stiffness"] = pull;
        // VRoid exports next to no drag, which Flow, keeping all of a move each frame, would never stop swinging from
        const float MOST_DAMPING = 0.9f;
        physics["damping"] = glm::clamp(1.0f - drag, 0.0f, MOST_DAMPING);
        physics["gravity"] = gravityPower * gravityY * FRAME / DELTA_SQUARED;
        physics["radius"] = glm::max(radius, 0.005f);
        // a VRM collides a joint's tail, not its whole bone
        physics["tipCollision"] = true;
        info.flowPhysics.insert(group, physics);
    };
    // a joint that branches stays as animated, and each branch is a thread of its own, as Flow follows one child. A file
    // can loop or repeat a child, so a node is taken once and a chain no deeper than any real rig
    const int MOST_CHAIN_DEPTH = 256;
    std::function<void(int, QVector<int>&, QSet<int>&, int)> collectChain = [&](int node, QVector<int>& chain, QSet<int>& visited, int depth) {
        if (node < 0 || node >= nodes.size() || depth > MOST_CHAIN_DEPTH || visited.contains(node)) {
            return;
        }
        visited.insert(node);
        const QVector<int> nodeChildren = children(node);
        if (nodeChildren.size() >= 2) {
            for (int child : nodeChildren) {
                QVector<int> branch;
                collectChain(child, branch, visited, depth + 1);
                chain += branch;
            }
            return;
        }
        chain.append(node);
        if (nodeChildren.size() == 1) {
            collectChain(nodeChildren[0], chain, visited, depth + 1);
        }
    };
    // a node's collider, kept if it's the largest so far (Flow has one sphere a joint)
    QHash<int, std::pair<glm::vec3, float>> colliders;
    auto addCollider = [&](int node, const glm::vec3& offset, float radius) {
        if (node >= 0 && (!colliders.contains(node) || colliders[node].second < radius)) {
            colliders[node] = { offset, radius };
        }
    };
    auto vectorOf = [](const QJsonValue& value) {
        if (value.isArray()) {
            const QJsonArray array = value.toArray();
            return glm::vec3(array.at(0).toDouble(), array.at(1).toDouble(), array.at(2).toDouble());
        }
        const QJsonObject object = value.toObject();
        return glm::vec3(object["x"].toDouble(), object["y"].toDouble(), object["z"].toDouble());
    };
    if (info.version == 1) {
        const QJsonObject springBone = extensions["VRMC_springBone"].toObject();
        for (const QJsonValue& spring : springBone["springs"].toArray()) {
            const QJsonArray joints = spring.toObject()["joints"].toArray();
            QVector<int> springNodes;
            float stiffness = 0.0f, drag = 0.0f, gravityPower = 0.0f, gravityY = 0.0f, radius = 0.0f;
            // a joint's settings are for the segment to the next joint, so the last one's go unused
            const int counted = glm::max(joints.size() - 1, 1);
            for (int j = 0; j < joints.size(); j++) {
                const QJsonObject joint = joints.at(j).toObject();
                springNodes.append(joint["node"].toInt(-1));
                if (j < counted) {
                    stiffness += (float)joint["stiffness"].toDouble(1.0) / counted;
                    drag += (float)joint["dragForce"].toDouble(0.5) / counted;
                    gravityPower += (float)joint["gravityPower"].toDouble(0.0) / counted;
                    gravityY += (joint.contains("gravityDir") ? vectorOf(joint["gravityDir"]).y : -1.0f) / counted;
                    radius += (float)joint["hitRadius"].toDouble(0.0) / counted;
                }
            }
            addSpring(springNodes, stiffness, drag, gravityPower, gravityY, radius);
        }
        for (const QJsonValue& collider : springBone["colliders"].toArray()) {
            const int node = collider.toObject()["node"].toInt(-1);
            const QJsonObject shape = collider.toObject()["shape"].toObject();
            if (shape.contains("sphere")) {
                const QJsonObject sphere = shape["sphere"].toObject();
                addCollider(node, vectorOf(sphere["offset"]), (float)sphere["radius"].toDouble());
            } else if (shape.contains("capsule")) {
                // the sphere at its middle
                const QJsonObject capsule = shape["capsule"].toObject();
                addCollider(node, 0.5f * (vectorOf(capsule["offset"]) + vectorOf(capsule["tail"])), (float)capsule["radius"].toDouble());
            }
        }
    } else {
        const QJsonObject secondary = vrm0["secondaryAnimation"].toObject();
        for (const QJsonValue& boneGroup : secondary["boneGroups"].toArray()) {
            const QJsonObject group = boneGroup.toObject();
            QVector<int> springNodes;
            QSet<int> visited;
            for (const QJsonValue& bone : group["bones"].toArray()) {
                collectChain(bone.toInt(-1), springNodes, visited, 0);
            }
            const float gravityY = group.contains("gravityDir") ? vectorOf(group["gravityDir"]).y : -1.0f;
            // 0.x's misspelling
            addSpring(springNodes, (float)group["stiffiness"].toDouble(1.0), (float)group["dragForce"].toDouble(0.4),
                      (float)group["gravityPower"].toDouble(0.0), gravityY, (float)group["hitRadius"].toDouble(0.02));
        }
        for (const QJsonValue& colliderGroup : secondary["colliderGroups"].toArray()) {
            const int node = colliderGroup.toObject()["node"].toInt(-1);
            for (const QJsonValue& collider : colliderGroup.toObject()["colliders"].toArray()) {
                // 0.x's offsets are Unity's, whose z is glTF's turned round
                glm::vec3 offset = vectorOf(collider.toObject()["offset"]);
                offset.z = -offset.z;
                addCollider(node, offset, (float)collider.toObject()["radius"].toDouble());
            }
        }
    }
    // Node constraints (1.0): twist bones that follow an arm's twist, parts that aim at others
    auto finalName = [&](int node) {
        const QString name = nodeName(node);
        return info.jointNames.value(name, info.flowJointNames.value(name, name));
    };
    for (int node = 0; node < nodes.size(); node++) {
        const QJsonObject constraint =
            nodes.at(node).toObject()["extensions"].toObject()["VRMC_node_constraint"].toObject()["constraint"].toObject();
        if (constraint.isEmpty()) {
            continue;
        }
        hfm::NodeConstraint nodeConstraint;
        QJsonObject settings;
        if (constraint.contains("roll")) {
            settings = constraint["roll"].toObject();
            nodeConstraint.type = hfm::NodeConstraint::Type::Roll;
            const QString axis = settings["rollAxis"].toString("X");
            nodeConstraint.axis = axis == "Y" ? glm::vec3(0.0f, 1.0f, 0.0f) : axis == "Z" ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        } else if (constraint.contains("aim")) {
            settings = constraint["aim"].toObject();
            nodeConstraint.type = hfm::NodeConstraint::Type::Aim;
            const QString axis = settings["aimAxis"].toString("PositiveX");
            const float sign = axis.startsWith("Negative") ? -1.0f : 1.0f;
            nodeConstraint.axis = sign * (axis.endsWith("Y") ? glm::vec3(0.0f, 1.0f, 0.0f) : axis.endsWith("Z") ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                                                                 : glm::vec3(1.0f, 0.0f, 0.0f));
        } else if (constraint.contains("rotation")) {
            settings = constraint["rotation"].toObject();
            nodeConstraint.type = hfm::NodeConstraint::Type::Rotation;
        } else {
            continue;
        }
        nodeConstraint.joint = finalName(node);
        nodeConstraint.source = finalName(settings["source"].toInt(-1));
        nodeConstraint.weight = (float)settings["weight"].toDouble(1.0);
        if (!nodeConstraint.joint.isEmpty() && !nodeConstraint.source.isEmpty()) {
            info.nodeConstraints.push_back(nodeConstraint);
        }
    }

    for (auto itr = colliders.begin(); itr != colliders.end(); itr++) {
        const QString name = nodeName(itr.key());
        const QString jointName = info.jointNames.value(name, info.flowJointNames.value(name, name));
        if (jointName.isEmpty()) {
            continue;
        }
        QVariantMap offset;
        offset["x"] = itr.value().first.x;
        offset["y"] = itr.value().first.y;
        offset["z"] = itr.value().first.z;
        QVariantMap collision;
        collision["radius"] = itr.value().second;
        collision["offset"] = offset;
        info.flowCollisions.insert(jointName, collision);
    }

    // How far the eyes turn: each range map turns up to inputMaxValue (1.0) or xRange (0.x) degrees of gaze into up to
    // outputScale or yRange degrees of eye; one scale for all, and the smallest range, which keeps every way inside the eye
    {
        QString type;
        QVector<std::pair<float, float>> ranges;  // input, output
        if (info.version == 1) {
            const QJsonObject lookAt = vrm1["lookAt"].toObject();
            type = lookAt["type"].toString("bone");
            for (const char* key : { "rangeMapHorizontalInner", "rangeMapHorizontalOuter", "rangeMapVerticalDown", "rangeMapVerticalUp" }) {
                if (lookAt.contains(key)) {
                    const QJsonObject range = lookAt[key].toObject();
                    ranges.append({ (float)range["inputMaxValue"].toDouble(90.0), (float)range["outputScale"].toDouble(10.0) });
                }
            }
        } else {
            const QJsonObject firstPerson = vrm0["firstPerson"].toObject();
            type = firstPerson["lookAtTypeName"].toString("Bone").toLower();
            for (const char* key : { "lookAtHorizontalInner", "lookAtHorizontalOuter", "lookAtVerticalDown", "lookAtVerticalUp" }) {
                if (firstPerson.contains(key)) {
                    const QJsonObject range = firstPerson[key].toObject();
                    ranges.append({ (float)range["xRange"].toDouble(90.0), (float)range["yRange"].toDouble(10.0) });
                }
            }
        }
        if (type != "bone") {
            // the eyes look by expression (Seed-san): lookUp at the gaze's turn up over the range's input, times its output
            info.eyeLookScale = 0.0f;
            float input = 0.0f, output = 0.0f;
            for (const auto& range : ranges) {
                input += range.first;
                output += range.second;
            }
            info.eyeLookExpressionScale = input > 0.0f ? output / input : 1.0f / 90.0f;
        } else if (!ranges.isEmpty()) {
            float input = 0.0f, output = 0.0f, smallest = ranges.front().second;
            for (const auto& range : ranges) {
                input += range.first;
                output += range.second;
                smallest = glm::min(smallest, range.second);
            }
            info.eyeLookScale = input > 0.0f ? output / input : 1.0f;
            info.eyeLookMaxAngle = smallest;
        }
    }

    // MToon materials' toon shading
    {
        const QJsonArray materials = gltf["materials"].toArray();
        QHash<QString, int> nameCounts;
        for (const QJsonValue& material : materials) {
            nameCounts[material.toObject()["name"].toString()]++;
        }
        auto add = [&](int index, const VRMToonMaterial& toon) {
            info.toonMaterials[index] = toon;
            const QString name = materials.at(index).toObject()["name"].toString();
            if (!name.isEmpty() && nameCounts.value(name) == 1) {
                info.toonMaterialsByName[name] = toon;
            }
        };
        if (info.version == 1) {
            for (int i = 0; i < materials.size(); i++) {
                const QJsonObject mtoon = materials.at(i).toObject()["extensions"].toObject()["VRMC_materials_mtoon"].toObject();
                if (mtoon.isEmpty()) {
                    continue;
                }
                VRMToonMaterial toon;
                const QJsonArray shade = mtoon["shadeColorFactor"].toArray();
                toon.shadeColor = glm::vec3(shade.at(0).toDouble(0.0), shade.at(1).toDouble(0.0), shade.at(2).toDouble(0.0));
                toon.shadingShift = (float)mtoon["shadingShiftFactor"].toDouble(0.0);
                toon.shadingToony = (float)mtoon["shadingToonyFactor"].toDouble(0.9);
                toon.giEqualization = (float)mtoon["giEqualizationFactor"].toDouble(0.9);
                add(i, toon);
            }
        } else {
            // 0.x's material properties are in the materials' order; its colours are sRGB, and its shade shift and
            // toony step differently, converted as three-vrm's VRMMaterialsV0CompatPlugin does
            const QJsonArray properties = vrm0["materialProperties"].toArray();
            for (int i = 0; i < properties.size() && i < materials.size(); i++) {
                const QJsonObject property = properties.at(i).toObject();
                if (property["shader"].toString() != "VRM/MToon") {
                    continue;
                }
                const QJsonObject floats = property["floatProperties"].toObject();
                const QJsonArray shade = property["vectorProperties"].toObject()["_ShadeColor"].toArray();
                auto toLinear = [](double value) {
                    return (float)(value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4));
                };
                VRMToonMaterial toon;
                toon.shadeColor = glm::vec3(toLinear(shade.at(0).toDouble(0.97)), toLinear(shade.at(1).toDouble(0.81)),
                                            toLinear(shade.at(2).toDouble(0.86)));
                const float shadeShift = (float)floats["_ShadeShift"].toDouble(0.0);
                const float shadeToony = (float)floats["_ShadeToony"].toDouble(0.9);
                toon.shadingToony = glm::mix(shadeToony, 1.0f, 0.5f + 0.5f * shadeShift);
                toon.shadingShift = -shadeShift - (1.0f - toon.shadingToony);
                toon.giEqualization = 1.0f - (float)floats["_IndirectLightIntensity"].toDouble(0.1);
                add(i, toon);
            }
        }
    }

    // who made it, and on what terms
    if (info.version == 1) {
        const QJsonObject meta = vrm1["meta"].toObject();
        info.title = meta["name"].toString();
        QStringList authors;
        for (const QJsonValue& author : meta["authors"].toArray()) {
            authors << author.toString();
        }
        info.authors = authors.join(", ");
        info.licence = QString("avatar use %1, commercial use %2, %3").arg(meta["avatarPermission"].toString(),
            meta["commercialUsage"].toString(), meta["licenseUrl"].toString());
        info.onlyAuthorMayWear = meta["avatarPermission"].toString() != "everyone";
        // 1.0 allows redistribution only when it says so
        info.redistributionAllowed = meta["allowRedistribution"].toBool(false);
    } else {
        const QJsonObject meta = vrm0["meta"].toObject();
        info.title = meta["title"].toString();
        info.authors = meta["author"].toString();
        info.licence = QString("%1, avatar use %2, commercial use %3").arg(meta["licenseName"].toString(),
            meta["allowedUserName"].toString(), meta["commercialUssageName"].toString());
        info.onlyAuthorMayWear = meta["allowedUserName"].toString() != "Everyone";
        info.redistributionAllowed = meta["licenseName"].toString() != "Redistribution_Prohibited";
    }
    return info;
}
