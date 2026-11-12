//
//  AccountNameTests.cpp
//  tests/networking/src
//
//  Created by servers on 2026-11-12.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "AccountNameTests.h"

#include <QUrlQuery>

#include <AccountName.h>

QTEST_MAIN(AccountNameTests)

namespace {

// The address AccountManager::createRequest makes of a path (AccountManager.cpp: cut at the first '?', the rest the
// query), on a made-up metaverse
QUrl requestUrl(const QString& path) {
    QUrl url("https://metaverse.example");
    const int queryAt = path.indexOf("?");
    url.setPath("/" + path.left(queryAt));
    if (queryAt >= 0) {
        url.setQuery(QUrlQuery(path.mid(queryAt + 1)));
    }
    return url;
}

}

void AccountNameTests::notAccountNames_data() {
    QTest::addColumn<QString>("name");
    QTest::newRow("empty") << "";
    QTest::newRow("a slash") << "a/b";
    QTest::newRow("a question mark") << "a?b";
    QTest::newRow("a hash") << "a#b";
    QTest::newRow("a percent") << "a%2Fb";
    QTest::newRow("a backslash") << "a\\b";
    QTest::newRow("a space") << "a b";
    QTest::newRow("a leading dot") << ".abc";
    QTest::newRow("a trailing dot") << "abc.";
    QTest::newRow("only dots") << "..";
    QTest::newRow("a newline at the end") << "abc\n";
    QTest::newRow("a letter outside ASCII") << QString::fromUtf8("caf\xc3\xa9");
    QTest::newRow("over-long") << QString(account_name::MAX_LENGTH + 1, 'a');
}

void AccountNameTests::notAccountNames() {
    // refused at the door, so never put into an address, and an answer carrying one is dropped
    QFETCH(QString, name);
    QVERIFY(!account_name::isAccountName(name));
    QVERIFY(account_name::nameFromRequest(QJsonObject { { "username", name } }).isEmpty());
}

void AccountNameTests::accountNames_data() {
    QTest::addColumn<QString>("name");
    QTest::newRow("plain") << "test_account";
    QTest::newRow("mixed case") << "Test_Account";
    QTest::newRow("an inner dot and an underscore") << "first.last_2";
    QTest::newRow("two inner dots") << "a..b";
    QTest::newRow("one character") << "a";
    QTest::newRow("an underscore") << "_";
    QTest::newRow("digits") << "2026";
    QTest::newRow("the longest") << QString(account_name::MAX_LENGTH, 'z');
}

void AccountNameTests::accountNames() {
    QFETCH(QString, name);
    QVERIFY(account_name::isAccountName(name));
    const QString lower = name.toLower();
    // the address names exactly this account, nothing after it, and cleaning it changes nothing
    const QUrl url = requestUrl(account_name::keyRequestPath(lower));
    QCOMPARE(url.path(), "/api/v1/users/" + lower + "/public_key");
    QVERIFY(!url.hasQuery() && !url.hasFragment());
    QCOMPARE(url.adjusted(QUrl::NormalizePathSegments), url);
    // the answer is filed under the name the request carried
    QCOMPARE(account_name::nameFromRequest(account_name::requestData(lower)), lower);
    // a name carried in another case is filed lower-cased, as the tables keep them
    QCOMPARE(account_name::nameFromRequest(QJsonObject { { "username", name } }), lower);
    const QUrl groups = requestUrl(account_name::groupMembershipsPath(lower));
    QCOMPARE(groups.path(), "/api/v1/groups/members/" + lower);
    QVERIFY(!groups.hasQuery() && !groups.hasFragment());
}

void AccountNameTests::anAnswerWithoutItsNameIsDropped() {
    QVERIFY(account_name::nameFromRequest(QJsonObject()).isEmpty());
    QVERIFY(account_name::nameFromRequest(QJsonObject { { "username", "" } }).isEmpty());
    QVERIFY(account_name::nameFromRequest(QJsonObject { { "username", 42 } }).isEmpty());
    QVERIFY(account_name::nameFromRequest(QJsonObject { { "user", "test_account" } }).isEmpty());
}
