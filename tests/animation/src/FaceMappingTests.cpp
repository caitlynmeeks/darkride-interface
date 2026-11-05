//
//  FaceMappingTests.cpp
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "FaceMappingTests.h"

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

QTEST_MAIN(FaceMappingTests)

namespace {

const QStringList TARGET_NAMES { "jawOpen", "eyeBlinkLeft", "mouthSmileLeft" };

// A glTF binary with one triangle whose three morph targets have ARKit's names, each moving one vertex
QByteArray makeArkitGlb() {
    QByteArray bin;
    QJsonArray bufferViews;
    QJsonArray accessors;
    auto addVec3s = [&](const std::vector<float>& values) {
        while (bin.size() % 4 != 0) {
            bin.append('\0');
        }
        const QByteArray bytes((const char*)values.data(), (int)(values.size() * sizeof(float)));
        bufferViews.append(QJsonObject{ { "buffer", 0 }, { "byteOffset", bin.size() }, { "byteLength", bytes.size() } });
        bin.append(bytes);
        float lowest[3] = { 0.0f, 0.0f, 0.0f }, highest[3] = { 0.0f, 0.0f, 0.0f };
        for (size_t i = 0; i < values.size(); i++) {
            lowest[i % 3] = std::min(lowest[i % 3], values[i]);
            highest[i % 3] = std::max(highest[i % 3], values[i]);
        }
        accessors.append(QJsonObject{ { "bufferView", bufferViews.size() - 1 }, { "componentType", 5126 },
                                      { "count", (int)values.size() / 3 }, { "type", "VEC3" },
                                      { "min", QJsonArray{ lowest[0], lowest[1], lowest[2] } },
                                      { "max", QJsonArray{ highest[0], highest[1], highest[2] } } });
        return accessors.size() - 1;
    };
    const int position = addVec3s({ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f });
    QJsonArray targets;
    for (int t = 0; t < TARGET_NAMES.size(); t++) {
        std::vector<float> offsets(9, 0.0f);
        offsets[t * 3 + 2] = 0.1f;  // target t moves vertex t forward
        targets.append(QJsonObject{ { "POSITION", addVec3s(offsets) } });
    }
    while (bin.size() % 4 != 0) {
        bin.append('\0');
    }
    const QJsonObject gltf{
        { "asset", QJsonObject{ { "version", "2.0" } } },
        { "scene", 0 },
        { "scenes", QJsonArray{ QJsonObject{ { "nodes", QJsonArray{ 0 } } } } },
        { "nodes", QJsonArray{ QJsonObject{ { "name", "Face" }, { "mesh", 0 } } } },
        { "meshes", QJsonArray{ QJsonObject{ { "name", "Face" },
            { "extras", QJsonObject{ { "targetNames", QJsonArray::fromStringList(TARGET_NAMES) } } },
            { "primitives", QJsonArray{ QJsonObject{ { "attributes", QJsonObject{ { "POSITION", position } } },
                                                     { "targets", targets } } } } } } },
        { "buffers", QJsonArray{ QJsonObject{ { "byteLength", bin.size() } } } },
        { "bufferViews", bufferViews },
        { "accessors", accessors },
    };
    QByteArray json = QJsonDocument(gltf).toJson(QJsonDocument::Compact);
    while (json.size() % 4 != 0) {
        json.append(' ');
    }
    QByteArray glb;
    auto appendUint32 = [&glb](uint32_t value) {
        value = qToLittleEndian(value);
        glb.append((const char*)&value, sizeof(value));
    };
    appendUint32(0x46546C67);  // "glTF"
    appendUint32(2);
    appendUint32(12 + 8 + json.size() + 8 + bin.size());
    appendUint32(json.size());
    appendUint32(0x4E4F534A);  // "JSON"
    glb.append(json);
    appendUint32(bin.size());
    appendUint32(0x004E4942);  // "BIN"
    glb.append(bin);
    return glb;
}

HFMModel::Pointer read(const hifi::VariantHash& mapping) {
    AssimpSerializer serializer;
    return serializer.read(makeArkitGlb(), mapping, hifi::URL("file:///arkit-face.glb"));
}

// whether the model's blendshape moves anything
bool moves(const HFMModel& model, Blendshapes blendshape) {
    for (const hfm::Mesh& mesh : model.meshes) {
        if ((int)blendshape < mesh.blendshapes.size() && !mesh.blendshapes[(int)blendshape].indices.isEmpty()) {
            return true;
        }
    }
    return false;
}

float gain(const HFMModel& model, Blendshapes blendshape) {
    return (int)blendshape < model.proceduralBlendshapeGains.size() ? model.proceduralBlendshapeGains[(int)blendshape] : 1.0f;
}

}

void FaceMappingTests::initTestCase() {
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

void FaceMappingTests::cleanupTestCase() {
    DependencyManager::get<ResourceManager>()->cleanup();
}

// Worn without an .fst (SkeletonModel's "autoHumanoid"): ARKit's names make the engine's blendshapes, one to one, with
// the voice's gains for the procedural face
void FaceMappingTests::testWithoutFst() {
    hifi::VariantHash mapping;
    mapping.insert("autoHumanoid", true);
    HFMModel::Pointer model = read(mapping);
    QVERIFY(model);
    QVERIFY(moves(*model, Blendshapes::JawOpen));
    QVERIFY(moves(*model, Blendshapes::EyeBlink_L));
    QVERIFY(moves(*model, Blendshapes::MouthSmile_L));
    QVERIFY(!moves(*model, Blendshapes::EyeBlink_R));
    QCOMPARE(gain(*model, Blendshapes::JawOpen), 0.5f);
    QCOMPARE(gain(*model, Blendshapes::MouthSmile_L), 0.7f);
    QCOMPARE(gain(*model, Blendshapes::EyeBlink_L), 1.0f);
    // at weight 1: the jaw's vertex moves all of its target's 0.1
    for (const hfm::Mesh& mesh : model->meshes) {
        const hfm::Blendshape& jaw = mesh.blendshapes[(int)Blendshapes::JawOpen];
        if (!jaw.indices.isEmpty()) {
            QVERIFY(std::abs(jaw.vertices[0].z - 0.1f) < 0.0001f);
        }
    }
}

// An .fst that maps a few shapes (F1c) keeps its other shapes still: the voice doesn't move the model's brows or smile
void FaceMappingTests::testFstMapsAFew() {
    hifi::VariantHash blendshapes;
    blendshapes.insert("JawOpen", QVariantList{ "jawOpen", 0.7 });
    hifi::VariantHash mapping;
    mapping.insert("bs", blendshapes);
    HFMModel::Pointer model = read(mapping);
    QVERIFY(model);
    QVERIFY(moves(*model, Blendshapes::JawOpen));
    QVERIFY(!moves(*model, Blendshapes::EyeBlink_L));
    QVERIFY(!moves(*model, Blendshapes::MouthSmile_L));
    QVERIFY(model->proceduralBlendshapeGains.isEmpty());
}

// A model entity, read with no mapping at all, takes no other names either
void FaceMappingTests::testModelEntity() {
    HFMModel::Pointer model = read(hifi::VariantHash());
    QVERIFY(model);
    QVERIFY(!moves(*model, Blendshapes::JawOpen));
    QVERIFY(!moves(*model, Blendshapes::EyeBlink_L));
}
