//
//  HTTPResourceRequest.cpp
//  libraries/networking/src
//
//  Created by Ryan Huffman on 2015/07/23
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "HTTPResourceRequest.h"

#include <QAbstractNetworkCache>
#include <QFile>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QMetaEnum>

#include <SharedUtil.h>
#include <StatTracker.h>
#include <Gzip.h>
#include <Brotli.h>

#include "AccountManager.h"
#include "NetworkAccessManager.h"
#include "NetworkingConstants.h"
#include "RedirectCache.h"
#include "ResourceCache.h"
#include "ResourceSizeLimit.h"
#include "NetworkLogging.h"

HTTPResourceRequest::~HTTPResourceRequest() {
    if (_reply) {
        _reply->disconnect(this);
        _reply->deleteLater();
        _reply = nullptr;
    }
}

void HTTPResourceRequest::setupTimer() {
    Q_ASSERT(!_sendTimer);
    static const int TIMEOUT_MS = 10000;

    _sendTimer = new QTimer();
    connect(this, &QObject::destroyed, _sendTimer, &QTimer::deleteLater);
    connect(_sendTimer, &QTimer::timeout, this, &HTTPResourceRequest::onTimeout);

    _sendTimer->setSingleShot(true);
    _sendTimer->start(TIMEOUT_MS);
}

void HTTPResourceRequest::cleanupTimer() {
    Q_ASSERT(_sendTimer);
    _sendTimer->disconnect(this);
    _sendTimer->deleteLater();
    _sendTimer = nullptr;
}

// The metaverse's files host, files.<metaverse host>: a private file there goes only to a signed-in Darkride client.
static bool isMetaverseFilesHost(const QUrl& url) {
    const QUrl metaverse = NetworkingConstants::METAVERSE_SERVER_URL();
    return url.scheme() == metaverse.scheme() && url.port() == metaverse.port() &&
        url.host().compare("files." + metaverse.host(), Qt::CaseInsensitive) == 0;
}

// The player's login as an Authorization header value, or nothing when signed out or expired. Read as it is, without
// AccountManager::hasValidAccessToken, which may start a token refresh from this thread.
static QByteArray accessTokenHeaderValue() {
    const auto& token = DependencyManager::get<AccountManager>()->getAccountInfo().getAccessToken();
    if (token.token.isEmpty() || token.isExpired()) {
        return QByteArray();
    }
    return token.authorizationHeaderValue();
}

void HTTPResourceRequest::doSend() {
    DependencyManager::get<StatTracker>()->incrementStat(STAT_HTTP_REQUEST_STARTED);

    // a URL whose redirect an earlier request followed is asked where it led (RedirectCache); if that fails, the
    // original URL is asked once more (onRequestFinished). With the player's login, only the files-host URL that
    // refused the file is asked: never _url, which may be anyone's and only redirected there
    _requestUrl = _url;
    _viaRedirectCache = false;
    if (_withAccessToken) {
        _requestUrl = _accessTokenUrl;
    } else if (_ruled) {
        // a script's own download: each address judged and pinned, so no remembered redirect target is used
        _hopUrl = _url;
        _hops = 0;
        judgeAndSend();
        return;
    } else if (!_triedOriginal) {
        QUrl remembered = RedirectCache::target(_url);
        if (!remembered.isEmpty()) {
            _requestUrl = remembered;
            _viaRedirectCache = true;
        }
    }

    sendNow();
}

void HTTPResourceRequest::judgeAndSend() {
    const int judging = ++_judging;
    ScriptNetworkPolicy::decide(_hopUrl, _ruleOrigin, this, [this, judging](const ScriptNetworkPolicy::Decision& decision) {
        if (judging != _judging || _state != InProgress) {
            return;
        }
        if (!decision.allowed) {
            finishRefused(decision.reason);
            return;
        }
        _hopDecision = decision;
        _requestUrl = _hopUrl;
        sendNow();
    }, _ruleWorldIsLocal);
}

void HTTPResourceRequest::finishRefused(const QString& reason) {
    qCDebug(networking) << ScriptNetworkPolicy::refusalLine(_ruleOrigin, _hopUrl, reason);
    _result = AccessDenied;
    _state = Finished;
    emit finished();
}

bool HTTPResourceRequest::followRedirect() {
    // a ruled download's redirect: the next hop, relative to this one by its name, judged and pinned as the first was
    const QUrl next = _hopUrl.resolved(QUrl::fromEncoded(_reply->rawHeader("Location")));
    _reply->disconnect(this);
    _reply->deleteLater();
    _reply = nullptr;
    if (++_hops > QNetworkRequest().maximumRedirectsAllowed()) {
        finishRefused("too many redirects");
        return true;
    }
    if (_hopUrl.scheme().toLower() == "https" && next.scheme().toLower() != "https") {
        _hopUrl = next;
        finishRefused("a redirect from https to a plain address");
        return true;
    }
    _hopUrl = next;
    judgeAndSend();
    return true;
}

void HTTPResourceRequest::sendNow() {
    QNetworkRequest networkRequest(_requestUrl);
    // with the player's login, no redirect is followed: Qt would send the login on to wherever it led (a private file
    // is passed through by the files host, never redirected)
    networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, !_withAccessToken && !_ruled
        ? QNetworkRequest::NoLessSafeRedirectPolicy : QNetworkRequest::ManualRedirectPolicy);
    if (_ruled && !_withAccessToken) {
        // to the address the rule checked and no other; a redirect comes back to onRequestFinished
        networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        ScriptNetworkPolicy::pin(networkRequest, _hopDecision);
    }
    // not HTTP/2: Qt 5.15's implementation made cold loads slower on this link, with 64 KB windows and with 32 MB
    // ones (measured 10-23, ARCHITECTURE.md §6 phase 2b); six HTTP/1.1 connections per host it is
    networkRequest.setHeader(QNetworkRequest::UserAgentHeader, TIVOLI_CLOUD_VR_USER_AGENT);

    if (_cacheEnabled) {
        // does browser-like caching unlike PreferCache
        networkRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferNetwork);
    } else {
        networkRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    }

    if (_byteRange.isSet()) {
        QString byteRange;
        if (_byteRange.fromInclusive < 0) {
            byteRange = QString("bytes=%1").arg(_byteRange.fromInclusive);
        } else {
            // HTTP byte ranges are inclusive on the `to` end: [from, to]
            byteRange = QString("bytes=%1-%2").arg(_byteRange.fromInclusive).arg(_byteRange.toExclusive - 1);
        }
        networkRequest.setRawHeader("Range", byteRange.toLatin1());
    }
    networkRequest.setAttribute(QNetworkRequest::HttpPipeliningAllowedAttribute, false);

    // when overriding Accept-Encoding, qt disables auto decompress
    networkRequest.setRawHeader("Accept-Encoding", "gzip, br");

    // a private file: the player's login, to our own files host only, only after it refused the file without it
    if (_withAccessToken && isMetaverseFilesHost(_requestUrl)) {
        networkRequest.setRawHeader(ACCESS_TOKEN_AUTHORIZATION_HEADER, accessTokenHeaderValue());
    }

    _reply = NetworkAccessManager::getInstance().get(networkRequest);
    
    connect(_reply, &QNetworkReply::finished, this, &HTTPResourceRequest::onRequestFinished);
    // the first headers of a reply, a redirect's included, say the host is there (ResourceCache::hasHostAnswered)
    const QString host = _url.host();
    connect(_reply, &QNetworkReply::metaDataChanged, this, [host] { ResourceCache::markHostAnswered(host); });
    connect(_reply, &QNetworkReply::redirected, this, [host](const QUrl&) { ResourceCache::markHostAnswered(host); });
    connect(_reply, &QNetworkReply::downloadProgress, this, &HTTPResourceRequest::onDownloadProgress);

    setupTimer();
}

void HTTPResourceRequest::onRequestFinished() {
    Q_ASSERT(_state == InProgress);
    Q_ASSERT(_reply);

    cleanupTimer();

    // Content-Range headers have the form: 
    //
    //   Content-Range: <unit> <range-start>-<range-end>/<size>
    //   Content-Range: <unit> <range-start>-<range-end>/*
    //   Content-Range: <unit> */<size>
    //
    static auto parseContentRangeHeader = [](QString contentRangeHeader) -> std::pair<bool, uint64_t> {
        auto unitRangeParts = contentRangeHeader.split(' ');
        if (unitRangeParts.size() != 2) {
            return { false, 0 };
        }

        auto rangeSizeParts = unitRangeParts[1].split('/');
        if (rangeSizeParts.size() != 2) {
            return { false, 0 };
        }

        auto sizeStr = rangeSizeParts[1];
        if (sizeStr == "*") {
            return { true, 0 };
        } else {
            bool ok;
            auto size = sizeStr.toLong(&ok);
            return { ok, size };
        }
    };

    static auto parseMediaType = [](QString contentTypeHeader) -> std::pair<bool, QString> {
        auto contentTypeParts = contentTypeHeader.split(';');
        if (contentTypeParts.size() < 1) {
            return { false, "" };
        }

        return { true, contentTypeParts[0] };
    };

    const int statusCode = _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    _statusCode = statusCode;
    if (_ruled && !_withAccessToken && statusCode >= 300 && statusCode < 400 && _reply->hasRawHeader("Location") &&
        (statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308)) {
        followRedirect();
        return;
    }
    switch(_reply->error()) {
        case QNetworkReply::NoError:
            if (_withAccessToken && statusCode >= 300 && statusCode < 400) {
                // redirects aren't followed with the login (doSend): a file that went public meanwhile, asked again
                // next time without it
                _result = Error;
                break;
            }
            if (_reply->bytesAvailable() > ResourceSizeLimit::forPath(_url.path())) {
                // a body past the limit for its kind (one the progress check didn't see, e.g. from the disk cache)
                // isn't read at all
                qWarning(networking) << "Refused a download over the size limit for its kind:" << _url.toString()
                                     << _reply->bytesAvailable() << "bytes";
                _result = Error;
                break;
            }
            _data = _reply->readAll();
            _loadedFromCache = _reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();
            _result = Success;

            if (_byteRange.isSet()) {
                if (statusCode == 206) {
                    _rangeRequestSuccessful = true;
                    auto contentRangeHeader = _reply->rawHeader("Content-Range");
                    bool success;
                    uint64_t size;
                    std::tie(success, size) = parseContentRangeHeader(contentRangeHeader);
                    if (success) {
                        _totalSizeOfResource = size;
                    } else {
                        qWarning(networking) << "Error parsing content-range header: " << contentRangeHeader;
                        _totalSizeOfResource = 0;
                    }
                } else {
                    _rangeRequestSuccessful = false;
                    _totalSizeOfResource = _data.size();
                }
            }

            {
                auto contentTypeHeader = _reply->rawHeader("Content-Type");
                bool success;
                QString mediaType;
                std::tie(success, mediaType) = parseMediaType(contentTypeHeader);
                if (success) {
                    _webMediaType = mediaType;
                }
            }

            {
                auto contentEncoding = _reply->rawHeader("Content-Encoding");
                if (contentEncoding.size() > 0) {
                    QByteArray decompressedData;
                    // what it unpacks to has the same limit as a download of its kind
                    const int maxSize = ResourceSizeLimit::forPath(_url.path());
                    if (contentEncoding.compare("gzip", Qt::CaseInsensitive) == 0) {
                        if (gunzip(_data, decompressedData, maxSize)) {
                            _data = decompressedData;
                        } else if (!_data.startsWith("\x1f\x8b")) {
                            // not gzip after all: a cached copy that a request with Qt's own decompression saved
                            // unpacked under the gzip label, or a host that labels plain text gzip; use it as it is
                        } else {
                            qWarning(networking) << "Failed to decompress gzip for: " << _url;
                            _result = Error;
                        }
                    } else if (contentEncoding.compare("br", Qt::CaseInsensitive) == 0) {
                        if (brotliDecompress(_data, decompressedData, maxSize)) {
                            _data = decompressedData;
                        } else {
                            qWarning(networking) << "Failed to decompress brotli for: " << _url;
                            _result = Error;
                        }
                    } else {
                        qWarning(networking) << "Unknown content-encoding: " << contentEncoding;
                        _result = Error;
                    }
                }
            }

            recordBytesDownloadedInStats(STAT_HTTP_RESOURCE_TOTAL_BYTES, _data.size());

            break;

        case QNetworkReply::TimeoutError:
            _result = Timeout;
            break;

        case QNetworkReply::ContentNotFoundError: // Script.include('https://httpbin.org/status/404')
            _result = NotFound;
            break;

        case QNetworkReply::ProtocolInvalidOperationError: // Script.include('https://httpbin.org/status/400')
            _result = InvalidURL;
            break;

        case QNetworkReply::UnknownContentError: // Script.include('QUrl("https://httpbin.org/status/402")')
        case QNetworkReply::ContentAccessDenied: // Script.include('https://httpbin.org/status/403'): Qt's name for a 403
        case QNetworkReply::ContentOperationNotPermittedError: // a 405
        case QNetworkReply::AuthenticationRequiredError: // Script.include('https://httpbin.org/basic-auth/user/passwd')
            _result = AccessDenied;
            break;

        case QNetworkReply::HostNotFoundError:      // Script.include('http://foo.bar.highfidelity.io')
            _result = HostNotFound;
            break;
        case QNetworkReply::RemoteHostClosedError:  // Script.include('http://127.0.0.1:22')
        case QNetworkReply::ConnectionRefusedError: // Script.include(http://127.0.0.1:1')
        case QNetworkReply::ServiceUnavailableError: // Script.include('QUrl("https://httpbin.org/status/503")')
            _result = ServerUnavailable;
            break;

        case QNetworkReply::UnknownServerError: // Script.include('QUrl("https://httpbin.org/status/504")')
        case QNetworkReply::InternalServerError: // Script.include('QUrl("https://httpbin.org/status/500")')
        default:
            qCDebug(networking) << "HTTPResourceRequest error:" << QMetaEnum::fromType<QNetworkReply::NetworkError>().valueToKey(_reply->error());
            _result = Error;
            break;
    }

    // our files host refuses a private file (403) to a request without the player's login: the URL that refused it
    // (where the request ended, after a remembered target or Qt's redirects) is asked once more with it, in this same
    // request, so the login goes only to our files host, and only for private files
    const QUrl refusedUrl = _reply->url();
    if (statusCode == 403 && !_withAccessToken && isMetaverseFilesHost(refusedUrl) &&
        !accessTokenHeaderValue().isEmpty()) {
        _reply->disconnect(this);
        _reply->deleteLater();
        _reply = nullptr;
        _withAccessToken = true;
        _accessTokenUrl = refusedUrl;
        doSend();
        return;
    }

    // where the request ended, after the redirects Qt followed, is remembered for the next request of this URL; a
    // remembered target that failed is forgotten and the original URL asked once more, in this same request
    const QUrl finalUrl = _reply->url();
    if (_ruled) {
        // a ruled download's hops were judged one by one: nothing to remember (its URL is a checked address)
    } else if (_result == Success) {
        if (finalUrl != _url) {
            RedirectCache::remember(_url, finalUrl);
        }
    } else if (_viaRedirectCache && !_triedOriginal) {
        qCDebug(networking) << "The remembered redirect target failed; asking the original URL:" << _url.toString();
        RedirectCache::forget(_url);
        _reply->disconnect(this);
        _reply->deleteLater();
        _reply = nullptr;
        _triedOriginal = true;
        DependencyManager::get<StatTracker>()->incrementStat(STAT_HTTP_REQUEST_FAILED);
        doSend();
        return;
    }

    _reply->disconnect(this);
    _reply->deleteLater();
    _reply = nullptr;
    
    _state = Finished;
    emit finished();

    auto statTracker = DependencyManager::get<StatTracker>();
    if (_result == Success) {
        statTracker->incrementStat(STAT_HTTP_REQUEST_SUCCESS);

        if (loadedFromCache()) {
            statTracker->incrementStat(STAT_HTTP_REQUEST_CACHE);
        }
    } else {
        statTracker->incrementStat(STAT_HTTP_REQUEST_FAILED);
    }
}

void HTTPResourceRequest::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal) {
    Q_ASSERT(_state == InProgress);

    // past the size limit for its kind (or announced past it), the download stops here and the request fails, as a
    // timeout does: nothing more is received or held
    const qint64 maxSize = ResourceSizeLimit::forPath(_url.path());
    if (bytesReceived > maxSize || bytesTotal > maxSize) {
        qWarning(networking) << "Stopped a download over the size limit for its kind:" << _url.toString()
                             << qMax(bytesReceived, bytesTotal) << "bytes, limit" << maxSize;
        _reply->disconnect(this);
        _reply->abort();
        _reply->deleteLater();
        _reply = nullptr;

        cleanupTimer();

        _result = Error;
        _state = Finished;
        emit finished();

        DependencyManager::get<StatTracker>()->incrementStat(STAT_HTTP_REQUEST_FAILED);
        return;
    }

    // We've received data, so reset the timer
    _sendTimer->start();

    emit progress(bytesReceived, bytesTotal);

    recordBytesDownloadedInStats(STAT_HTTP_RESOURCE_TOTAL_BYTES, bytesReceived);
}

void HTTPResourceRequest::onTimeout() {
    qCDebug(networking) << "Timeout, no data for 10 s:" << _requestUrl.toString();
    Q_ASSERT(_state == InProgress);
    // A download that stops short of its size and never finishes can be a cut-short copy in the disk cache: with
    // PreferNetwork the server's "not modified" sends Qt back to its own copy, which ends at the same byte on every
    // retry. That copy goes, under the address asked for and the one a redirect led to, so the retry downloads it whole.
    if (auto cache = NetworkAccessManager::getInstance().cache()) {
        cache->remove(_requestUrl);
        cache->remove(_reply->url());
    }
    _reply->disconnect(this);
    _reply->abort();
    _reply->deleteLater();
    _reply = nullptr;

    cleanupTimer();
    
    _result = Timeout;
    _state = Finished;
    emit finished();

    DependencyManager::get<StatTracker>()->incrementStat(STAT_HTTP_REQUEST_FAILED);
}
