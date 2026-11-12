//
//  LinkAccountBody.qml
//
//  Created by Clement on 7/18/16
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

import Hifi 1.0
import QtQuick 2.7

import "." as LoginDialog

import controlsUit 1.0 as HifiControlsUit
import stylesUit 1.0 as HifiStylesUit
import TabletScriptingInterface 1.0

Item {
    z: -2
    id: linkAccountBody
    clip: true
    height: root.height
    width: root.width
    property int textFieldHeight: 31
    property string fontFamily: "Roboto"
    property int fontSize: 15
    property int textFieldFontSize: 18
    property bool fontBold: true
    readonly property var passwordImageRatio: 16 / 23

    property bool keyboardEnabled: false
    property bool keyboardRaised: false
    property bool punctuationMode: false

    property string errorString: errorString
    property bool lostFocus: false

    readonly property bool loginDialogPoppedUp: loginDialog.getLoginDialogPoppedUp()

    QtObject {
        id: d
        readonly property int minWidth: 480
        readonly property int minWidthButton: !root.isTablet ? 256 : 174
        property int maxWidth: root.width
        readonly property int minHeight: 120
        readonly property int minHeightButton: 36
        property int maxHeight: root.height
        property bool resizing: false

        function resize() {
            // The desktop's page fills the window (LoginDialog.qml binds root's size to it): nothing to fit. And the size
            // this works out is root's own plus a step (mainContainer follows root), so every call grew it: called a
            // moment later (Qt.callLater, 10-25 morning), each call set off the next, at every pass of the event loop,
            // the form slid off the bottom and the main thread never rested (Caity's MacBook, 10-25: only the navy
            // showed, and nothing could be clicked). The headset's page (OverlayLoginDialog.qml) fills its surface the
            // same way, and fitting it cut it short: the form loads before the page has a size, so the fit made it
            // 480 x 134 and the surface's own size then only stretched it to ~218 tall, the rest of the surface left
            // white over the password and Log In (Caity's VR sitting, 10-29). Only the tablet's dialog fits its content,
            // at once and once: root's change calls back here, and is let go.
            if (!root.isTablet || resizing) {
                return;
            }
            resizing = true;
            maxWidth = root.width;
            maxHeight = root.height;
            var targetWidth = Math.max(titleWidth, mainContainer.width);
            var targetHeight =  hifi.dimensions.contentSpacing.y + mainContainer.height + 4 * hifi.dimensions.contentSpacing.y;

            var newWidth = Math.max(d.minWidth, Math.min(d.maxWidth, targetWidth));
            if (!isNaN(newWidth)) {
                parent.width = root.width = newWidth;
            }

            parent.height = root.height = Math.max(d.minHeight, Math.min(d.maxHeight, targetHeight)) + hifi.dimensions.contentSpacing.y;
            resizing = false;
        }
    }

    function login() {
        loginDialog.login(emailField.text, passwordField.text);
        bodyLoader.setSource("LoggingInBody.qml", { "loginDialog": loginDialog, "root": root, "bodyLoader": bodyLoader });
    }

    function init() {
        // a sign-in the metaverse no longer took (SignInGate): the form says why, once, with that account's name
        var endedName = "";
        if (Settings.getValue("private/signIn/ended", false) === true) {
            Settings.setValue("private/signIn/ended", false);
            endedName = String(Settings.getValue("private/signIn/endedName", "") || "");
            Settings.setValue("private/signIn/endedName", "");
            if (linkAccountBody.errorString === "") {
                linkAccountBody.errorString = "You've been signed out. Please sign in again.";
            }
        }
        // going to/from sign in/up dialog.
        loginErrorMessage.text = linkAccountBody.errorString;
        loginErrorMessage.visible = (linkAccountBody.errorString  !== "");
        if (loginErrorMessageTextMetrics.width > emailField.width) {
            loginErrorMessage.wrapMode = Text.WordWrap;
            errorContainer.height = (loginErrorMessageTextMetrics.width / emailField.width) * loginErrorMessageTextMetrics.height;
        }
        loginButton.text = "Log In";
        loginButton.color = hifi.buttons.blue;
        emailField.placeholderText = "Username or Email";
        var savedUsername = Settings.getValue("keepMeLoggedIn/savedUsername", "");
        emailField.text = keepMeLoggedInCheckbox.checked ? savedUsername === "Unknown user" ? "" : savedUsername : "";
        if (emailField.text === "" && endedName !== "") {
            emailField.text = endedName;
        }
        loginButton.anchors.left = emailField.left;
        loginContainer.visible = true;
    }

    Item {
        z: 10
        id: mainContainer
        width: root.width
        height: root.height
        onHeightChanged: d.resize(); onWidthChanged: d.resize();

        LoginDialog.LoginDialogLightbox {
            id: lightboxPopup;
            visible: false;
            anchors.fill: parent;
        }

        Item {
            id: loginContainer
            width: emailField.width
            height: errorContainer.height + emailField.height + passwordField.height + 5 * hifi.dimensions.contentSpacing.y +
                keepMeLoggedInCheckbox.height + loginButton.height + createAccountButton.height + cantAccessTextMetrics.height
            anchors {
                top: parent.top
                topMargin: root.bannerHeight + 0.25 * parent.height
                left: parent.left
                leftMargin: (parent.width - emailField.width) / 2
            }

            Item {
                id: errorContainer
                width: parent.width
                height: loginErrorMessageTextMetrics.height
                anchors {
                    bottom: emailField.top;
                    bottomMargin: hifi.dimensions.contentSpacing.y;
                    left: emailField.left;
                }
                TextMetrics {
                    id: loginErrorMessageTextMetrics
                    font: loginErrorMessage.font
                    text: loginErrorMessage.text
                }
                Text {
                    id: loginErrorMessage;
                    color: "red";
                    font.family: linkAccountBody.fontFamily
                    font.pixelSize: linkAccountBody.textFieldFontSize
                    font.bold: linkAccountBody.fontBold
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                    text: ""
                    visible: false
                }
            }

            // No display name field: a Darkride avatar is called by its account's username, which the viewer sets when
            // the sign-in completes (Application, AccountManager::usernameChanged).
            HifiControlsUit.TextField {
                id: emailField
                width: root.bannerWidth
                height: linkAccountBody.textFieldHeight
                font.pixelSize: linkAccountBody.textFieldFontSize
                styleRenderType: Text.QtRendering
                anchors {
                    top: parent.top
                    topMargin: errorContainer.height
                }
                placeholderText: "Username or Email"
                activeFocusOnPress: true
                Keys.onPressed: {
                    switch (event.key) {
                        case Qt.Key_Tab:
                            event.accepted = true;
                            passwordField.focus = true;
                            break;
                        case Qt.Key_Backtab:
                            event.accepted = true;
                            passwordField.focus = true;
                            break;
                        case Qt.Key_Enter:
                        case Qt.Key_Return:
                            event.accepted = true;
                            if (keepMeLoggedInCheckbox.checked) {
                                Settings.setValue("keepMeLoggedIn/savedUsername", emailField.text);
                            }
                            linkAccountBody.login();
                            break;
                    }
                }
                onFocusChanged: {
                    root.text = "";
                    if (focus) {
                        root.isPassword = false;
                    }
                }
            }
            HifiControlsUit.TextField {
                id: passwordField
                width: root.bannerWidth
                height: linkAccountBody.textFieldHeight
                font.pixelSize: linkAccountBody.textFieldFontSize
                styleRenderType: Text.QtRendering
                placeholderText: "Password"
                activeFocusOnPress: true
                echoMode: passwordFieldMouseArea.showPassword ? TextInput.Normal : TextInput.Password
                anchors {
                    top: emailField.bottom
                    topMargin: 1.5 * hifi.dimensions.contentSpacing.y
                }

                onFocusChanged: {
                    root.text = "";
                    root.isPassword = focus;
                }

                Item {
                    id: showPasswordContainer
                    z: 10
                    // width + image's rightMargin
                    width: showPasswordImage.width + 8
                    height: parent.height
                    anchors {
                        right: parent.right
                    }

                    Image {
                        id: showPasswordImage
                        width: passwordField.height * passwordImageRatio
                        height: passwordField.height * passwordImageRatio
                        anchors {
                            right: parent.right
                            rightMargin: 8
                            top: parent.top
                            topMargin: passwordFieldMouseArea.showPassword ? 6 : 8
                            bottom: parent.bottom
                            bottomMargin: passwordFieldMouseArea.showPassword ? 5 : 8
                        }
                        source: passwordFieldMouseArea.showPassword ?  "../../images/eyeClosed.svg" : "../../images/eyeOpen.svg"
                        MouseArea {
                            id: passwordFieldMouseArea
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton
                            property bool showPassword: false
                            onClicked: {
                                showPassword = !showPassword;
                            }
                        }
                    }
                }
                Keys.onPressed: {
                    switch (event.key) {
                        case Qt.Key_Tab:
                            event.accepted = true;
                            emailField.focus = true;
                            break;
                        case Qt.Key_Backtab:
                            event.accepted = true;
                            emailField.focus = true;
                            break;
                        case Qt.Key_Enter:
                        case Qt.Key_Return:
                            event.accepted = true;
                            if (keepMeLoggedInCheckbox.checked) {
                                Settings.setValue("keepMeLoggedIn/savedUsername", emailField.text);
                            }
                            linkAccountBody.login();
                            break;
                    }
                }
            }
            HifiControlsUit.CheckBox {
                id: keepMeLoggedInCheckbox
                checked: Settings.getValue("keepMeLoggedIn", true);
                text: qsTr("Remember me");
                boxSize: 18;
                labelFontFamily: linkAccountBody.fontFamily
                labelFontSize: 18;
                color: hifi.colors.white;
                anchors {
                    top: passwordField.bottom;
                    topMargin: hifi.dimensions.contentSpacing.y;
                    left: passwordField.left;
                }
                onCheckedChanged: {
                    Settings.setValue("keepMeLoggedIn", checked);
                    if (keepMeLoggedInCheckbox.checked) {
                        Settings.setValue("keepMeLoggedIn/savedUsername", emailField.text);
                    } else {
                        Settings.setValue("keepMeLoggedIn/savedUsername", "");
                    }
                }
                Component.onCompleted: {
                    keepMeLoggedInCheckbox.checked = !Account.loggedIn;
                }
            }
            HifiControlsUit.Button {
                id: loginButton
                width: passwordField.width
                height: d.minHeightButton
                text: qsTr("Log In")
                fontFamily: linkAccountBody.fontFamily
                fontSize: linkAccountBody.fontSize
                fontBold: linkAccountBody.fontBold
                anchors {
                    top: keepMeLoggedInCheckbox.bottom
                    topMargin: hifi.dimensions.contentSpacing.y
                }
                onClicked: {
                    linkAccountBody.login();
                }
            }
            // making an account is on the website, in the browser. Right under Log In, so it shows wherever Log In does:
            // a row of its own further down fell off the bottom of a short window (on the desktop the tablet's window,
            // where a world's sign-in request opens this, is taller than a 600 px app window; 10-30)
            HifiControlsUit.Button {
                id: createAccountButton
                width: passwordField.width
                height: d.minHeightButton
                text: qsTr("Create an Account")
                color: hifi.buttons.none
                fontFamily: linkAccountBody.fontFamily
                fontSize: linkAccountBody.fontSize
                fontBold: linkAccountBody.fontBold
                anchors {
                    top: loginButton.bottom
                    topMargin: hifi.dimensions.contentSpacing.y
                }
                onClicked: {
                    Tablet.playSound(TabletEnums.ButtonClick);
                    WindowScriptingInterface.openUrl("https://darkride.ai/?signUp");
                }
            }
            TextMetrics {
                id: cantAccessTextMetrics
                font: cantAccessText.font
                text: "Can't access your account?"
            }
            HifiStylesUit.ShortcutText {
                id: cantAccessText
                z: 10
                anchors {
                    top: createAccountButton.bottom
                    topMargin: hifi.dimensions.contentSpacing.y
                    left: emailField.left
                }
                font.family: linkAccountBody.fontFamily
                font.pixelSize: linkAccountBody.textFieldFontSize
                font.bold: linkAccountBody.fontBold

                // the website's account recovery, in the browser
                text: "<a href='https://darkride.ai/?forgotPassword'> Can't access your account?</a>"

                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                linkColor: hifi.colors.blueAccent
                onLinkActivated: {
                    Tablet.playSound(TabletEnums.ButtonClick);
                    WindowScriptingInterface.openUrl(link);
                    lightboxPopup.titleText = "Can't Access Account";
                    lightboxPopup.bodyText = lightboxPopup.cantAccessBodyText;
                    lightboxPopup.button2text = "CLOSE";
                    lightboxPopup.button2method = function() {
                        lightboxPopup.visible = false;
                    }
                    lightboxPopup.visible = true;
                }
            }
        }
        // No "No thanks, take me in-world!" here: Darkride has no anonymous sign-in, so the screen stays until
        // someone signs in (the window can still be closed).
    }

    Connections {
        target: loginDialog
        function onFocusEnabled() {
            if (!linkAccountBody.lostFocus) {
                Qt.callLater(function() {
                    emailField.forceActiveFocus();
                });
            }
        }
        function onFocusDisabled() {
            linkAccountBody.lostFocus = !root.isTablet && !root.isOverlay;
            if (linkAccountBody.lostFocus) {
                Qt.callLater(function() {
                    emailField.focus = false;
                    passwordField.focus = false;
                });
            }
        }
    }

    Component.onCompleted: {
        //but rise Tablet's one instead for Tablet interface
        root.keyboardEnabled = HMD.active;
        root.keyboardRaised = Qt.binding( function() { return keyboardRaised; })
        root.text = "";
        d.resize();
        init();
        Qt.callLater(function() {
            emailField.forceActiveFocus();
        });
    }

    Keys.onPressed: {
        if (!visible) {
            return;
        }

        switch (event.key) {
            case Qt.Key_Enter:
            case Qt.Key_Return:
                event.accepted = true;
                Settings.setValue("keepMeLoggedIn/savedUsername", emailField.text);
                linkAccountBody.login();
                break;
        }
    }
}
