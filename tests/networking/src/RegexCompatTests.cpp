//
//  RegexCompatTests.cpp
//  tests/networking/src
//
//  Created by servers on 2026-11-04.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "RegexCompatTests.h"

#include <QtCore/QStringList>

#include <AssetUtils.h>
#include <RegexCompat.h>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QtCore/QRegExp>
#endif

QTEST_MAIN(RegexCompatTests)

namespace {

struct Pattern {
    QString text;
    Qt::CaseSensitivity caseSensitivity;
    QList<int> capturesRead {};
};

// every pattern the servers' code matches with (copied from the code; AssetUtils' from its header)
QList<Pattern> patterns() {
    const QString UUID = "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}";
    const QString FLOAT = "([-+]?[0-9]*\\.?[0-9]+(?:[eE][-+]?[0-9]+)?)";
    const QString COMMA = "\\s*,\\s*";
    return {
        { AssetUtils::ASSET_FILE_PATH_REGEX_STRING, Qt::CaseSensitive },
        { AssetUtils::ASSET_PATH_REGEX_STRING, Qt::CaseSensitive },
        { AssetUtils::ASSET_HASH_REGEX_STRING, Qt::CaseSensitive },
        { "^atp:([A-Fa-f0-9]{64})(\\.[\\w]+)?$", Qt::CaseSensitive },
        { ".*\\.backup\\.\\d{8}-\\d{6}$", Qt::CaseSensitive },
        { "[^a-zA-Z0-9\\-_]+", Qt::CaseSensitive },
        { "\\/assignment\\/(" + UUID + ")\\/?$", Qt::CaseSensitive, {1} },
        { "\\/nodes\\/(" + UUID + ").json\\/?$", Qt::CaseSensitive, {1} },
        { "/nodes/?$", Qt::CaseSensitive },
        { "/nodes/(" + UUID + ")$", Qt::CaseSensitive, {1} },
        { "DS_WEB_SESSION_UUID=([\\d\\w-]+)($|;)", Qt::CaseSensitive, {1} },
        { R":(name="(restore-file.*)".*filename="(.+)"):", Qt::CaseSensitive, {1, 2} },
        { "api\\/v1\\/users\\/([A-Za-z0-9_\\.]+)\\/public_key", Qt::CaseSensitive, {1} },
        { "api\\/v1\\/groups\\/members\\/([A-Za-z0-9_\\.]+)", Qt::CaseSensitive, {1} },
        { "\\d{1,5}(\\/.*)?", Qt::CaseSensitive },
        { "hifi:\\/?($|\\w+)", Qt::CaseInsensitive, {1} },
        { "^[0-9A-Za-z](([0-9A-Za-z]|-(?!-))*[^\\W_]$|$)", Qt::CaseSensitive },
        { "^((?:(?:[0-9]|[1-9][0-9]|1[0-9]{2}|2[0-4][0-9]|25[0-5])\\.){3}"
          "(?:[0-9]|[1-9][0-9]|1[0-9]{2}|2[0-4][0-9]|25[0-5]))(?::(\\d{1,5}))?$", Qt::CaseSensitive, {1, 2} },
        { "^((?:[A-Z0-9]|[A-Z0-9][A-Z0-9\\-]{0,61}[A-Z0-9])"
          "(?:\\.(?:[A-Z0-9]|[A-Z0-9][A-Z0-9\\-]{0,61}[A-Z0-9]))+|localhost)(?::(\\d{1,5}))?$", Qt::CaseInsensitive, {1, 2} },
        { UUID, Qt::CaseInsensitive },
        { "\\/" + FLOAT + COMMA + FLOAT + COMMA + FLOAT + "\\s*(?:$|\\/)", Qt::CaseSensitive, {1, 2, 3} },
        { "\\/" + FLOAT + COMMA + FLOAT + COMMA + FLOAT + COMMA + FLOAT + "\\s*$", Qt::CaseSensitive, {1, 2, 3, 4} },
        { "^@(\\S+)", Qt::CaseSensitive, {1} },
        { "<!--\\s*#include\\s+(virtual|file)\\s?=\\s?\"(\\S+)\"\\s*-->", Qt::CaseSensitive, {1, 2} },
        // DomainContentBackupManager / ContentSettingsBackupHandler: prefixes escaped, a name, the time
        { "(" + QRegularExpression::escape("autobackup-") + "|" + QRegularExpression::escape("backup-") + ")" +
          "(.+)-(\\d{4}-\\d{2}-\\d{2}_\\d{2}-\\d{2}-\\d{2})\\.zip", Qt::CaseSensitive, {1, 2, 3} },
        { "autobackup-" + QRegularExpression::escape("models.json") + "\\-(\\d{4}-\\d{2}-\\d{2}_\\d{2}-\\d{2}-\\d{2})\\.zip",
          Qt::CaseSensitive, {1} },
        { "[a-zA-Z0-9\\-_ ]+", Qt::CaseSensitive },
    };
}

// texts that tell the two apart if anything does: valid ones for each pattern, then edges (newlines, especially a
// final one; Unicode letters, digits and spaces; NUL; case; empty)
QStringList corpus() {
    const QString hash = QString("a").repeated(64);
    const QString uuid = "0a1b2c3d-0000-4000-8000-00112233aabb";
    QStringList texts {
        "", "\n", "a", "/", "//", "/a", "/a/", "/a/b", "/a//b", "/a/b/", "a/b", "/a\n", "/a\nb", "/a\0b",
        hash, hash + "\n", hash.toUpper(), hash.left(63), hash + "a", "atp:" + hash, "atp:" + hash + ".fbx",
        "atp:" + hash + ".fbx\n", "atp:" + hash + ".fbx/x",
        "models.json.gz.backup.20261104-031500", "models.json.gz.backup.20261104-031500\n", "x.backup.2026110-031500",
        "/assignment/" + uuid, "/assignment/" + uuid + "/", "/assignment/" + uuid + "\n", "/assignment/" + uuid + "x",
        "/nodes/" + uuid + ".json", "/nodes/" + uuid + "Xjson/", "/nodes/" + uuid + ".json\n", "/nodes", "/nodes/",
        "/nodes/\n", "/nodes/" + uuid, "/nodes/" + uuid + "\n",
        "DS_WEB_SESSION_UUID=" + uuid, "a=b; DS_WEB_SESSION_UUID=" + uuid + "; c=d",
        "DS_WEB_SESSION_UUID=" + uuid + "\n", QString::fromUtf8("DS_WEB_SESSION_UUID=\xc3\xa9t\xc3\xa9"),
        "Content-Disposition: form-data; name=\"restore-file\"; filename=\"backup.zip\"",
        "name=\"restore-file-1\"\nfilename=\"a\nb\"",
        "https://vr.darkride.ai/api/v1/users/caity_m.1/public_key", "api/v1/groups/members/a.b_c",
        "api/v1/users/" + QString::fromUtf8("\xc3\xa9") + "/public_key",
        "40100", "40100/spot", "40100\n", "123456",
        "hifi:/", "hifi:/x", "HIFI:", "hifi://darkside", QString::fromUtf8("hifi:\xc3\xa9t\xc3\xa9"),
        "darkside", "dark-side", "dark--side", "dark_side", "-dark", "darkside-", QString::fromUtf8("caf\xc3\xa9"),
        QString::fromUtf8("caf\xc3\xa9\xd9\xa3"), "d\n",
        "127.0.0.1", "127.0.0.1:40102", "256.0.0.1", "1.2.3.4:", "1.2.3.4\n", "01.2.3.4",
        "vr.darkride.ai", "vr.darkride.ai:443", "localhost", "a-.b", "VR.DARKRIDE.AI", "vr.darkride.ai\n",
        uuid, uuid.toUpper(), "{" + uuid + "}", uuid + "\n",
        "/12,3,-7", "/12, 3 ,-7/0,0,0,1", "/1e3,-.5,+2", "/1,2,3\n", QString::fromUtf8("/1,\xe2\x80\x83" "2,3"),
        "/0,0,0,1", "/0, 0, 0, 1 ", "/0,0,0,1\n",
        "@caity", "@ caity", QString::fromUtf8("@caf\xc3\xa9"), "@\n",
        "<!--#include virtual=\"/header.html\"-->", "<!-- #include file = \"a.html\" -->", "<!--#include file=\"a b\"-->",
        "autobackup-daily-2026-11-04_03-15-00.zip", "backup-my-world-2026-11-04_03-15-00.zip\n",
        "backup-a-b-2026-11-04_03-15-00.zip", "autobackup-models.json-2026-11-04_03-15-00.zip",
        "autobackup-modelsXjson-2026-11-04_03-15-00.zip", "backup-\n-2026-11-04_03-15-00.zip",
        QString::fromUtf8("manual-backup-\xd9\xa3\xd9\xa3\xd9\xa3\xd9\xa3-11-04_03-15-00.zip"),
        "my backup_1", "my backup\n", QString::fromUtf8("my caf\xc3\xa9"),
        "$", "a", "]", "$a]", "]$",
    };
    texts << QString(QChar(0));
    return texts;
}

}

void RegexCompatTests::dollarIsTheEndOnly() {
    QCOMPARE(regex_compat::pattern("a$"), QString("a\\z"));
    QCOMPARE(regex_compat::pattern("a\\$"), QString("a\\$"));
    QCOMPARE(regex_compat::pattern("[$]$"), QString("[$]\\z"));
    QCOMPARE(regex_compat::pattern("[]$]$"), QString("[]$]\\z"));
    QCOMPARE(regex_compat::pattern("[^]$]($|;)"), QString("[^]$](\\z|;)"));
    QVERIFY(!regex_compat::search("a$").match("a\n").hasMatch());
    QVERIFY(regex_compat::search("a$").match("ba").hasMatch());
}

void RegexCompatTests::exactMatchesAsQRegExp() {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    int compared = 0;
    QStringList differences;
    for (const auto& pattern : patterns()) {
        const QRegExp old(pattern.text, pattern.caseSensitivity);
        QVERIFY2(old.isValid(), qPrintable(pattern.text));
        const auto now = regex_compat::exact(pattern.text, pattern.caseSensitivity);
        QVERIFY2(now.isValid(), qPrintable(pattern.text + ": " + now.errorString()));
        for (const auto& text : corpus()) {
            QRegExp copy = old;
            const bool oldMatched = copy.exactMatch(text);
            const auto match = now.match(text);
            if (oldMatched != match.hasMatch()) {
                differences << QString("exactMatch: /%1/ on \"%2\"").arg(pattern.text, text.toHtmlEscaped());
            } else if (oldMatched) {
                for (int i : pattern.capturesRead) {
                    if (copy.cap(i) != match.captured(i)) {
                        differences << QString("exact capture %1: /%2/ on \"%3\"").arg(i).arg(pattern.text, text.toHtmlEscaped());
                    }
                }
            }
            compared++;
        }
    }
    QVERIFY2(differences.isEmpty(), qPrintable(differences.mid(0, 20).join("\n")));
    QVERIFY(compared > 2000);
#else
    QSKIP("QRegExp is in the Qt 5 build only");
#endif
}

void RegexCompatTests::searchesAsQRegExp() {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QStringList differences;
    for (const auto& pattern : patterns()) {
        const auto now = regex_compat::search(pattern.text, pattern.caseSensitivity);
        for (const auto& text : corpus()) {
            QRegExp old(pattern.text, pattern.caseSensitivity);
            const int position = old.indexIn(text);
            const auto match = now.match(text);
            const QString where = QString("/%1/ on \"%2\"").arg(pattern.text, text.toHtmlEscaped());
            if ((position >= 0) != match.hasMatch()) {
                differences << "found differs: " + where;
                continue;
            }
            if (position < 0) {
                continue;
            }
            if (position != match.capturedStart(0) || old.matchedLength() != match.capturedLength(0)) {
                differences << "where or how long differs: " + where;
            }
            // the captures the servers' code reads (a group inside a repeated group can keep another repetition's
            // text in one and not the other; no code reads such a capture)
            for (int i : pattern.capturesRead) {
                if (old.cap(i) != match.captured(i)) {
                    differences << QString("capture %1 (\"%2\" vs \"%3\"): ").arg(i).arg(old.cap(i), match.captured(i)) + where;
                }
            }
        }
    }
    QVERIFY2(differences.isEmpty(), qPrintable(differences.mid(0, 30).join("\n")));
#else
    QSKIP("QRegExp is in the Qt 5 build only");
#endif
}
