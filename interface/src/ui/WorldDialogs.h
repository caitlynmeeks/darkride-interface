//
//  WorldDialogs.h
//  interface/src/ui
//
//  A world's script's dialogs (Window.alert, confirm, prompt, promptAsync: the ones its API guard lets it call), on
//  the desktop and on the VR screen alike: titled
//  "<world> asks" in a look unlike the app's own (no Darkride picture, the world's words as plain text, a frame no skin changes); one
//  open at a time, at most a few in a short time; from the second, a tick "Don't let this world show more dialogs",
//  which holds until the visitor leaves; leaving the world answers Cancel on the one still open; a prompt carries the
//  line "Darkride never asks for your password in a world's dialog.". The caller is a world's when scripting's
//  ScriptCaller mark says so (the API guards mark each call a world's script makes); the visitor's own scripts and
//  the app's dialogs never come here. Main thread: WindowScriptingInterface checks the mark on the script's thread and
//  calls in here.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#pragma once
#ifndef hifi_WorldDialogs_h
#define hifi_WorldDialogs_h

#include <deque>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QVariant>
#include <QtWidgets/QMessageBox>

class QQuickItem;
class ModalDialogListener;

class WorldDialogs : public QObject {
    Q_OBJECT
public:
    explicit WorldDialogs(QObject* parent = nullptr);

    // Each makes the world's dialog, or refuses it (one is open, too many in a short time, or the visitor said no more
    // for this world) and answers as a Cancel would: nothing, false, null.
    Q_INVOKABLE void alert(const QString& text);
    Q_INVOKABLE bool confirm(const QString& text);
    Q_INVOKABLE QVariant prompt(const QString& text, const QString& defaultText, const QVariantMap& properties);
    /// the listener of the prompt, or null when refused
    Q_INVOKABLE ModalDialogListener* promptAsync(const QString& text, const QString& defaultText,
                                                 const QVariantMap& properties);

    /// the name a world's dialog gives its world: the place's name, or the server's address
    static QString worldName();

private slots:
    void answered();   // a world's dialog's selected or canceled signal: the visitor's "no more" tick holds

private:
    QVariantMap admit();   // the dialog's world properties, or empty when it's refused
    void track(QQuickItem* dialog);
    void leftWorld();

    QPointer<QQuickItem> _open;   // the world's dialog showing, if one is
    std::deque<quint64> _recent;   // when the last ones opened (usecs), for the limit
    int _shown { 0 };   // this visit's, for the "no more" tick (from the second)
    bool _blocked { false };   // the visitor's "Don't let this world show more dialogs", until the visitor leaves
};

#endif // hifi_WorldDialogs_h
