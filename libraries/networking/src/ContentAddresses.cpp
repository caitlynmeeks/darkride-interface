//
//  ContentAddresses.cpp
//  libraries/networking/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#include "ContentAddresses.h"

#include <QtCore/QDir>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

namespace ContentAddresses {

    static const QString APP_RESOURCES_PREFIX = QStringLiteral("/~/");
    static const QString FILE_SCHEME = QStringLiteral("file");

    // "/~/rest" is the app's own resources folder (PathUtils::expandToLocalDataAbsolutePath puts rest after the
    // folder's path), which only ".." can leave: the rest is decoded once, as the loader decodes it, its separators
    // made the same, its leading separators dropped (they only double the folder's own) and the path cleaned; what
    // still starts with ".." climbs out.
    static bool climbsOutOfResources(const QString& address) {
        QString rest = QUrl::fromPercentEncoding(address.mid(APP_RESOURCES_PREFIX.size()).toUtf8());
        rest.replace('\\', '/');
        while (rest.startsWith('/')) {
            rest.remove(0, 1);
        }
        const QString cleaned = QDir::cleanPath(rest);
        return cleaned == QStringLiteral("..") || cleaned.startsWith(QStringLiteral("../"));
    }

    // "C:\...", "C:/..." or "C:" alone
    static bool isDriveLetter(const QString& address) {
        return address.size() >= 2 && address.at(0).isLetter() && address.at(1) == ':' &&
               (address.size() == 2 || address.at(2) == '/' || address.at(2) == '\\');
    }

    bool isPathOnThisComputer(const QString& address) {
        const QString trimmed = address.trimmed();
        if (trimmed.isEmpty()) {
            return false;
        }
        if (trimmed.startsWith(APP_RESOURCES_PREFIX)) {
            return climbsOutOfResources(trimmed);
        }
        if (trimmed.startsWith('/') || trimmed.startsWith('\\') || isDriveLetter(trimmed)) {
            return true;
        }
        return QUrl(trimmed).scheme() == FILE_SCHEME;   // QUrl lowercases the scheme, so "FILE:" is caught too
    }

    bool isOnThisComputer(const QString& address) {
        const QString trimmed = address.trimmed();
        if (trimmed.isEmpty()) {
            return false;
        }
        if (trimmed.startsWith(APP_RESOURCES_PREFIX)) {
            return climbsOutOfResources(trimmed);
        }
        // the schemes ResourceManager::createResourceRequest fetches over the network or from the app's own resources
        // (getKnownUrls there, less file:); every other address is read as "file:///" + the address. Nearly every
        // real address starts with one of them: that is settled by a prefix compare, before any parsing.
        static const QStringList FETCHED_PREFIXES = { QStringLiteral("http://"), QStringLiteral("https://"),
                                                      QStringLiteral("atp:"),    QStringLiteral("tea://"),
                                                      QStringLiteral("qrc:"),    QStringLiteral("ftp://") };
        for (const QString& prefix : FETCHED_PREFIXES) {
            if (trimmed.startsWith(prefix, Qt::CaseInsensitive)) {
                return false;
            }
        }
        if (isPathOnThisComputer(trimmed)) {
            return true;
        }
        static const QStringList FETCHED_SCHEMES = { QStringLiteral("http"), QStringLiteral("https"),
                                                     QStringLiteral("ftp"),  QStringLiteral("atp"),
                                                     QStringLiteral("tea"),  QStringLiteral("qrc") };
        const QString scheme = QUrl(trimmed).scheme();
        return scheme.isEmpty() || !FETCHED_SCHEMES.contains(scheme);
    }

}
