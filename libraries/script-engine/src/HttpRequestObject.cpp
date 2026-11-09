//
//  HttpRequestObject.cpp
//  libraries/script-engine/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "HttpRequestObject.h"

#include <QtCore/QEventLoop>
#include <QtCore/QJsonDocument>

#include <QtCore/QPointer>

#include <NetworkAccessManager.h>
#include <NetworkingConstants.h>
#include <ScriptNetworkPolicy.h>
#include <SharedUtil.h>

#include "ResourceRequestObserver.h"
#include "ScriptEngine.h"

HttpRequestObject::HttpRequestObject(QObject* parent) : QObject(parent), _timer(this) {
    _request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);   // what FollowRedirectsAttribute meant
    _request.setHeader(QNetworkRequest::UserAgentHeader, TIVOLI_CLOUD_VR_USER_AGENT);
    _timer.setSingleShot(true);
    connect(&_timer, &QTimer::timeout, this, &HttpRequestObject::onTimeout);
}

HttpRequestObject::~HttpRequestObject() {
    abortRequest();
}

int HttpRequestObject::status() const {
    return _reply ? _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : _lastStatus;
}

QString HttpRequestObject::statusText() const {
    return _reply ? _reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString() : _lastStatusText;
}

void HttpRequestObject::open(const QString& method, const QString& url, bool async, const QString& username,
                             const QString& password) {
    if (_readyState != UNSENT) {
        return;
    }
    _method = method;
    _url.setUrl(url);
    _async = async;
    if (!username.isEmpty()) {
        _url.setUserName(username);
    }
    if (!password.isEmpty()) {
        _url.setPassword(password);
    }
    _request.setUrl(_url);
    setReadyState(OPENED);
}

void HttpRequestObject::send() {
    send(QVariant());
}

void HttpRequestObject::send(const QVariant& data) {
    if (_readyState != OPENED || _reply || _deciding) {
        return;
    }
    if (data.isValid() && !data.isNull()) {
        _sendData = data.type() == QVariant::ByteArray ? data.toByteArray() : data.toString().toUtf8();
    }
    _hopUrl = _url;
    _redirects = 0;
    _decision = ScriptNetworkPolicy::Decision();
    if (!_ruled) {
        doSend();
    } else {
        // the network rule for a script's own requests: judged at once, or after a name lookup
        _deciding = true;
        const int generation = ++_generation;
        ScriptNetworkPolicy::decide(_url, _origin, this, [this, generation](const ScriptNetworkPolicy::Decision& decision) {
            if (generation != _generation) {
                return;     // aborted meanwhile
            }
            if (decision.allowed) {
                _deciding = false;
                _decision = decision;
                doSend();
                return;
            }
            // refused: ends as a failed request does, after send() has returned
            const QString reason = decision.reason;
            QMetaObject::invokeMethod(this, [this, generation, reason] {
                if (generation == _generation) {
                    _deciding = false;
                    refuse(reason);
                }
            }, Qt::QueuedConnection);
        }, _worldIsLocal);
    }
    if (!_async && _readyState != DONE) {
        // the script's queued callbacks run while it waits: a world's script's wait ends when its engine stops or its
        // world is left (checked every 500 ms), and the request is aborted, so nothing of it answers later
        auto ending = [this] { return isEnding(); };
        if (!ending()) {
            QEventLoop loop;
            QTimer monitor;
            connect(this, &HttpRequestObject::requestComplete, &loop, &QEventLoop::quit);
            connect(&monitor, &QTimer::timeout, &loop, [&loop, ending] {
                if (ending()) {
                    loop.exit(-1);
                }
            });
            monitor.start(500);
            loop.exec();
        }
        if (_readyState != DONE && ending()) {
            abort();
        }
    }
}

void HttpRequestObject::refuse(const QString& reason) {
    _timer.stop();
    if (_logger) {
        _logger(ScriptNetworkPolicy::refusalLine(_origin, _url, reason));
    }
    _errorCode = QNetworkReply::ContentAccessDenied;
    _lastStatus = 0;
    _lastStatusText.clear();
    _headers.clear();
    _sendData.clear();
    setReadyState(DONE);
    emit requestComplete();
}

void HttpRequestObject::doSend() {
    DependencyManager::get<ResourceRequestObserver>()->update(_url, -1, "HttpRequestObject::doSend");
    _byHand = _ruled && ScriptNetworkPolicy::applies(_origin, _worldIsLocal);
    QNetworkRequest request = _request;
    request.setUrl(_hopUrl);
    if (_byHand) {
        // to the address the rule checked and no other (no second lookup of the name); a redirect comes back here,
        // judged and pinned as the request was
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        ScriptNetworkPolicy::pin(request, _decision);
    }
    _reply = NetworkAccessManager::getInstance().sendCustomRequest(request, _method.toLatin1(), _sendData);
    connect(_reply, &QNetworkReply::finished, this, &HttpRequestObject::onFinished);
    connect(_reply, &QNetworkReply::downloadProgress, this, &HttpRequestObject::onDownloadProgress);
    if (_timeout > 0) {
        _timer.start(_timeout);
    }
}

void HttpRequestObject::onDownloadProgress(qint64 received, qint64 total) {
    Q_UNUSED(total);
    if (_byHand && _reply && _reply->hasRawHeader("Location") &&
        _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() / 100 == 3) {
        return;     // a redirect's body is not the response
    }
    if (_readyState == OPENED && received > 0) {
        setReadyState(HEADERS_RECEIVED);
        setReadyState(LOADING);
    }
}

void HttpRequestObject::onTimeout() {
    if (!isEnding()) {
        emit timedOut();
    }
    abortRequest();
    _errorCode = QNetworkReply::TimeoutError;
    setReadyState(DONE);
    emit requestComplete();
}

void HttpRequestObject::onFinished() {
    const int status = _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (_byHand && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
        _reply->hasRawHeader("Location")) {
        followRedirect(status);
        return;
    }
    _timer.stop();
    _errorCode = _reply->error();
    _lastStatus = _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    _lastStatusText = _reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    _headers = _reply->rawHeaderPairs();
    if (_errorCode == QNetworkReply::NoError) {
        _rawResponse = _reply->readAll();
        if (_responseType == "json") {
            QJsonParseError parseError;
            QJsonDocument document = QJsonDocument::fromJson(_rawResponse, &parseError);
            _response = parseError.error == QJsonParseError::NoError ? document.toVariant() : QVariant::fromValue(nullptr);
        } else if (_responseType == "arraybuffer") {
            _response = _rawResponse;
        } else {
            _response = QString::fromUtf8(_rawResponse);
        }
    }
    // the reply is read; the state change runs the script's handler, which may start another request. What was
    // sent is not needed any more: a request the script keeps would otherwise keep its body
    QNetworkReply* reply = _reply;
    _reply = nullptr;
    reply->deleteLater();
    _sendData.clear();
    setReadyState(DONE);
    emit requestComplete();
}

void HttpRequestObject::followRedirect(int status) {
    // relative to the hop's address by its name, not the address it was pinned to
    const QUrl next = _hopUrl.resolved(QUrl::fromEncoded(_reply->rawHeader("Location")));
    QNetworkReply* reply = _reply;
    disconnect(reply, nullptr, this, nullptr);
    _reply = nullptr;
    reply->deleteLater();
    QString reason;
    if (++_redirects > _request.maximumRedirectsAllowed()) {
        reason = "too many redirects";
    } else if (_hopUrl.scheme().toLower() == "https" && next.scheme().toLower() != "https") {
        reason = "a redirect from https to a plain address";   // a secure request stays secure
    }
    if (!reason.isEmpty()) {
        _hopUrl = next;
        refuse(reason);
        return;
    }
    if (status == 303 || ((status == 301 || status == 302) && _method.toUpper() == "POST")) {
        _method = "GET";        // as browsers do
        _sendData.clear();
    }
    _hopUrl = next;
    _deciding = true;
    const int generation = ++_generation;
    ScriptNetworkPolicy::decide(next, _origin, this, [this, generation, next](const ScriptNetworkPolicy::Decision& decision) {
        if (generation != _generation) {
            return;
        }
        _deciding = false;
        if (!decision.allowed) {
            refuse(decision.reason + " (a redirect)");
            return;
        }
        _decision = decision;
        doSend();
    }, _worldIsLocal);
}

void HttpRequestObject::abort() {
    ++_generation;
    _deciding = false;
    abortRequest();
}

void HttpRequestObject::abortRequest() {
    _timer.stop();
    if (_reply) {
        disconnect(_reply, nullptr, this, nullptr);
        _reply->abort();
        _reply->deleteLater();
        _reply = nullptr;
    }
}

void HttpRequestObject::setRequestHeader(const QString& name, const QString& value) {
    _request.setRawHeader(name.toLatin1(), value.toLatin1());
}

QString HttpRequestObject::getAllResponseHeaders() const {
    QByteArray headers;
    for (const auto& pair : _headers) {
        headers.append(pair.first);
        headers.append(": ");
        headers.append(pair.second);
        headers.append("\n");
    }
    return QString::fromUtf8(headers);
}

QVariant HttpRequestObject::getResponseHeader(const QString& name) const {
    const QByteArray wanted = name.toLatin1();
    for (const auto& pair : _headers) {
        if (pair.first.compare(wanted, Qt::CaseInsensitive) == 0) {
            return QString::fromUtf8(pair.second);
        }
    }
    return QVariant::fromValue(nullptr);
}

void HttpRequestObject::setReadyState(ReadyState state) {
    if (state != _readyState) {
        _readyState = state;
        // a world's script hears no answer once its engine stops or its world is left
        if (!isEnding()) {
            emit readyStateChanged();
        }
    }
}


// --- the factory ---------------------------------------------------------------------------------------------------

HttpRequestFactory::HttpRequestFactory(ScriptEngine* engine) : QObject(engine), _engine(engine) {}

HttpRequestObject* HttpRequestFactory::createRequest(const QString& origin) {
    auto request = new HttpRequestObject(this);
    if (_engine) {
        request->setOrigin(_engine->ruledOrigin(_engine->verifiedScriptOrigin(origin)), _engine->networkRuled(), _engine->worldIsLocal());
        QPointer<ScriptEngine> engine(_engine);
        request->setLogger([engine](const QString& line) {
            if (engine) {
                engine->scriptWarningMessage(line);
            }
        });
        request->setEndingCheck([engine] { return !engine || engine->isWorldEnding(); });
    }
    return request;
}

void HttpRequestFactory::dispose(QObject* request) {
    if (request && request->parent() == this) {
        request->deleteLater();
    }
}
