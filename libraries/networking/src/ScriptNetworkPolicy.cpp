//
//  ScriptNetworkPolicy.cpp
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ScriptNetworkPolicy.h"

#include <cstring>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>
#include <QtNetwork/QHostInfo>

#include <DependencyManager.h>
#include <PathUtils.h>
#include <ScriptCaller.h>

#include "NetworkLogging.h"
#include "NodeList.h"

namespace {

const QStringList WEB_SCHEMES { "http", "https", "ws", "wss" };
const QString ASSET_SCHEME = "atp";
const QString WORLD_LEFT = "the world the script belongs to was left";

bool inIPv4Block(quint32 address, quint32 block, int bits) {
    const quint32 mask = bits == 0 ? 0 : (0xffffffffu << (32 - bits));
    return (address & mask) == block;
}

bool isPrivateIPv4(quint32 address) {
    return inIPv4Block(address, 0x00000000u, 8)      // 0.0.0.0/8: this network
        || inIPv4Block(address, 0x0A000000u, 8)      // 10/8
        || inIPv4Block(address, 0x64400000u, 10)     // 100.64/10: carrier-grade NAT, shared address space
        || inIPv4Block(address, 0x7F000000u, 8)      // 127/8: loopback
        || inIPv4Block(address, 0xA9FE0000u, 16)     // 169.254/16: link-local (and the cloud metadata address)
        || inIPv4Block(address, 0xAC100000u, 12)     // 172.16/12
        || inIPv4Block(address, 0xC0000000u, 24)     // 192.0.0/24: protocol assignments
        || inIPv4Block(address, 0xC0A80000u, 16)     // 192.168/16
        || inIPv4Block(address, 0xC6120000u, 15)     // 198.18/15: benchmarking
        || inIPv4Block(address, 0xE0000000u, 4)      // 224/4: multicast
        || inIPv4Block(address, 0xF0000000u, 4);     // 240/4: reserved, and the broadcast address
}

}  // namespace

bool ScriptNetworkPolicy::isPrivateAddress(const QHostAddress& address) {
    if (address.isNull()) {
        return true;
    }
    bool isIPv4 = false;
    const quint32 ipv4 = address.toIPv4Address(&isIPv4);   // an IPv4-mapped IPv6 address converts too
    if (isIPv4) {
        return isPrivateIPv4(ipv4);
    }
    if (address.protocol() != QAbstractSocket::IPv6Protocol) {
        return true;
    }
    if (address == QHostAddress(QHostAddress::AnyIPv6) || address.isLoopback() || address.isLinkLocal() ||
        address.isSiteLocal() || address.isUniqueLocalUnicast() || address.isMulticast()) {
        return true;
    }
    // an IPv4 address carried in IPv6 by a translator (64:ff9b::/96) or the deprecated compatible form (::/96)
    const Q_IPV6ADDR bytes = address.toIPv6Address();
    static const quint8 NAT64[12] = { 0x00, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0 };
    static const quint8 COMPATIBLE[12] = { 0 };
    if (memcmp(bytes.c, NAT64, 12) == 0 || memcmp(bytes.c, COMPATIBLE, 12) == 0) {
        const quint32 carried = (quint32(bytes.c[12]) << 24) | (quint32(bytes.c[13]) << 16) |
                                (quint32(bytes.c[14]) << 8) | quint32(bytes.c[15]);
        return isPrivateIPv4(carried);
    }
    return false;
}

bool ScriptNetworkPolicy::isLocalHostName(const QString& hostName) {
    const QString host = hostName.toLower();
    if (host.isEmpty() || host == "localhost") {
        return true;
    }
    if (!host.contains('.')) {
        return true;    // a name on the local network (printer, nas, router)
    }
    static const QStringList LOCAL_SUFFIXES { ".localhost", ".local", ".lan", ".home", ".internal", ".home.arpa",
                                              ".in-addr.arpa", ".ip6.arpa" };
    for (const QString& suffix : LOCAL_SUFFIXES) {
        if (host.endsWith(suffix)) {
            return true;
        }
    }
    return false;
}

bool ScriptNetworkPolicy::isLocalOrigin(const QUrl& scriptUrl) {
    if (scriptUrl.isEmpty()) {
        return false;   // unknown: held to the rule
    }
    const QString scheme = scriptUrl.scheme().toLower();
    if (scheme == "file" || scheme == "qrc") {
        return true;
    }
    if (!WEB_SCHEMES.contains(scheme)) {
        // atp (the world's asset server) and about (an entities engine itself, an embedded entity script): judged by
        // the world; no scheme at all (a bare path, or an entity script's own code standing in for its URL): not a
        // URL, so not local. The engines give file URLs for scripts from disk.
        return false;
    }
    const QString host = scriptUrl.host().toLower();
    const QHostAddress address(host);
    return address.isNull() ? isLocalHostName(host) : isPrivateAddress(address);
}

bool ScriptNetworkPolicy::isWorldLocal() {
    auto nodeList = DependencyManager::get<NodeList>();
    if (!nodeList) {
        return false;
    }
    const DomainHandler& domain = nodeList->getDomainHandler();
    const QHostAddress& ip = domain.getIP();
    if (!ip.isNull()) {
        return isPrivateAddress(ip);
    }
    const QString host = domain.getHostname().toLower();
    if (host.isEmpty()) {
        return domain.isServerless();   // a world loaded from a file on this machine
    }
    const QHostAddress address(host);
    return address.isNull() ? isLocalHostName(host) : isPrivateAddress(address);
}

bool ScriptNetworkPolicy::isStrict() {
    static const bool strict = qEnvironmentVariable("DARKRIDE_SCRIPT_NETWORK_STRICT") == "1";
    return strict;
}

// A file's path with links followed and "." and ".." resolved; a path that names no file is cleaned by its spelling.
static QString canonicalPath(const QString& path) {
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(path) : canonical;
}

bool ScriptNetworkPolicy::isAppScript(const QUrl& scriptUrl) {
    if (scriptUrl.scheme().toLower() == "qrc") {
        return true;
    }
    if (!scriptUrl.isLocalFile()) {
        return false;
    }
    const QUrl defaults = PathUtils::defaultScriptsLocation();
    if (!defaults.isLocalFile()) {
        return false;
    }
    // both paths canonical, so that a path which only starts in the folder ("scripts/../..") doesn't count; the app's
    // own paths never hold a backslash (a separator on Windows, a plain character elsewhere)
    const QString local = scriptUrl.toLocalFile();
    if (local.contains('\\')) {
        return false;
    }
    const QString folder = canonicalPath(defaults.toLocalFile());
    const QString path = canonicalPath(local);
    if (folder.isEmpty() || folder == "/") {
        return false;
    }
    return path.compare(folder, PathUtils::getFSCaseSensitivity()) == 0 ||
           path.startsWith(folder + "/", PathUtils::getFSCaseSensitivity());
}

bool ScriptNetworkPolicy::applies(const QUrl& origin, bool worldIsLocal) {
    if (isStrict()) {
        return !isAppScript(origin);
    }
    return !isLocalOrigin(origin) && !worldIsLocal;
}

ScriptNetworkPolicy::Decision ScriptNetworkPolicy::check(const QUrl& target, const QUrl& origin, bool worldIsLocal) {
    Decision decision;
    if (ScriptCaller::isRetired()) {
        // a script of a world that was left reaches nothing more, wherever it came from (security #32)
        decision.allowed = false;
        decision.reason = WORLD_LEFT;
        return decision;
    }
    if (!applies(origin, worldIsLocal)) {
        return decision;
    }
    const QString scheme = target.scheme().toLower();
    if (scheme == ASSET_SCHEME) {
        return decision;    // the world's own asset server, reached through the world's connection
    }
    if (!WEB_SCHEMES.contains(scheme)) {
        decision.allowed = false;
        decision.reason = scheme.isEmpty() ? "not a web address" : "not a web address (" + scheme + ")";
        return decision;
    }
    const QString host = target.host().toLower();
    if (host.isEmpty()) {
        decision.allowed = false;
        decision.reason = "no host in the address";
        return decision;
    }
    const QHostAddress address(host);
    if (!address.isNull()) {
        if (isPrivateAddress(address)) {
            decision.allowed = false;
            decision.reason = "an address on this machine or its network";
        }
        return decision;
    }
    if (isLocalHostName(host)) {
        decision.allowed = false;
        decision.reason = "a name for this machine or its network";
        return decision;
    }
    decision.allowed = false;
    decision.needsLookup = true;
    return decision;
}

ScriptNetworkPolicy::Decision ScriptNetworkPolicy::checkAddresses(const QList<QHostAddress>& addresses) {
    Decision decision;
    if (addresses.isEmpty()) {
        decision.allowed = false;
        decision.reason = "the name has no address";
        return decision;
    }
    for (const QHostAddress& address : addresses) {
        if (isPrivateAddress(address)) {
            // one private answer among public ones refuses the name: a name that answers both ways is a sign of rebinding
            decision.allowed = false;
            decision.reason = "the name leads to an address on this machine or its network (" + address.toString() + ")";
            return decision;
        }
    }
    // an IPv4 answer first when there is one: a machine without working IPv6 would fail on an IPv6 one, where the
    // network manager used to try the next
    decision.address = addresses.first();
    for (const QHostAddress& address : addresses) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol) {
            decision.address = address;
            break;
        }
    }
    return decision;
}

void ScriptNetworkPolicy::pin(QNetworkRequest& request, const Decision& decision) {
    if (!decision.allowed || decision.address.isNull()) {
        return;
    }
    QUrl url = request.url();
    const QString name = url.host();
    if (name.isEmpty() || !QHostAddress(name).isNull()) {
        return;
    }
    url.setHost(decision.address.toString());   // an IPv6 address gets its brackets from QUrl
    request.setUrl(url);
    qCDebug(networking) << "a script's request to" << name << "pinned to" << decision.address.toString();
    const int port = url.port(-1);
    request.setRawHeader("Host", (port == -1 ? name : name + ":" + QString::number(port)).toUtf8());
    // the name travels in the Host header, which is HTTP/1.1's: HTTP/2 names the server from the URL (now the
    // address) and a host serving many sites refuses it. Qt 5 asks for HTTP/1.1 by default; Qt 6 asks for HTTP/2
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    const QString scheme = url.scheme().toLower();
    if (scheme == "https" || scheme == "wss") {
        request.setPeerVerifyName(name);
    }
}

void ScriptNetworkPolicy::decide(const QUrl& target, const QUrl& origin, QObject* context,
                                 std::function<void(const Decision&)> callback, bool worldIsLocal) {
    const Decision first = check(target, origin, worldIsLocal);
    if (!first.needsLookup) {
        callback(first);
        return;
    }
    QHostInfo::lookupHost(target.host(), context, [callback](const QHostInfo& info) {
        if (ScriptCaller::isRetired()) {
            // the world was left while the name was looked up (the callback runs on the script's own thread)
            Decision decision;
            decision.allowed = false;
            decision.reason = WORLD_LEFT;
            callback(decision);
            return;
        }
        if (info.error() != QHostInfo::NoError) {
            Decision decision;
            decision.allowed = false;
            decision.reason = "the name could not be looked up (" + info.errorString() + ")";
            callback(decision);
            return;
        }
        callback(checkAddresses(info.addresses()));
    });
}

bool ScriptNetworkPolicy::decideBlocking(const QUrl& target, const QUrl& origin, QString& reason, bool worldIsLocal) {
    Decision decision = check(target, origin, worldIsLocal);
    if (decision.needsLookup) {
        const QHostInfo info = QHostInfo::fromName(target.host());
        if (info.error() != QHostInfo::NoError) {
            reason = "the name could not be looked up (" + info.errorString() + ")";
            return false;
        }
        decision = checkAddresses(info.addresses());
    }
    reason = decision.reason;
    return decision.allowed;
}

QString ScriptNetworkPolicy::refusalLine(const QUrl& origin, const QUrl& target, const QString& reason) {
    // a line stays a line: a very long address (code in a URL) is cut
    static constexpr int MAX_SHOWN = 200;
    auto shown = [](const QUrl& url) {
        const QString text = url.toDisplayString();
        return text.length() > MAX_SHOWN ? text.left(MAX_SHOWN) + "..." : text;
    };
    return QString("%1 may not reach %2: %3. A script from the network uses web addresses to public hosts only; "
                   "a script loaded from this machine, or a world served from it or its network, keeps every address.")
        .arg(origin.isEmpty() ? QString("a script") : shown(origin), shown(target), reason);
}
