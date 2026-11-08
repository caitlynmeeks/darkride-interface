//
//  MetaverseRequests.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MetaverseRequests.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtQml/QJSValue>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <AccountManager.h>
#include <NetworkAccessManager.h>
#include <NetworkingConstants.h>

#include "InterfaceLogging.h"
#include "RightsLine.h"

MetaverseRequests::MetaverseRequests(QJSEngine* engine, QObject* parent) : QObject(parent), _engine(engine) {
}

void MetaverseRequests::send(const QString& method, const QString& path, const QVariant& body, QJSValue callback) {
    // the answer is made in this object's engine, the one its panels (and so the callback) run in. Not qjsEngine(this):
    // that is the first engine to see the object, and Qt 6 refuses a call whose argument was made in another engine
    QPointer<QJSEngine> engine = _engine;
    auto answer = [callback, engine](const QJSValue& error, int status, const QJsonValue& data) mutable {
        if (!callback.isCallable()) {
            return;
        }
        if (!engine) {
            qCWarning(interfaceapp) << "Metaverse request answered after its panels' QML engine was gone";
            return;
        }
        // from the JSON, not a QVariant: Qt 6 makes a QVariantList a list-like object for which Array.isArray is
        // false, and Explore showed no worlds (11-08); a QJsonArray becomes a real array on Qt 5 and Qt 6
        callback.call({ error, QJSValue(status), engine->toScriptValue(data) });
    };
    if (!path.startsWith("/api/") || path.contains("..")) {
        answer(QJSValue("not a metaverse API path: " + path), 0, QJsonValue());
        return;
    }
    if (method != "GET" && method != "POST" && method != "PUT" && method != "DELETE") {
        answer(QJSValue("unsupported method: " + method), 0, QJsonValue());
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
    reply->setParent(this);   // a surface that closes takes its panels' unanswered requests with it
    connect(reply, &QNetworkReply::finished, this, [reply, answer]() mutable {
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        const QJsonValue data = document.isArray() ? QJsonValue(document.array())
            : document.isObject() ? QJsonValue(document.object()) : QJsonValue();   // null when it isn't JSON
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
