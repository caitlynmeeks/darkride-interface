//
//  FstRoundTripTests.cpp
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "FstRoundTripTests.h"

#include <functional>

#include <glm/gtx/transform.hpp>

#include <BlendshapeConstants.h>
#include <FSTReader.h>
#include <FstBake.h>
#include <GLMHelpers.h>
#include <StandardSkeleton.h>
#include <model-baker/ParseFlowDataTask.h>
#include <model-baker/PrepareJointsTask.h>

QTEST_MAIN(FstRoundTripTests)

namespace {

// The default avatar's skeleton, its joints named by `rename` and their axes turned by `axes` (as Blender's are), as a
// serializer gives it: local translations and rotations, and the global transforms
HFMModel makeModel(const std::function<QString(const QString&)>& rename, const glm::quat& axes) {
    const autorig::StandardSkeleton& standard = autorig::StandardSkeleton::get();
    HFMModel model;
    for (int i = 0; i < standard.size(); i++) {
        const autorig::TemplateJoint& joint = standard.joint(i);
        const int parent = standard.parentIndex(i);
        const glm::vec3 position = joint.position * 0.01f;
        const glm::quat rotation = joint.rotation * axes;
        HFMJoint hfmJoint;
        hfmJoint.name = rename(joint.name);
        hfmJoint.parentIndex = parent;
        hfmJoint.isSkeletonJoint = true;
        if (parent >= 0) {
            const glm::vec3 parentPosition = standard.joint(parent).position * 0.01f;
            const glm::quat parentRotation = standard.joint(parent).rotation * axes;
            hfmJoint.translation = glm::inverse(parentRotation) * (position - parentPosition);
            hfmJoint.rotation = glm::inverse(parentRotation) * rotation;
        } else {
            hfmJoint.translation = position;
            hfmJoint.rotation = rotation;
        }
        hfmJoint.globalTransform = glm::translate(position) * glm::mat4_cast(rotation);
        model.joints.push_back(hfmJoint);
    }
    return model;
}

struct Baked {
    std::vector<hfm::Joint> joints;
    QMap<int, glm::quat> offsets;
    QHash<QString, int> jointIndices;
};

Baked bake(const std::vector<hfm::Joint>& joints, const hifi::VariantMultiHash& mapping) {
    PrepareJointsTask task;
    PrepareJointsTask::Input input;
    input.edit0() = joints;
    input.edit1() = mapping;
    PrepareJointsTask::Output output;
    task.run(nullptr, input, output);
    return { output.get0(), output.get1(), output.get2() };
}

// Wearing the model without an .fst, and with the .fst Import Avatar writes for it: the same names, offsets and
// colliders
void verifyRoundTrip(const HFMModel& model, bool isVRM) {
    hifi::VariantMultiHash bare;
    bare.insert("autoHumanoid", true);
    const Baked worn = bake(model.joints, bare);
    QVariantMap wornCollisions = model.flowData._collisionsConfig;
    turnFlowCollisionsToRigAxes(wornCollisions, worn.jointIndices, worn.offsets);

    QStringList lines, found;
    autorig::bakeFst(model, isVRM, lines, found);
    const hifi::VariantMultiHash mapping = FSTReader::readMapping(("name = test\nfilename = test.glb\n" + lines.join("\n")).toUtf8());
    const Baked baked = bake(model.joints, mapping);
    FlowData flowData;
    ParseFlowDataTask().run(nullptr, mapping, flowData);

    QCOMPARE(baked.joints.size(), worn.joints.size());
    for (size_t i = 0; i < worn.joints.size(); i++) {
        QCOMPARE(baked.joints[i].name, worn.joints[i].name);
    }
    QVERIFY(!worn.offsets.isEmpty());
    QCOMPARE(baked.offsets.keys(), worn.offsets.keys());
    for (int index : worn.offsets.keys()) {
        QVERIFY2(fabsf(glm::dot(baked.offsets[index], worn.offsets[index])) > 0.99999f,
                 qPrintable(worn.joints[index].name + "'s offsets differ"));
    }
    QCOMPARE(flowData._collisionsConfig.keys(), wornCollisions.keys());
    for (const QString& joint : wornCollisions.keys()) {
        const QVariantMap a = wornCollisions[joint].toMap()["offset"].toMap();
        const QVariantMap b = flowData._collisionsConfig[joint].toMap()["offset"].toMap();
        for (const char* axis : { "x", "y", "z" }) {
            QVERIFY2(fabsf(a[axis].toFloat() - b[axis].toFloat()) < 1.0e-5f, qPrintable(joint + "'s collider differs"));
        }
        QCOMPARE(flowData._collisionsConfig[joint].toMap()["radius"].toFloat(), wornCollisions[joint].toMap()["radius"].toFloat());
    }
}

}

// Mixamo's names behind a prefix and Blender's axes: jointMap lines for the names, jointRotationOffset2 for the axes
void FstRoundTripTests::testPrefixedNamesBlenderAxes() {
    const HFMModel model = makeModel([](const QString& name) { return "mixamorig:" + name; },
                                     glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    verifyRoundTrip(model, false);
}

// A VRM's: Darkride's names from its reader, its axes not Mixamo's, and colliders in its joints' own axes, which the .fst
// has in the rig's
void FstRoundTripTests::testVRMColliders() {
    HFMModel model = makeModel([](const QString& name) { return name; },
                               glm::angleAxis(glm::radians(-90.0f), glm::vec3(0.0f, 0.0f, 1.0f)));
    auto collider = [](float radius, const glm::vec3& offset) {
        return QVariantMap{ { "radius", radius },
                            { "offset", QVariantMap{ { "x", offset.x }, { "y", offset.y }, { "z", offset.z } } } };
    };
    model.flowData._collisionsConfig.insert("Head", collider(0.09f, glm::vec3(0.0f, 0.06f, 0.01f)));
    model.flowData._collisionsConfig.insert("LeftHand", collider(0.04f, glm::vec3(0.05f, 0.0f, 0.0f)));
    model.flowData._physicsConfig.insert("vrm0", QVariantMap{ { "active", true }, { "stiffness", 0.2f } });
    verifyRoundTrip(model, true);
}

// The face lines map the engine's blendshapes one to one, with the procedural face's gains, as wearing it does
void FstRoundTripTests::testFace() {
    const QStringList names { "jawOpen", "eyeBlinkLeft", "mouthSmileLeft", "EyeBlink_R" };
    const QStringList lines = autorig::bakeFaceLines(names);
    const hifi::VariantMultiHash mapping = FSTReader::readMapping(lines.join("\n").toUtf8());
    const hifi::VariantMultiHash bs = hifi::multiHashFromVariant(mapping.value("bs"));
    for (int i = 0; i < (int)Blendshapes::BlendshapeCount; i++) {
        const BlendshapeSource source = findBlendshapeSource(i, names);
        if (source.name.isEmpty() || source.name == BLENDSHAPE_NAMES[i]) {
            QVERIFY2(!bs.contains(BLENDSHAPE_NAMES[i]), BLENDSHAPE_NAMES[i]);
            continue;
        }
        const QVariantList line = bs.value(BLENDSHAPE_NAMES[i]).toList();
        QCOMPARE(line.value(0).toString(), source.name);
        QCOMPARE(line.value(1).toFloat(), source.weight);
    }
    HFMModel model;
    FSTReader::applyBlendshapeGains(mapping, model);
    QCOMPARE(model.proceduralBlendshapeGains.value((int)Blendshapes::JawOpen), 0.5f);
    QCOMPARE(model.proceduralBlendshapeGains.value((int)Blendshapes::MouthSmile_L), 0.7f);
    QCOMPARE(model.proceduralBlendshapeGains.value((int)Blendshapes::EyeBlink_L), 1.0f);
}

// Names from the model file go onto an .fst line only when the line can hold them (FSTReader::isLineValue): a joint or
// morph target whose name has a line break or '=' is left out, and the JSON lines keep such names, escaped, on their line
void FstRoundTripTests::testNamesStayOnTheirLine() {
    const QString breakName = "mixamorig:Hips\nextra = 1";
    const QString equalsName = "mixamorig:Spine=1";
    HFMModel model = makeModel([&](const QString& name) {
        return name == "Hips" ? breakName : name == "Spine" ? equalsName : "mixamorig:" + name;
    }, glm::angleAxis(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    const QString colliderName = "Head\n=extra";
    model.flowData._collisionsConfig.insert(colliderName, QVariantMap{ { "radius", 0.09f } });

    QStringList lines, found;
    autorig::bakeFst(model, false, lines, found);
    const QStringList faceNames { "Face\nextra = s.jawOpen", "Face=1.mouthSmileLeft", "eyeBlinkLeft" };
    lines << autorig::bakeFaceLines(faceNames);

    QVERIFY(!lines.isEmpty());
    for (const QString& line : lines) {
        QVERIFY2(FSTReader::isOneLine(line), qPrintable(line));
        QVERIFY2(!line.contains(breakName) && !line.contains(equalsName), qPrintable(line));
    }
    const hifi::VariantMultiHash mapping = FSTReader::readMapping(lines.join("\n").toUtf8());
    QVERIFY(!mapping.contains("extra"));
    // the names, and with them the offsets, worked out on each load: no jointMap line, as one of them can't be written
    QVERIFY(!mapping.contains("jointMap"));
    QVERIFY(found.join(" ").contains("worked out on each load"));
    // the face: only the shape whose name a line can hold
    const hifi::VariantMultiHash bs = hifi::multiHashFromVariant(mapping.value("bs"));
    QVERIFY(!bs.contains("JawOpen"));
    QVERIFY(!bs.contains("MouthSmile_L"));
    QCOMPARE(bs.value("EyeBlink_L").toList().value(0).toString(), QString("eyeBlinkLeft"));
    // the collider's name, escaped on its line, reads back as it was
    FlowData flowData;
    ParseFlowDataTask().run(nullptr, mapping, flowData);
    QCOMPARE(flowData._collisionsConfig.keys(), QStringList{ colliderName });
}
