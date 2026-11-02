//
//  EntityContentAddressesTests.h
//  tests/entity-content/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_EntityContentAddressesTests_h
#define hifi_EntityContentAddressesTests_h

#include <QtTest/QtTest>

class EntityContentAddressesTests : public QObject {
    Q_OBJECT
private slots:
    void testThisComputer();
    void testPathOnThisComputer();
    void testClearsProperties();
    void testPartialEditUntouched();
    void testKeepsFetchedAndSpecialValues();
    void testTextures();
    void testTexturesReadAsRenderer();
    void testMaterialData();
    void testImpostorEntry();
    void testProceduralShaders();
};

#endif  // hifi_EntityContentAddressesTests_h
