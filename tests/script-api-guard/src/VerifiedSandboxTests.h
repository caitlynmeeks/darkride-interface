//
//  VerifiedSandboxTests.h
//  tests/script-api-guard
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_VerifiedSandboxTests_h
#define hifi_VerifiedSandboxTests_h

#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

class WindowScriptingInterface : public QObject {
    Q_OBJECT
};

class TabletProxy : public QObject {
    Q_OBJECT
};

class VerifiedSandboxTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void testClaims();
    void testClientEngineIgnoresClaims();
    void testInclude();
    void testRequire();
    void testFetch();
    void testHttpRequest();
    void testWebSocket();
    void testWorldRule();
    void testWorldRuleOnRequests();
    void testRefusalAtLoad();
    void testEntityScriptDownloadIsRuled();
    void testAppScriptsFolder();
    void testAppScriptsForAWorldFromTheNetwork();
    void testResourcesNotForWorlds();
    void testInputAndPrivacyNotForWorlds();
    void testSignalsNotForWorlds();
    void testWorldScriptStartsNoScript();
    void testServerEnginesAreRuled();
    void testWorldMappingDestinations();

private:
    QTemporaryDir _dir;
    QString _root;      // the folder's canonical path (macOS's temporary folder is behind a link)
};

#endif // hifi_VerifiedSandboxTests_h
