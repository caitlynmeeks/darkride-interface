//
//  AssimpSkinTests.h
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_AssimpSkinTests_h
#define hifi_AssimpSkinTests_h

#include <QtTest/QtTest>

class AssimpSkinTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void testSharedVertexBuffer();
    void testWeightsSumToOne();
    void testSkinPerMesh();
    void testUnpaddedJsonChunk();
    void testMorphTargetPastVertices();
    void testPointsOnlyMeshKeptDrawable();
};

#endif // hifi_AssimpSkinTests_h
