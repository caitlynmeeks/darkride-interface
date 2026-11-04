//
//  ScriptNetworkPolicyTests.cpp
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ScriptNetworkPolicyTests.h"
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QHostInfo>

#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>

#include <memory>

#include <PathUtils.h>
#include <ResourceManager.h>
#include <ResourceRequest.h>
#include <ScriptCaller.h>
#include <ScriptNetworkPolicy.h>

QTEST_MAIN(ScriptNetworkPolicyTests)

void ScriptNetworkPolicyTests::privateAddresses_data() {
    QTest::addColumn<QString>("address");
    QTest::addColumn<bool>("isPrivate");
    QTest::newRow("loopback") << "127.0.0.1" << true;
    QTest::newRow("loopback, another") << "127.3.4.5" << true;
    QTest::newRow("this network") << "0.0.0.0" << true;
    QTest::newRow("10/8") << "10.20.30.40" << true;
    QTest::newRow("172.16/12 low") << "172.16.0.1" << true;
    QTest::newRow("172.16/12 high") << "172.31.255.254" << true;
    QTest::newRow("just past 172.16/12") << "172.32.0.1" << false;
    QTest::newRow("192.168/16") << "192.168.1.1" << true;
    QTest::newRow("link-local, cloud metadata") << "169.254.169.254" << true;
    QTest::newRow("carrier-grade NAT") << "100.64.0.1" << true;
    QTest::newRow("just past carrier-grade NAT") << "100.128.0.1" << false;
    QTest::newRow("benchmarking") << "198.18.0.1" << true;
    QTest::newRow("multicast") << "224.0.0.1" << true;
    QTest::newRow("broadcast") << "255.255.255.255" << true;
    QTest::newRow("public") << "8.8.8.8" << false;
    QTest::newRow("public, another") << "46.224.99.35" << false;
    QTest::newRow("IPv6 loopback") << "::1" << true;
    QTest::newRow("IPv6 unspecified") << "::" << true;
    QTest::newRow("IPv6 link-local") << "fe80::1" << true;
    QTest::newRow("IPv6 unique local") << "fd12:3456::1" << true;
    QTest::newRow("IPv6 site-local") << "fec0::1" << true;
    QTest::newRow("IPv6 multicast") << "ff02::1" << true;
    QTest::newRow("IPv4-mapped private") << "::ffff:10.0.0.1" << true;
    QTest::newRow("IPv4-mapped public") << "::ffff:8.8.8.8" << false;
    QTest::newRow("NAT64 private") << "64:ff9b::7f00:1" << true;
    QTest::newRow("NAT64 public") << "64:ff9b::808:808" << false;
    QTest::newRow("IPv6 public") << "2606:4700::6810:84e5" << false;
}

void ScriptNetworkPolicyTests::privateAddresses() {
    QFETCH(QString, address);
    QFETCH(bool, isPrivate);
    QHostAddress parsed(address);
    QVERIFY2(!parsed.isNull(), "the test's address did not parse");
    QCOMPARE(ScriptNetworkPolicy::isPrivateAddress(parsed), isPrivate);
}

void ScriptNetworkPolicyTests::localHostNames_data() {
    QTest::addColumn<QString>("host");
    QTest::addColumn<bool>("isLocal");
    QTest::newRow("localhost") << "localhost" << true;
    QTest::newRow("LOCALHOST") << "LOCALHOST" << true;
    QTest::newRow("under .localhost") << "app.localhost" << true;
    QTest::newRow(".local") << "printer.local" << true;
    QTest::newRow(".lan") << "nas.lan" << true;
    QTest::newRow(".home.arpa") << "router.home.arpa" << true;
    QTest::newRow("no dot") << "nas" << true;
    QTest::newRow("empty") << "" << true;
    QTest::newRow("public") << "api.example.com" << false;
    QTest::newRow("public with local-looking label") << "local.example.com" << false;
}

void ScriptNetworkPolicyTests::localHostNames() {
    QFETCH(QString, host);
    QFETCH(bool, isLocal);
    QCOMPARE(ScriptNetworkPolicy::isLocalHostName(host), isLocal);
}

void ScriptNetworkPolicyTests::localOrigins_data() {
    QTest::addColumn<QString>("origin");
    QTest::addColumn<bool>("isLocal");
    QTest::newRow("a file") << "file:///Users/someone/scripts/test.js" << true;
    QTest::newRow("a resource of the app") << "qrc:///scripts/system/x.js" << true;
    QTest::newRow("a bare path") << "/Users/someone/scripts/test.js" << false;    // not a URL: the engines give file URLs
    QTest::newRow("an embedded entity script's code") << "(function () { print('x'); })" << false;
    QTest::newRow("an embedded entity script") << "about:EmbeddedEntityScript" << false;
    QTest::newRow("served from this machine") << "http://127.0.0.1:8080/test.js" << true;
    QTest::newRow("served from localhost") << "http://localhost:8080/test.js" << true;
    QTest::newRow("served from the home network") << "http://192.168.1.10/test.js" << true;
    QTest::newRow("served from the internet") << "https://cdn.example.com/worlds/x.js" << false;
    QTest::newRow("served from a public address") << "http://46.224.99.35/x.js" << false;
    QTest::newRow("the world's asset server") << "atp:/scripts/x.js" << false;
    QTest::newRow("an entities engine itself") << "about:Entities" << false;
    QTest::newRow("unknown") << "" << false;
}

void ScriptNetworkPolicyTests::localOrigins() {
    QFETCH(QString, origin);
    QFETCH(bool, isLocal);
    QCOMPARE(ScriptNetworkPolicy::isLocalOrigin(QUrl(origin)), isLocal);
}

void ScriptNetworkPolicyTests::requestsFromTheNetwork_data() {
    QTest::addColumn<QString>("target");
    QTest::addColumn<bool>("allowed");
    QTest::addColumn<bool>("needsLookup");
    QTest::newRow("a public name") << "https://api.example.com/v1" << false << true;
    QTest::newRow("a public address") << "https://8.8.8.8/" << true << false;
    QTest::newRow("a public websocket") << "wss://api.example.com/socket" << false << true;
    QTest::newRow("the asset server") << "atp:/sounds/x.wav" << true << false;
    QTest::newRow("a local file") << "file:///etc/hosts" << false << false;
    QTest::newRow("ftp") << "ftp://example.com/x" << false << false;
    QTest::newRow("data") << "data:text/plain,hello" << false << false;
    QTest::newRow("no scheme") << "/api/x" << false << false;
    QTest::newRow("loopback") << "http://127.0.0.1:8798/mood" << false << false;
    QTest::newRow("IPv6 loopback") << "http://[::1]:8080/" << false << false;
    QTest::newRow("the home network") << "http://192.168.1.1/" << false << false;
    QTest::newRow("link-local") << "http://169.254.169.254/latest/meta-data/" << false << false;
    QTest::newRow("localhost") << "ws://localhost:43100/" << false << false;
    QTest::newRow("a .local name") << "http://printer.local/" << false << false;
    QTest::newRow("a name without a dot") << "http://nas/" << false << false;
    QTest::newRow("a decimal address") << "http://2130706433/" << false << false;    // QUrl reads it as 127.0.0.1
}

void ScriptNetworkPolicyTests::requestsFromTheNetwork() {
    QFETCH(QString, target);
    QFETCH(bool, allowed);
    QFETCH(bool, needsLookup);
    const QUrl origin("https://cdn.example.com/worlds/x.js");
    const auto decision = ScriptNetworkPolicy::check(QUrl(target), origin);
    QCOMPARE(decision.allowed, allowed);
    QCOMPARE(decision.needsLookup, needsLookup);
    if (!allowed && !needsLookup) {
        QVERIFY2(!decision.reason.isEmpty(), "a refusal needs a reason");
    }
}

void ScriptNetworkPolicyTests::requestsFromLocalScripts() {
    // a local script keeps every address, the local ones and the files included; nothing is looked up first
    const QStringList origins { "file:///Users/someone/test.js", "http://127.0.0.1:8080/test.js",
                                "http://192.168.1.10/test.js" };
    const QStringList targets { "http://127.0.0.1:8798/mood", "ws://localhost:43100/", "file:///etc/hosts",
                                "https://api.example.com/v1", "http://192.168.1.1/" };
    for (const QString& origin : origins) {
        for (const QString& target : targets) {
            const auto decision = ScriptNetworkPolicy::check(QUrl(target), QUrl(origin));
            QVERIFY2(decision.allowed && !decision.needsLookup,
                     qPrintable(origin + " -> " + target + " should be allowed at once"));
        }
    }
    // the same requests from a script on the network
    for (const QString& target : targets) {
        const auto decision = ScriptNetworkPolicy::check(QUrl(target), QUrl("https://cdn.example.com/x.js"));
        QVERIFY2(!decision.allowed, qPrintable(target + " should not be allowed at once from the network"));
    }
}

void ScriptNetworkPolicyTests::resolvedAddresses() {
    QVERIFY(ScriptNetworkPolicy::checkAddresses({ QHostAddress("93.184.216.34") }).allowed);
    QVERIFY(ScriptNetworkPolicy::checkAddresses({ QHostAddress("93.184.216.34"), QHostAddress("2606:2800:220:1::1") }).allowed);
    // one private address among public ones refuses the name (a name may be made to answer with both)
    const auto mixed = ScriptNetworkPolicy::checkAddresses({ QHostAddress("93.184.216.34"), QHostAddress("127.0.0.1") });
    QVERIFY(!mixed.allowed);
    QVERIFY(mixed.reason.contains("127.0.0.1"));
    QVERIFY(!ScriptNetworkPolicy::checkAddresses({}).allowed);
}

void ScriptNetworkPolicyTests::lookupOfAPublicName() {
    // decide() answers at once without a lookup, and later with one; here a name that is an address already
    bool called = false;
    ScriptNetworkPolicy::Decision got;
    ScriptNetworkPolicy::decide(QUrl("https://8.8.8.8/"), QUrl("https://cdn.example.com/x.js"), this,
                                [&](const ScriptNetworkPolicy::Decision& decision) { called = true; got = decision; });
    QVERIFY(called);
    QVERIFY(got.allowed);
    // localhost by name resolves to loopback: refused after the lookup, on this thread
    called = false;
    QEventLoop loop;
    ScriptNetworkPolicy::decide(QUrl("http://127.0.0.1.nip.io/"), QUrl("https://cdn.example.com/x.js"), this,
                                [&](const ScriptNetworkPolicy::Decision& decision) { called = true; got = decision; loop.quit(); });
    if (!called) {
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (!called) {
        QSKIP("no name lookup possible here (offline)");
    }
    QVERIFY(!got.allowed);     // either the lookup failed (refused) or it led to 127.0.0.1 (refused)
}

void ScriptNetworkPolicyTests::blockingDecisions() {
    QString reason;
    QVERIFY(ScriptNetworkPolicy::decideBlocking(QUrl("http://127.0.0.1:8080/x.json"), QUrl("file:///Users/someone/test.js"), reason));
    QVERIFY(!ScriptNetworkPolicy::decideBlocking(QUrl("http://127.0.0.1:8080/x.json"), QUrl("https://cdn.example.com/x.js"), reason));
    QVERIFY(!reason.isEmpty());
    QVERIFY(ScriptNetworkPolicy::decideBlocking(QUrl("https://8.8.8.8/x.json"), QUrl("https://cdn.example.com/x.js"), reason));
    QVERIFY(!ScriptNetworkPolicy::decideBlocking(QUrl("file:///etc/hosts"), QUrl("https://cdn.example.com/x.js"), reason));
    // a name: looked up here, on this thread; refused when it leads home or cannot be found
    QVERIFY(!ScriptNetworkPolicy::decideBlocking(QUrl("http://127.0.0.1.nip.io/"), QUrl("https://cdn.example.com/x.js"), reason));
}

// The app's own scripts: qrc, or a file in the app's scripts folder once both paths are canonical, so that a path which
// only starts in the folder, however it is spelled, doesn't count.
void ScriptNetworkPolicyTests::appScripts() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QVERIFY(QDir(root).mkpath("app/scripts/sub"));
    QVERIFY(QDir(root).mkpath("app/scripts-other"));
    QVERIFY(QDir(root).mkpath("outside"));
    for (const QString& name : { "app/scripts/x.js", "app/scripts/sub/y.js", "app/scripts-other/x.js", "outside/z.js" }) {
        QFile file(root + "/" + name);
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    QVERIFY(QFile::link(root + "/outside", root + "/app/scripts/link"));
    const QString folder = root + "/app/scripts";
    PathUtils::defaultScriptsLocation(folder);
    auto file = [](const QString& path) { return QUrl::fromLocalFile(path); };

    QVERIFY(ScriptNetworkPolicy::isAppScript(QUrl("qrc:///scripts/x.js")));
    QVERIFY(ScriptNetworkPolicy::isAppScript(file(folder)));
    QVERIFY(ScriptNetworkPolicy::isAppScript(file(folder + "/x.js")));
    QVERIFY(ScriptNetworkPolicy::isAppScript(file(folder + "/sub/y.js")));
    QVERIFY(ScriptNetworkPolicy::isAppScript(file(folder + "/sub/../x.js")));
    QVERIFY(ScriptNetworkPolicy::isAppScript(file(folder + "/not-there.js")));

    QVERIFY(!ScriptNetworkPolicy::isAppScript(file(folder + "/../../outside/z.js")));
    QVERIFY(!ScriptNetworkPolicy::isAppScript(file(folder + "/sub/../../../outside/z.js")));
    QVERIFY(!ScriptNetworkPolicy::isAppScript(file(folder + "/../../outside/not-there.js")));
    QVERIFY(!ScriptNetworkPolicy::isAppScript(QUrl("file://" + folder + "/%2e%2e/%2E%2E/outside/z.js")));
    QVERIFY(!ScriptNetworkPolicy::isAppScript(QUrl("file://" + folder + "/..%2f..%2foutside/z.js")));
    QVERIFY(!ScriptNetworkPolicy::isAppScript(file(folder + "/..\\..\\outside\\z.js")));
    QVERIFY(!ScriptNetworkPolicy::isAppScript(QUrl("file://" + folder + "/..%5c..%5coutside%5cz.js")));
    QVERIFY(!ScriptNetworkPolicy::isAppScript(file(folder + "/link/z.js")));        // a link out of the folder
    QVERIFY(!ScriptNetworkPolicy::isAppScript(file(root + "/app/scripts-other/x.js")));   // a folder beside it, same start
    QVERIFY(!ScriptNetworkPolicy::isAppScript(file(root + "/outside/z.js")));
    QVERIFY(!ScriptNetworkPolicy::isAppScript(QUrl("https://example.invalid/scripts/x.js")));
    QVERIFY(!ScriptNetworkPolicy::isAppScript(QUrl("http://127.0.0.1/x.js")));
    QVERIFY(!ScriptNetworkPolicy::isAppScript(QUrl()));
}

void ScriptNetworkPolicyTests::testRequestsGoToTheCheckedAddress() {
    // a name's answers: all public, the first is the address; one private among them refuses the name
    const QHostAddress publicV4("93.184.216.34"), publicV6("2606:2800:220:1::1");
    ScriptNetworkPolicy::Decision decision = ScriptNetworkPolicy::checkAddresses({ publicV4, publicV6 });
    QVERIFY(decision.allowed);
    QCOMPARE(decision.address, publicV4);
    QVERIFY(!ScriptNetworkPolicy::checkAddresses({ publicV4, QHostAddress("127.0.0.1") }).allowed);
    QVERIFY(!ScriptNetworkPolicy::checkAddresses({ publicV6, QHostAddress("::ffff:192.168.1.1") }).allowed);
    QVERIFY(!ScriptNetworkPolicy::checkAddresses({ QHostAddress("fe80::1") }).allowed);

    // pinned: the address in the URL, the name in the Host header (with a port that isn't the default) and for the
    // certificate and the handshake's name
    QNetworkRequest https(QUrl("https://example.com:8443/a?b=1"));
    ScriptNetworkPolicy::pin(https, decision);
    QCOMPARE(https.url().toString(), QString("https://93.184.216.34:8443/a?b=1"));
    QCOMPARE(https.rawHeader("Host"), QByteArray("example.com:8443"));
    QCOMPARE(https.peerVerifyName(), QString("example.com"));
    ScriptNetworkPolicy::Decision v6 = ScriptNetworkPolicy::checkAddresses({ publicV6 });
    QNetworkRequest http(QUrl("http://example.com/"));
    ScriptNetworkPolicy::pin(http, v6);
    QCOMPARE(http.url().toString(), QString("http://[2606:2800:220:1::1]/"));
    QCOMPARE(http.rawHeader("Host"), QByteArray("example.com"));
    QVERIFY(http.peerVerifyName().isEmpty());
    // an address, a refusal, or a judgement without a lookup leave the request as it is
    QNetworkRequest literal(QUrl("http://93.184.216.34/"));
    ScriptNetworkPolicy::pin(literal, decision);
    QCOMPARE(literal.url().toString(), QString("http://93.184.216.34/"));
    QVERIFY(!literal.hasRawHeader("Host"));
    QNetworkRequest unjudged(QUrl("https://example.com/"));
    ScriptNetworkPolicy::pin(unjudged, ScriptNetworkPolicy::Decision());
    QCOMPARE(unjudged.url().toString(), QString("https://example.com/"));
}

void ScriptNetworkPolicyTests::testPinnedNameIsCheckedOnEveryRequest() {
    // two requests to one address, the second under a name its certificate doesn't carry: the connection the first
    // left open is not reused for it, and its certificate check fails (the connection cache is keyed on the name)
    const QHostInfo info = QHostInfo::fromName("example.com");
    if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
        QSKIP("no network");
    }
    const ScriptNetworkPolicy::Decision decision = ScriptNetworkPolicy::checkAddresses(info.addresses());
    QVERIFY(decision.allowed);
    QNetworkAccessManager manager;
    auto fetch = [&manager](QNetworkRequest request) {
        QNetworkReply* reply = manager.get(request);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(15000, &loop, &QEventLoop::quit);
        loop.exec();
        const QNetworkReply::NetworkError error = reply->isFinished() ? reply->error() : QNetworkReply::TimeoutError;
        reply->deleteLater();
        return error;
    };
    QNetworkRequest good(QUrl("https://example.com/"));
    ScriptNetworkPolicy::pin(good, decision);
    const QNetworkReply::NetworkError first = fetch(good);
    if (first == QNetworkReply::TimeoutError || first == QNetworkReply::HostNotFoundError) {
        QSKIP("example.com not reachable");
    }
    QCOMPARE(first, QNetworkReply::NoError);
    QNetworkRequest wrong(QUrl("https://example.com/"));
    ScriptNetworkPolicy::pin(wrong, decision);
    wrong.setPeerVerifyName("not-example.invalid");
    QCOMPARE(fetch(wrong), QNetworkReply::SslHandshakeFailedError);
}

void ScriptNetworkPolicyTests::everyWebRequestRuled() {
    // a process whose web requests are all its scripts' (the entity script server): every http, https and ftp request
    // is held to the rule as from the network; its own files are not web requests
    ResourceManager manager(false);
    const QUrl origin("about:entity-server");
    auto made = [&manager](const QString& url) {
        return std::unique_ptr<ResourceRequest>(manager.createResourceRequest(nullptr, QUrl(url), false));
    };
    QVERIFY(!made("http://127.0.0.1:1/sound.wav")->isNetworkRuled());
    manager.ruleEveryWebRequest(origin);
    for (const char* url : { "http://127.0.0.1:1/sound.wav", "https://example.com/script.js", "ftp://example.com/a.wav" }) {
        auto request = made(url);
        QVERIFY2(request && request->isNetworkRuled(), url);
        QCOMPARE(request->networkRuleOrigin(), origin);
    }
    QVERIFY(!made(QUrl::fromLocalFile(QDir::tempPath() + "/a.wav").toString())->isNetworkRuled());
}

void ScriptNetworkPolicyTests::worldLeft() {
    // security #32: a script of a world that was left reaches nothing more, wherever it came from (a local one too,
    // which keeps every address while its world is there)
    const int LEFT = 9301;
    const QUrl target("https://example.com/data");
    const QUrl local("file:///tmp/world/script.js");
    QVERIFY(ScriptNetworkPolicy::check(target, local, true).allowed);
    ScriptCaller::retireWorld(LEFT);
    {
        ScriptCaller::WorldScope world(LEFT);
        for (const QUrl& origin : { local, QUrl("https://cdn.example.com/x.js") }) {
            const auto decision = ScriptNetworkPolicy::check(target, origin, true);
            QVERIFY(!decision.allowed);
            QVERIFY(!decision.needsLookup);
            QVERIFY(decision.reason.contains("was left"));
        }
        QString reason;
        QVERIFY(!ScriptNetworkPolicy::decideBlocking(target, local, reason, true));
        QVERIFY(reason.contains("was left"));
    }
    {
        ScriptCaller::WorldScope world(LEFT + 1);
        QVERIFY(ScriptNetworkPolicy::check(target, local, true).allowed);
    }
}
