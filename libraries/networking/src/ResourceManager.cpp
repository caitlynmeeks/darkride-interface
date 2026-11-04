//
//  ResourceManager.cpp
//  libraries/networking/src
//
//  Created by Ryan Huffman on 2015/07/23
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "ResourceManager.h"

#include <mutex>

#include <QNetworkDiskCache>
#include <QStandardPaths>
#include <QThread>
#include <QFileInfo>
#include <QRegularExpression>

#include <SharedUtil.h>
#include <plugins/Forward.h>
#include <plugins/PluginManager.h>
#include <plugins/TeaProtocolPlugin.h>

#include "AssetResourceRequest.h"
#include "FileResourceRequest.h"
#include "HTTPResourceRequest.h"
#include "NetworkAccessManager.h"
#include "NetworkLogging.h"
#include "RedirectCache.h"

ResourceManager::ResourceManager(bool atpSupportEnabled) : _atpSupportEnabled(atpSupportEnabled) {
    _thread.setObjectName("Resource Manager Thread");

    if (_atpSupportEnabled) {
        auto assetClient = DependencyManager::set<AssetClient>();
        assetClient->moveToThread(&_thread);
        QObject::connect(&_thread, &QThread::started, assetClient.data(), &AssetClient::initCaching);
    }

    _thread.start();
}

ResourceManager::~ResourceManager() {
    if (_thread.isRunning()) {
        _thread.quit();
        static const auto MAX_RESOURCE_MANAGER_THREAD_QUITTING_TIME = MSECS_PER_SECOND / 2;
        if (!_thread.wait(MAX_RESOURCE_MANAGER_THREAD_QUITTING_TIME)) {
            _thread.terminate();
        }
    }
}

// An address read as one on this machine: a file or qrc URL, or a string the loaders take as a path (no scheme, or a
// Windows drive letter); a prefix that is part of such an address (empty, "fi") counts too.
static bool isLocalAddress(const QString& address) {
    const QString scheme = QUrl(address).scheme().toLower();
    return scheme.isEmpty() || scheme.length() == 1 || scheme == "file" || scheme == "qrc";
}

void ResourceManager::setUrlPrefixOverride(const QString& prefix, const QString& replacement) {
    // what is on this machine is never served from elsewhere
    if (!replacement.isEmpty() && isLocalAddress(prefix) && !isLocalAddress(replacement)) {
        qCWarning(networking) << "Not mapping addresses on this machine elsewhere:" << prefix.left(200) << replacement.left(200);
        return;
    }
    QMutexLocker locker(&_prefixMapLock);
    if (replacement.isEmpty()) {
        _prefixMap.erase(prefix);
    } else {
        _prefixMap[prefix] = replacement;
    }
}

// Whether the rest of an address after a prefix stays under the prefix's replacement: its path (up to a query or a
// fragment), percent-decoded, with backslashes read as separators, never climbs above where it starts.
static bool staysUnderReplacement(const QString& rest) {
    QString path = rest;
    const int end = path.indexOf(QRegularExpression("[?#]"));
    if (end >= 0) {
        path.truncate(end);
    }
    path = QUrl::fromPercentEncoding(path.toUtf8());
    path.replace('\\', '/');
    int depth = 0;
    for (const QString& part : path.split('/')) {
        if (part == "..") {
            if (--depth < 0) {
                return false;
            }
        } else if (!part.isEmpty() && part != ".") {
            ++depth;
        }
    }
    return true;
}

QString ResourceManager::normalizeURL(const QString& urlString) {
    QString result = urlString;
    PrefixMap copy;

    {
        QMutexLocker locker(&_prefixMapLock);
        copy = _prefixMap;
    }

    foreach (const auto& entry, copy) {
        const auto& prefix = entry.first;
        const auto& replacement = entry.second;
        if (result.startsWith(prefix)) {
            // a prefix maps what is under it, never what is beside it: an address that climbs out of the replacement
            // ("../", however spelled), or one that only starts with a prefix without a closing slash ("…/model-1" for
            // "…/model-1-other/"), is left as written
            const QString rest = result.mid(prefix.size());
            const bool atBoundary = prefix.endsWith('/') || rest.isEmpty() || rest.startsWith('/') || rest.startsWith('?') ||
                                    rest.startsWith('#');
            if (!atBoundary || !staysUnderReplacement(rest)) {
                qCWarning(networking) << "Not replacing" << prefix << "in an address outside it:" << result.left(200);
                continue;
            }
            qCDebug(networking) << "Replacing " << prefix << " with " << replacement;
            result.replace(0, prefix.size(), replacement);
        }
    }
    return result;
}

const QSet<QString>& getKnownUrls() {
    static QSet<QString> knownUrls;
    static std::once_flag once;
    std::call_once(once, [] {
        knownUrls.insert(URL_SCHEME_QRC);
        knownUrls.insert(HIFI_URL_SCHEME_FILE);
        knownUrls.insert(HIFI_URL_SCHEME_HTTP);
        knownUrls.insert(HIFI_URL_SCHEME_HTTPS);
        knownUrls.insert(HIFI_URL_SCHEME_FTP);
        knownUrls.insert(URL_SCHEME_ATP);
        knownUrls.insert(URL_SCHEME_TEA);
    });
    return knownUrls;
}

// tea://<username>/<path>, a user file by its path, was fetched by Tivoli's tea protocol plugin, which asks
// tea.tivolicloud.com (gone) and can't be had any more (its download is gone too, and there was never one for Apple
// silicon). The metaverse's files host serves the same path to everyone unless the file is tea-only (private):
// https://files.<metaverse host>/<username>/<path>, with any query and fragment kept. A username's case doesn't
// matter there, so the host part's lowercasing by QUrl is harmless.
static QUrl teaUrlOnFilesHost(const QUrl& teaUrl) {
    QUrl metaverse = NetworkingConstants::METAVERSE_SERVER_URL();
    QUrl files;
    files.setScheme(metaverse.scheme());
    files.setHost("files." + metaverse.host());
    files.setPort(metaverse.port());
    files.setPath("/" + teaUrl.host(QUrl::FullyEncoded) + teaUrl.path(QUrl::FullyEncoded), QUrl::StrictMode);
    if (teaUrl.hasQuery()) {
        files.setQuery(teaUrl.query(QUrl::FullyEncoded), QUrl::StrictMode);
    }
    if (teaUrl.hasFragment()) {
        files.setFragment(teaUrl.fragment(QUrl::FullyEncoded), QUrl::StrictMode);
    }
    return files;
}

// ContentAddresses::isOnThisComputer answers "would this read a file on this computer" by this function's rule
// (getKnownUrls, the file: fallback below): keep the two in step.
QUrl ResourceManager::normalizeURL(const QUrl& originalUrl) {
    QUrl url = QUrl(normalizeURL(originalUrl.toString()));
    auto scheme = url.scheme();
    if (scheme == URL_SCHEME_TEA) {
        return teaUrlOnFilesHost(url);
    }
    if (!getKnownUrls().contains(scheme)) {
        // check the degenerative file case: on windows we can often have urls of the form c:/filename
        // this checks for and works around that case.
        QUrl urlWithFileScheme{ HIFI_URL_SCHEME_FILE + ":///" + url.toString() };
        if (!urlWithFileScheme.toLocalFile().isEmpty()) {
            return urlWithFileScheme;
        }
    }
    return url;
}

void ResourceManager::cleanup() {
    RedirectCache::save();
    // cleanup the AssetClient thread
    DependencyManager::destroy<AssetClient>();
    _thread.quit();
    _thread.wait();
}

ResourceRequest* ResourceManager::createResourceRequest(
    QObject* parent,
    const QUrl& url,
    const bool isObservable,
    const qint64 callerId,
    const QString& extra
) {
    auto normalizedURL = normalizeURL(url);
    auto scheme = normalizedURL.scheme();

    ResourceRequest* request = nullptr;

    if (scheme == HIFI_URL_SCHEME_FILE || scheme == URL_SCHEME_QRC) {
        request = new FileResourceRequest(normalizedURL, isObservable, callerId, extra);
    } else if (scheme == HIFI_URL_SCHEME_HTTP || scheme == HIFI_URL_SCHEME_HTTPS || scheme == HIFI_URL_SCHEME_FTP) {
        request = new HTTPResourceRequest(normalizedURL, isObservable, callerId, extra);
    } else if (scheme == URL_SCHEME_ATP) {
        if (!_atpSupportEnabled) {
            qCDebug(networking) << "ATP support not enabled, unable to create request for URL: " << url.url();
            return nullptr;
        }
        request = new AssetResourceRequest(normalizedURL, isObservable, callerId, extra);
    } else if (scheme == URL_SCHEME_TEA) {
        auto pluginManager = DependencyManager::get<PluginManager>();
        TeaProtocolPluginPointer teaProtocolPlugin = pluginManager->getTeaProtocolPlugin();
        if (teaProtocolPlugin) {
            request = teaProtocolPlugin->initRequest(normalizedURL, isObservable, callerId, extra);
        } else {
            return nullptr;
        }
    } else {
        qCDebug(networking) << "Unknown scheme (" << scheme << ") for URL: " << url.url();
        return nullptr;
    }
    Q_ASSERT(request);

    if (parent) {
        QObject::connect(parent, &QObject::destroyed, request, &QObject::deleteLater);
    }
    request->moveToThread(&_thread);
    return request;
}

bool ResourceManager::resourceExists(const QUrl& url) {
    auto scheme = url.scheme();
    if (scheme == HIFI_URL_SCHEME_FILE) {
        QFileInfo file{ url.toString() };
        return file.exists();
    } else if (scheme == HIFI_URL_SCHEME_HTTP || scheme == HIFI_URL_SCHEME_HTTPS || scheme == HIFI_URL_SCHEME_FTP) {
        auto& networkAccessManager = NetworkAccessManager::getInstance();
        QNetworkRequest request{ url };

        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setHeader(QNetworkRequest::UserAgentHeader, TIVOLI_CLOUD_VR_USER_AGENT);

        auto reply = networkAccessManager.head(request);

        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        reply->deleteLater();

        return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
    } else if (scheme == URL_SCHEME_ATP && _atpSupportEnabled) {
        auto request = new AssetResourceRequest(url, ResourceRequest::IS_NOT_OBSERVABLE);
        ByteRange range;
        range.fromInclusive = 1;
        range.toExclusive = 1;
        request->setByteRange(range);
        request->setCacheEnabled(false);

        QEventLoop loop;

        QObject::connect(request, &AssetResourceRequest::finished, &loop, &QEventLoop::quit);

        request->send();

        loop.exec();

        request->deleteLater();

        return request->getResult() == ResourceRequest::Success;
    }

    qCDebug(networking) << "Unknown scheme (" << scheme << ") for URL: " << url.url();
    return false;
}
