//
//  ScriptSettingsInterface.h
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  The Settings object a script engine gets: the client's settings through one object per engine, which knows where
//  the engine's scripts came from. A script from the network (a world's script, by the network rule for a script's
//  own requests, ScriptNetworkPolicy) may not write the settings that decide what the client runs at start, where it
//  goes, whose avatar it wears, what it is allowed to do, or where its files are, nor read them or hear them change
//  (where the visitor has been, their account's settings); a local script, or any script of a local world, reads and
//  writes as before. A settings space per world is the next step.
//

#ifndef hifi_ScriptSettingsInterface_h
#define hifi_ScriptSettingsInterface_h

#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QVariant>

class ScriptEngine;

class ScriptSettingsInterface : public QObject {
    Q_OBJECT
public:
    explicit ScriptSettingsInterface(ScriptEngine* engine);

    /// Whether a script from the network may not read or write this setting.
    static bool isProtected(const QString& setting);

public slots:
    QVariant getValue(const QString& setting);
    QVariant getValue(const QString& setting, const QVariant& defaultValue);
    void setValue(const QString& setting, const QVariant& value);

signals:
    void valueChanged(const QString& setting, const QVariant& value);

private:
    QString ruledKey(const QString& setting) const;
    bool refusesAlways(const QString& setting);
    bool refuses(const QString& setting) const;
    /// refuses() for a read, with the refusal logged once per setting.
    bool refusesRead(const QString& setting);

    ScriptEngine* _engine { nullptr };
    QSet<QString> _refusedReads;
};

#endif // hifi_ScriptSettingsInterface_h
