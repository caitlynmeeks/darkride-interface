//
//  ScriptOpenerTests.cpp
//  tests/script-engine/src
//
//  Created by scripting on 2026-11-07.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ScriptOpenerTests.h"

#include <ScriptOpener.h>

QTEST_MAIN(ScriptOpenerTests)

namespace {

const bool FROM_THE_NETWORK = false;
const bool LOCAL_WORLD = true;

const QString CODE = "(function() {\n    this.preload = function() {};\n})";

}

void ScriptOpenerTests::testIsAddress() {
    QVERIFY(ScriptOpener::isAddress("https://example.com/door.js"));
    QVERIFY(ScriptOpener::isAddress("  atp:/scripts/door.js  "));
    QVERIFY(ScriptOpener::isAddress("file:///Users/me/door.js"));
    QVERIFY(!ScriptOpener::isAddress(""));
    QVERIFY(!ScriptOpener::isAddress("   "));
    QVERIFY(!ScriptOpener::isAddress(CODE));
    QVERIFY(!ScriptOpener::isAddress("scripts/door.js"));                  // no scheme: not an address
    QVERIFY(!ScriptOpener::isAddress("https://example.com/door.js\nvar x;")); // two lines: code
}

void ScriptOpenerTests::testCodeIsWrittenIn() {
    for (bool local : { FROM_THE_NETWORK, LOCAL_WORLD }) {
        const auto opened = ScriptOpener::judge(CODE, local);
        QVERIFY(opened.ok());
        QCOMPARE(opened.where, ScriptOpener::WRITTEN_IN);
        QCOMPARE(opened.text, CODE);
        QVERIFY(opened.url.isEmpty());
    }
    // an empty field: a new script typed in the editor goes into it
    const auto empty = ScriptOpener::judge("", FROM_THE_NETWORK);
    QVERIFY(empty.ok());
    QCOMPARE(empty.where, ScriptOpener::WRITTEN_IN);
    QVERIFY(empty.text.isEmpty());
    // a bare path is no address here, as in the Code panel: shown as the text it is, never read as a file
    const auto path = ScriptOpener::judge("scripts/door.js", LOCAL_WORLD);
    QCOMPARE(path.where, ScriptOpener::WRITTEN_IN);
    QCOMPARE(path.text, QString("scripts/door.js"));
}

void ScriptOpenerTests::testFileOnThisComputerRefused() {
    // in a world from the network and in a local one alike (a world file could name any file here)
    for (bool local : { FROM_THE_NETWORK, LOCAL_WORLD }) {
        const auto opened = ScriptOpener::judge("file:///Users/me/.ssh/id_rsa", local);
        QVERIFY(!opened.ok());
        QCOMPARE(opened.where, ScriptOpener::LOCAL_FILE);
        QVERIFY(opened.text.isEmpty());
        QVERIFY(opened.problem.contains("this computer"));
    }
    const auto upper = ScriptOpener::judge("FILE:///Users/me/door.js", FROM_THE_NETWORK);
    QCOMPARE(upper.where, ScriptOpener::LOCAL_FILE);
    QVERIFY(!upper.ok());
}

void ScriptOpenerTests::testWebAndWorldFilesOpen() {
    for (bool local : { FROM_THE_NETWORK, LOCAL_WORLD }) {
        const auto web = ScriptOpener::judge("https://cdn.example.com/scripts/door.js", local);
        QVERIFY2(web.ok(), qPrintable(web.problem));
        QCOMPARE(web.where, ScriptOpener::WEB);
        QCOMPARE(web.url, QUrl("https://cdn.example.com/scripts/door.js"));
        QVERIFY(web.text.isEmpty());   // the text comes from the fetch

        const auto http = ScriptOpener::judge("http://cdn.example.com/door.js", local);
        QVERIFY2(http.ok(), qPrintable(http.problem));
        QCOMPARE(http.where, ScriptOpener::WEB);

        const auto files = ScriptOpener::judge("atp:/scripts/door.js", local);
        QVERIFY2(files.ok(), qPrintable(files.problem));
        QCOMPARE(files.where, ScriptOpener::WORLD_FILES);
        QCOMPARE(files.url, QUrl("atp:/scripts/door.js"));
    }
    // surrounding space is no part of the address
    const auto spaced = ScriptOpener::judge("  https://cdn.example.com/door.js \n", FROM_THE_NETWORK);
    QVERIFY(spaced.ok());
    QCOMPARE(spaced.url, QUrl("https://cdn.example.com/door.js"));
}

void ScriptOpenerTests::testPrivateAddressByTheWorld() {
    // a world from the network reaches no address on this machine or its network; a local world keeps them
    const QStringList privateAddresses {
        "http://127.0.0.1:8080/door.js",
        "http://localhost/door.js",
        "http://192.168.1.20/door.js",
        "http://10.0.0.5/door.js",
        "http://router.local/door.js",
        "http://nas/door.js",
        "http://[::1]/door.js",
    };
    for (const QString& address : privateAddresses) {
        const auto refused = ScriptOpener::judge(address, FROM_THE_NETWORK);
        QVERIFY2(!refused.ok(), qPrintable(address));
        QCOMPARE(refused.where, ScriptOpener::WEB);
        QVERIFY(refused.text.isEmpty());
        QVERIFY2(refused.problem.contains("world from the network"), qPrintable(refused.problem));

        const auto allowed = ScriptOpener::judge(address, LOCAL_WORLD);
        QVERIFY2(allowed.ok(), qPrintable(address + ": " + allowed.problem));
    }
    // a public name is judged by its addresses when the request goes out, not here
    const auto name = ScriptOpener::judge("https://scripts.example.org/door.js", FROM_THE_NETWORK);
    QVERIFY(name.ok());
    // an address with no host is nothing to fetch
    const auto noHost = ScriptOpener::judge("https:///door.js", FROM_THE_NETWORK);
    QVERIFY(!noHost.ok());
}

void ScriptOpenerTests::testOtherSchemesCantOpen() {
    const QStringList others {
        "javascript:alert(1)",
        "qrc:///scripts/system/door.js",
        "C:\\Users\\me\\door.js",
        "ftp://example.com/door.js",
        "data:text/javascript,alert(1)",
    };
    for (const QString& value : others) {
        for (bool local : { FROM_THE_NETWORK, LOCAL_WORLD }) {
            const auto opened = ScriptOpener::judge(value, local);
            QVERIFY2(!opened.ok(), qPrintable(value));
            QVERIFY2(opened.where.isEmpty(), qPrintable(value));
            QVERIFY(opened.url.isEmpty());
            QVERIFY(opened.text.isEmpty());
        }
    }
}

void ScriptOpenerTests::testWasmNotText() {
    const auto module = ScriptOpener::judge("https://cdn.example.com/door.wasm", FROM_THE_NETWORK);
    QVERIFY(!module.ok());
    QCOMPARE(module.where, ScriptOpener::WEB);
    QVERIFY(module.problem.contains("WebAssembly"));
    const auto inFiles = ScriptOpener::judge("atp:/door.WASM", LOCAL_WORLD);
    QVERIFY(!inFiles.ok());
    QCOMPARE(inFiles.where, ScriptOpener::WORLD_FILES);
}
