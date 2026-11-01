//
//  UrlPrefixTests.cpp
//  tests/networking/src
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "UrlPrefixTests.h"

#include <DependencyManager.h>
#include <ResourceManager.h>

QTEST_GUILESS_MAIN(UrlPrefixTests)

namespace {
const QString PREFIX = "https://cdn.example.invalid/avatars/model-1/";
const QString REPLACEMENT = "file:///app/resources/avatars/model/";
const QString BARE_PREFIX = "https://cdn.example.invalid/avatars/model-2";
const QString BARE_REPLACEMENT = "file:///app/resources/avatars/model2";
}

void UrlPrefixTests::initTestCase() {
    DependencyManager::set<ResourceManager>(false);
    DependencyManager::get<ResourceManager>()->setUrlPrefixOverride(PREFIX, REPLACEMENT);
    DependencyManager::get<ResourceManager>()->setUrlPrefixOverride(BARE_PREFIX, BARE_REPLACEMENT);
}

void UrlPrefixTests::prefixes_data() {
    QTest::addColumn<QString>("rest");      // after the prefix
    QTest::addColumn<bool>("replaced");
    QTest::newRow("a file") << "model.fst" << true;
    QTest::newRow("a file in a folder") << "textures/skin.png" << true;
    QTest::newRow("down and back") << "textures/../model.fst" << true;
    QTest::newRow("dots") << "./textures/./skin.png" << true;
    QTest::newRow("a query that climbs") << "model.fst?x=../../../etc/hosts" << true;
    QTest::newRow("a fragment that climbs") << "model.fst#../../../etc/hosts" << true;
    QTest::newRow("a name with dots") << "..skin..png" << true;
    QTest::newRow("up") << "../model.fst" << false;
    QTest::newRow("down and up past it") << "textures/../../model.fst" << false;
    QTest::newRow("far up") << "../../../../../../etc/hosts" << false;
    QTest::newRow("percent dots") << "%2e%2e/%2E%2E/etc/hosts" << false;
    QTest::newRow("percent slash") << "..%2f..%2fetc%2fhosts" << false;
    QTest::newRow("backslash") << "..\\..\\etc\\hosts" << false;
    QTest::newRow("percent backslash") << "..%5c..%5cetc%5chosts" << false;
}

void UrlPrefixTests::prefixes() {
    QFETCH(QString, rest);
    QFETCH(bool, replaced);
    const QString url = PREFIX + rest;
    const QString normalized = DependencyManager::get<ResourceManager>()->normalizeURL(url);
    QCOMPARE(normalized, replaced ? REPLACEMENT + rest : url);
    // another address is never touched
    QCOMPARE(DependencyManager::get<ResourceManager>()->normalizeURL("https://elsewhere.invalid/" + rest),
             "https://elsewhere.invalid/" + rest);
}

// a prefix without a closing slash maps its own name, not a sibling that starts the same way
void UrlPrefixTests::prefixWithoutSlash() {
    auto resources = DependencyManager::get<ResourceManager>();
    for (const QString& rest : { QString(), QString("/model.fst"), QString("?v=2"), QString("#top") }) {
        QCOMPARE(resources->normalizeURL(BARE_PREFIX + rest), BARE_REPLACEMENT + rest);
    }
    for (const QString& rest : { QString("-other/model.fst"), QString("x/model.fst"), QString("/../model.fst") }) {
        QCOMPARE(resources->normalizeURL(BARE_PREFIX + rest), BARE_PREFIX + rest);
    }
}

// what is on this machine is never mapped elsewhere; the other way round, and local to local, is
void UrlPrefixTests::localNeverRemote() {
    auto resources = DependencyManager::get<ResourceManager>();
    const QStringList refused { "file:///app/scripts/", "qrc:///scripts/", "/app/scripts/", "c:/app/scripts/", "", "fi" };
    for (const QString& prefix : refused) {
        resources->setUrlPrefixOverride(prefix, "https://example.invalid/x/");
    }
    for (const QString& address : { "file:///app/scripts/a.js", "qrc:///scripts/a.js", "/app/scripts/a.js", "c:/app/scripts/a.js" }) {
        QCOMPARE(resources->normalizeURL(address), QString(address));
    }
    resources->setUrlPrefixOverride("atp:/", "file:///app/test/");
    QCOMPARE(resources->normalizeURL("atp:/a.fst"), QString("file:///app/test/a.fst"));
    resources->setUrlPrefixOverride("file:///app/one/", "file:///app/two/");
    QCOMPARE(resources->normalizeURL("file:///app/one/a.js"), QString("file:///app/two/a.js"));
    resources->setUrlPrefixOverride("atp:/", QString());
    resources->setUrlPrefixOverride("file:///app/one/", QString());
}
