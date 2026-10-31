//
//  ScriptAddressCheckTests.cpp
//  tests/entity-rights/src
//
//  What an entity's script may hold on the servers (ScriptAddressCheck): inline code, or an http, https, atp or tea
//  address that passes the host checks; nothing else, however it is spelled.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "ScriptAddressCheckTests.h"

#include <ScriptAddressCheck.h>

QTEST_GUILESS_MAIN(ScriptAddressCheckTests)

void ScriptAddressCheckTests::testValues_data() {
    QTest::addColumn<QString>("value");
    QTest::addColumn<bool>("allowed");

    // kept
    QTest::newRow("none") << "" << true;
    QTest::newRow("files host") << "https://files.darkride.ai/someone/scripts/seat.js" << true;
    QTest::newRow("http with a query") << "http://things.tivolicloud.com/sit/sitClient.js?v=2" << true;
    QTest::newRow("capitals") << "HTTPS://Files.Darkride.ai/x.js" << true;
    QTest::newRow("assets") << "atp:/darkside/rover.js" << true;
    QTest::newRow("private files") << "tea://someone/private/x.js" << true;
    QTest::newRow("code") << "(function() { this.preload = function(id) {}; })" << true;
    QTest::newRow("code over lines") << "(function () {\n return {}; })" << true;
    QTest::newRow("javascript:") << "javascript:print(1)" << true;
    QTest::newRow("public address") << "https://8.8.8.8/x.js" << true;
    QTest::newRow("port and fragment") << "https://example.com:8443/a/b.js#frag" << true;
    QTest::newRow("cdn outside the avatar folder") << "https://cdn.darkride.ai/scripts/x.js" << true;
    QTest::newRow("module") << "https://files.darkride.ai/someone/scripts/x.wasm" << true;
    QTest::newRow("module in assets") << "atp:/darkside/x.wasm" << true;

    // addresses not on the list
    QTest::newRow("file:") << "file:///tmp/a.js" << false;
    QTest::newRow("FILE:") << "FILE:///tmp/a.js" << false;
    QTest::newRow("/~/") << "/~/x.js" << false;
    QTest::newRow("home") << "~/foo.js" << false;
    QTest::newRow("path") << "/tmp/a.js" << false;
    QTest::newRow("drive") << "c:/Users/x/a.js" << false;
    QTest::newRow("drive, backslashes") << "C:\\Users\\x\\a.js" << false;
    QTest::newRow("space, /~/") << "  /~/x.js" << false;
    QTest::newRow("space, https (read as a path)") << " https://example.com/x.js" << false;
    QTest::newRow("qrc:") << "qrc:///qml/x.js" << false;
    QTest::newRow("data:") << "data:text/javascript,print(1)" << false;
    QTest::newRow("blob:") << "blob:https://x/1" << false;
    QTest::newRow("about:") << "about:blank" << false;
    QTest::newRow("ws:") << "ws://example.com/x" << false;
    QTest::newRow("ftp:") << "ftp://example.com/x.js" << false;
    QTest::newRow("hifi:") << "hifi://darkside/x" << false;
    QTest::newRow("a word") << "javascript" << false;
    QTest::newRow("code ending in .wasm") << "(function(){ })//x.wasm" << false;
    QTest::newRow("javascript: ending in .wasm") << "javascript:print(1)//a.wasm" << false;
    QTest::newRow("module, /~/") << "/~/x.wasm" << false;

    // private and loopback hosts
    QTest::newRow("localhost") << "http://localhost/x.js" << false;
    QTest::newRow("LOCALHOST.") << "http://LOCALHOST./x.js" << false;
    QTest::newRow("under localhost") << "http://foo.localhost/x.js" << false;
    QTest::newRow(".local") << "http://printer.local/x.js" << false;
    QTest::newRow("single name") << "http://nas/x.js" << false;
    QTest::newRow("loopback") << "http://127.0.0.1/x.js" << false;
    QTest::newRow("loopback, dot") << "http://127.0.0.1./x.js" << false;
    QTest::newRow("loopback, short") << "http://127.1/x.js" << false;
    QTest::newRow("loopback, decimal") << "http://2130706433/x.js" << false;
    QTest::newRow("loopback, hex") << "http://0x7f000001/x.js" << false;
    QTest::newRow("loopback, hex short") << "http://0x7f.1/x.js" << false;
    QTest::newRow("loopback, octal") << "http://0177.0.0.1/x.js" << false;
    QTest::newRow("loopback, octal whole") << "http://017700000001/x.js" << false;
    QTest::newRow("10/8") << "http://10.0.0.5/x.js" << false;
    QTest::newRow("172.16/12") << "http://172.16.4.4/x.js" << false;
    QTest::newRow("172.16/12 top") << "http://172.31.255.255/x.js" << false;
    QTest::newRow("192.168/16") << "http://192.168.1.1/x.js" << false;
    QTest::newRow("link-local") << "http://169.254.1.1/x.js" << false;
    QTest::newRow("carrier-grade NAT") << "http://100.64.1.1/x.js" << false;
    QTest::newRow("this network") << "http://0.0.0.0/x.js" << false;
    QTest::newRow("IPv6 loopback") << "http://[::1]/x.js" << false;
    QTest::newRow("IPv4 in IPv6") << "http://[::ffff:127.0.0.1]/x.js" << false;
    QTest::newRow("IPv6 link-local") << "http://[fe80::1%25en0]/x.js" << false;
    QTest::newRow("IPv6 public") << "http://[2606:4700::1111]/x.js" << false;
    QTest::newRow("user name") << "http://user@example.com/x.js" << false;
    QTest::newRow("user name and password") << "http://user:pw@example.com/x.js" << false;
    QTest::newRow("loopback by name") << "http://127.0.0.1.nip.io/x.js" << false;
    QTest::newRow("localtest.me") << "http://localtest.me/x.js" << false;
    QTest::newRow("under localtest.me") << "http://a.localtest.me/x.js" << false;
    QTest::newRow("five parts") << "http://1.2.3.4.5/x.js" << false;
    QTest::newRow("part over 255") << "http://256.1.1.1/x.js" << false;
    QTest::newRow("number for a domain") << "http://example.123/x.js" << false;

    // the built-in avatar's addresses, steps up a folder, backslashes
    QTest::newRow("avatar folder") << "https://cdn.darkride.ai/avatars/x.js" << false;
    QTest::newRow("up, %2e") << "https://example.com/a/%2e%2e/b.js" << false;
    QTest::newRow("up, %252e") << "https://example.com/a/%252e%252e/b.js" << false;
    QTest::newRow("avatar folder, capitals") << "https://CDN.darkride.ai/avatars/x.js" << false;
    QTest::newRow("backslash, %5c") << "https://example.com/a/..%5cb.js" << false;
    QTest::newRow("up") << "https://example.com/a/../b.js" << false;
    QTest::newRow("up, assets") << "atp:/a/../b.js" << false;
    QTest::newRow("backslash") << "https://example.com/a\\b.js" << false;
}

void ScriptAddressCheckTests::testValues() {
    QFETCH(QString, value);
    QFETCH(bool, allowed);
    const QString refusal = script_address::refusalReason(value);
    QVERIFY2(refusal.isEmpty() == allowed, qPrintable(value + " -> " + (refusal.isEmpty() ? "allowed" : refusal)));
}
