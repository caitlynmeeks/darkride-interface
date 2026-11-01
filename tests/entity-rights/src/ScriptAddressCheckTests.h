//
//  ScriptAddressCheckTests.h
//  tests/entity-rights/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_ScriptAddressCheckTests_h
#define hifi_ScriptAddressCheckTests_h

#include <QtTest/QtTest>

class ScriptAddressCheckTests : public QObject {
    Q_OBJECT
private slots:
    void testValues_data();
    void testValues();
};

#endif  // hifi_ScriptAddressCheckTests_h
