//
//  SettingKeyTests.h
//  tests/shared/src
//
//  Created by scripting on 2026-11-01.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_SettingKeyTests_h
#define hifi_SettingKeyTests_h

#include <QtTest/QtTest>

class SettingKeyTests : public QObject {
    Q_OBJECT
private slots:
    void normalizedKey_data();
    void normalizedKey();
    void neverToScripts();
};

#endif // hifi_SettingKeyTests_h
