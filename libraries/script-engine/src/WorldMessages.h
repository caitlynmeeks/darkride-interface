//
//  WorldMessages.h
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  The Messages object a world's entity scripts get on a visitor's client, in place of the shared MessagesClient.
//  It sends through the client's world path, so a message delivered on this computer arrives at every listener with
//  fromOwnScripts false; it refuses the app's own channels (a world's script may not send on them, nor hear them), so
//  a listener that acts for the visitor (a snapshot, a notification, a setting) can't be driven by a world; and it
//  relays every other message to the world's scripts as the shared object would.
//

#ifndef hifi_WorldMessages_h
#define hifi_WorldMessages_h

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QUuid>

class WorldMessages : public QObject {
    Q_OBJECT
public:
    explicit WorldMessages(QObject* parent);

    /// The app's own channels: io.darkride.*, io.highfidelity.*, com.tivolicloud.*, com.highfidelity.*, tivoli.*,
    /// Snap.*, and Hifi-Notifications, Darkride-Panels, entityToolUpdates, chat. High Fidelity's game channels
    /// (Hifi-Teleport-*, Hifi-Hand-*, Hifi-Object-Manipulation...) and Darkride-Hop stay open to worlds (Caity, 10-30).
    static bool isAppChannel(const QString& channel);

    Q_INVOKABLE void sendMessage(QString channel, QString message, bool localOnly = false);
    Q_INVOKABLE void sendLocalMessage(QString channel, QString message);
    Q_INVOKABLE void sendData(QString channel, QByteArray data, bool localOnly = false);
    Q_INVOKABLE void subscribe(QString channel);
    Q_INVOKABLE void unsubscribe(QString channel);

signals:
    void messageReceived(QString channel, QString message, QUuid senderUUID, bool localOnly, bool fromOwnScripts);
    void dataReceived(QString channel, QByteArray data, QUuid senderUUID, bool localOnly, bool fromOwnScripts);

private:
    bool refused(const QString& what, const QString& channel) const;
};

#endif // hifi_WorldMessages_h
