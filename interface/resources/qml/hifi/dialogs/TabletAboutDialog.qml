//
//  TabletAboutDialog.qml
//
//  Created by David Rowe on 18 Apr 2017
//  Copyright 2017 High Fidelity, Inc.
//
//  Darkride's About box: the brand mark, the version, darkride.ai, the copyright, and the code contributors to
//  High Fidelity, Tivoli Cloud VR and Darkride (credits.js, from tools/make-credits.py), rolling like end credits.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

import QtQuick 2.15
import stylesUit 1.0
import "credits.js" as Credits

Rectangle {
    id: root
    width: 480
    height: 706

    // darkride.ai's colours (--dr-bg, --dr-cyan, --dr-violet, --dr-muted)
    readonly property color navy: "#1c2136"
    readonly property color cyan: "#00afff"
    readonly property color violet: "#a548ff"
    readonly property color muted: "#9aa1bd"
    readonly property color ink: "#f4f6f8"

    color: navy

    Column {
        id: header
        anchors.top: parent.top
        anchors.topMargin: 24
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width - 64
        spacing: 6

        Image {
            source: "../../../images/darkride-mark.png"
            width: 260
            height: 220
            fillMode: Image.PreserveAspectFit
            mipmap: true
            anchors.horizontalCenter: parent.horizontalCenter
        }
        RobotoRegular {
            text: "Version " + About.buildVersion + "  ·  " + About.buildDate
            size: 13
            color: root.muted
            anchors.horizontalCenter: parent.horizontalCenter
        }
        RobotoRegular {
            textFormat: Text.StyledText
            linkColor: root.cyan
            text: "<a href=\"https://darkride.ai\">darkride.ai</a>"
            size: 18
            anchors.horizontalCenter: parent.horizontalCenter
            onLinkActivated: WindowScriptingInterface.openUrl("https://darkride.ai")
        }
        RobotoRegular {
            text: "© 2026 Darkride Technologies / Caitlyn Meeks"
            size: 14
            color: root.ink
            anchors.horizontalCenter: parent.horizontalCenter
        }
    }

    Rectangle {
        id: divider
        anchors.top: header.bottom
        anchors.topMargin: 18
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width - 96
        height: 1
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 0.3; color: root.cyan }
            GradientStop { position: 0.7; color: root.violet }
            GradientStop { position: 1.0; color: "transparent" }
        }
    }

    RobotoRegular {
        id: creditsTitle
        anchors.top: divider.bottom
        anchors.topMargin: 14
        anchors.horizontalCenter: parent.horizontalCenter
        text: "CREDITS"
        font.letterSpacing: 3
        size: 12
        color: root.cyan
    }
    RobotoRegular {
        id: creditsNote
        anchors.top: creditsTitle.bottom
        anchors.topMargin: 4
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width - 64
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: "Code contributors to High Fidelity, Tivoli Cloud VR and Darkride"
        size: 12
        color: root.muted
    }

    ListView {
        id: credits
        anchors.top: creditsNote.bottom
        anchors.topMargin: 8
        anchors.bottom: footer.top
        anchors.bottomMargin: 12
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 32
        anchors.rightMargin: 32
        clip: true
        model: Credits.names
        spacing: 3
        boundsBehavior: Flickable.StopAtBounds
        // a blank page before and after, so the names roll in from the bottom and out at the top
        header: Item { width: 1; height: credits.height }
        footer: Item { width: 1; height: credits.height }
        delegate: RobotoRegular {
            width: credits.width
            horizontalAlignment: Text.AlignHCenter
            text: modelData
            size: 13
            color: root.ink
        }

    }

    // rolls like end credits, from the top again after the last name; it holds only while dragged by hand
    Timer {
        interval: 16
        repeat: true
        running: root.visible && !credits.moving
        onTriggered: {
            if (credits.atYEnd) {
                credits.contentY = credits.originY;
            } else {
                credits.contentY += 0.6;
            }
        }
    }

    // the names fade in and out at the list's edges
    Rectangle {
        anchors.top: credits.top
        anchors.left: credits.left
        anchors.right: credits.right
        height: 36
        gradient: Gradient {
            GradientStop { position: 0.0; color: root.navy }
            GradientStop { position: 1.0; color: "transparent" }
        }
    }
    Rectangle {
        anchors.bottom: credits.bottom
        anchors.left: credits.left
        anchors.right: credits.right
        height: 36
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 1.0; color: root.navy }
        }
    }

    Column {
        id: footer
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 18
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width - 64
        spacing: 3

        RobotoRegular {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: "Built on High Fidelity and Tivoli Cloud VR, under the Apache License 2.0"
            size: 11
            color: root.muted
        }
        RobotoRegular {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "Qt " + About.qtVersion + "  ·  physics by Bullet and Jolt"
            size: 11
            color: root.muted
        }
    }
}
