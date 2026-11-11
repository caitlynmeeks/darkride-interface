//
//  SignInGate.h
//  interface/src
//
//  The sign-in gate: Darkride starts on the sign-in screen, and goes back to it after signing out; until an account
//  is chosen (or the remembered one continued), the avatar is hidden, the movement mappings are off, no world is
//  joined, no scripts run and the menus are hidden. showLoginScreen() puts the screen up; pauseUntilLoginDetermined()
//  holds everything as above, saving what it changes; resumeAfterLoginDialogActionTaken() undoes it, loads the
//  scripts and queues the trip to the starting location (it waits for the pause when asked before it);
//  onDismissedLoginDialog() is the screen going down. A headset shows the screen as a web entity in front of the
//  camera (createLoginDialog), kept in view and with the hands' state once a frame (update()), and taken down when
//  the screen goes; displayPluginChanged() moves the screen between the headset's entity and the desktop's dialog.
//  poppedUp() says the screen is up; signInPending() says the viewer has not gone in yet (the 3D view isn't drawn
//  then); startUpFinished() says it went in once. The focus signals tell the desktop's dialog when it has the
//  keyboard. Application owns one, reached by getSignInGate(); its getLoginDialogPoppedUp() and isSignInPending()
//  forward here. These were Application's _loginDialogPoppedUp, _signInPending, _startUpFinished, _loginDialogID,
//  _loginStateManager and the pause's saved state, the sign-in members of ApplicationUi.cpp, the frame loop's
//  dialog step and the start-up's display-change handler (ARCHITECTURE.md §5.4 step 4, the seventh owner class).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_SignInGate_h
#define hifi_SignInGate_h

#include <atomic>

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

#include <SettingHandle.h>

#include "LoginStateManager.h"

class QWidget;

class SignInGate : public QObject {
    Q_OBJECT
public:
    bool poppedUp() const { return _poppedUp; }                     // the sign-in screen is up (the desktop's dialog or the headset's entity)
    bool signInPending() const { return _signInPending; }           // from the start, and again after signing out, until the viewer goes in
    bool startUpFinished() const { return _startUpFinished; }       // the viewer went in once
    const QUuid& loginDialogID() const { return _loginDialogID; }   // the headset's sign-in dialog entity, null when there is none

    void showLoginScreen();
    void pauseUntilLoginDetermined();
    void resumeAfterLoginDialogActionTaken();
    void createLoginDialog();      // the headset's: a web entity in front of the camera, with the keyboard beside it
    void update();                 // once a frame: while the headset's dialog is up, the hands' state and the dialog kept in view
    void displayPluginChanged();   // the screen follows the display: the headset's entity or the desktop's dialog
    void shutdown();               // the setting cleared
    /// A sign-in the metaverse no longer takes (ended from elsewhere: "Sign out everywhere else" on the website): the
    /// saved one goes and the sign-in screen comes back, as after signing out, the name filled in, saying why ("You've
    /// been signed out. Please sign in again."). Any thread; nothing while the screen is already up.
    void signInAgain();

public slots:
    void onDismissedLoginDialog();

signals:
    void loginDialogFocusEnabled();
    void loginDialogFocusDisabled();

private:
    void updateLoginDialogPosition();
    // While sign-in is pending on a desktop, the sign-in page is looked for each second and put back after 2 s without
    // it (Caity, 10-25: a first run on her MacBook showed it for a second, then only the navy behind it, with nothing
    // to click)
    void checkSignInPage();
    QTimer _pageCheck;
    int _pageMissingFor{ 0 };   // seconds
    QString _pageState;         // the last state logged

    bool _poppedUp{ false };
    Setting::Handle<bool> _poppedUpSetting{ "loginDialogPoppedUp", false };   // written for whoever reads the settings file; not read here
    std::atomic<bool> _signInPending{ true };
    bool _startUpFinished{ false };
    bool _interstitialModeEnabled{ false };   // the domain handler's, saved by the pause and restored by the resume
    bool _developerMenuVisible{ false };      // likewise the Developer menu's visibility
    QString _previousAvatarSkeletonModel;     // likewise the avatar's model and scale
    float _previousAvatarTargetScale{ 1.0f };
    QUuid _loginDialogID;
    LoginStateManager _loginStateManager;
    bool _resumeWasPostponed{ false };   // the resume was asked before the pause had run
    bool _resumeSafeToRun{ false };      // the pause has run
    QList<QPointer<QWidget>> _hiddenDocks;   // the docks showing when the screen came up, shown again after it
};

#endif // hifi_SignInGate_h
