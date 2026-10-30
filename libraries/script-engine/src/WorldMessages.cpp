//
//  WorldMessages.cpp
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "WorldMessages.h"

#include <QtCore/QStringList>

#include <DependencyManager.h>
#include <MessagesClient.h>

#include "ScriptEngineLogging.h"

namespace {

const QStringList APP_CHANNEL_PREFIXES { "io.darkride.", "io.highfidelity.", "com.tivolicloud.", "com.highfidelity.",
                                         "tivoli.", "snap." };
const QStringList APP_CHANNELS { "hifi-notifications", "darkride-panels", "entitytoolupdates", "chat" };

} // namespace

WorldMessages::WorldMessages(QObject* parent) : QObject(parent) {
    auto messages = DependencyManager::get<MessagesClient>();
    if (!messages) {
        return;
    }
    // relayed on this object's thread (the engine's); the app's channels never reach a world's scripts
    connect(messages.data(), &MessagesClient::messageReceived, this,
            [this](QString channel, QString message, QUuid sender, bool localOnly, bool fromOwnScripts) {
                if (!isAppChannel(channel)) {
                    emit messageReceived(channel, message, sender, localOnly, fromOwnScripts);
                }
            });
    connect(messages.data(), &MessagesClient::dataReceived, this,
            [this](QString channel, QByteArray data, QUuid sender, bool localOnly, bool fromOwnScripts) {
                if (!isAppChannel(channel)) {
                    emit dataReceived(channel, data, sender, localOnly, fromOwnScripts);
                }
            });
}

bool WorldMessages::isAppChannel(const QString& channel) {
    const QString lower = channel.trimmed().toLower();
    if (APP_CHANNELS.contains(lower)) {
        return true;
    }
    for (const QString& prefix : APP_CHANNEL_PREFIXES) {
        if (lower.startsWith(prefix)) {
            return true;
        }
    }
    return false;
}

bool WorldMessages::refused(const QString& what, const QString& channel) const {
    if (!isAppChannel(channel)) {
        return false;
    }
    qCWarning(scriptengine) << QString("Messages.%1 on \"%2\" is for the visitor's own scripts; a world's script may not use "
                                       "the app's channels").arg(what, channel.left(200));
    return true;
}

void WorldMessages::sendMessage(QString channel, QString message, bool localOnly) {
    if (!refused("sendMessage", channel)) {
        DependencyManager::get<MessagesClient>()->sendMessageFromWorld(channel, message, localOnly);
    }
}

void WorldMessages::sendLocalMessage(QString channel, QString message) {
    if (!refused("sendLocalMessage", channel)) {
        DependencyManager::get<MessagesClient>()->sendMessageFromWorld(channel, message, true);
    }
}

void WorldMessages::sendData(QString channel, QByteArray data, bool localOnly) {
    if (!refused("sendData", channel)) {
        DependencyManager::get<MessagesClient>()->sendDataFromWorld(channel, data, localOnly);
    }
}

void WorldMessages::subscribe(QString channel) {
    if (!refused("subscribe", channel)) {
        DependencyManager::get<MessagesClient>()->subscribe(channel);
    }
}

void WorldMessages::unsubscribe(QString channel) {
    if (!isAppChannel(channel)) {
        DependencyManager::get<MessagesClient>()->unsubscribe(channel);
    }
}
