//
//  EntityRightsTests.h
//  tests/entity-rights/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_EntityRightsTests_h
#define hifi_EntityRightsTests_h

#include <QtTest/QtTest>

class EntityRightsTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void testEdits();
    void testDeletes();
    void testCertificateClaims();
    void testScriptAddresses();
};

#endif  // hifi_EntityRightsTests_h
