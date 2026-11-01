//
//  SettingKey.h
//  libraries/shared/src
//
//  Created by scripting on 2026-11-01.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_SettingKey_h
#define hifi_SettingKey_h

#include <QtCore/QString>

namespace Setting {
    /// A settings key as QSettings stores it: '\\' read as '/' (as Windows' registry does), repeated '/' as one, and
    /// none at either end. A check of a key looks at this form, which is where the value goes.
    QString normalizedKey(const QString& key);

    /// A setting no script gets, whatever its origin or world, the visitor's own scripts included: no read, no write,
    /// no change signal. Matched on the stored form (normalizedKey), in any case.
    bool neverToScripts(const QString& key);

    /// Whether a script is refused a protected setting: a world's script (an entity script, on a visitor's client or
    /// on a server) always, whatever its world; any other script only while the network rule holds it.
    bool refusedProtected(bool worldScript, bool heldToNetworkRule);
}

#endif // hifi_SettingKey_h
