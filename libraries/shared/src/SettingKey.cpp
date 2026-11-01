//
//  SettingKey.cpp
//  libraries/shared/src
//
//  Created by scripting on 2026-11-01.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "SettingKey.h"

#include <QtCore/QStringList>

namespace {
// a tracking key no script needs
const QStringList NEVER_TO_SCRIPTS { "private/domainIdSecret" };
}

QString Setting::normalizedKey(const QString& key) {
    QString result;
    result.reserve(key.size());
    for (QChar c : key) {
        if (c == QLatin1Char('\\')) {
            c = QLatin1Char('/');
        }
        if (c == QLatin1Char('/') && (result.isEmpty() || result.endsWith(QLatin1Char('/')))) {
            continue;   // a leading one, or a second in a row
        }
        result.append(c);
    }
    if (result.endsWith(QLatin1Char('/'))) {
        result.chop(1);
    }
    return result;
}

bool Setting::neverToScripts(const QString& key) {
    const QString stored = normalizedKey(key);
    for (const QString& never : NEVER_TO_SCRIPTS) {
        if (stored.compare(never, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool Setting::refusedProtected(bool worldScript, bool heldToNetworkRule) {
    return worldScript || heldToNetworkRule;
}
