//
//  ModelSanitizeTests.cpp
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "ModelSanitizeTests.h"

#include <FBXSerializer.h>
#include <OBJSerializer.h>
#include <hfm/HFMModelMath.h>

QTEST_MAIN(ModelSanitizeTests)

namespace {

// a unit square: vertices 0 1 2 3, two triangles and the same square as a quad
hfm::Mesh makeSquare() {
    hfm::Mesh mesh;
    mesh.vertices = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    mesh.normals = QVector<glm::vec3>(4, glm::vec3(0.0f, 0.0f, 1.0f));
    mesh.texCoords = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f } };
    hfm::MeshPart part;
    part.triangleIndices = { 0, 1, 2, 0, 2, 3 };
    part.quadIndices = { 0, 1, 2, 3 };
    part.quadTrianglesIndices = { 0, 1, 3, 1, 2, 3 };
    mesh.parts.push_back(part);
    hfm::Blendshape smile;  // moves vertices 2 and 3 up
    smile.indices = { 2, 3 };
    smile.vertices = { { 0.0f, 0.1f, 0.0f }, { 0.0f, 0.1f, 0.0f } };
    smile.normals = QVector<glm::vec3>(2, glm::vec3(0.0f, 0.0f, 1.0f));
    mesh.blendshapes.append(smile);
    return mesh;
}

// what the model baker needs to make a drawable for a mesh with parts: a vertex and a triangle or quad-triangle index
// (BuildGraphicsMeshTask); the renderer reads the drawable of every part unchecked
void verifyDrawable(const hfm::Mesh& mesh) {
    if (mesh.parts.empty()) {
        return;
    }
    QVERIFY(!mesh.vertices.isEmpty());
    int indices = 0;
    for (const auto& part : mesh.parts) {
        indices += part.triangleIndices.size() + part.quadTrianglesIndices.size();
    }
    QVERIFY(indices > 0);
}

// every index of every part names a vertex the mesh has, and every list holds whole faces
void verifyHoldsTogether(const hfm::Mesh& mesh) {
    const int vertexCount = mesh.vertices.size();
    for (const auto& part : mesh.parts) {
        QCOMPARE(part.triangleIndices.size() % 3, 0);
        QCOMPARE(part.quadTrianglesIndices.size() % 3, 0);
        QCOMPARE(part.quadIndices.size() % 4, 0);
        for (const QVector<int>* indices : { &part.triangleIndices, &part.quadTrianglesIndices, &part.quadIndices }) {
            for (int index : *indices) {
                QVERIFY2(index >= 0 && index < vertexCount,
                         qPrintable(QString("index %1 of %2 vertices").arg(index).arg(vertexCount)));
            }
        }
    }
}

}

void ModelSanitizeTests::testGoodMeshUntouched() {
    hfm::Mesh mesh = makeSquare();
    const hfm::Mesh copy = mesh;  // shares every list with mesh

    const hfm::MeshSanitizeResult result = hfm::sanitizeMesh(mesh);

    QCOMPARE(result.facesRemoved, 0);
    QCOMPARE(result.listsCleared, 0);
    QCOMPARE(result.blendshapesCleared, 0);
    QVERIFY(!result.changed());
    QCOMPARE(mesh.parts[0].triangleIndices, copy.parts[0].triangleIndices);
    QCOMPARE(mesh.parts[0].quadIndices, copy.parts[0].quadIndices);
    QCOMPARE(mesh.parts[0].quadTrianglesIndices, copy.parts[0].quadTrianglesIndices);
    QCOMPARE(mesh.normals.size(), 4);
    QCOMPARE(mesh.texCoords.size(), 4);
    // only read: a mesh that holds together is never copied
    QVERIFY(mesh.parts[0].triangleIndices.isSharedWith(copy.parts[0].triangleIndices));
    QVERIFY(mesh.parts[0].quadIndices.isSharedWith(copy.parts[0].quadIndices));
    QVERIFY(mesh.parts[0].quadTrianglesIndices.isSharedWith(copy.parts[0].quadTrianglesIndices));
    QVERIFY(mesh.blendshapes.isSharedWith(copy.blendshapes));
}

void ModelSanitizeTests::testBadBlendshapesEmptied() {
    hfm::Mesh mesh = makeSquare();
    const hfm::Blendshape good = mesh.blendshapes[0];
    hfm::Blendshape pastVertices = good;  // names vertex 9 of 4
    pastVertices.indices = { 2, 9 };
    hfm::Blendshape negative = good;
    negative.indices = { -1, 3 };
    hfm::Blendshape shortVertices = good;  // two indices, one vertex
    shortVertices.vertices.resize(1);
    hfm::Blendshape shortNormals = good;  // the normals go, the baker makes them again; the shape stays
    shortNormals.normals.resize(1);
    hfm::Blendshape longTangents = good;
    longTangents.tangents = QVector<glm::vec3>(3, glm::vec3(1.0f, 0.0f, 0.0f));
    mesh.blendshapes << pastVertices << negative << shortVertices << shortNormals << longTangents;

    const hfm::MeshSanitizeResult result = hfm::sanitizeMesh(mesh);

    QCOMPARE(result.blendshapesCleared, 3);
    QCOMPARE(result.listsCleared, 2);
    QCOMPARE(result.facesRemoved, 0);
    QCOMPARE(mesh.blendshapes.size(), 6);  // every slot stays: the channels are numbered by it
    QCOMPARE(mesh.blendshapes[0].indices, good.indices);
    QCOMPARE(mesh.blendshapes[0].normals.size(), 2);
    for (int i : { 1, 2, 3 }) {
        QVERIFY(mesh.blendshapes[i].indices.isEmpty());
        QVERIFY(mesh.blendshapes[i].vertices.isEmpty());
        QVERIFY(mesh.blendshapes[i].normals.isEmpty());
        QVERIFY(mesh.blendshapes[i].tangents.isEmpty());
    }
    QCOMPARE(mesh.blendshapes[4].indices, good.indices);
    QCOMPARE(mesh.blendshapes[4].vertices.size(), 2);
    QVERIFY(mesh.blendshapes[4].normals.isEmpty());
    QCOMPARE(mesh.blendshapes[5].normals.size(), 2);
    QVERIFY(mesh.blendshapes[5].tangents.isEmpty());
    // what's left holds together: every index inside the mesh, every list one per index or empty
    for (const auto& shape : mesh.blendshapes) {
        QCOMPARE(shape.vertices.size(), shape.indices.size());
        QVERIFY(shape.normals.isEmpty() || shape.normals.size() == shape.indices.size());
        QVERIFY(shape.tangents.isEmpty() || shape.tangents.size() == shape.indices.size());
        for (int index : shape.indices) {
            QVERIFY(index >= 0 && index < mesh.vertices.size());
        }
    }
}

void ModelSanitizeTests::testBadFacesRemoved() {
    hfm::Mesh mesh = makeSquare();
    hfm::MeshPart& part = mesh.parts[0];
    // past the vertices, negative, good, then a triangle cut short
    part.triangleIndices = { 0, 1, 2, 0, 9, 2, 0, 2, 3, -1, 0, 1, 2, 3 };
    part.quadIndices = { 0, 1, 2, 3, 0, 1, 2, 4 };
    part.quadTrianglesIndices = { 0, 1, 3, 1, 2, 3, 0, 1, 4, 1, 2, 4 };
    mesh.normals.resize(3);  // one short: cleared, the baker makes them again
    mesh.colors.clear();     // none at all is fine

    const hfm::MeshSanitizeResult result = hfm::sanitizeMesh(mesh);

    QCOMPARE(part.triangleIndices, QVector<int>({ 0, 1, 2, 0, 2, 3 }));
    QCOMPARE(part.quadIndices, QVector<int>({ 0, 1, 2, 3 }));
    QCOMPARE(part.quadTrianglesIndices, QVector<int>({ 0, 1, 3, 1, 2, 3 }));
    QCOMPARE(result.facesRemoved, 3 + 1 + 2);
    QCOMPARE(result.listsCleared, 1);
    QVERIFY(mesh.normals.isEmpty());
    QCOMPARE(mesh.texCoords.size(), 4);
    QCOMPARE(mesh.vertices.size(), 4);
    verifyHoldsTogether(mesh);

    // no vertices at all: every face goes, and the blendshape with them
    hfm::Mesh empty = makeSquare();
    empty.vertices.clear();
    const hfm::MeshSanitizeResult emptied = hfm::sanitizeMesh(empty);
    QCOMPARE(emptied.facesRemoved, 2 + 1 + 2);
    QCOMPARE(emptied.blendshapesCleared, 1);
    verifyHoldsTogether(empty);
}

// An FBX polygon of one corner at the end of the list: the reader's triangle fan adds one index and stops, leaving
// the triangle list one short of whole triangles (every pass after the reader reads it three at a time).
void ModelSanitizeTests::testFbxPolygonCutShort() {
    const QByteArray FBX_TEXT =
        "; FBX 7.4.0 project file\n"
        "FBXHeaderExtension:  {\n"
        "    FBXHeaderVersion: 1003\n"
        "    FBXVersion: 7400\n"
        "}\n"
        "Objects:  {\n"
        "    Geometry: 1000, \"Geometry::Square\", \"Mesh\" {\n"
        "        Vertices: *12 {\n"
        "            a: 0,0,0,1,0,0,1,1,0,0,1,0\n"
        "        }\n"
        "        PolygonVertexIndex: *7 {\n"
        "            a: 0,1,-3,0,2,-4,-1\n"
        "        }\n"
        "    }\n"
        "}\n";

    const HFMModel::Pointer model = FBXSerializer().read(FBX_TEXT, hifi::VariantMultiHash(), QUrl("file:///cut-short.fbx"));

    QVERIFY(model);
    QCOMPARE((int)model->meshes.size(), 1);
    const hfm::Mesh& mesh = model->meshes[0];
    int triangleIndexCount = 0;
    for (const auto& part : mesh.parts) {
        triangleIndexCount += part.triangleIndices.size();
    }
    QCOMPARE(triangleIndexCount, 6);  // the square's two triangles, the one-corner polygon left out
    verifyHoldsTogether(mesh);
}

// An OBJ face naming a vertex past the file's (and one by a relative index before the first): left out whole.
void ModelSanitizeTests::testObjFacePastVertices() {
    const QByteArray OBJ_TEXT =
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 1 1 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n"
        "f 1 3 9\n"
        "f 1 -9 3\n"
        "f 1 3 4\n";

    const HFMModel::Pointer model = OBJSerializer().read(OBJ_TEXT, hifi::VariantMultiHash(), QUrl());

    QVERIFY(model);
    QCOMPARE((int)model->meshes.size(), 1);
    const hfm::Mesh& mesh = model->meshes[0];
    int triangleIndexCount = 0;
    for (const auto& part : mesh.parts) {
        triangleIndexCount += part.triangleIndices.size();
    }
    QCOMPARE(triangleIndexCount, 6);  // faces 1 and 4
    QCOMPARE(mesh.normals.size(), mesh.vertices.size());
    QCOMPARE(mesh.texCoords.size(), mesh.vertices.size());
    verifyHoldsTogether(mesh);
}

// Old High Fidelity content (the picture frame's default-image-model.fbx, the HQ water tank): a first mesh that is one
// point, a vertex and a one-corner polygon. The last check leaves the polygon out, the mesh keeps its part with no
// face, and the model baker would make it no drawable (the renderer crashed on it). Kept as one empty triangle; the
// square beside it, and the mesh numbering, as read.
void ModelSanitizeTests::testFbxSinglePointMeshKeptDrawable() {
    const QByteArray FBX_TEXT =
        "; FBX 7.4.0 project file\n"
        "FBXHeaderExtension:  {\n"
        "    FBXHeaderVersion: 1003\n"
        "    FBXVersion: 7400\n"
        "}\n"
        "Objects:  {\n"
        "    Geometry: 1000, \"Geometry::Point\", \"Mesh\" {\n"
        "        Vertices: *3 {\n"
        "            a: 0.5,0.5,0\n"
        "        }\n"
        "        PolygonVertexIndex: *1 {\n"
        "            a: -1\n"
        "        }\n"
        "    }\n"
        "    Geometry: 1001, \"Geometry::Square\", \"Mesh\" {\n"
        "        Vertices: *12 {\n"
        "            a: 0,0,0,1,0,0,1,1,0,0,1,0\n"
        "        }\n"
        "        PolygonVertexIndex: *6 {\n"
        "            a: 0,1,-3,0,2,-4\n"
        "        }\n"
        "    }\n"
        "}\n";

    const HFMModel::Pointer model = FBXSerializer().read(FBX_TEXT, hifi::VariantMultiHash(), QUrl("file:///single-point.fbx"));
    QVERIFY(model);
    QCOMPARE((int)model->meshes.size(), 2);
    const int point = model->meshes[0].vertices.size() == 1 ? 0 : 1;
    const int square = 1 - point;
    QCOMPARE(model->meshes[point].vertices.size(), 1);
    QVERIFY(model->meshes[square].vertices.size() > 1);  // the reader keeps a vertex per polygon corner
    const auto squareParts = model->meshes[square].parts.size();
    const auto squareIndices = model->meshes[square].parts[0].triangleIndices;

    QCOMPARE(hfm::keepMeshesDrawable(*model), 1);

    QCOMPARE((int)model->meshes.size(), 2);
    const hfm::Mesh& kept = model->meshes[point];
    QCOMPARE(kept.vertices.size(), 1);
    QCOMPARE(kept.parts[0].triangleIndices, (QVector<int>{ 0, 0, 0 }));
    verifyHoldsTogether(kept);
    verifyDrawable(kept);
    QCOMPARE(model->meshes[square].parts.size(), squareParts);
    QCOMPARE(model->meshes[square].parts[0].triangleIndices, squareIndices);
    QCOMPARE(hfm::keepMeshesDrawable(*model), 0);  // once kept, it holds
}

// An OBJ whose every face names a vertex past the file's: whatever part the reader keeps, the model can be drawn.
void ModelSanitizeTests::testObjAllFacesPastKeptDrawable() {
    const QByteArray OBJ_TEXT =
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 1 1 0\n"
        "f 1 2 9\n"
        "f 7 8 9\n";

    const HFMModel::Pointer model = OBJSerializer().read(OBJ_TEXT, hifi::VariantMultiHash(), QUrl());
    QVERIFY(model);
    hfm::keepMeshesDrawable(*model);
    for (const auto& mesh : model->meshes) {
        verifyHoldsTogether(mesh);
        verifyDrawable(mesh);
    }
}

// A mesh with parts and no vertex (every face named vertices it doesn't have): one vertex at its origin, one empty
// triangle, and nothing left that names a vertex it doesn't have; the blendshape slot stays, emptied.
void ModelSanitizeTests::testNoVertexMeshKeptDrawable() {
    hfm::Model model;
    hfm::Mesh mesh = makeSquare();
    mesh.vertices.clear();
    mesh.clusterIndices = { 0, 1, 2, 3 };
    mesh.clusterWeights = { 1, 1, 1, 1 };
    mesh.originalIndices = { 0, 1, 2, 3 };
    hfm::sanitizeMesh(mesh);
    model.meshes.push_back(mesh);

    QCOMPARE(hfm::keepMeshesDrawable(model), 1);

    const hfm::Mesh& kept = model.meshes[0];
    QCOMPARE(kept.vertices.size(), 1);
    QCOMPARE(kept.vertices[0], glm::vec3(0.0f));
    QVERIFY(kept.normals.isEmpty());
    QVERIFY(kept.texCoords.isEmpty());
    QVERIFY(kept.clusterIndices.empty());
    QVERIFY(kept.clusterWeights.empty());
    QVERIFY(kept.originalIndices.isEmpty());
    QCOMPARE(kept.blendshapes.size(), 1);
    QVERIFY(kept.blendshapes[0].indices.isEmpty());
    QCOMPARE(kept.parts.size(), (size_t)1);
    verifyHoldsTogether(kept);
    verifyDrawable(kept);
}

// A mesh the baker can draw is left alone, even with one part of no face beside a good one (the drawable is made); a
// mesh with no part at all has nothing to draw and no draw item either.
void ModelSanitizeTests::testDrawableMeshesUntouched() {
    hfm::Model model;
    model.meshes.push_back(makeSquare());
    hfm::Mesh twoParts = makeSquare();
    twoParts.parts.push_back(hfm::MeshPart());
    model.meshes.push_back(twoParts);
    hfm::Mesh noParts = makeSquare();
    noParts.parts.clear();
    model.meshes.push_back(noParts);
    hfm::Mesh noVertexNoParts;
    model.meshes.push_back(noVertexNoParts);
    const auto before = model.meshes[0].parts[0].triangleIndices;
    const int* data = before.constData();

    QCOMPARE(hfm::keepMeshesDrawable(model), 0);

    QCOMPARE(model.meshes[0].parts[0].triangleIndices, before);
    QCOMPARE(model.meshes[0].parts[0].triangleIndices.constData(), data);  // read, not copied
    QCOMPARE(model.meshes[1].parts.size(), (size_t)2);
    QVERIFY(model.meshes[1].parts[1].triangleIndices.isEmpty());
    QVERIFY(model.meshes[2].parts.empty());
    QVERIFY(model.meshes[3].vertices.isEmpty());
}

