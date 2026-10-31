//
//  PanelTheme.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "PanelTheme.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtQml/QQmlEngine>
#include <QtQml/qqml.h>

#include "InterfaceLogging.h"

const QStringList PanelTheme::COLOR_KEYS {
    "background", "card", "cardPressed", "separator", "text", "secondaryText", "tertiaryText", "tint", "on",
    "destructive", "fill", "segmentThumb", "sliderTrack"
};

static const QStringList BUILT_IN_SCHEMES { "graphite", "midnight", "dusk", "daylight" };
static const QString DEFAULT_SCHEME { "graphite" };
static const qint64 MAX_THEME_FILE_BYTES = 64 * 1024;
static const int MAX_NAME_LENGTH = 40;

PanelTheme::PanelTheme() {
    _custom = _customSetting.get().toMap();
    _scheme = _schemeSetting.get();
    if (!BUILT_IN_SCHEMES.contains(_scheme) && !(_scheme == "custom" && !_custom.isEmpty())) {
        _scheme = DEFAULT_SCHEME;
    }
}

void PanelTheme::registerWithQml() {
    // every engine gets the one PanelTheme: the desktop's panels and the tablet in a headset each have an engine of their
    // own, and qmlRegisterSingletonInstance serves one engine only, handing the second a null (a crash in VR, 10-31)
    qmlRegisterSingletonType<PanelTheme>("DarkridePanels", 1, 0, "PanelTheme", [](QQmlEngine*, QJSEngine*) -> QObject* {
        auto instance = DependencyManager::get<PanelTheme>().data();
        QQmlEngine::setObjectOwnership(instance, QQmlEngine::CppOwnership);   // the engines never delete it
        return instance;
    });
}

void PanelTheme::setSkin(const QVariantMap& skin) {
    if (skin != _skin) {
        _skin = skin;
        emit changed();
    }
}

void PanelTheme::setScheme(const QString& scheme) {
    if (!BUILT_IN_SCHEMES.contains(scheme) && !(scheme == "custom" && !_custom.isEmpty())) {
        return;
    }
    if (scheme != _scheme) {
        _scheme = scheme;
        _schemeSetting.set(_scheme);
        emit changed();
    }
}

QString PanelTheme::loadFile(const QString& pathOrUrl) {
    QUrl url(pathOrUrl);
    QString path = url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
    QFileInfo info(path);
    if (!info.isFile()) {
        return "There is no file there.";
    }
    if (info.size() > MAX_THEME_FILE_BYTES) {
        return "It's too big for a theme file.";
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return "It couldn't be opened.";
    }
    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return "It isn't a theme file (a JSON object of colours).";
    }

    // only names and colours are taken; anything else in the file is left out
    static const QRegularExpression COLOR_PATTERN("^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$");
    QJsonObject object = document.object();
    QVariantMap theme;
    int colors = 0;
    for (const QString& key : COLOR_KEYS) {
        QString value = object.value(key).toString();
        if (COLOR_PATTERN.match(value).hasMatch()) {
            theme[key] = value;
            colors++;
        }
    }
    if (colors == 0) {
        return "It has no colours this version knows.";
    }
    QString name = object.value("name").toString().trimmed().left(MAX_NAME_LENGTH);
    theme["name"] = name.isEmpty() ? info.completeBaseName().left(MAX_NAME_LENGTH) : name;
    QString base = object.value("base").toString();
    theme["base"] = BUILT_IN_SCHEMES.contains(base) ? base : DEFAULT_SCHEME;

    _custom = theme;
    _scheme = "custom";
    _customSetting.set(_custom);
    _schemeSetting.set(_scheme);
    qCDebug(interfaceapp) << "Panel theme loaded:" << theme["name"].toString() << colors << "colours from" << path;
    emit changed();
    return QString();
}
