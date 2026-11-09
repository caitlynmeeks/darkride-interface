//
//  EditFilterTests.h
//  tests/edit-filters/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_EditFilterTests_h
#define hifi_EditFilterTests_h

#include <QtCore/QObject>
#include <QtTest/QtTest>

class EditFilterTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void testFilters();
    void testRunawayFilterOnWebAssembly();
    void testFilterScriptObject();
};

#endif // hifi_EditFilterTests_h
