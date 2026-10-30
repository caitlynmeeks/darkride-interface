//
//  LoginDialog.h
//  interface/src/ui
//
//  Created by Bradley Austin Davis on 2015/04/14
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once

#ifndef hifi_LoginDialog_h
#define hifi_LoginDialog_h

#include <atomic>

#include <OffscreenQmlDialog.h>

extern const QUrl LOGIN_DIALOG;

class LoginDialog : public OffscreenQmlDialog {
    Q_OBJECT
    HIFI_QML_DECL

public:
    static void toggleAction();

    LoginDialog(QQuickItem* parent = nullptr);

    ~LoginDialog();

    static void showWithSelection();

    // a sign-in page is up, however it came: the start's full-window one (LoginDialog.qml), the headset's
    // (OverlayLoginDialog.qml) or the one in the tablet window from inside a world (dialogs/TabletLoginDialog.qml);
    // each makes one of these. Its links (Create an Account, Can't access your account?) open the web browser
    static bool isOpen() { return _openCount > 0; }

signals:
    void handleLoginCompleted();
    void handleLoginFailed();

    // occurs upon dismissing the encouraging log in.
    void dismissedLoginDialog();

    void focusEnabled();
    void focusDisabled();

protected slots:
    Q_INVOKABLE void dismissLoginDialog();

    Q_INVOKABLE void login(const QString& username, const QString& password) const;

    Q_INVOKABLE bool getLoginDialogPoppedUp() const;

private:
    static std::atomic<int> _openCount;
};

#endif // hifi_LoginDialog_h
