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
    return mesh;
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
    QCOMPARE(mesh.parts[0].triangleIndices, copy.parts[0].triangleIndices);
    QCOMPARE(mesh.parts[0].quadIndices, copy.parts[0].quadIndices);
    QCOMPARE(mesh.parts[0].quadTrianglesIndices, copy.parts[0].quadTrianglesIndices);
    QCOMPARE(mesh.normals.size(), 4);
    QCOMPARE(mesh.texCoords.size(), 4);
    // only read: a mesh that holds together is never copied
    QVERIFY(mesh.parts[0].triangleIndices.isSharedWith(copy.parts[0].triangleIndices));
    QVERIFY(mesh.parts[0].quadIndices.isSharedWith(copy.parts[0].quadIndices));
    QVERIFY(mesh.parts[0].quadTrianglesIndices.isSharedWith(copy.parts[0].quadTrianglesIndices));
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

    // no vertices at all: every face goes
    hfm::Mesh empty = makeSquare();
    empty.vertices.clear();
    QCOMPARE(hfm::sanitizeMesh(empty).facesRemoved, 2 + 1 + 2);
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

    const HFMModel::Pointer model = FBXSerializer().read(FBX_TEXT, hifi::VariantHash(), QUrl("file:///cut-short.fbx"));

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

    const HFMModel::Pointer model = OBJSerializer().read(OBJ_TEXT, hifi::VariantHash(), QUrl());

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
