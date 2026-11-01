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
    _request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
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
        QEventLoop loop;
        connect(this, &HttpRequestObject::requestComplete, &loop, &QEventLoop::quit);
        loop.exec();
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
    const bool ruled = _ruled && ScriptNetworkPolicy::applies(_origin, _worldIsLocal);
    if (ruled) {
        // a redirect is judged as the request was
        _request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, false);
        _request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    }
    _reply = NetworkAccessManager::getInstance().sendCustomRequest(_request, _method.toLatin1(), _sendData);
    connect(_reply, &QNetworkReply::finished, this, &HttpRequestObject::onFinished);
    connect(_reply, &QNetworkReply::downloadProgress, this, &HttpRequestObject::onDownloadProgress);
    if (ruled) {
        QNetworkReply* reply = _reply;
        connect(reply, &QNetworkReply::redirected, this, [this, reply](const QUrl& url) {
            if (reply->url().scheme().toLower() == "https" && url.scheme().toLower() != "https") {
                // a secure request stays secure
                if (_logger) {
                    _logger(ScriptNetworkPolicy::refusalLine(_origin, url, "a redirect from https to a plain address"));
                }
                reply->abort();
                return;
            }
            ScriptNetworkPolicy::decide(url, _origin, this, [this, reply, url](const ScriptNetworkPolicy::Decision& decision) {
                if (_reply != reply) {
                    return;
                }
                if (decision.allowed) {
                    emit reply->redirectAllowed();
                    return;
                }
                if (_logger) {
                    _logger(ScriptNetworkPolicy::refusalLine(_origin, url, decision.reason + " (a redirect)"));
                }
                reply->abort();
            }, _worldIsLocal);
        });
    }
    if (_timeout > 0) {
        _timer.start(_timeout);
    }
}

void HttpRequestObject::onDownloadProgress(qint64 received, qint64 total) {
    Q_UNUSED(total);
    if (_readyState == OPENED && received > 0) {
        setReadyState(HEADERS_RECEIVED);
        setReadyState(LOADING);
    }
}

void HttpRequestObject::onTimeout() {
    emit timedOut();
    abortRequest();
    _errorCode = QNetworkReply::TimeoutError;
    setReadyState(DONE);
    emit requestComplete();
}

void HttpRequestObject::onFinished() {
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
        emit readyStateChanged();
    }
}


// --- the factory ---------------------------------------------------------------------------------------------------

HttpRequestFactory::HttpRequestFactory(ScriptEngine* engine) : QObject(engine), _engine(engine) {}

HttpRequestObject* HttpRequestFactory::createRequest(const QString& origin) {
    auto request = new HttpRequestObject(this);
    if (_engine) {
        request->setOrigin(_engine->verifiedScriptOrigin(origin), _engine->isClientSide(), _engine->worldIsLocal());
        QPointer<ScriptEngine> engine(_engine);
        request->setLogger([engine](const QString& line) {
            if (engine) {
                engine->scriptWarningMessage(line);
            }
        });
    }
    return request;
}

void HttpRequestFactory::dispose(QObject* request) {
    if (request && request->parent() == this) {
        request->deleteLater();
    }
}
