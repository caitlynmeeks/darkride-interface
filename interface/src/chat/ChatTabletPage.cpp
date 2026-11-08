//
//  ChatTabletPage.cpp
//  interface/src/chat
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ChatTabletPage.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkReply>

#include <AccountManager.h>
#include <AddressManager.h>
#include <ChatService.h>
#include <DependencyManager.h>
#include <NetworkAccessManager.h>
#include <NetworkingConstants.h>
#include <ui/TabletScriptingInterface.h>

#include "InterfaceLogging.h"
#include "WorldChat.h"

namespace {
    // the web chat, when the metaverse hasn't named a chat server yet (not signed in): the page then says to sign in
    const QString DEFAULT_CHAT_URL = "https://chat.darkride.ai/";
    const QString CHAT_TOKEN_PATH = "/api/v1/user/chat-token";
    const QString MESSAGE_TYPE = "darkride-chat";

    QPointer<ChatTabletPage> thePage;
}

ChatTabletPage* ChatTabletPage::make(TabletProxy* tablet) {
    if (!thePage && tablet) {
        thePage = new ChatTabletPage(tablet);
    }
    return thePage;
}

ChatTabletPage* ChatTabletPage::get() {
    return thePage;
}

ChatTabletPage::ChatTabletPage(TabletProxy* tablet) : QObject(tablet), _tablet(tablet) {
    connect(tablet, &TabletProxy::screenChanged, this, &ChatTabletPage::screenChanged);
    connect(tablet, &TabletProxy::webEventReceived, this, &ChatTabletPage::webEventReceived);
    // Local Chat's lines to the page as they change, while it's there, and the line beside its title
    connect(WorldChat::instance(), &WorldChat::itemsChanged, this, [this] {
        if (!_bridge.isEmpty()) {
            sendWorld();
        }
    });
    connect(WorldChat::instance(), &WorldChat::peopleChanged, this, [this] {
        if (!_bridge.isEmpty()) {
            sendWorldHere();
        }
    });
}

void ChatTabletPage::toggle() {
    if (_showing && _tablet) {
        _tablet->gotoHomeScreen();
    } else {
        open();
    }
}

void ChatTabletPage::openDirectMessage(const QString& username) {
    open("/dm/" + username);
}

void ChatTabletPage::open(const QString& link) {
    if (!_tablet) {
        return;
    }
    QString server = DependencyManager::get<ChatService>()->homeserver();
    QUrl url(server.isEmpty() ? DEFAULT_CHAT_URL : server);
    url.setPath("/");
    // a new code each time the page opens, from the system's random source: 128 bits
    QByteArray code(16, 0);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(code.data()), code.size() / sizeof(quint32));
    _bridge = QString::fromLatin1(code.toHex());
    QUrlQuery query;
    query.addQueryItem("metaverseUrl", NetworkingConstants::METAVERSE_SERVER_URL().toString());
    query.addQueryItem("bridge", _bridge);
    url.setQuery(query);
    _pageUrl = url.toString();
    if (!link.isEmpty()) {
        url.setFragment(link);   // the page opens it once it has synced (the web chat's deep links)
    }
    qCDebug(interfaceapp) << "chat on the tablet:" << url.toString(QUrl::RemoveQuery);
    _tablet->gotoWebScreen(url.toString());
}

void ChatTabletPage::screenChanged(const QVariant& type, const QVariant& url) {
    QString screen = type.toString();
    bool showing = screen == "Web" && !_pageUrl.isEmpty() && url.toString().startsWith(_pageUrl);
    if (!showing && screen != "Closed" && screen != "Unknown") {
        // the page is gone from the tablet: its code no longer answers (closing the tablet keeps the page)
        _bridge.clear();
        _pageUrl.clear();
    }
    if (showing != _showing) {
        _showing = showing;
        emit showingChanged(showing);
    }
}

void ChatTabletPage::webEventReceived(const QVariant& message) {
    QJsonObject event = message.type() == QVariant::String
        ? QJsonDocument::fromJson(message.toString().toUtf8()).object()
        : QJsonObject::fromVariantMap(message.toMap());
    if (event.value("type").toString() != MESSAGE_TYPE) {
        return;   // another page's
    }
    QJsonObject args = event.value("args").toObject();
    QString command = event.value("command").toString();
    if (_bridge.isEmpty() || args.value("bridge").toString() != _bridge) {
        qCWarning(interfaceapp) << "chat on the tablet: a" << command << "without the page's code, ignored";
        return;
    }
    if (command == "signIn") {
        signIn();
    } else if (command == "world") {
        sendWorld();
        sendWorldHere();
    } else if (command == "worldSend") {
        WorldChat::instance()->send(args.value("text").toString());
    } else if (command == "worldRead") {
        WorldChat::instance()->markRead();
    } else if (command == "goToAddress") {
        // a friend's Join (hifi://@username), or a place link
        static const QRegularExpression USER("^hifi://@([^/?#]+)$", QRegularExpression::CaseInsensitiveOption);
        QString address = args.value("address").toString();
        auto user = USER.match(address);
        if (user.hasMatch()) {
            // the metaverse checks again that they let you
            DependencyManager::get<AddressManager>()->goToUser(user.captured(1));
        } else if (address.startsWith("hifi://", Qt::CaseInsensitive)) {
            DependencyManager::get<AddressManager>()->handleLookupString(address);
        }
    }
    // "notify" (a direct message or a mention): nothing here, the Chat button's count shows what you missed
}

void ChatTabletPage::signIn() {
    auto accountManager = DependencyManager::get<AccountManager>();
    if (!accountManager->isLoggedIn()) {
        answer({ { "event", "signIn" }, { "status", 401 }, { "error", "Sign in to Darkride first." } });
        return;
    }
    QNetworkRequest request = accountManager->createRequest(CHAT_TOKEN_PATH, AccountManagerAuth::Required);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* reply = NetworkAccessManager::getInstance().post(request, QByteArray());
    QString bridge = _bridge;
    connect(reply, &QNetworkReply::finished, this, [this, reply, bridge] {
        reply->deleteLater();
        if (bridge != _bridge) {
            return;   // the page went meanwhile: the token goes to no other
        }
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(interfaceapp) << "chat on the tablet: no chat token:" << status << reply->errorString();
            answer({ { "event", "signIn" }, { "status", status }, { "error", body.value("message").toString() } });
            return;
        }
        QJsonObject data = body.value("data").toObject();
        answer({ { "event", "signIn" },
                 { "token", data.value("token") },
                 { "homeserver", data.value("homeserver") },
                 { "user_id", data.value("user_id") },
                 { "display_name", data.value("display_name") } });
    });
}

void ChatTabletPage::sendWorld() {
    answer({ { "event", "world" }, { "items", WorldChat::instance()->items() } });
}

void ChatTabletPage::sendWorldHere() {
    answer({ { "event", "worldHere" }, { "text", WorldChat::instance()->hereLine() } });
}

void ChatTabletPage::answer(QJsonObject message) {
    if (_tablet) {
        message.insert("type", MESSAGE_TYPE);
        _tablet->emitScriptEvent(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
    }
}
