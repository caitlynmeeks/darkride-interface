//
//  AccountName.h
//  libraries/networking/src
//
//  The names a world server's door takes from a connect request, and how it asks the metaverse about one: only an
//  account's name is ever put into an address, and every answer is filed under the name its request carried, never
//  under one read back from an address.
//
//  Created by servers on 2026-11-12.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_AccountName_h
#define hifi_AccountName_h

#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QString>
#include <QtCore/QUrl>

namespace account_name {

// The most characters a name may have (sign-up allows 16; older accounts get room)
const int MAX_LENGTH = 64;

// Whether a name can be an account's: the metaverse's own rule (letters, digits and underscores; dots inside, never
// first or last: its server/src/user/account-name.ts) and at most MAX_LENGTH characters.
inline bool isAccountName(const QString& name) {
    // anchored at the very start and end (\A, \z): a '$' would let a name end in a newline
    static const QRegularExpression ACCOUNT_NAME {
        QRegularExpression::anchoredPattern("[a-zA-Z0-9_](?:[a-zA-Z0-9_.]*[a-zA-Z0-9_])?")
    };
    return !name.isEmpty() && name.size() <= MAX_LENGTH && ACCOUNT_NAME.match(name).hasMatch();
}

// The metaverse's addresses for an account's key and its group memberships, the name percent-encoded into them
inline QString keyRequestPath(const QString& lowerName) {
    return QString("api/v1/users/%1/public_key").arg(QString::fromLatin1(QUrl::toPercentEncoding(lowerName)));
}
inline QString groupMembershipsPath(const QString& lowerName) {
    return QString("api/v1/groups/members/%1").arg(QString::fromLatin1(QUrl::toPercentEncoding(lowerName)));
}

// What a request about a name carries to its answer (JSONCallbackParameters::callbackData): the name
inline QJsonObject requestData(const QString& lowerName) {
    return QJsonObject { { "username", lowerName } };
}

// The name an answer is filed under: the one its request carried, lower-cased; empty (the answer is dropped) when
// what came back carries none, or one that isn't an account's
inline QString nameFromRequest(const QJsonObject& data) {
    const QString name = data.value("username").toString();
    return isAccountName(name) ? name.toLower() : QString();
}

}

#endif // hifi_AccountName_h
