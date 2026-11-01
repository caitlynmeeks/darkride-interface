//
//  SettingKeyTests.cpp
//  tests/shared/src
//
//  Created by scripting on 2026-11-01.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "SettingKeyTests.h"

#include <SettingKey.h>

QTEST_MAIN(SettingKeyTests)

void SettingKeyTests::normalizedKey_data() {
    // a key as QSettings stores it: what a check of the key must look at
    QTest::addColumn<QString>("written");
    QTest::addColumn<QString>("stored");
    QTest::newRow("plain") << "RunningScripts" << "RunningScripts";
    QTest::newRow("leading slash") << "/RunningScripts" << "RunningScripts";
    QTest::newRow("trailing slash") << "RunningScripts/" << "RunningScripts";
    QTest::newRow("double leading") << "//RunningScripts" << "RunningScripts";
    QTest::newRow("doubled inside") << "private//domainIdSecret" << "private/domainIdSecret";
    QTest::newRow("all of them") << "/private//domainIdSecret/" << "private/domainIdSecret";
    QTest::newRow("backslash") << "private\\domainIdSecret" << "private/domainIdSecret";
    QTest::newRow("leading backslash") << "\\Avatar\\fullAvatarURL" << "Avatar/fullAvatarURL";
    QTest::newRow("mixed") << "/\\private/\\x" << "private/x";
    QTest::newRow("slashes only") << "//" << "";
    QTest::newRow("empty") << "" << "";
}

void SettingKeyTests::normalizedKey() {
    QFETCH(QString, written);
    QFETCH(QString, stored);
    QCOMPARE(Setting::normalizedKey(written), stored);
}

void SettingKeyTests::neverToScripts() {
    // refused to every script, however it is written
    for (const char* key : { "private/domainIdSecret", "/private//domainIdSecret/", "PRIVATE\\DomainIdSecret",
                             "\\private\\domainidsecret" }) {
        QVERIFY2(Setting::neverToScripts(key), key);
    }
    for (const char* key : { "private/other", "domainIdSecret", "private/domainIdSecretX", "RunningScripts" }) {
        QVERIFY2(!Setting::neverToScripts(key), key);
    }
}

void SettingKeyTests::refusedProtected() {
    // a world's script: always refused, whether or not the network rule holds it (a world on this machine's network)
    QVERIFY(Setting::refusedProtected(true, false));
    QVERIFY(Setting::refusedProtected(true, true));
    // the visitor's own scripts: only while the network rule holds them
    QVERIFY(Setting::refusedProtected(false, true));
    QVERIFY(!Setting::refusedProtected(false, false));
}
