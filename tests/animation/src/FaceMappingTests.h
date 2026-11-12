//
//  FaceMappingTests.h
//
//  Copyright 2026 Darkride.
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_FaceMappingTests_h
#define hifi_FaceMappingTests_h

#include <QtTest/QtTest>

class FaceMappingTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void testWithoutFst();
    void testFstMapsAFew();
    void testModelEntity();
};

#endif // hifi_FaceMappingTests_h
