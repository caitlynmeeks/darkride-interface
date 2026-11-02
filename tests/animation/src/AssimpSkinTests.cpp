//
//  AssimpSkinTests.cpp
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "AssimpSkinTests.h"

#include <set>

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QtEndian>

#include <AccountManager.h>
#include <AddressManager.h>
#include <AssimpSerializer.h>
#include <hfm/HFMModelMath.h>
#include <DependencyManager.h>
#include <NodeList.h>
#include <ResourceCache.h>
#include <ResourceManager.h>
#include <ResourceRequestObserver.h>
#include <StatTracker.h>

QTEST_MAIN(AssimpSkinTests)

namespace {

// A skinned glTF binary made as VRoid makes them: one mesh whose two primitives (a body and a glove, each its own
// material) share one vertex buffer and pick their triangles from it by index. Joints Hips, Spine and Hand.
// - the glove's vertices come first (0 to 4): all weighted Hand 1, vertex 4 a copy of vertex 2, to be joined
// - the body's follow (5 to 7): vertex 5 weighted Hips 1 and Spine 1, summing to 2; 6 Hips 1; 7 Spine 1
// So in the body's mesh the Hand bone weights only vertices the body's faces don't use.
// padJson false leaves the JSON chunk short of a multiple of 4, as some exporters do (16 of 45 VRMs in one collection)
QByteArray makeSharedBufferGlb(bool padJson = true) {
    struct Vertex {
        float position[3];
        uint8_t joints[4];
        float weights[4];
    };
    const Vertex VERTICES[] = {
        { { 0.0f, 1.0f, 0.0f }, { 2, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
        { { 0.2f, 1.0f, 0.0f }, { 2, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
        { { 0.0f, 1.2f, 0.0f }, { 2, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
        { { 0.2f, 1.2f, 0.0f }, { 2, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
        { { 0.0f, 1.2f, 0.0f }, { 2, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
        { { 0.0f, 0.0f, 0.0f }, { 0, 1, 0, 0 }, { 1.0f, 1.0f, 0.0f, 0.0f } },
        { { 0.2f, 0.0f, 0.0f }, { 0, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
        { { 0.0f, 0.3f, 0.0f }, { 1, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } },
    };
    const int VERTEX_COUNT = sizeof(VERTICES) / sizeof(VERTICES[0]);
    const uint16_t GLOVE_INDICES[] = { 0, 1, 2, 1, 3, 4 };
    const uint16_t BODY_INDICES[] = { 5, 6, 7 };
    // the inverse bind matrices, column major: Hips at the origin, Spine 0.5 up, Hand 1 up
    const float BIND_HEIGHTS[] = { 0.0f, 0.5f, 1.0f };

    QByteArray bin;
    QJsonArray bufferViews;
    QJsonArray accessors;
    auto addView = [&](const QByteArray& bytes) {
        while (bin.size() % 4 != 0) {
            bin.append('\0');
        }
        bufferViews.append(QJsonObject{ { "buffer", 0 }, { "byteOffset", bin.size() }, { "byteLength", bytes.size() } });
        bin.append(bytes);
        return bufferViews.size() - 1;
    };
    auto addAccessor = [&](const QByteArray& bytes, int componentType, int count, const QString& type) {
        accessors.append(QJsonObject{ { "bufferView", addView(bytes) }, { "componentType", componentType }, { "count", count },
                                      { "type", type } });
        return accessors.size() - 1;
    };
    QByteArray positions, normals, joints, weights;
    for (const Vertex& vertex : VERTICES) {
        positions.append((const char*)vertex.position, sizeof(vertex.position));
        const float NORMAL[] = { 0.0f, 0.0f, 1.0f };
        normals.append((const char*)NORMAL, sizeof(NORMAL));
        joints.append((const char*)vertex.joints, sizeof(vertex.joints));
        weights.append((const char*)vertex.weights, sizeof(vertex.weights));
    }
    QByteArray inverseBindMatrices;
    for (float height : BIND_HEIGHTS) {
        const float MATRIX[] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -height, 0, 1 };
        inverseBindMatrices.append((const char*)MATRIX, sizeof(MATRIX));
    }

    const int FLOAT = 5126;
    const int UNSIGNED_BYTE = 5121;
    const int UNSIGNED_SHORT = 5123;
    const int position = addAccessor(positions, FLOAT, VERTEX_COUNT, "VEC3");
    const int normal = addAccessor(normals, FLOAT, VERTEX_COUNT, "VEC3");
    const int joint = addAccessor(joints, UNSIGNED_BYTE, VERTEX_COUNT, "VEC4");
    const int weight = addAccessor(weights, FLOAT, VERTEX_COUNT, "VEC4");
    const int gloveIndices = addAccessor(QByteArray((const char*)GLOVE_INDICES, sizeof(GLOVE_INDICES)), UNSIGNED_SHORT, 6, "SCALAR");
    const int bodyIndices = addAccessor(QByteArray((const char*)BODY_INDICES, sizeof(BODY_INDICES)), UNSIGNED_SHORT, 3, "SCALAR");
    const int bindMatrices = addAccessor(inverseBindMatrices, FLOAT, 3, "MAT4");
    while (bin.size() % 4 != 0) {
        bin.append('\0');
    }

    const QJsonObject attributes{ { "POSITION", position }, { "NORMAL", normal }, { "JOINTS_0", joint }, { "WEIGHTS_0", weight } };
    QJsonObject gltf{
        { "asset", QJsonObject{ { "version", "2.0" } } },
        { "scene", 0 },
        { "scenes", QJsonArray{ QJsonObject{ { "nodes", QJsonArray{ 0 } } } } },
        { "nodes", QJsonArray{
            QJsonObject{ { "name", "Armature" }, { "children", QJsonArray{ 1, 4 } } },
            QJsonObject{ { "name", "Hips" }, { "children", QJsonArray{ 2 } } },
            QJsonObject{ { "name", "Spine" }, { "translation", QJsonArray{ 0, 0.5, 0 } }, { "children", QJsonArray{ 3 } } },
            QJsonObject{ { "name", "Hand" }, { "translation", QJsonArray{ 0, 0.5, 0 } } },
            QJsonObject{ { "name", "Body" }, { "mesh", 0 }, { "skin", 0 } } } },
        { "skins", QJsonArray{ QJsonObject{ { "joints", QJsonArray{ 1, 2, 3 } }, { "inverseBindMatrices", bindMatrices } } } },
        { "materials", QJsonArray{ QJsonObject{ { "name", "Glove" } }, QJsonObject{ { "name", "Skin" } } } },
        { "meshes", QJsonArray{ QJsonObject{ { "name", "Body" }, { "primitives", QJsonArray{
            QJsonObject{ { "attributes", attributes }, { "indices", gloveIndices }, { "material", 0 } },
            QJsonObject{ { "attributes", attributes }, { "indices", bodyIndices }, { "material", 1 } } } } } } },
        { "buffers", QJsonArray{ QJsonObject{ { "byteLength", bin.size() } } } },
        { "bufferViews", bufferViews },
        { "accessors", accessors },
    };
    QByteArray json = QJsonDocument(gltf).toJson(QJsonDocument::Compact);
    while ((json.size() % 4 != 0) == padJson) {
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

// A glTF binary with one quad and one morph target given as a SPARSE accessor whose single index names vertex
// `badIndex` (of 4): a morph target naming a vertex the mesh does not have. Whatever Assimp makes of it, what the
// reader hands on must hold together (hfm::sanitizeMesh runs at the end of each mesh).
QByteArray makeMorphTargetGlb(uint16_t badIndex) {
    const float POSITIONS[] = { 0, 0, 0,  1, 0, 0,  0, 1, 0,  1, 1, 0 };
    const uint16_t INDICES[] = { 0, 1, 2, 1, 3, 2 };
    const uint16_t SPARSE_INDEX[] = { badIndex, 0 };   // one index, padded to 4 bytes
    const float SPARSE_VALUE[] = { 0, 0, 1 };

    QByteArray bin;
    QJsonArray bufferViews;
    auto addView = [&](const QByteArray& bytes) {
        while (bin.size() % 4 != 0) {
            bin.append('\0');
        }
        bufferViews.append(QJsonObject{ { "buffer", 0 }, { "byteOffset", bin.size() }, { "byteLength", bytes.size() } });
        bin.append(bytes);
        return bufferViews.size() - 1;
    };
    const int FLOAT = 5126;
    const int UNSIGNED_SHORT = 5123;
    const int positionView = addView(QByteArray((const char*)POSITIONS, sizeof(POSITIONS)));
    const int indexView = addView(QByteArray((const char*)INDICES, sizeof(INDICES)));
    const int sparseIndexView = addView(QByteArray((const char*)SPARSE_INDEX, sizeof(uint16_t)));
    const int sparseValueView = addView(QByteArray((const char*)SPARSE_VALUE, sizeof(SPARSE_VALUE)));
    while (bin.size() % 4 != 0) {
        bin.append('\0');
    }
    QJsonArray accessors{
        QJsonObject{ { "bufferView", positionView }, { "componentType", FLOAT }, { "count", 4 }, { "type", "VEC3" },
                     { "min", QJsonArray{ 0, 0, 0 } }, { "max", QJsonArray{ 1, 1, 0 } } },
        QJsonObject{ { "bufferView", indexView }, { "componentType", UNSIGNED_SHORT }, { "count", 6 }, { "type", "SCALAR" } },
        QJsonObject{ { "componentType", FLOAT }, { "count", 4 }, { "type", "VEC3" },
                     { "sparse", QJsonObject{ { "count", 1 },
                                              { "indices", QJsonObject{ { "bufferView", sparseIndexView }, { "componentType", UNSIGNED_SHORT } } },
                                              { "values", QJsonObject{ { "bufferView", sparseValueView } } } } } },
    };
    QJsonObject gltf{
        { "asset", QJsonObject{ { "version", "2.0" } } },
        { "scene", 0 },
        { "scenes", QJsonArray{ QJsonObject{ { "nodes", QJsonArray{ 0 } } } } },
        { "nodes", QJsonArray{ QJsonObject{ { "name", "Quad" }, { "mesh", 0 } } } },
        { "meshes", QJsonArray{ QJsonObject{
            { "primitives", QJsonArray{ QJsonObject{ { "attributes", QJsonObject{ { "POSITION", 0 } } }, { "indices", 1 },
                                                      { "targets", QJsonArray{ QJsonObject{ { "POSITION", 2 } } } } } } },
            { "extras", QJsonObject{ { "targetNames", QJsonArray{ "Smile" } } } } } } },
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

HFMModel::Pointer readSharedBufferGlb(bool padJson = true) {
    AssimpSerializer serializer;
    return serializer.read(makeSharedBufferGlb(padJson), hifi::VariantHash(), hifi::URL("file:///skin-test.glb"));
}

const hfm::SkinDeformer* skinOf(const HFMModel& model, uint32_t meshIndex) {
    for (const hfm::Shape& shape : model.shapes) {
        if (shape.mesh == meshIndex && shape.skinDeformer < model.skinDeformers.size()) {
            return &model.skinDeformers[shape.skinDeformer];
        }
    }
    return nullptr;
}

// the names of the joints that weight any of the mesh's vertices
std::set<QString> jointsWeighting(const HFMModel& model, uint32_t meshIndex) {
    std::set<QString> names;
    const hfm::Mesh& mesh = model.meshes[meshIndex];
    const hfm::SkinDeformer* skin = skinOf(model, meshIndex);
    if (!skin) {
        return names;
    }
    for (size_t i = 0; i < mesh.clusterIndices.size() && i < mesh.clusterWeights.size(); i++) {
        if (mesh.clusterWeights[i] > 0 && mesh.clusterIndices[i] < skin->clusters.size()) {
            names.insert(model.joints[skin->clusters[mesh.clusterIndices[i]].jointIndex].name);
        }
    }
    return names;
}

// the glove's mesh (the one weighted to the Hand only) and the body's
void findMeshes(const HFMModel& model, int& glove, int& body) {
    glove = body = -1;
    for (uint32_t meshIndex = 0; meshIndex < (uint32_t)model.meshes.size(); meshIndex++) {
        if (jointsWeighting(model, meshIndex) == std::set<QString>{ "Hand" }) {
            glove = meshIndex;
        } else {
            body = meshIndex;
        }
    }
}

}

namespace {

// a .glb of two meshes: 0 a triangle, 1 three points (a POINTS primitive, mode 0). The glTF reader keeps only
// triangles, so mesh 1 comes out with a part and no index: the model baker made it no drawable and the renderer crashed.
QByteArray makePointsGlb() {
    const float POSITIONS[] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    const uint16_t INDICES[] = { 0, 1, 2 };
    QByteArray bin;
    bin.append((const char*)POSITIONS, sizeof(POSITIONS));
    const int indicesOffset = bin.size();
    bin.append((const char*)INDICES, sizeof(INDICES));
    while (bin.size() % 4 != 0) {
        bin.append('\0');
    }
    const int FLOAT = 5126;
    const int UNSIGNED_SHORT = 5123;
    const QJsonArray bufferViews{
        QJsonObject{ { "buffer", 0 }, { "byteOffset", 0 }, { "byteLength", (int)sizeof(POSITIONS) } },
        QJsonObject{ { "buffer", 0 }, { "byteOffset", indicesOffset }, { "byteLength", (int)sizeof(INDICES) } } };
    const QJsonArray accessors{
        QJsonObject{ { "bufferView", 0 }, { "componentType", FLOAT }, { "count", 3 }, { "type", "VEC3" },
                     { "min", QJsonArray{ 0, 0, 0 } }, { "max", QJsonArray{ 1, 1, 0 } } },
        QJsonObject{ { "bufferView", 1 }, { "componentType", UNSIGNED_SHORT }, { "count", 3 }, { "type", "SCALAR" } } };
    const QJsonObject attributes{ { "POSITION", 0 } };
    const QJsonObject gltf{
        { "asset", QJsonObject{ { "version", "2.0" } } },
        { "scene", 0 },
        { "scenes", QJsonArray{ QJsonObject{ { "nodes", QJsonArray{ 0, 1 } } } } },
        { "nodes", QJsonArray{ QJsonObject{ { "name", "Triangle" }, { "mesh", 0 } },
                               QJsonObject{ { "name", "Points" }, { "mesh", 1 }, { "translation", QJsonArray{ 2, 0, 0 } } } } },
        { "meshes", QJsonArray{
            QJsonObject{ { "name", "Triangle" }, { "primitives", QJsonArray{ QJsonObject{ { "attributes", attributes }, { "indices", 1 } } } } },
            QJsonObject{ { "name", "Points" }, { "primitives", QJsonArray{ QJsonObject{ { "attributes", attributes }, { "mode", 0 } } } } } } },
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

}

void AssimpSkinTests::initTestCase() {
    // as AnimTests does: ResourceManager (which the serializer asks to normalize the model's URL) needs a NodeList
    DependencyManager::registerInheritance<LimitedNodeList, NodeList>();
    DependencyManager::set<AccountManager>();
    DependencyManager::set<AddressManager>();
    DependencyManager::set<NodeList>(NodeType::Agent);
    DependencyManager::set<ResourceManager>();
    DependencyManager::set<ResourceRequestObserver>();
    DependencyManager::set<ResourceCacheSharedItems>();
    DependencyManager::set<StatTracker>();
}

void AssimpSkinTests::cleanupTestCase() {
    DependencyManager::get<ResourceManager>()->cleanup();
}

// Each primitive's mesh keeps only the vertices its faces use, with their weights: before, the body's Hand bone kept
// weights numbered for the glove's vertices, which the join then gave to the body's own vertices. Identical vertices
// are still joined.
void AssimpSkinTests::testSharedVertexBuffer() {
    HFMModel::Pointer model = readSharedBufferGlb();
    QVERIFY(model);
    QCOMPARE(model->meshes.size(), (size_t)2);
    int glove, body;
    findMeshes(*model, glove, body);
    QVERIFY(glove >= 0);
    QVERIFY(body >= 0);

    QCOMPARE(model->meshes[glove].vertices.size(), 4);
    QCOMPARE(model->meshes[body].vertices.size(), 3);
    const std::set<QString> BODY_JOINTS{ "Hips", "Spine" };
    QVERIFY(jointsWeighting(*model, body) == BODY_JOINTS);
}

// A vertex's weights sum to one, as the shader blends them, even where the file's don't
void AssimpSkinTests::testWeightsSumToOne() {
    HFMModel::Pointer model = readSharedBufferGlb();
    QVERIFY(model);
    for (const hfm::Mesh& mesh : model->meshes) {
        QCOMPARE(mesh.clusterWeightsPerVertex, (uint16_t)4);
        QCOMPARE(mesh.clusterWeights.size(), (size_t)mesh.vertices.size() * 4);
        for (size_t vertexIndex = 0; vertexIndex < (size_t)mesh.vertices.size(); vertexIndex++) {
            int sum = 0;
            for (size_t i = 0; i < 4; i++) {
                sum += mesh.clusterWeights[vertexIndex * 4 + i];
            }
            QVERIFY2(std::abs(sum - 65535) <= 2, qPrintable(QString("vertex %1 weights sum to %2").arg(vertexIndex).arg(sum)));
        }
    }

    // the body's vertex weighted Hips 1 and Spine 1 is half each
    int glove, body;
    findMeshes(*model, glove, body);
    QVERIFY(body >= 0);
    const hfm::Mesh& mesh = model->meshes[body];
    bool found = false;
    for (size_t vertexIndex = 0; vertexIndex < (size_t)mesh.vertices.size(); vertexIndex++) {
        if (glm::length(mesh.vertices[(int)vertexIndex]) < 0.001f) {
            found = true;
            QVERIFY(std::abs((int)mesh.clusterWeights[vertexIndex * 4] - 32768) <= 1);
            QVERIFY(std::abs((int)mesh.clusterWeights[vertexIndex * 4 + 1] - 32768) <= 1);
        }
    }
    QVERIFY(found);
}

// Each mesh has a skin of its own, its clusters numbered from 0, as the skinning shader takes 128 a mesh
void AssimpSkinTests::testSkinPerMesh() {
    HFMModel::Pointer model = readSharedBufferGlb();
    QVERIFY(model);
    std::set<const hfm::SkinDeformer*> skins;
    for (uint32_t meshIndex = 0; meshIndex < (uint32_t)model->meshes.size(); meshIndex++) {
        const hfm::SkinDeformer* skin = skinOf(*model, meshIndex);
        QVERIFY(skin);
        skins.insert(skin);
        for (uint16_t clusterIndex : model->meshes[meshIndex].clusterIndices) {
            QVERIFY(clusterIndex < skin->clusters.size());
        }
    }
    QCOMPARE(skins.size(), model->meshes.size());
}

// A JSON chunk left unpadded is read all the same (before, Assimp missed the BIN chunk and the model didn't load), and
// as the padded file is
void AssimpSkinTests::testUnpaddedJsonChunk() {
    QVERIFY(makeSharedBufferGlb(false).size() % 4 != 0);
    HFMModel::Pointer padded = readSharedBufferGlb();
    HFMModel::Pointer unpadded = readSharedBufferGlb(false);
    QVERIFY(padded);
    QVERIFY(unpadded);
    QCOMPARE(unpadded->meshes.size(), padded->meshes.size());
    QCOMPARE(unpadded->joints.size(), padded->joints.size());
    for (size_t i = 0; i < padded->meshes.size(); i++) {
        QCOMPARE(unpadded->meshes[i].vertices.size(), padded->meshes[i].vertices.size());
    }
}

// A morph target naming a vertex the mesh does not have (a sparse accessor's index past the count): the reader either
// refuses the file or hands on blendshapes that hold together (every index inside the mesh, every list one per index
// or empty), never one the blend or the baker would read past. A good target, for the same path, must come through.
void AssimpSkinTests::testMorphTargetPastVertices() {
    AssimpSerializer serializer;
    // "Smile" is no standard blendshape name: the model's own shapes are kept only when the .fst asks for them
    hifi::VariantHash mapping;
    mapping.insert("ownBlendshapes", "true");
    // QVERIFY and QCOMPARE return void on failure, so the lambda gathers what doesn't hold and the test checks it after
    QStringList broken;
    auto check = [&](uint16_t index, const char* what) {
        HFMModel::Pointer model = serializer.read(makeMorphTargetGlb(index), mapping, hifi::URL("file:///morph-test.glb"));
        if (!model) {
            qDebug() << what << ": the reader refused the file";
            return false;
        }
        bool anyShape = false;
        for (const hfm::Mesh& mesh : model->meshes) {
            for (const hfm::Blendshape& shape : mesh.blendshapes) {
                if (shape.vertices.size() != shape.indices.size() ||
                    !(shape.normals.isEmpty() || shape.normals.size() == shape.indices.size()) ||
                    !(shape.tangents.isEmpty() || shape.tangents.size() == shape.indices.size())) {
                    broken << QString("%1: a blendshape's lists aren't one per index").arg(what);
                }
                for (int vertex : shape.indices) {
                    if (vertex < 0 || vertex >= mesh.vertices.size()) {
                        broken << QString("%1: a blendshape names vertex %2 of %3").arg(what).arg(vertex).arg(mesh.vertices.size());
                    }
                }
                anyShape = anyShape || !shape.indices.isEmpty();
            }
        }
        qDebug() << what << ": read," << model->meshes.size() << "meshes, a blendshape with vertices:" << anyShape;
        return anyShape;
    };
    QVERIFY(check(1, "good target (vertex 1 of 4)"));   // the same path with a sound index comes through
    // refused, or sanitized: either way nothing to read past. Assimp 5.0 refuses both itself ("Sparse accessor index 9
    // is out of range"); the cases stay so an Assimp upgrade gets the same check (security, 2026-11-01)
    check(9, "bad target (vertex 9 of 4)");
    check(60000, "bad target (vertex 60000 of 4)");
    QVERIFY2(broken.isEmpty(), qPrintable(broken.join("; ")));
}

// A glTF mesh of points only (any reader can hand out a part with no face): after the read every mesh with parts can
// be drawn, and the triangle mesh, the mesh count and the numbering are as read.
void AssimpSkinTests::testPointsOnlyMeshKeptDrawable() {
    AssimpSerializer serializer;
    const HFMModel::Pointer model = serializer.read(makePointsGlb(), hifi::VariantHash(), hifi::URL("file:///points.glb"));
    QVERIFY(model);
    const size_t meshCount = model->meshes.size();
    QVERIFY(meshCount >= 1);
    int faceless = 0;
    std::vector<QVector<int>> before;
    for (const auto& mesh : model->meshes) {
        int indices = 0;
        for (const auto& part : mesh.parts) {
            indices += part.triangleIndices.size() + part.quadTrianglesIndices.size();
        }
        faceless += (!mesh.parts.empty() && indices == 0) ? 1 : 0;
        before.push_back(mesh.parts.empty() ? QVector<int>() : mesh.parts[0].triangleIndices);
    }
    qDebug() << "points.glb:" << meshCount << "meshes," << faceless << "with parts and no face as read";

    QCOMPARE(hfm::keepMeshesDrawable(*model), faceless);

    QCOMPARE(model->meshes.size(), meshCount);
    for (size_t i = 0; i < model->meshes.size(); i++) {
        const auto& mesh = model->meshes[i];
        if (mesh.parts.empty()) {
            continue;
        }
        QVERIFY(!mesh.vertices.isEmpty());
        int indices = 0;
        for (const auto& part : mesh.parts) {
            indices += part.triangleIndices.size() + part.quadTrianglesIndices.size();
            for (int index : part.triangleIndices) {
                QVERIFY(index >= 0 && index < mesh.vertices.size());
            }
        }
        QVERIFY(indices > 0);
        if (!before[i].isEmpty()) {
            QCOMPARE(mesh.parts[0].triangleIndices, before[i]);  // a mesh with faces is as read
        }
    }
}
