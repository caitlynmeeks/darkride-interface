//
//  PanelTheme.h
//  interface/src/ui
//
//  The colour scheme of Darkride's QML panels (qml/darkride/panel/Theme.qml reads it): one of the built-in schemes,
//  or one loaded from a theme file. A theme file is data only, a JSON object of colours (and a name): nothing in it
//  runs, so a theme from anyone is safe to load (Caity, 10-23: themes now, skins and a store category later). One
//  object for every QML engine, so the desktop's panels and the tablet's change together; the choice is saved.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_PanelTheme_h
#define hifi_PanelTheme_h

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>

#include <DependencyManager.h>
#include <SettingHandle.h>

class PanelTheme : public QObject, public Dependency {
    Q_OBJECT
    SINGLETON_DEPENDENCY

    /// the scheme in use: a built-in one's name, or "custom" for a loaded file
    Q_PROPERTY(QString scheme READ getScheme NOTIFY changed)
    /// a loaded file's colours (key → "#rrggbb" or "#aarrggbb"), its "name" and its "base" scheme; empty when none
    Q_PROPERTY(QVariantMap custom READ getCustom NOTIFY changed)
    /// the skin on (ui/Skins.h): { name, colors, frames: { part: { source, hover, pressed, checked, disabled, slice,
    /// padding } }, fontFamily, decorations: [{ source, at, offset, size, stretch }] }; empty when none. Its colours go
    /// over the scheme's; its frames, the panel controls draw; its decorations on a panel, the pages draw.
    Q_PROPERTY(QVariantMap skin READ getSkin NOTIFY changed)

public:
    /// the keys a theme file may set, the colours Theme.qml has
    static const QStringList COLOR_KEYS;

    /// registers the object as the QML singleton DarkridePanels.PanelTheme, for every engine
    static void registerWithQml();

    QString getScheme() const { return _scheme; }
    QVariantMap getCustom() const { return _custom; }
    QVariantMap getSkin() const { return _skin; }
    /// Skins puts its skin here, or {} when it takes it off
    void setSkin(const QVariantMap& skin);

    /// a built-in scheme by name ("graphite", "midnight", "dusk", "daylight"), or "custom" for the loaded file
    Q_INVOKABLE void setScheme(const QString& scheme);

    /// reads a theme file (a path or a file: URL); "" when it was loaded and is now in use, else what is wrong with it.
    /// It reads any local path and says whether a file is there: fine only while no world code reaches a QML engine
    /// (a world's Web entity shows web pages only, RenderableWebEntityItem; the tablet gives worlds no loadQMLSource).
    /// Every engine has PanelTheme since 10-31 (security, 10-31)
    Q_INVOKABLE QString loadFile(const QString& pathOrUrl);

signals:
    void changed();

private:
    PanelTheme();

    Setting::Handle<QString> _schemeSetting { "panelTheme/scheme", QString("graphite") };
    Setting::Handle<QVariant> _customSetting { "panelTheme/custom", QVariantMap() };
    QString _scheme;
    QVariantMap _custom;
    QVariantMap _skin;
};

#endif // hifi_PanelTheme_h
