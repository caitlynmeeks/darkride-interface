//
//  SignInRefusedTests.cpp
//  tests/networking/src
//
//  Created by servers on 2026-11-11.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "SignInRefusedTests.h"

#include <memory>

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <AccountManager.h>
#include <DataServerAccountInfo.h>
#include <NetworkAccessManager.h>

QTEST_MAIN(SignInRefusedTests)

namespace {

const int WAIT_MS = 5000;
const int SETTLE_MS = 400;  // time for anything that would follow an answer to go out

const QByteArray PROFILE = "/api/v1/user/profile";
const QByteArray TOKEN = "/oauth/token";

// the metaverse's own answers, as its guards and filters write them
const QByteArray REFUSED = R"({"statusCode":401,"message":"Unauthorized"})";
const QByteArray UNVERIFIED_GUARD = R"({"message":"Unverified","error":"Unauthorized","reason":"unverified"})";
const QByteArray UNVERIFIED_FILTER = R"({"message":"Verify your email address to do that.","reason":"unverified"})";
const QByteArray INVALID_GRANT = R"({"error":"invalid_grant","error_description":"Invalid refresh token"})";
const QByteArray NEW_TOKEN =
    R"({"access_token":"tok-2","refresh_token":"ref-2","expires_in":2592000,"token_type":"Bearer"})";

struct Answer {
    int status { 404 };
    QByteArray body { "{}" };
    bool drop { false };  // close without an answer
};

struct Seen {
    QByteArray method;
    QByteArray path;
    QByteArray authorization;
};

// A stand-in metaverse on 127.0.0.1: answers each path from its list in order (the last one repeats), one request a
// connection, and remembers every request it saw.
class StandIn {
public:
    StandIn() {
        _server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&_server, &QTcpServer::newConnection, [this] {
            while (QTcpSocket* socket = _server.nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    const int end = buffer->indexOf("\r\n\r\n");
                    if (end < 0) {
                        return;
                    }
                    const QList<QByteArray> lines = buffer->left(end).split('\n');
                    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
                    Seen seen;
                    seen.method = requestLine.value(0);
                    seen.path = requestLine.value(1).split('?').value(0);
                    int length = 0;
                    for (int i = 1; i < lines.size(); ++i) {
                        const QByteArray line = lines[i].trimmed();
                        const int colon = line.indexOf(':');
                        const QByteArray key = line.left(colon).trimmed().toLower();
                        const QByteArray value = line.mid(colon + 1).trimmed();
                        if (key == "authorization") {
                            seen.authorization = value;
                        } else if (key == "content-length") {
                            length = value.toInt();
                        }
                    }
                    if (buffer->size() < end + 4 + length) {
                        return;
                    }
                    buffer->clear();
                    requests.append(seen);

                    const Answer answer = next(seen.path);
                    if (answer.drop) {
                        socket->abort();
                        return;
                    }
                    socket->write("HTTP/1.1 " + QByteArray::number(answer.status) + " Stand-in\r\n"
                                  "Content-Type: application/json\r\n"
                                  "Content-Length: " + QByteArray::number(answer.body.size()) + "\r\n"
                                  "Connection: close\r\n\r\n" + answer.body);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    QUrl url() const { return QUrl(QString("http://127.0.0.1:%1").arg(_server.serverPort())); }

    int count(const QByteArray& path) const {
        int n = 0;
        for (const Seen& seen : requests) {
            n += seen.path == path ? 1 : 0;
        }
        return n;
    }

    QHash<QByteArray, QList<Answer>> answers;
    QList<Seen> requests;

private:
    Answer next(const QByteArray& path) {
        QList<Answer>& list = answers[path];
        if (list.isEmpty()) {
            return Answer();
        }
        return list.size() > 1 ? list.takeFirst() : list.first();
    }

    QTcpServer _server;
};

Answer answer(int status, const QByteArray& body) {
    Answer a;
    a.status = status;
    a.body = body;
    return a;
}

Answer noAnswer() {
    Answer a;
    a.drop = true;
    return a;
}

// signed in with access token tok-1 (30 days left, so nothing refreshes on its own) and, if asked, refresh token ref-1
std::unique_ptr<AccountManager> signedIn(const StandIn& standIn, bool withRefreshToken = true,
                                         bool accountSettings = false) {
    auto accountManager = std::make_unique<AccountManager>(accountSettings);
    accountManager->setEndsRefusedSignIn(true);
    accountManager->setAuthURL(standIn.url());
    QJsonObject token { { "access_token", "tok-1" }, { "expires_in", 2592000 }, { "token_type", "Bearer" } };
    if (withRefreshToken) {
        token["refresh_token"] = "ref-1";
    }
    DataServerAccountInfo accountInfo;
    accountInfo.setAccessTokenFromJSON(token);
    accountManager->setAccountInfo(accountInfo);
    return accountManager;
}

QString currentToken(AccountManager& accountManager) {
    return accountManager.getAccountInfo().getAccessToken().token;
}

// a request another part of the app makes with a token, to a path of the stand-in: the reply, finished
QNetworkReply* finishedRequest(const QUrl& base, const QByteArray& path, const QByteArray& token) {
    QUrl url = base;
    url.setPath(QString::fromLatin1(path));
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + token);
    QNetworkReply* reply = NetworkAccessManager::getInstance().get(request);
    QElapsedTimer timer;
    timer.start();
    while (!reply->isFinished() && timer.elapsed() < WAIT_MS) {
        QTest::qWait(10);
    }
    return reply;
}

}  // namespace

void SignInRefusedTests::initTestCase() {
    // the account file stays in a folder of this test's own (PathUtils reads it once, at its first use)
    QVERIFY(_appData.isValid());
    qputenv("TIVOLI_APP_DATA_DIR", _appData.path().toLocal8Bit());
}

void SignInRefusedTests::unverifiedStaysSignedIn() {
    // (a) a valid token whose account hasn't verified its email, three times, in both shapes: the sign-in stands
    StandIn standIn;
    standIn.answers[PROFILE] = { answer(401, UNVERIFIED_GUARD), answer(401, UNVERIFIED_FILTER),
                                 answer(401, UNVERIFIED_GUARD) };
    auto accountManager = signedIn(standIn);
    QSignalSpy ended(accountManager.get(), &AccountManager::signInEnded);

    for (int i = 1; i <= 3; ++i) {
        accountManager->requestProfile();
        QTRY_COMPARE_WITH_TIMEOUT(standIn.count(PROFILE), i, WAIT_MS);
        QTest::qWait(SETTLE_MS);
    }
    QCOMPARE(standIn.count(TOKEN), 0);
    QCOMPARE(ended.count(), 0);
    QVERIFY(accountManager->isLoggedIn());
    QCOMPARE(currentToken(*accountManager), QString("tok-1"));
}

void SignInRefusedTests::refusedTokenAndRefreshEnds() {
    // (b) the token refused, then its refresh refused (signed out everywhere, banned): the sign-in ends, and nothing
    // carrying a token goes out after it (the logout's settings upload included)
    StandIn standIn;
    standIn.answers[PROFILE] = { answer(401, REFUSED) };
    standIn.answers[TOKEN] = { answer(400, INVALID_GRANT) };
    auto accountManager = signedIn(standIn, true, true);
    QSignalSpy ended(accountManager.get(), &AccountManager::signInEnded);
    QSignalSpy loggedOut(accountManager.get(), &AccountManager::logoutComplete);

    accountManager->requestProfile();
    QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, WAIT_MS);
    QCOMPARE(ended.at(0).at(0).toString(), QString("signed-out-elsewhere"));
    QCOMPARE(loggedOut.count(), 1);
    QVERIFY(!accountManager->isLoggedIn());
    QVERIFY(currentToken(*accountManager).isEmpty());

    QCOMPARE(standIn.count(PROFILE), 1);
    QCOMPARE(standIn.requests.at(0).authorization, QByteArray("Bearer tok-1"));
    QCOMPARE(standIn.count(TOKEN), 1);
    const int seenAtTheEnd = standIn.requests.size();
    QTest::qWait(SETTLE_MS);
    for (int i = seenAtTheEnd; i < standIn.requests.size(); ++i) {
        const QByteArray what = "a token went out after the end: " + standIn.requests.at(i).method + " " +
                                standIn.requests.at(i).path;
        QVERIFY2(!standIn.requests.at(i).authorization.startsWith("Bearer tok"), what.constData());
    }
}

void SignInRefusedTests::refreshThenUnverifiedStaysSignedIn() {
    // (c) the token refused, the refresh works, and the new token's profile check says only "unverified": still in
    StandIn standIn;
    standIn.answers[PROFILE] = { answer(401, REFUSED), answer(401, UNVERIFIED_GUARD) };
    standIn.answers[TOKEN] = { answer(200, NEW_TOKEN) };
    auto accountManager = signedIn(standIn);
    QSignalSpy ended(accountManager.get(), &AccountManager::signInEnded);

    accountManager->requestProfile();
    QTRY_COMPARE_WITH_TIMEOUT(standIn.count(PROFILE), 2, WAIT_MS);
    QTest::qWait(SETTLE_MS);
    QCOMPARE(standIn.count(TOKEN), 1);
    QCOMPARE(standIn.requests.last().authorization, QByteArray("Bearer tok-2"));
    QCOMPARE(ended.count(), 0);
    QVERIFY(accountManager->isLoggedIn());
    QCOMPARE(currentToken(*accountManager), QString("tok-2"));
}

void SignInRefusedTests::refreshThenRefusedAgainEnds() {
    // the refresh works, but the new token is refused in turn: the sign-in ends, with no second refresh
    StandIn standIn;
    standIn.answers[PROFILE] = { answer(401, REFUSED) };
    standIn.answers[TOKEN] = { answer(200, NEW_TOKEN) };
    auto accountManager = signedIn(standIn);
    QSignalSpy ended(accountManager.get(), &AccountManager::signInEnded);

    accountManager->requestProfile();
    QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, WAIT_MS);
    QCOMPARE(ended.at(0).at(0).toString(), QString("signed-out-elsewhere"));
    QCOMPARE(standIn.count(PROFILE), 2);
    QCOMPARE(standIn.count(TOKEN), 1);
    QVERIFY(!accountManager->isLoggedIn());
}

void SignInRefusedTests::serverErrorOrNoAnswerStaysSignedIn() {
    // (d) a 503 or no answer, to the profile check or to the refresh, never ends a sign-in
    struct Case {
        const char* name;
        QList<Answer> profile;
        QList<Answer> token;
        int tokenRequests;
    };
    const QList<Case> cases {
        { "profile 503", { answer(503, "{}") }, {}, 0 },
        { "profile no answer", { noAnswer() }, {}, 0 },
        { "refused, refresh 503", { answer(401, REFUSED) }, { answer(503, "{}") }, 1 },
        { "refused, refresh no answer", { answer(401, REFUSED) }, { noAnswer() }, 1 },
    };
    for (const Case& c : cases) {
        StandIn standIn;
        standIn.answers[PROFILE] = c.profile;
        standIn.answers[TOKEN] = c.token;
        auto accountManager = signedIn(standIn);
        QSignalSpy ended(accountManager.get(), &AccountManager::signInEnded);

        accountManager->requestProfile();
        QTRY_VERIFY2_WITH_TIMEOUT(standIn.count(PROFILE) >= 1, c.name, WAIT_MS);
        if (c.tokenRequests > 0) {
            QTRY_VERIFY2_WITH_TIMEOUT(standIn.count(TOKEN) >= 1, c.name, WAIT_MS);
        }
        QTest::qWait(SETTLE_MS);
        QVERIFY2(ended.count() == 0, c.name);
        QVERIFY2(accountManager->isLoggedIn(), c.name);
        QVERIFY2(currentToken(*accountManager) == "tok-1", c.name);
        if (c.tokenRequests == 0) {
            QVERIFY2(standIn.count(TOKEN) == 0, c.name);
        }
    }
}

void SignInRefusedTests::noRefreshTokenEnds() {
    StandIn standIn;
    standIn.answers[PROFILE] = { answer(401, REFUSED) };
    auto accountManager = signedIn(standIn, false);
    QSignalSpy ended(accountManager.get(), &AccountManager::signInEnded);

    accountManager->requestProfile();
    QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, WAIT_MS);
    QCOMPARE(ended.at(0).at(0).toString(), QString("no-refresh-token"));
    QCOMPARE(standIn.count(TOKEN), 0);
    QVERIFY(!accountManager->isLoggedIn());
}

void SignInRefusedTests::offChangesNothing() {
    // the servers and AI residents' key sign-ins leave it off: a 401 changes nothing, as before
    StandIn standIn;
    standIn.answers[PROFILE] = { answer(401, REFUSED) };
    standIn.answers[TOKEN] = { answer(400, INVALID_GRANT) };
    auto accountManager = signedIn(standIn);
    accountManager->setEndsRefusedSignIn(false);
    QSignalSpy ended(accountManager.get(), &AccountManager::signInEnded);

    accountManager->requestProfile();
    QTRY_COMPARE_WITH_TIMEOUT(standIn.count(PROFILE), 1, WAIT_MS);
    QTest::qWait(SETTLE_MS);
    QCOMPARE(standIn.count(TOKEN), 0);
    QCOMPARE(ended.count(), 0);
    QVERIFY(accountManager->isLoggedIn());
}

void SignInRefusedTests::olderTokensReplyIsIgnored() {
    // a 401 to a request that carried another sign-in's token says nothing about this one
    StandIn standIn;
    const QByteArray CHAT = "/api/v1/chat/token";
    standIn.answers[CHAT] = { answer(401, REFUSED) };
    standIn.answers[TOKEN] = { answer(400, INVALID_GRANT) };
    auto accountManager = signedIn(standIn);
    QSignalSpy ended(accountManager.get(), &AccountManager::signInEnded);

    QNetworkReply* reply = finishedRequest(standIn.url(), CHAT, "old-token");
    QVERIFY(reply->isFinished());
    accountManager->reportRefusedAccessToken(reply, "test");
    reply->deleteLater();
    QTest::qWait(SETTLE_MS);
    QCOMPARE(standIn.count(TOKEN), 0);
    QCOMPARE(ended.count(), 0);
    QVERIFY(accountManager->isLoggedIn());
}

void SignInRefusedTests::otherHostsReplyIsIgnored() {
    // only the metaverse our token is for judges it: a 401 from another address (here another port) says nothing
    StandIn metaverse;
    metaverse.answers[TOKEN] = { answer(400, INVALID_GRANT) };
    StandIn elsewhere;
    const QByteArray PATH = "/api/v1/anything";
    elsewhere.answers[PATH] = { answer(401, REFUSED) };
    auto accountManager = signedIn(metaverse);
    QSignalSpy ended(accountManager.get(), &AccountManager::signInEnded);

    QNetworkReply* reply = finishedRequest(elsewhere.url(), PATH, "tok-1");
    QVERIFY(reply->isFinished());
    accountManager->reportRefusedAccessToken(reply, "test");
    reply->deleteLater();
    QTest::qWait(SETTLE_MS);
    QCOMPARE(metaverse.count(TOKEN), 0);
    QCOMPARE(ended.count(), 0);
    QVERIFY(accountManager->isLoggedIn());
}

void SignInRefusedTests::reportedRefusalRefreshesOnce() {
    // another part of the app (chat's token request) reports two refusals of our token: one refresh decides for both
    StandIn standIn;
    const QByteArray CHAT = "/api/v1/chat/token";
    standIn.answers[CHAT] = { answer(401, REFUSED) };
    standIn.answers[TOKEN] = { answer(400, INVALID_GRANT) };
    auto accountManager = signedIn(standIn);
    QSignalSpy ended(accountManager.get(), &AccountManager::signInEnded);

    QNetworkReply* first = finishedRequest(standIn.url(), CHAT, "tok-1");
    QNetworkReply* second = finishedRequest(standIn.url(), CHAT, "tok-1");
    QVERIFY(first->isFinished() && second->isFinished());
    accountManager->reportRefusedAccessToken(first, "chat-token");
    accountManager->reportRefusedAccessToken(second, "chat-token");
    first->deleteLater();
    second->deleteLater();

    QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, WAIT_MS);
    QCOMPARE(ended.at(0).at(0).toString(), QString("signed-out-elsewhere"));
    QTest::qWait(SETTLE_MS);
    QCOMPARE(standIn.count(TOKEN), 1);
    QCOMPARE(ended.count(), 1);
}
