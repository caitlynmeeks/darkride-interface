//
//  VRMReaderTests.cpp
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "VRMReaderTests.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QtEndian>

#include <AccountManager.h>
#include <AddressManager.h>
#include <AssimpSerializer.h>
#include <BlendshapeConstants.h>
#include <DependencyManager.h>
#include <NodeList.h>
#include <ResourceCache.h>
#include <ResourceManager.h>
#include <ResourceRequestObserver.h>
#include <StatTracker.h>
#include <VRMReader.h>

QTEST_MAIN(VRMReaderTests)

namespace {

// A glTF binary: the header, the JSON chunk and, if there is one, the binary chunk
QByteArray makeGlb(const QJsonObject& gltf, QByteArray bin = QByteArray()) {
    QByteArray json = QJsonDocument(gltf).toJson(QJsonDocument::Compact);
    while (json.size() % 4 != 0) {
        json.append(' ');
    }
    while (bin.size() % 4 != 0) {
        bin.append('\0');
    }
    QByteArray glb;
    auto appendUint32 = [&glb](uint32_t value) {
        value = qToLittleEndian(value);
        glb.append((const char*)&value, sizeof(value));
    };
    appendUint32(0x46546C67);  // "glTF"
    appendUint32(2);
    appendUint32(12 + 8 + json.size() + (bin.isEmpty() ? 0 : 8 + bin.size()));
    appendUint32(json.size());
    appendUint32(0x4E4F534A);  // "JSON"
    glb.append(json);
    if (!bin.isEmpty()) {
        appendUint32(bin.size());
        appendUint32(0x004E4942);  // "BIN"
        glb.append(bin);
    }
    return glb;
}

QJsonArray namedNodes(const QStringList& names) {
    QJsonArray nodes;
    for (const QString& name : names) {
        nodes.append(QJsonObject{ { "name", name } });
    }
    return nodes;
}

QJsonObject vrm0(const QJsonObject& vrm, const QJsonArray& nodes, const QJsonArray& meshes = QJsonArray()) {
    return QJsonObject{ { "asset", QJsonObject{ { "version", "2.0" } } }, { "nodes", nodes }, { "meshes", meshes },
                        { "extensions", QJsonObject{ { "VRM", vrm } } } };
}

QJsonObject vrm1(const QJsonObject& vrm, const QJsonArray& nodes, const QJsonArray& meshes = QJsonArray()) {
    return QJsonObject{ { "asset", QJsonObject{ { "version", "2.0" } } }, { "nodes", nodes }, { "meshes", meshes },
                        { "extensions", QJsonObject{ { "VRMC_vrm", vrm } } } };
}

QJsonObject bone0(const QString& bone, int node) {
    return QJsonObject{ { "bone", bone }, { "node", node } };
}

QJsonObject bind0(int mesh, int index, double weight) {
    return QJsonObject{ { "mesh", mesh }, { "index", index }, { "weight", weight } };
}

QJsonObject group0(const QString& preset, const QJsonArray& binds) {
    return QJsonObject{ { "presetName", preset }, { "binds", binds } };
}

QJsonObject faceMesh(const QString& name, const QStringList& targetNames) {
    return QJsonObject{ { "name", name }, { "extras", QJsonObject{ { "targetNames", QJsonArray::fromStringList(targetNames) } } } };
}

// one source, its name and weight
void verifySource(const VRMInfo& info, Blendshapes blendshape, const QString& name, float weight) {
    const QVector<BlendshapeSource> sources = info.blendshapes.value((int)blendshape);
    QVERIFY2(sources.size() == 1, qPrintable(QString("%1 has %2 sources").arg(BLENDSHAPE_NAMES[(int)blendshape]).arg(sources.size())));
    QCOMPARE(sources[0].name, name);
    QVERIFY2(std::abs(sources[0].weight - weight) < 0.001f,
             qPrintable(QString("%1 at %2, not %3").arg(BLENDSHAPE_NAMES[(int)blendshape]).arg(sources[0].weight).arg(weight)));
}

}

void VRMReaderTests::initTestCase() {
    // as AssimpSkinTests does: ResourceManager (which the serializer asks to normalize the model's URL) needs a NodeList
    DependencyManager::registerInheritance<LimitedNodeList, NodeList>();
    DependencyManager::set<AccountManager>();
    DependencyManager::set<AddressManager>();
    DependencyManager::set<NodeList>(NodeType::Agent);
    DependencyManager::set<ResourceManager>();
    DependencyManager::set<ResourceRequestObserver>();
    DependencyManager::set<ResourceCacheSharedItems>();
    DependencyManager::set<StatTracker>();
}

void VRMReaderTests::cleanupTestCase() {
    DependencyManager::get<ResourceManager>()->cleanup();
}

// A glTF without a VRM extension, or something that isn't a glTF binary, isn't a VRM
void VRMReaderTests::testNotVRM() {
    QVERIFY(!readVRM(makeGlb(QJsonObject{ { "asset", QJsonObject{ { "version", "2.0" } } } })).isVRM());
    QVERIFY(!readVRM(QByteArray("{\"asset\":{\"version\":\"2.0\"}}")).isVRM());
    QVERIFY(!readVRM(QByteArray()).isVRM());
    // a JSON chunk longer than the file
    QByteArray truncated = makeGlb(vrm0(QJsonObject{ { "humanoid", QJsonObject() } }, QJsonArray()));
    truncated.chop(8);
    QVERIFY(!readVRM(truncated).isVRM());
}

// 0.x: its thumbs start at their "proximal", which is Darkride's Thumb1; a chest with no upper chest is the Spine2
void VRMReaderTests::testHumanoid0() {
    const QJsonArray bones{ bone0("hips", 0), bone0("chest", 1), bone0("leftThumbProximal", 2),
                            bone0("leftThumbIntermediate", 3), bone0("leftThumbDistal", 4), bone0("leftUpperArm", 5) };
    const VRMInfo info = readVRM(makeGlb(vrm0(QJsonObject{ { "humanoid", QJsonObject{ { "humanBones", bones } } } },
        namedNodes({ "J_Bip_C_Hips", "J_Bip_C_Chest", "J_Bip_L_Thumb1", "J_Bip_L_Thumb2", "J_Bip_L_Thumb3", "J_Bip_L_UpperArm" }))));
    QCOMPARE(info.version, 0);
    QCOMPARE(info.jointNames.value("J_Bip_C_Hips"), QString("Hips"));
    QCOMPARE(info.jointNames.value("J_Bip_C_Chest"), QString("Spine2"));
    QCOMPARE(info.jointNames.value("J_Bip_L_Thumb1"), QString("LeftHandThumb1"));
    QCOMPARE(info.jointNames.value("J_Bip_L_Thumb2"), QString("LeftHandThumb2"));
    QCOMPARE(info.jointNames.value("J_Bip_L_Thumb3"), QString("LeftHandThumb3"));
    QCOMPARE(info.jointNames.value("J_Bip_L_UpperArm"), QString("LeftArm"));
}

// 1.0: its thumbs start at their metacarpal; with an upper chest, the chest is the Spine1 and the upper chest the Spine2
void VRMReaderTests::testHumanoid1() {
    const QJsonObject bones{ { "hips", QJsonObject{ { "node", 0 } } }, { "chest", QJsonObject{ { "node", 1 } } },
                             { "upperChest", QJsonObject{ { "node", 2 } } }, { "rightThumbMetacarpal", QJsonObject{ { "node", 3 } } },
                             { "rightThumbProximal", QJsonObject{ { "node", 4 } } }, { "rightThumbDistal", QJsonObject{ { "node", 5 } } },
                             { "rightLittleProximal", QJsonObject{ { "node", 6 } } } };
    const VRMInfo info = readVRM(makeGlb(vrm1(QJsonObject{ { "humanoid", QJsonObject{ { "humanBones", bones } } } },
        namedNodes({ "hips", "chest", "upperChest", "thumbA", "thumbB", "thumbC", "little1" }))));
    QCOMPARE(info.version, 1);
    QCOMPARE(info.jointNames.value("hips"), QString("Hips"));
    QCOMPARE(info.jointNames.value("chest"), QString("Spine1"));
    QCOMPARE(info.jointNames.value("upperChest"), QString("Spine2"));
    QCOMPARE(info.jointNames.value("thumbA"), QString("RightHandThumb1"));
    QCOMPARE(info.jointNames.value("thumbB"), QString("RightHandThumb2"));
    QCOMPARE(info.jointNames.value("thumbC"), QString("RightHandThumb3"));
    QCOMPARE(info.jointNames.value("little1"), QString("RightHandPinky1"));
}

// 0.x's presets, lowercase, the look presets among them (F7); one to one, its weights out of 100; one blink for both eyes
// is half on each
void VRMReaderTests::testExpressions0() {
    const QJsonArray groups{
        group0("A", QJsonArray{ bind0(0, 0, 100.0) }),
        group0("Blink", QJsonArray{ bind0(0, 1, 100.0) }),
        group0("LookUp", QJsonArray{ bind0(0, 2, 80.0) }),
        group0("lookleft", QJsonArray{ bind0(0, 3, 100.0) }),
        group0("U", QJsonArray{ bind0(0, 4, 100.0) }),
    };
    const VRMInfo info = readVRM(makeGlb(vrm0(
        QJsonObject{ { "blendShapeMaster", QJsonObject{ { "blendShapeGroups", groups } } } }, QJsonArray(),
        QJsonArray{ faceMesh("Face", { "Fcl_MTH_A", "Fcl_EYE_Close", "Fcl_EYE_Up", "Fcl_EYE_Left", "Fcl_MTH_U" }) })));
    verifySource(info, Blendshapes::JawOpen, "Fcl_MTH_A", 1.0f);
    verifySource(info, Blendshapes::LipsFunnel, "Fcl_MTH_U", 1.0f);
    verifySource(info, Blendshapes::EyeBlink_L, "Fcl_EYE_Close", 0.5f);
    verifySource(info, Blendshapes::EyeBlink_R, "Fcl_EYE_Close", 0.5f);
    verifySource(info, Blendshapes::EyeUp_L, "Fcl_EYE_Up", 0.4f);
    verifySource(info, Blendshapes::EyeUp_R, "Fcl_EYE_Up", 0.4f);
    // looking to its left is the left eye's out and the right eye's in
    verifySource(info, Blendshapes::EyeOut_L, "Fcl_EYE_Left", 0.5f);
    verifySource(info, Blendshapes::EyeIn_R, "Fcl_EYE_Left", 0.5f);
    QVERIFY(!info.blendshapes.contains((int)Blendshapes::EyeDown_L));
}

// 1.0's presets bind a node's mesh's targets; separate blinks go one to an eye
void VRMReaderTests::testExpressions1() {
    auto bind = [](int index) {
        return QJsonObject{ { "morphTargetBinds", QJsonArray{ QJsonObject{ { "node", 0 }, { "index", index }, { "weight", 1.0 } } } } };
    };
    const QJsonObject presets{ { "aa", bind(0) }, { "blinkLeft", bind(1) }, { "blinkRight", bind(2) }, { "lookDown", bind(3) } };
    const VRMInfo info = readVRM(makeGlb(vrm1(QJsonObject{ { "expressions", QJsonObject{ { "preset", presets } } } },
        QJsonArray{ QJsonObject{ { "name", "Face" }, { "mesh", 0 } } },
        QJsonArray{ faceMesh("FaceMesh", { "A", "BlinkL", "BlinkR", "LookDown" }) })));
    verifySource(info, Blendshapes::JawOpen, "A", 1.0f);
    QCOMPARE(info.blendshapes.value((int)Blendshapes::JawOpen)[0].meshName, QString("FaceMesh"));
    QCOMPARE(info.blendshapes.value((int)Blendshapes::JawOpen)[0].index, 0);
    verifySource(info, Blendshapes::EyeBlink_L, "BlinkL", 1.0f);
    verifySource(info, Blendshapes::EyeBlink_R, "BlinkR", 1.0f);
    verifySource(info, Blendshapes::EyeDown_L, "LookDown", 0.5f);
    verifySource(info, Blendshapes::EyeDown_R, "LookDown", 0.5f);
}

// A range map that leaves out its output: 10 degrees for eyes that turn, all of the expression for eyes that look by
// expression (F7), which ten would drive ten times too hard
void VRMReaderTests::testLookAtDefaults() {
    QJsonObject ranges1;
    QJsonObject ranges0;
    for (const char* key : { "rangeMapHorizontalInner", "rangeMapHorizontalOuter", "rangeMapVerticalDown", "rangeMapVerticalUp" }) {
        ranges1.insert(key, QJsonObject{ { "inputMaxValue", 90.0 } });
    }
    for (const char* key : { "lookAtHorizontalInner", "lookAtHorizontalOuter", "lookAtVerticalDown", "lookAtVerticalUp" }) {
        ranges0.insert(key, QJsonObject{ { "xRange", 90.0 } });
    }

    QJsonObject lookAt = ranges1;
    lookAt.insert("type", "expression");
    VRMInfo info = readVRM(makeGlb(vrm1(QJsonObject{ { "lookAt", lookAt } }, QJsonArray())));
    QCOMPARE(info.eyeLookScale, 0.0f);
    QVERIFY(std::abs(info.eyeLookExpressionScale - 1.0f / 90.0f) < 1.0e-5f);

    lookAt.insert("type", "bone");
    info = readVRM(makeGlb(vrm1(QJsonObject{ { "lookAt", lookAt } }, QJsonArray())));
    QVERIFY(std::abs(info.eyeLookScale - 10.0f / 90.0f) < 1.0e-5f);
    QCOMPARE(info.eyeLookMaxAngle, 10.0f);
    QCOMPARE(info.eyeLookExpressionScale, 0.0f);

    QJsonObject firstPerson = ranges0;
    firstPerson.insert("lookAtTypeName", "BlendShape");
    info = readVRM(makeGlb(vrm0(QJsonObject{ { "firstPerson", firstPerson } }, QJsonArray())));
    QCOMPARE(info.eyeLookScale, 0.0f);
    QVERIFY(std::abs(info.eyeLookExpressionScale - 1.0f / 90.0f) < 1.0e-5f);
}

// Spring bones whose nodes loop, repeat a child or go deeper than any rig are read once each, to a depth of 256
void VRMReaderTests::testLoopingNodes() {
    QJsonArray nodes{
        QJsonObject{ { "name", "HairA" }, { "children", QJsonArray{ 1 } } },
        QJsonObject{ { "name", "HairB" }, { "children", QJsonArray{ 0 } } },  // back to HairA
        QJsonObject{ { "name", "Tail" }, { "children", QJsonArray{ 2, 2 } } },  // itself, twice
        QJsonObject{ { "name", "Out" }, { "children", QJsonArray{ 999, -5 } } },  // children that aren't there
    };
    const int DEEP_START = nodes.size();
    const int DEEP_COUNT = 400;
    for (int i = 0; i < DEEP_COUNT; i++) {
        QJsonObject node{ { "name", QString("Deep%1").arg(i) } };
        if (i + 1 < DEEP_COUNT) {
            node.insert("children", QJsonArray{ DEEP_START + i + 1 });
        }
        nodes.append(node);
    }
    const QJsonArray boneGroups{ QJsonObject{ { "bones", QJsonArray{ 0 } } }, QJsonObject{ { "bones", QJsonArray{ 2 } } },
                                 QJsonObject{ { "bones", QJsonArray{ 3, 12345 } } },
                                 QJsonObject{ { "bones", QJsonArray{ DEEP_START } } } };
    const VRMInfo info = readVRM(makeGlb(vrm0(QJsonObject{ { "secondaryAnimation", QJsonObject{ { "boneGroups", boneGroups } } } },
                                              nodes)));
    QVERIFY(info.isVRM());
    QCOMPARE(info.flowJointNames.value("HairA"), QString("flow_vrm0_HairA"));
    QCOMPARE(info.flowJointNames.value("HairB"), QString("flow_vrm0_HairB"));
    // a joint that branches stays as animated, and its branches here are itself
    QVERIFY(!info.flowJointNames.contains("Tail"));
    int deep = 0;
    for (const QString& name : info.flowJointNames.keys()) {
        deep += name.startsWith("Deep") ? 1 : 0;
    }
    QCOMPARE(deep, 257);
}

// A humanoid bone's name already taken by another node (F8): the bone gets it, the node another
void VRMReaderTests::testClashingRename() {
    // one triangle, so Assimp has a scene
    const float POSITIONS[] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    const QByteArray bin((const char*)POSITIONS, sizeof(POSITIONS));
    const QJsonArray bones{ bone0("hips", 1), bone0("chest", 2) };
    QJsonObject gltf = vrm0(QJsonObject{ { "humanoid", QJsonObject{ { "humanBones", bones } } } },
        QJsonArray{
            QJsonObject{ { "name", "Armature" }, { "children", QJsonArray{ 1, 3 } } },
            QJsonObject{ { "name", "mixamorig:Hips" }, { "children", QJsonArray{ 2 } } },
            QJsonObject{ { "name", "mixamorig:Spine1" }, { "translation", QJsonArray{ 0, 0.3, 0 } } },
            QJsonObject{ { "name", "Spine2" }, { "mesh", 0 } } },
        QJsonArray{ QJsonObject{ { "name", "Body" }, { "primitives", QJsonArray{
            QJsonObject{ { "attributes", QJsonObject{ { "POSITION", 0 } } } } } } } });
    gltf.insert("scene", 0);
    gltf.insert("scenes", QJsonArray{ QJsonObject{ { "nodes", QJsonArray{ 0 } } } });
    gltf.insert("buffers", QJsonArray{ QJsonObject{ { "byteLength", bin.size() } } });
    gltf.insert("bufferViews", QJsonArray{ QJsonObject{ { "buffer", 0 }, { "byteOffset", 0 }, { "byteLength", bin.size() } } });
    gltf.insert("accessors", QJsonArray{ QJsonObject{ { "bufferView", 0 }, { "componentType", 5126 }, { "count", 3 },
                                                      { "type", "VEC3" }, { "min", QJsonArray{ 0, 0, 0 } },
                                                      { "max", QJsonArray{ 1, 1, 0 } } } });

    AssimpSerializer serializer;
    HFMModel::Pointer model = serializer.read(makeGlb(gltf, bin), hifi::VariantMultiHash(), hifi::URL("file:///clash-test.vrm"));
    QVERIFY(model);
    int spine2 = -1, renamed = -1, hips = -1;
    for (int i = 0; i < (int)model->joints.size(); i++) {
        const QString& name = model->joints[i].name;
        spine2 = name == "Spine2" ? i : spine2;
        renamed = name == "Spine2_joint" ? i : renamed;
        hips = name == "Hips" ? i : hips;
        QVERIFY2(!name.startsWith("mixamorig:"), qPrintable(name + " kept its name"));
    }
    QVERIFY(spine2 >= 0);
    QVERIFY(renamed >= 0);
    QVERIFY(hips >= 0);
    // the bone is the chest, the Hips' child, and the node that had the name is the Armature's
    QCOMPARE(model->joints[spine2].parentIndex, hips);
    QVERIFY(model->joints[renamed].parentIndex != hips);
    QCOMPARE(model->jointIndices.value("Spine2", -1), spine2);
    QCOMPARE(model->jointIndices.value("Spine2_joint", -1), renamed);
}
