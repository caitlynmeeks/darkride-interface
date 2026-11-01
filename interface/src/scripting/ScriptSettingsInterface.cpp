//
//  ScriptSettingsInterface.cpp
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ScriptSettingsInterface.h"

#include <SettingKey.h>
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
    connect(SettingsScriptingInterface::getInstance(), &SettingsScriptingInterface::valueChanged, this,
            [this](const QString& setting, const QVariant& value) {
                // a setting a script may not read, it doesn't hear change either
                if (!refuses(setting) && !Setting::neverToScripts(setting)) {
                    emit valueChanged(setting, value);
                }
            });
}

bool ScriptSettingsInterface::isProtected(const QString& written) {
    // the key as it is stored: QSettings drops the slashes at either end, joins doubled ones, and Windows reads '\\'
    // as '/'
    const QString setting = Setting::normalizedKey(written);
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

QString ScriptSettingsInterface::ruledKey(const QString& setting) const {
    // a script held to the rule reads and writes the key it was judged by, as it is stored; any other script's key
    // goes on as it was written
    if (!_engine) {
        return setting;
    }
    const bool worldScript = _engine->isEntityClientScript() || _engine->isEntityServerScript();
    if (!worldScript && (!_engine->isClientSide() ||
                         !ScriptNetworkPolicy::applies(_engine->currentScriptOrigin(), _engine->worldIsLocal()))) {
        return setting;
    }
    return Setting::normalizedKey(setting);
}

bool ScriptSettingsInterface::refuses(const QString& setting) const {
    if (!_engine || !isProtected(setting)) {
        return false;
    }
    // a world's script never gets a protected setting, whatever its world (one served from this machine or its
    // network included); the network rule's exemptions are for what a script may reach, not for these
    const bool worldScript = _engine->isEntityClientScript() || _engine->isEntityServerScript();
    return Setting::refusedProtected(worldScript, !worldScript && _engine->isClientSide() &&
                                     ScriptNetworkPolicy::applies(_engine->currentScriptOrigin(), _engine->worldIsLocal()));
}

bool ScriptSettingsInterface::refusesRead(const QString& setting) {
    if (!refuses(setting)) {
        return false;
    }
    if (!_refusedReads.contains(setting)) {
        _refusedReads.insert(setting);
        const QUrl origin = _engine->currentScriptOrigin();
        _engine->scriptWarningMessage(QString("%1 may not read the setting \"%2\": a script from the network does not read "
                                              "what the client runs, where it has been, whose avatar it wears or where its "
                                              "files are; a script loaded from this machine, or a world served from it or its "
                                              "network, keeps every setting.")
                                          .arg(origin.isEmpty() ? QString("a script") : origin.toDisplayString().left(200), setting));
    }
    return true;
}

bool ScriptSettingsInterface::refusesAlways(const QString& setting) {
    if (!Setting::neverToScripts(setting)) {
        return false;
    }
    if (_engine && !_refusedReads.contains(setting)) {
        _refusedReads.insert(setting);
        _engine->scriptWarningMessage(QString("No script may read or change the setting \"%1\".").arg(setting.left(200)));
    }
    return true;
}

QVariant ScriptSettingsInterface::getValue(const QString& written) {
    if (refusesAlways(written)) {
        return QVariant();
    }
    const QString setting = ruledKey(written);
    if (refusesRead(setting)) {
        return QVariant();
    }
    return SettingsScriptingInterface::getInstance()->getValue(setting);
}

QVariant ScriptSettingsInterface::getValue(const QString& written, const QVariant& defaultValue) {
    if (refusesAlways(written)) {
        return defaultValue;
    }
    const QString setting = ruledKey(written);
    if (refusesRead(setting)) {
        return defaultValue;
    }
    return SettingsScriptingInterface::getInstance()->getValue(setting, defaultValue);
}

void ScriptSettingsInterface::setValue(const QString& written, const QVariant& value) {
    if (refusesAlways(written)) {
        return;
    }
    const QString setting = ruledKey(written);
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
