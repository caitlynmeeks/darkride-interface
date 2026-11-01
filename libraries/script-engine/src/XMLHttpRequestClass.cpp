//
//  XMLHttpRequestClass.cpp
//  libraries/script-engine/src/
//
//  Created by Ryan Huffman on 5/2/14.
//  Copyright (c) 2014 High Fidelity, Inc. All rights reserved.
//
//  This class is an implementation of the XMLHttpRequest object for scripting use.  It provides a near-complete implementation
//  of the class described in the Mozilla docs: https://developer.mozilla.org/en-US/docs/Web/API/XMLHttpRequest
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "XMLHttpRequestClass.h"

#include <ScriptNetworkPolicy.h>

#include <QEventLoop>
#include <qurlquery.h>

#include <AccountManager.h>
#include <NetworkAccessManager.h>
#include <NetworkingConstants.h>

#include "ResourceRequestObserver.h"
#include "ScriptEngine.h"

const QString METAVERSE_API_URL = NetworkingConstants::METAVERSE_SERVER_URL().toString() + "/api/";

Q_DECLARE_METATYPE(QByteArray*)

XMLHttpRequestClass::XMLHttpRequestClass(QScriptEngine* engine) :
    _engine(engine),
    _timer(this) {

    _request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    _timer.setSingleShot(true);
    if (auto scriptEngine = qobject_cast<ScriptEngine*>(engine)) {
        _origin = scriptEngine->ruledOrigin(scriptEngine->currentScriptOrigin());
        _ruled = scriptEngine->networkRuled();
        _worldIsLocal = scriptEngine->worldIsLocal();
    }
}

XMLHttpRequestClass::~XMLHttpRequestClass() {
    if (_reply) { _reply->deleteLater(); }
}

QScriptValue XMLHttpRequestClass::constructor(QScriptContext* context, QScriptEngine* engine) {
    return engine->newQObject(new XMLHttpRequestClass(engine), QScriptEngine::ScriptOwnership);
}

QScriptValue XMLHttpRequestClass::getStatus() const {
    if (_reply) {
        return QScriptValue(_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
    } 
    return QScriptValue(0);
}

QString XMLHttpRequestClass::getStatusText() const {
    if (_reply) {
        return _reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    }
    return "";
}

void XMLHttpRequestClass::abort() {
    ++_generation;
    _deciding = false;
    abortRequest();
}

void XMLHttpRequestClass::setRequestHeader(const QString& name, const QString& value) {
    _request.setHeader(QNetworkRequest::UserAgentHeader, TIVOLI_CLOUD_VR_USER_AGENT);
    _request.setRawHeader(QByteArray(name.toLatin1()), QByteArray(value.toLatin1()));
}

void XMLHttpRequestClass::requestMetaDataChanged() {
    if (isRedirectHop()) {
        return;     // a redirect's headers and body are not the response
    }
    QVariant redirect = _reply->attribute(QNetworkRequest::RedirectionTargetAttribute);

    // If this is a redirect, abort the current request and start a new one
    if (redirect.isValid() && _numRedirects < MAXIMUM_REDIRECTS) {
        _numRedirects++;
        abortRequest();

        QUrl newUrl = _url.resolved(redirect.toUrl().toString());
        _request.setUrl(newUrl);
        doSend();
    }
}

void XMLHttpRequestClass::requestDownloadProgress(qint64 bytesReceived, qint64 bytesTotal) {
    if (isRedirectHop()) {
        return;     // a redirect's headers and body are not the response
    }
    if (_readyState == OPENED && bytesReceived > 0) {
        setReadyState(HEADERS_RECEIVED);
        setReadyState(LOADING);
    }
}

QScriptValue XMLHttpRequestClass::getAllResponseHeaders() const {
    if (_reply) {
        QList<QNetworkReply::RawHeaderPair> headerList = _reply->rawHeaderPairs();
        QByteArray headers;
        for (int i = 0; i < headerList.size(); i++) {
            headers.append(headerList[i].first);
            headers.append(": ");
            headers.append(headerList[i].second);
            headers.append("\n");
        }
        return QString(headers.data());
    }
    return QScriptValue("");
}

QScriptValue XMLHttpRequestClass::getResponseHeader(const QString& name) const {
    if (_reply && _reply->hasRawHeader(name.toLatin1())) {
        return QScriptValue(QString(_reply->rawHeader(name.toLatin1())));
    }
    return QScriptValue::NullValue;
}

/**jsdoc
 * Called when the request's ready state changes.
 * @callback XMLHttpRequest~onReadyStateChangeCallback
 */
void XMLHttpRequestClass::setReadyState(ReadyState readyState) {
    if (readyState != _readyState) {
        _readyState = readyState;
        if (_onReadyStateChange.isFunction()) {
            _onReadyStateChange.call(QScriptValue::NullValue);
        }
    }
}

void XMLHttpRequestClass::open(const QString& method, const QString& url, bool async, const QString& username,
                               const QString& password) {
    if (_readyState == UNSENT) {
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
}

void XMLHttpRequestClass::send() {
    send(QScriptValue::NullValue);
}

void XMLHttpRequestClass::send(const QScriptValue& data) {
    if (_readyState != OPENED || _reply || _deciding) {
        return;
    }
    if (!data.isNull()) {
        if (data.isObject()) {
            _sendData = qscriptvalue_cast<QByteArray>(data);
        } else {
            _sendData = data.toString().toUtf8();
        }
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
        QEventLoop loop;
        connect(this, SIGNAL(requestComplete()), &loop, SLOT(quit()));
        loop.exec();
    }
}

void XMLHttpRequestClass::refuse(const QString& reason) {
    if (auto scriptEngine = qobject_cast<ScriptEngine*>(_engine)) {
        scriptEngine->scriptWarningMessage(ScriptNetworkPolicy::refusalLine(_origin, _url, reason));
    }
    _errorCode = QNetworkReply::ContentAccessDenied;
    setReadyState(DONE);
    emit requestComplete();
}

void XMLHttpRequestClass::doSend() {
    DependencyManager::get<ResourceRequestObserver>()->update(_url, -1, "XMLHttpRequestClass::doSend");
    _byHand = _ruled && ScriptNetworkPolicy::applies(_origin, _worldIsLocal);
    QNetworkRequest request = _request;
    request.setUrl(_hopUrl);
    if (_byHand) {
        // to the address the rule checked and no other (no second lookup of the name); a redirect comes back here,
        // judged and pinned as the request was
        request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, false);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        ScriptNetworkPolicy::pin(request, _decision);
    }
    _reply = NetworkAccessManager::getInstance().sendCustomRequest(request, _method.toLatin1(), _sendData);
    connectToReply(_reply);

    if (_timeout > 0) {
        _timer.start(_timeout);
        connect(&_timer, SIGNAL(timeout()), this, SLOT(requestTimeout()));
    }
}

/**jsdoc
 * Called when the request times out.
 * @callback XMLHttpRequest~onTimeoutCallback 
 */
void XMLHttpRequestClass::requestTimeout() {
    if (_onTimeout.isFunction()) {
        _onTimeout.call(QScriptValue::NullValue);
    }
    abortRequest();
    _errorCode = QNetworkReply::TimeoutError;
    setReadyState(DONE);
    emit requestComplete();
}

void XMLHttpRequestClass::requestError(QNetworkReply::NetworkError code) {
}

bool XMLHttpRequestClass::isRedirectHop() const {
    if (!_byHand || !_reply) {
        return false;
    }
    const int status = _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && _reply->hasRawHeader("Location");
}

void XMLHttpRequestClass::followRedirect(int status) {
    // relative to the hop's address by its name, not the address it was pinned to
    const QUrl next = _hopUrl.resolved(QUrl::fromEncoded(_reply->rawHeader("Location")));
    QNetworkReply* reply = _reply;
    disconnectFromReply(reply);
    _reply = nullptr;
    reply->deleteLater();
    QString reason;
    if (++_redirects > _request.maximumRedirectsAllowed()) {
        reason = "too many redirects";
    } else if (_hopUrl.scheme().toLower() == "https" && next.scheme().toLower() != "https") {
        reason = "a redirect from https to a plain address";   // a secure request stays secure
    }
    if (!reason.isEmpty()) {
        refuse(reason + " (" + next.toDisplayString() + ")");
        return;
    }
    if (status == 303 || ((status == 301 || status == 302) && _method.toUpper() == "POST")) {
        _method = "GET";        // as browsers do
        _sendData.clear();
    }
    _hopUrl = next;
    _deciding = true;
    const int generation = ++_generation;
    ScriptNetworkPolicy::decide(next, _origin, this, [this, generation](const ScriptNetworkPolicy::Decision& decision) {
        if (generation != _generation) {
            return;
        }
        _deciding = false;
        if (!decision.allowed) {
            refuse(decision.reason + " (a redirect to " + _hopUrl.toDisplayString() + ")");
            return;
        }
        _decision = decision;
        doSend();
    }, _worldIsLocal);
}

void XMLHttpRequestClass::requestFinished() {
    if (isRedirectHop()) {
        followRedirect(_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        return;
    }
    disconnect(&_timer, SIGNAL(timeout()), this, SLOT(requestTimeout()));

    _errorCode = _reply->error();

    if (_errorCode == QNetworkReply::NoError) {
        _rawResponseData.append(_reply->readAll());

        if (_responseType == "json") {
            // the reply is data: JSON.parse, never evaluate (a server's answer must not get to run as code in the
            // requesting script); a reply that isn't JSON gives null, as before
            QScriptValue parse = _engine->globalObject().property("JSON").property("parse");
            _responseData = parse.call(QScriptValue(), QScriptValueList() << QString::fromUtf8(_rawResponseData));
            if (_engine->hasUncaughtException() || _responseData.isError()) {
                _engine->clearExceptions();
                _responseData = QScriptValue::NullValue;
            }
        } else if (_responseType == "arraybuffer") {
            QScriptValue data = _engine->newVariant(QVariant::fromValue(_rawResponseData));
            _responseData = _engine->newObject(reinterpret_cast<ScriptEngine*>(_engine)->getArrayBufferClass(), data);
        } else {
            _responseData = QScriptValue(QString(_rawResponseData.data()));
        }
    }

    setReadyState(DONE);
    emit requestComplete();

    disconnectFromReply(_reply);
    _reply->deleteLater();
    _reply = nullptr;
}

void XMLHttpRequestClass::abortRequest() {
    // Disconnect from signals we don't want to receive any longer.
    disconnect(&_timer, SIGNAL(timeout()), this, SLOT(requestTimeout()));
    if (_reply) {
        disconnectFromReply(_reply);
        _reply->abort();
        _reply->deleteLater();
        _reply = nullptr;
    }
}

void XMLHttpRequestClass::connectToReply(QNetworkReply* reply) {
    connect(reply, SIGNAL(finished()), this, SLOT(requestFinished()));
    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(requestError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(downloadProgress(qint64, qint64)), this, SLOT(requestDownloadProgress(qint64, qint64)));
    connect(reply, SIGNAL(metaDataChanged()), this, SLOT(requestMetaDataChanged()));
}

void XMLHttpRequestClass::disconnectFromReply(QNetworkReply* reply) {
    disconnect(reply, SIGNAL(finished()), this, SLOT(requestFinished()));
    disconnect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(requestError(QNetworkReply::NetworkError)));
    disconnect(reply, SIGNAL(downloadProgress(qint64, qint64)), this, SLOT(requestDownloadProgress(qint64, qint64)));
    disconnect(reply, SIGNAL(metaDataChanged()), this, SLOT(requestMetaDataChanged()));
}
