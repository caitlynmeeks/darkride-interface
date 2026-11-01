//
//  ModelSanitizeTests.h
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_ModelSanitizeTests_h
#define hifi_ModelSanitizeTests_h

#include <QtTest/QtTest>

class ModelSanitizeTests : public QObject {
    Q_OBJECT
private slots:
    void testGoodMeshUntouched();
    void testBadFacesRemoved();
    void testFbxPolygonCutShort();
    void testObjFacePastVertices();
};

#endif // hifi_ModelSanitizeTests_h
