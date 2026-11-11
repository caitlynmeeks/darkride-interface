//
//  ContinueBody.qml
//
//  Darkride: what the sign-in screen shows at the start when the account is remembered ("Remember me"): one button
//  into the world as that account, and a way to sign in as someone else. The sign-in itself (a token, never the
//  password) is what the viewer kept; "Not you?" signs it out and shows the usual form.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

import Hifi 1.0
import QtQuick 2.7

import controlsUit 1.0 as HifiControlsUit
import stylesUit 1.0 as HifiStylesUit
import TabletScriptingInterface 1.0

Item {
    z: -2
    id: continueBody
    clip: true
    height: root.height
    width: root.width
    property string fontFamily: "Roboto"
    property int fontSize: 15
    property int textFieldFontSize: 18
    property bool fontBold: true

    HifiStylesUit.HifiConstants { id: hifi }

    // the saved sign-in may have ended from elsewhere ("Sign out everywhere else" on the website): it is asked first, so
    // a refused one gets the form saying why instead of a world that can't load. No answer in a few seconds, or no
    // network: in as before (a sign-in nobody refused stays good offline)
    property bool checking: false
    function goIn() {
        if (checking) {
            return;
        }
        checking = true;
        checkTimeout.start();
        MetaverseRequests.send("GET", "/api/v1/user/profile", null, function (error, code, data) {
            if (!checking) {
                return;   // the time ran out and the viewer went in
            }
            checking = false;
            checkTimeout.stop();
            // "Unverified" (an email not verified yet) is no ended sign-in: in as before (the account manager's rule too)
            var unverified = !!data && (data.message === "Unverified" || data.reason === "unverified");
            if (error && code === 401 && !unverified) {
                // the form, saying why; the sign-out itself is the account manager's (its refresh, then signInEnded
                // and logout(): two sign-outs would race)
                bodyLoader.setSource("LinkAccountBody.qml", { "loginDialog": loginDialog, "root": root,
                                     "bodyLoader": bodyLoader, "errorString": "You've been signed out. Please sign in again." });
                return;
            }
            enter();
        });
    }
    function enter() {
        loginDialog.dismissLoginDialog();
        root.tryDestroy();
    }
    Timer {
        id: checkTimeout
        interval: 4000
        onTriggered: {
            if (continueBody.checking) {
                continueBody.checking = false;
                continueBody.enter();
            }
        }
    }

    function signInAsSomeoneElse() {
        // the form, then the sign-out: the sign-out brings no second screen while this one is up (Application)
        bodyLoader.setSource("LinkAccountBody.qml", { "loginDialog": loginDialog, "root": root, "bodyLoader": bodyLoader });
        Account.logOut();
    }

    Item {
        id: continueContainer
        width: root.bannerWidth
        height: continueButton.height + notYouText.height + 2 * hifi.dimensions.contentSpacing.y
        anchors {
            top: parent.top
            topMargin: root.bannerHeight + 0.25 * parent.height
            horizontalCenter: parent.horizontalCenter
        }

        HifiControlsUit.Button {
            id: continueButton
            width: parent.width
            height: 2 * hifi.dimensions.contentSpacing.y + 31
            anchors.top: parent.top
            color: hifi.buttons.blue
            text: qsTr("Continue as ") + Account.username
            fontCapitalization: Font.MixedCase
            fontFamily: continueBody.fontFamily
            fontSize: continueBody.textFieldFontSize
            fontBold: continueBody.fontBold
            onClicked: continueBody.goIn()
        }

        Text {
            id: notYouText
            anchors {
                top: continueButton.bottom
                topMargin: 2 * hifi.dimensions.contentSpacing.y
                horizontalCenter: parent.horizontalCenter
            }
            font.family: continueBody.fontFamily
            font.pixelSize: continueBody.textFieldFontSize
            font.bold: continueBody.fontBold
            color: "white"
            linkColor: hifi.colors.blueAccent
            text: qsTr("Not you? ") + "<a href='#'>" + qsTr("Sign in as someone else") + "</a>"
            onLinkActivated: {
                Tablet.playSound(TabletEnums.ButtonClick);
                continueBody.signInAsSomeoneElse();
            }
        }
    }

    // the remembered sign-in turned out to be no good (expired, or signed out elsewhere): the usual form
    Connections {
        target: Account
        function onLoggedInChanged(loggedIn) {
            if (!loggedIn) {
                bodyLoader.setSource("LinkAccountBody.qml", { "loginDialog": loginDialog, "root": root, "bodyLoader": bodyLoader });
            }
        }
    }

    Keys.onPressed: {
        switch (event.key) {
            case Qt.Key_Enter:
            case Qt.Key_Return:
                event.accepted = true;
                continueBody.goIn();
                break;
        }
    }

    Component.onCompleted: {
        continueBody.forceActiveFocus();
    }
}
