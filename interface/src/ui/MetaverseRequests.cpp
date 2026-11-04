//
//  MetaverseRequests.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MetaverseRequests.h"

#include <QtCore/QJsonDocument>
#include <QtQml/QJSValue>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <AccountManager.h>
#include <NetworkAccessManager.h>
#include <NetworkingConstants.h>

#include "InterfaceLogging.h"
#include "RightsLine.h"

void MetaverseRequests::send(const QString& method, const QString& path, const QVariant& body, QJSValue callback) {
    // the engine that owns this object (the panels' QML engine), which made the callback: QJSValue::engine() is gone in Qt 6
    QJSEngine* engine = qjsEngine(this);
    auto answer = [callback, engine](const QJSValue& error, int status, const QVariant& data) mutable {
        if (callback.isCallable()) {
            callback.call({ error, QJSValue(status), engine ? engine->toScriptValue(data) : QJSValue() });
        }
    };
    if (!path.startsWith("/api/") || path.contains("..")) {
        answer(QJSValue("not a metaverse API path: " + path), 0, QVariant());
        return;
    }
    if (method != "GET" && method != "POST" && method != "PUT" && method != "DELETE") {
        answer(QJSValue("unsupported method: " + method), 0, QVariant());
        return;
    }

    QUrl url = NetworkingConstants::METAVERSE_SERVER_URL();
    url.setPath(path.section('?', 0, 0));
    if (path.contains('?')) {
        url.setQuery(path.section('?', 1));
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, TIVOLI_CLOUD_VR_USER_AGENT);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto accountManager = DependencyManager::get<AccountManager>();
    if (accountManager->hasValidAccessToken()) {
        request.setRawHeader(ACCESS_TOKEN_AUTHORIZATION_HEADER,
                             accountManager->getAccountInfo().getAccessToken().authorizationHeaderValue());
    }

    auto& networkAccessManager = NetworkAccessManager::getInstance();
    // a panel's JS object arrives as a QJSValue inside the QVariant, of which QJsonDocument::fromVariant makes nothing:
    // every request with a body went out empty (Caity's friends test 10-28: "username should not be empty")
    const QVariant plainBody = body.userType() == qMetaTypeId<QJSValue>() ? body.value<QJSValue>().toVariant() : body;
    const QByteArray json = QJsonDocument::fromVariant(plainBody).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = method == "POST" ? networkAccessManager.post(request, json)
        : method == "PUT" ? networkAccessManager.put(request, json)
        : method == "DELETE" ? networkAccessManager.deleteResource(request)
        : networkAccessManager.get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, answer]() mutable {
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QVariant data = QJsonDocument::fromJson(reply->readAll()).toVariant();
        if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300) {
            answer(QJSValue(QJSValue::NullValue), status, data);
        } else {
            qCDebug(interfaceapp) << "Metaverse request failed:" << reply->url().path() << status << reply->errorString();
            answer(QJSValue(status > 0 ? QString("the metaverse answered %1").arg(status) : reply->errorString()), status, data);
        }
        reply->deleteLater();
    });
}

QString MetaverseRequests::metaverseUrl() const {
    return NetworkingConstants::METAVERSE_SERVER_URL().toString();
}

QString MetaverseRequests::rightsLine() const {
    return rights::sentence();
}

QString MetaverseRequests::termsUrl() const {
    return rights::termsUrl().toString();
}
