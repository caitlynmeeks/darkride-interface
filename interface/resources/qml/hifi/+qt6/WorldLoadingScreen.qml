//
//  WorldLoadingScreen.qml
//
//  Darkride's loading screen, over the whole window while a hop or an arrival is under way: the world's picture,
//  blurred, behind everything; the picture itself on a rounded card; the world's name and its author; a thin bar
//  that glides; one short line saying what's happening. It fades in at once and fades out into the world.
//
//  It draws only; scripts/system/worldLoading.js decides, calling Window.setLoadingScreen({up: true|false, world,
//  author, picture: "<url>", status, progress, canCancel, canShowNow}), any field left out keeping its last value. progress is 0 to 1 while a
//  world its owner set to load whole first is loading, and the bar fills; negative (the default) and the bar glides. (QML may not listen to the
//  Messages client, which lives on the network thread; Window lives on the main thread with this.) In a headset
//  the script shows its own card instead, and this stays hidden.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

import QtQuick 2.15
import Qt5Compat.GraphicalEffects   // Qt 6: QtGraphicalEffects lives in the Qt5Compat module (the Qt 5 file is beside this +qt6 folder)

Item {
    id: root
    anchors.fill: parent
    // over every window and dialog (the desktop's modal level is 4000), under its menus (8000)
    z: 7000

    // the sign-in screen's navy and darkride.ai's page colour (--dr-bg), so every hand-off between them is unseen
    readonly property color night: "#1c2136"

    property bool up: false
    property string world: ""
    property string author: ""
    property string picture: ""
    property string status: ""
    property real progress: -1
    // the buttons the script offers: Cancel (back where you came from) and Show me now (a world loading whole)
    property bool canCancel: false
    property bool canShowNow: false

    readonly property bool shown: up && !HMD.active
    opacity: shown ? 1.0 : 0.0
    visible: opacity > 0.0
    Behavior on opacity {
        NumberAnimation {
            duration: root.shown ? 250 : 500
            easing.type: Easing.InOutQuad
        }
    }

    // while it's up, the world behind it takes no clicks
    MouseArea {
        anchors.fill: parent
        enabled: root.shown
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onWheel: wheel.accepted = true
    }

    Rectangle {
        anchors.fill: parent
        color: root.night
    }

    // the world's own colours: its picture, blurred and darkened, filling the window
    Image {
        id: backdrop
        anchors.fill: parent
        source: root.picture
        fillMode: Image.PreserveAspectCrop
        sourceSize.width: 256
        asynchronous: true
        visible: false
    }
    FastBlur {
        anchors.fill: parent
        source: backdrop
        radius: 64
        visible: backdrop.status === Image.Ready
    }
    Rectangle {
        anchors.fill: parent
        color: root.night
        opacity: 0.55
    }
    RadialGradient {
        anchors.fill: parent
        horizontalRadius: width * 0.75
        verticalRadius: height * 0.85
        gradient: Gradient {
            GradientStop { position: 0.45; color: "transparent" }
            GradientStop { position: 1.0; color: "#8c000000" }
        }
    }

    Column {
        anchors.centerIn: parent
        width: card.width

        Item {
            id: card
            width: Math.min(root.width * 0.42, 560)
            height: Math.round(width * 2 / 3)
            visible: root.picture !== "" && cardImage.status !== Image.Error

            Image {
                id: cardImage
                anchors.fill: parent
                source: root.picture
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                mipmap: true
                visible: false
            }
            Rectangle {
                id: cardShape
                anchors.fill: parent
                radius: 22
                visible: false
            }
            OpacityMask {
                id: cardRounded
                anchors.fill: parent
                source: cardImage
                maskSource: cardShape
                visible: false
            }
            DropShadow {
                anchors.fill: cardRounded
                source: cardRounded
                verticalOffset: 18
                radius: 40
                samples: 81
                color: "#8c000000"
            }
            // a hairline, so a dark picture still has an edge
            Rectangle {
                anchors.fill: parent
                radius: 22
                color: "transparent"
                border.color: "#24ffffff"
                border.width: 1
            }
        }

        Item {
            width: 1
            height: card.visible ? 38 : 0
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: root.world
            visible: text !== ""
            color: "white"
            font.pixelSize: 40
            font.weight: Font.DemiBold
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            topPadding: 6
            text: root.author !== "" ? "by " + root.author : ""
            visible: text !== ""
            color: "#99ffffff"
            font.pixelSize: 19
        }

        Item {
            width: 1
            height: 44
        }

        // a bar with a short light gliding along it, back and forth, while anything is under way; or, while a world
        // loads whole, filling from the left
        Item {
            id: bar
            anchors.horizontalCenter: parent.horizontalCenter
            width: 300
            height: 5

            Rectangle {
                anchors.fill: parent
                radius: height / 2
                color: "#2effffff"
            }
            Rectangle {
                id: fill
                visible: root.progress >= 0
                width: Math.max(height, bar.width * Math.min(1, Math.max(0, root.progress)))
                height: parent.height
                radius: height / 2
                color: "#e6ffffff"
                Behavior on width {
                    NumberAnimation { duration: 300; easing.type: Easing.OutCubic }
                }
            }
            Rectangle {
                id: glider
                visible: root.progress < 0
                width: 90
                height: parent.height
                radius: height / 2
                color: "#e6ffffff"

                SequentialAnimation on x {
                    running: root.visible && root.progress < 0
                    loops: Animation.Infinite
                    NumberAnimation { from: 0; to: bar.width - glider.width; duration: 1100; easing.type: Easing.InOutSine }
                    NumberAnimation { from: bar.width - glider.width; to: 0; duration: 1100; easing.type: Easing.InOutSine }
                }
            }
        }

        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            topPadding: 14
            text: root.status
            color: "#8cffffff"
            font.pixelSize: 15
        }

        Item {
            width: 1
            height: 30
        }

        // quiet pills that fade in when offered; the script says what each does (Window.loadingScreenButtonPressed)
        Row {
            id: buttons
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 12
            height: 36
            opacity: root.canCancel || root.canShowNow ? 1.0 : 0.0
            Behavior on opacity {
                NumberAnimation { duration: 300; easing.type: Easing.InOutQuad }
            }

            Repeater {
                // Cancel on the left, as on every pair of buttons (Caity, 10-22)
                model: [
                    { name: "cancel", label: "Cancel", shown: root.canCancel },
                    { name: "showNow", label: "Show me now", shown: root.canShowNow }
                ]
                delegate: Rectangle {
                    visible: modelData.shown
                    width: label.implicitWidth + 40
                    height: 36
                    radius: height / 2
                    color: area.pressed ? "#40ffffff" : (area.containsMouse ? "#2effffff" : "#1affffff")
                    border.color: "#33ffffff"
                    border.width: 1

                    Text {
                        id: label
                        anchors.centerIn: parent
                        text: modelData.label
                        color: "#e6ffffff"
                        font.pixelSize: 15
                        font.weight: Font.Medium
                    }
                    MouseArea {
                        id: area
                        anchors.fill: parent
                        enabled: root.shown && buttons.opacity > 0.5
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: WindowScriptingInterface.pressLoadingScreenButton(modelData.name)
                    }
                }
            }
        }
    }

    // Window.setLoadingScreen() from scripts/system/worldLoading.js: a field left out keeps its last value
    Connections {
        target: WindowScriptingInterface
        function onLoadingScreenChanged(state) {
            if (state.world !== undefined) {
                root.world = state.world || "";
            }
            if (state.author !== undefined) {
                root.author = state.author || "";
            }
            if (state.picture !== undefined) {
                root.picture = state.picture || "";
            }
            if (state.status !== undefined) {
                root.status = state.status || "";
            }
            if (state.progress !== undefined) {
                root.progress = state.progress;
            }
            if (state.canCancel !== undefined) {
                root.canCancel = !!state.canCancel;
            }
            if (state.canShowNow !== undefined) {
                root.canShowNow = !!state.canShowNow;
            }
            if (state.up !== undefined) {
                root.up = !!state.up;
            }
        }
    }
}
