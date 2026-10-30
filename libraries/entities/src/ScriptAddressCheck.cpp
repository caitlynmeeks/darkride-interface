//
//  ScriptAddressCheck.cpp
//  libraries/entities/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "ScriptAddressCheck.h"

#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

namespace script_address {

namespace {

// ScriptCache::getScriptContents' pattern for code written into the value
const QRegularExpression INLINE_FUNCTION { R"(\(function\([a-z]?[\w,]*\){)" };

// A step up a folder, as a whole path step. The client swaps some address prefixes for folders of its own before it
// reads the address (ResourceManager::normalizeURL), so a step up could leave that folder for any file on the computer.
const QRegularExpression PARENT_STEP { R"(/\.\.(/|\?|#|$))" };

// The default avatar's folder: the app answers for it from its own resources (MyAvatar::useBuiltInDefaultAvatar).
const QString BUILT_IN_AVATAR_HOST = "cdn.darkride.ai";
const QString BUILT_IN_AVATAR_PATH = "/avatars/";

// Names that resolve to this computer, well known as such; others can't be told from here.
const QStringList LOOPBACK_NAMES { "localhost", "localtest.me", "lvh.me", "vcap.me", "localho.st", "nip.io", "sslip.io",
                                   "xip.io" };

QString decodedFully(QString text) {
    // until it stops changing, so %252e%252e is caught as well as %2e%2e
    for (int i = 0; i < 4; i++) {
        QString decoded = QUrl::fromPercentEncoding(text.toUtf8());
        if (decoded == text) {
            break;
        }
        text = decoded;
    }
    return text;
}

// An IPv4 address in any spelling a resolver takes (inet_aton: one to four parts, each decimal, octal or hex, the last
// filling the bytes left: 127.1, 2130706433, 0x7f.0.0.1, 0177.0.0.1); false when the host isn't one.
bool parseIPv4(const QString& host, quint32& address) {
    static const QRegularExpression PART { "^(0[xX][0-9a-fA-F]*|[0-9]+)$" };
    const QStringList parts = host.split('.');
    if (parts.size() > 4) {
        return false;
    }
    quint64 values[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < parts.size(); i++) {
        const QString& part = parts[i];
        if (!PART.match(part).hasMatch()) {
            return false;
        }
        bool ok = true;
        if (part.startsWith("0x", Qt::CaseInsensitive)) {
            values[i] = part.size() == 2 ? 0 : part.mid(2).toULongLong(&ok, 16);
        } else if (part.size() > 1 && part.startsWith('0')) {
            values[i] = part.toULongLong(&ok, 8);
        } else {
            values[i] = part.toULongLong(&ok, 10);
        }
        if (!ok) {
            return false;
        }
    }
    const int last = parts.size() - 1;
    quint64 result = 0;
    for (int i = 0; i < last; i++) {
        if (values[i] > 0xff) {
            return false;
        }
        result |= values[i] << (8 * (3 - i));
    }
    if (values[last] > (0xffffffffULL >> (8 * last))) {
        return false;
    }
    address = (quint32)(result | values[last]);
    return true;
}

// Loopback, private, link-local, shared (carrier-grade NAT), documentation, multicast and reserved ranges: none of them
// is a server on the internet.
bool isPublicIPv4(quint32 address) {
    struct Range {
        quint32 base;
        int bits;
    };
    static const Range NOT_PUBLIC[] = {
        { 0x00000000, 8 },   // 0.0.0.0/8, this network
        { 0x0A000000, 8 },   // 10.0.0.0/8
        { 0x64400000, 10 },  // 100.64.0.0/10, carrier-grade NAT
        { 0x7F000000, 8 },   // 127.0.0.0/8, loopback
        { 0xA9FE0000, 16 },  // 169.254.0.0/16, link-local
        { 0xAC100000, 12 },  // 172.16.0.0/12
        { 0xC0000000, 24 },  // 192.0.0.0/24
        { 0xC0000200, 24 },  // 192.0.2.0/24, documentation
        { 0xC0A80000, 16 },  // 192.168.0.0/16
        { 0xC6120000, 15 },  // 198.18.0.0/15, benchmarking
        { 0xC6336400, 24 },  // 198.51.100.0/24, documentation
        { 0xCB007100, 24 },  // 203.0.113.0/24, documentation
        { 0xE0000000, 4 },   // 224.0.0.0/4, multicast
        { 0xF0000000, 4 },   // 240.0.0.0/4, reserved, and the broadcast address
    };
    for (const Range& range : NOT_PUBLIC) {
        const quint32 mask = 0xffffffffU << (32 - range.bits);
        if ((address & mask) == range.base) {
            return false;
        }
    }
    return true;
}

QString hostRefusal(const QUrl& url) {
    QString host = url.host(QUrl::EncodeUnicode).toLower();
    while (host.endsWith('.')) {
        host.chop(1);
    }
    if (host.isEmpty()) {
        return "no host";
    }
    if (host.contains(':') || host.contains('[') || host.contains('%')) {
        return "an IPv6 address";
    }
    for (const QString& name : LOOPBACK_NAMES) {
        if (host == name || host.endsWith("." + name)) {
            return "a name for this computer";
        }
    }
    if (host.endsWith(".local") || host.endsWith(".localhost")) {
        return "a name on the home network";
    }
    // a public name always has a domain; a single name is looked up on the home network
    const QString lastLabel = host.section('.', -1);
    if (!host.contains('.') || lastLabel.isEmpty() || lastLabel[0].isDigit()) {
        quint32 address = 0;
        if (!parseIPv4(host, address)) {
            return host.contains('.') ? "a host that is neither a name nor an address" : "a name on the home network";
        }
        if (!isPublicIPv4(address)) {
            return "a private or local address";
        }
    }
    return QString();
}

} // namespace

bool isInlineCode(const QString& value) {
    const QString scheme = QUrl(value).scheme();
    if (scheme.isEmpty() && value.simplified().replace(" ", "").contains(INLINE_FUNCTION)) {
        return true;
    }
    return scheme == "javascript";
}

QString refusalReason(const QString& value) {
    if (value.isEmpty() || isInlineCode(value)) {
        return QString();
    }

    // the scheme as the client reads it (QUrl), so what passes here is fetched from where it says there
    const QUrl url(value);
    const QString scheme = url.scheme();
    const bool web = scheme == "http" || scheme == "https";
    if (!web && scheme != "atp" && scheme != "tea") {
        return scheme.isEmpty() ? "no scheme (a file on the visitor's computer)" : "the scheme '" + scheme + "'";
    }

    const QString decoded = decodedFully(value);
    if (value.contains('\\') || decoded.contains('\\')) {
        return "a backslash";
    }
    if (value.contains(PARENT_STEP) || decoded.contains(PARENT_STEP)) {
        return "a step up a folder (..)";
    }

    if (web) {
        if (!url.userInfo().isEmpty()) {
            return "a user name in the address";
        }
        QString refusal = hostRefusal(url);
        if (!refusal.isEmpty()) {
            return refusal;
        }
        QString host = url.host(QUrl::EncodeUnicode).toLower();
        while (host.endsWith('.')) {
            host.chop(1);
        }
        if (host == BUILT_IN_AVATAR_HOST &&
            (url.path().startsWith(BUILT_IN_AVATAR_PATH) || decoded.contains(BUILT_IN_AVATAR_PATH, Qt::CaseInsensitive))) {
            return "the app's built-in avatar folder";
        }
    }
    return QString();
}

} // namespace script_address
