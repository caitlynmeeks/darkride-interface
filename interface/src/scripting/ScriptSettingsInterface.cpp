//
//  ScriptSettingsInterface.cpp
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ScriptSettingsInterface.h"

#include <QtCore/QStringList>

#include <ScriptEngine.h>
#include <ScriptNetworkPolicy.h>

#include "SettingsScriptingInterface.h"

namespace {

// The settings a script from the network may not write: the scripts the client runs at start (a world's script
// would install itself for good), where the client goes, whose avatar it wears, what scripting and development
// allow, the client's own options, and where its files are.
const QStringList PROTECTED_EXACT { "RunningScripts", "Start-Up Location", "keepMeLoggedIn", "address" };
const QStringList PROTECTED_PREFIXES { "RunningScripts/", "AddressManager/", "Start-Up Location/", "Avatar/", "Scripting/",
                                       "Settings/", "Developer/", "Darkride Options/", "private/", "Networking/",
                                       "Account/", "AccountManager/" };
const QStringList PROTECTED_SUFFIXES { "Location" };

}  // namespace

ScriptSettingsInterface::ScriptSettingsInterface(ScriptEngine* engine) : QObject(engine), _engine(engine) {
    connect(SettingsScriptingInterface::getInstance(), &SettingsScriptingInterface::valueChanged,
            this, &ScriptSettingsInterface::valueChanged);
}

bool ScriptSettingsInterface::isProtected(const QString& setting) {
    for (const QString& exact : PROTECTED_EXACT) {
        if (setting.compare(exact, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    for (const QString& prefix : PROTECTED_PREFIXES) {
        if (setting.startsWith(prefix, Qt::CaseInsensitive)) {
            return true;
        }
    }
    for (const QString& suffix : PROTECTED_SUFFIXES) {
        if (setting.endsWith(suffix, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

bool ScriptSettingsInterface::refuses(const QString& setting) const {
    if (!_engine || !_engine->isClientSide() || !isProtected(setting)) {
        return false;
    }
    return ScriptNetworkPolicy::applies(_engine->currentScriptOrigin(), _engine->worldIsLocal());
}

QVariant ScriptSettingsInterface::getValue(const QString& setting) {
    return SettingsScriptingInterface::getInstance()->getValue(setting);
}

QVariant ScriptSettingsInterface::getValue(const QString& setting, const QVariant& defaultValue) {
    return SettingsScriptingInterface::getInstance()->getValue(setting, defaultValue);
}

void ScriptSettingsInterface::setValue(const QString& setting, const QVariant& value) {
    if (refuses(setting)) {
        const QUrl origin = _engine->currentScriptOrigin();
        _engine->scriptWarningMessage(QString("%1 may not change the setting \"%2\": a script from the network does not change "
                                              "what the client runs, where it goes, whose avatar it wears or where its files "
                                              "are; a script loaded from this machine, or a world served from it or its "
                                              "network, keeps every setting.")
                                          .arg(origin.isEmpty() ? QString("a script") : origin.toDisplayString().left(200), setting));
        return;
    }
    SettingsScriptingInterface::getInstance()->setValue(setting, value);
}
