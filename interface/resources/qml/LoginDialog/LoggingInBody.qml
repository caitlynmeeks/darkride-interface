//
//  LoggingInBody.qml
//
//  Created by Wayne Chen on 10/18/18
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

import Hifi 1.0
import QtQuick 2.7

import controlsUit 1.0 as HifiControlsUit
import stylesUit 1.0 as HifiStylesUit

Item {
    id: loggingInBody
    clip: true
    height: root.height
    width: root.width
    property int textFieldHeight: 31
    property string fontFamily: "Roboto"
    property int fontSize: 15
    property bool fontBold: true

    readonly property bool loginDialogPoppedUp: loginDialog.getLoginDialogPoppedUp()

    QtObject {
        id: d
        readonly property int minWidth: 480
        property int maxWidth: root.width
        readonly property int minHeight: 120
        property int maxHeight: root.height

        function resize() {
            // the desktop's and the headset's pages fill their window or surface: only the tablet's dialog fits its
            // content (as LinkAccountBody.qml; here each fit made the page a step taller, in a binding loop)
            if (!root.isTablet) {
                return;
            }
            maxWidth = root.width;
            maxHeight = root.height;
            var targetWidth = Math.max(titleWidth, mainContainer.width);
            var targetHeight =  hifi.dimensions.contentSpacing.y + mainContainer.height +
                    4 * hifi.dimensions.contentSpacing.y;

            var newWidth = Math.max(d.minWidth, Math.min(d.maxWidth, targetWidth));
            if (!isNaN(newWidth)) {
                parent.width = root.width = newWidth;
            }

            parent.height = root.height = Math.max(d.minHeight, Math.min(d.maxHeight, targetHeight)) + hifi.dimensions.contentSpacing.y;
        }
    }

    // timer to kill the dialog upon login success
    Timer {
        id: successTimer
        interval: 1000;
        running: false;
        repeat: false;
        onTriggered: {
            if (loggingInBody.loginDialogPoppedUp) {
                loginDialog.dismissLoginDialog();
            }
            root.tryDestroy();
        }
    }

    function init() {
        // For the process of logging in.
        loggingInText.wrapMode = Text.NoWrap;
        loggingInText.text = "Logging in…";
        loggingInText.anchors.centerIn = loggingInHeader;
        loggingInSpinner.visible = true;
    }
    function loadingSuccess() {
        loggingInSpinner.visible = false;
        loggingInText.text = "You are now logged in!";
        // the avatar's name is the account's username, set by the viewer when the username arrives (Application)
        successTimer.start();
    }

    Item {
        id: mainContainer
        anchors.fill: parent
        onHeightChanged: d.resize(); onWidthChanged: d.resize();

        Item {
            id: loggingInContainer
            anchors.fill: parent
            onHeightChanged: d.resize(); onWidthChanged: d.resize();

            Item {
                id: loggingInHeader
                width: parent.width
                height: loggingInText.height
                // under the banner (0.18 of the height down, OverlayLoginDialog.qml), not across it
                anchors {
                    top: parent.top
                    topMargin: 0.18 * parent.height + root.bannerHeight + 0.08 * parent.height
                    left: parent.left
                }
                TextMetrics {
                    id: loggingInTextMetrics;
                    font: loggingInText.font;
                    text: loggingInText.text;
                }
                Text {
                    id: loggingInText;
                    width: loggingInTextMetrics.width
                    anchors.verticalCenter: parent.verticalCenter
                    // the sign-in form's type: Roboto, as the website and chat set it
                    color: "#f4f6f8";
                    font.family: loggingInBody.fontFamily
                    font.pixelSize: 20
                    font.weight: Font.Medium
                    wrapMode: Text.NoWrap
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                    text: "Logging in…"
                }
            }
            Item {
                id: loggingInFooter
                width: parent.width
                height: 0.5 * parent.height
                anchors {
                    top: loggingInHeader.bottom
                    topMargin: 2 * hifi.dimensions.contentSpacing.y
                }
                AnimatedImage {
                    id: loggingInSpinner
                    source: "images/loader-snake-256.gif"
                    width: 64
                    height: width
                    anchors.left: parent.left;
                    anchors.leftMargin: (parent.width - width) / 2;
                    anchors.top: parent.top
                    anchors.topMargin: hifi.dimensions.contentSpacing.y
                }
            }
        }
    }

    Component.onCompleted: {
        d.resize();
        loggingInBody.init();
    }

    Connections {
        target: loginDialog

        function onHandleLoginCompleted() {
            console.log("Login Succeeded");
            loggingInBody.loadingSuccess();
        }

        function onHandleLoginFailed() {
            console.log("Login Failed")
            loggingInSpinner.visible = false;
            bodyLoader.setSource("LinkAccountBody.qml", { "loginDialog": loginDialog, "root": root, "bodyLoader": bodyLoader,
                "errorString": "Username or password is incorrect." });
        }
    }
}
