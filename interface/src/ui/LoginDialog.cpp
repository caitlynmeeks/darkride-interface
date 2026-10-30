//
//  LoginDialog.cpp
//  interface/src/ui
//
//  Created by Bradley Austin Davis on 2015/04/14
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "LoginDialog.h"

#include <ui/TabletScriptingInterface.h>

#include "AccountManager.h"
#include "DependencyManager.h"
#include "Menu.h"

#include "Application.h"
#include "scripting/HMDScriptingInterface.h"

HIFI_QML_DEF(LoginDialog)

static const QUrl TABLET_LOGIN_DIALOG_URL("dialogs/TabletLoginDialog.qml");
const QUrl LOGIN_DIALOG = PathUtils::qmlUrl("OverlayLoginDialog.qml");

std::atomic<int> LoginDialog::_openCount { 0 };

LoginDialog::LoginDialog(QQuickItem *parent) : OffscreenQmlDialog(parent) {
    ++_openCount;
    auto accountManager = DependencyManager::get<AccountManager>();
    // the login hasn't been dismissed yet if the user isn't logged in and is encouraged to login.
#if !defined(Q_OS_ANDROID)
    connect(accountManager.data(), &AccountManager::loginComplete,
        this, &LoginDialog::handleLoginCompleted);
    connect(accountManager.data(), &AccountManager::loginFailed,
            this, &LoginDialog::handleLoginFailed);
    auto& signInGate = qApp->getSignInGate();
    connect(&signInGate, &SignInGate::loginDialogFocusEnabled, this, &LoginDialog::focusEnabled);
    connect(&signInGate, &SignInGate::loginDialogFocusDisabled, this, &LoginDialog::focusDisabled);
    connect(this, &LoginDialog::dismissedLoginDialog, &signInGate, &SignInGate::onDismissedLoginDialog);
#endif
}

LoginDialog::~LoginDialog() {
    --_openCount;
}

void LoginDialog::showWithSelection() {
    auto tabletScriptingInterface = DependencyManager::get<TabletScriptingInterface>();
    auto tablet = dynamic_cast<TabletProxy*>(tabletScriptingInterface->getTablet("com.highfidelity.interface.tablet.system"));
    auto hmd = DependencyManager::get<HMDScriptingInterface>();

    if (!qApp->isHMDMode()) {
        if (qApp->getLoginDialogPoppedUp()) {
            LoginDialog::show();
            return;
        } else {
            if (!tablet->isPathLoaded(TABLET_LOGIN_DIALOG_URL)) {
                tablet->loadQMLSource(TABLET_LOGIN_DIALOG_URL);
            }
        }
    } else {
        if (!qApp->getLoginDialogPoppedUp()) {
            tablet->initialScreen(TABLET_LOGIN_DIALOG_URL);
        } else {
            qApp->getSignInGate().createLoginDialog();
        }
    }

    if (!hmd->getShouldShowTablet()) {
        hmd->openTablet();
    }
}

void LoginDialog::toggleAction() {
    auto accountManager = DependencyManager::get<AccountManager>();
    QAction* loginAction = Menu::getInstance()->getActionForOption(MenuOption::Login);
    if (loginAction == nullptr) return;

    static QMetaObject::Connection connection;
    if (connection) {
        disconnect(connection);
    }

    if (accountManager->isLoggedIn()) {
        // change the menu item to logout
        loginAction->setText("Logout " + accountManager->getAccountInfo().getUsername());
        connection = connect(loginAction, &QAction::triggered, accountManager.data(), &AccountManager::logout);
    } else {
        // change the menu item to login
        loginAction->setText("Log In / Sign Up");
        connection = connect(loginAction, &QAction::triggered, [] {
            // if not in login state, show.
            if (!qApp->getLoginDialogPoppedUp()) {
                LoginDialog::showWithSelection();
            }
        });
    }
}

void LoginDialog::dismissLoginDialog() {
    QAction* loginAction = Menu::getInstance()->getActionForOption(MenuOption::Login);
    if (loginAction == nullptr) return;

    loginAction->setEnabled(true);

    emit dismissedLoginDialog();
}

void LoginDialog::login(const QString& username, const QString& password) const {
    qDebug() << "Attempting to login " << username;
    DependencyManager::get<AccountManager>()->requestAccessToken(username, password);
}

bool LoginDialog::getLoginDialogPoppedUp() const {
    return qApp->getLoginDialogPoppedUp();
}
