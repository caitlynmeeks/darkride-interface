//
//  Badge.qml
//  qml/darkride/make
//
//  The Make badge (Caity's idea, 11-07; mary's drawing team/mary/brand/make-panel/MAKE-BADGE.png): in the 3D view's
//  bottom-right corner, 12 px in, while Make works, waits on the person, has just finished or couldn't finish, and its
//  panel is out of sight (MakeBadge.h). A click opens the Make panel on Now. On the desktop's QML surface, with only
//  MakeBadge (its state and open()) on the context.
//  Cost (performance, OPTIMIZE-LIST 35: each change on this surface is a main-thread sync behind the Mac's GL lock):
//  hidden is visible: false; the spinner is a Timer stepping one rotation a second, running only while it shows and
//  the app is in front (no animation, no Animator); the other states are still; no fade (a hide is one change); no
//  shadow (mary's drawing, 21:19 11-07): the chip's 1 px edge and top line set it off the view.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
import QtQuick
import "../editor"

Item {
    id: root
    objectName: "MakeBadge"
    anchors.fill: parent
    readonly property bool shouldReposition: false

    // at the client's quit the context object goes before this item: null, not undefined (performance 23:09 11-07)
    readonly property bool live: typeof MakeBadge !== "undefined" && MakeBadge !== null
    readonly property string badgeState: live ? MakeBadge.state : ""
    readonly property bool answer: badgeState === "answer"
    readonly property bool inHeadset: typeof HMD !== "undefined" && HMD.active   // the VR screen's place is vr's call
    readonly property string words: badgeState === "working" ? "Working"
        : badgeState === "answer" ? "Needs you"
        : badgeState === "done" ? "Done"
        : badgeState === "failed" ? "Couldn't finish" : ""
    visible: words !== "" && !inHeadset

    property int lead: 0   // the spinner's step: its brightest spoke moves on one position a second

    Timer {
        interval: 1000
        repeat: true
        // a test's forced state steps in a background client too (performance's rows; MakeBadge.forced)
        running: root.visible && root.badgeState === "working"
            && (Qt.application.state === Qt.ApplicationActive || (root.live && MakeBadge.forced))
        onTriggered: root.lead = (root.lead + 1) % 12
    }

    Rectangle {
        id: chip
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 12
        anchors.bottomMargin: 12
        width: row.implicitWidth + 18   // 8 px left, 10 px right
        height: 24
        radius: BuildTheme.radius
        color: root.answer ? (mouse.containsMouse ? "#3a72a3" : BuildTheme.selection)
            : mouse.containsMouse ? BuildTheme.hover : Qt.rgba(0x38 / 255, 0x38 / 255, 0x38 / 255, 0.96)
        border.width: 1
        border.color: root.answer ? "#1f4566" : BuildTheme.edge
        Accessible.role: Accessible.Button
        Accessible.name: "Make: " + root.words
        Accessible.description: "Open Make"
        Accessible.onPressAction: MakeBadge.open()

        // the top line, as Build mode's buttons have
        Rectangle {
            x: 1
            y: 1
            width: parent.width - 2
            height: 1
            color: root.answer ? "#3a72a3" : "#404040"
        }

        Row {
            id: row
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 7

            Item {
                width: 12
                height: 12
                anchors.verticalCenter: parent.verticalCenter

                // Working: twelve spokes, the brightest one leading (macOS's spinner, stepped)
                Item {
                    anchors.fill: parent
                    visible: root.badgeState === "working"
                    rotation: root.lead * 30
                    Repeater {
                        model: 12
                        Rectangle {
                            x: 5.25
                            y: 0
                            width: 1.5
                            height: 3.6
                            radius: 0.75
                            color: BuildTheme.text
                            opacity: 0.22 + 0.78 * ((index + 1) / 12)
                            transform: Rotation { origin.x: 0.75; origin.y: 6; angle: index * 30 }
                        }
                    }
                }
                // Needs you: a white dot with a blue "?"
                Rectangle {
                    anchors.fill: parent
                    visible: root.answer
                    radius: 6
                    color: "#ffffff"
                    Text {
                        anchors.centerIn: parent
                        text: "?"
                        textFormat: Text.PlainText
                        color: BuildTheme.selection
                        font.pixelSize: 9
                        font.bold: true
                    }
                }
                // Done: a green tick
                Text {
                    anchors.centerIn: parent
                    visible: root.badgeState === "done"
                    text: "✓"
                    textFormat: Text.PlainText
                    color: BuildTheme.running
                    font.pixelSize: BuildTheme.textSize
                }
                // Couldn't finish: a red dot, until clicked or the panel is seen
                Rectangle {
                    anchors.centerIn: parent
                    visible: root.badgeState === "failed"
                    width: 7
                    height: 7
                    radius: 3.5
                    color: BuildTheme.problem
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Make"
                textFormat: Text.PlainText
                color: root.answer ? "#ffffff" : BuildTheme.text
                font.pixelSize: BuildTheme.textSize
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.words
                textFormat: Text.PlainText
                color: root.answer ? "#cfe0ef" : BuildTheme.mutedText
                font.pixelSize: BuildTheme.textSize
            }
        }

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: MakeBadge.open()
            onContainsMouseChanged: if (containsMouse) tipDelay.restart(); else tipDelay.stop()
        }
        Timer {
            id: tipDelay
            interval: 600
        }
    }

    // the tooltip "Open Make", above the chip after a short hover
    Rectangle {
        anchors.right: chip.right
        anchors.bottom: chip.top
        anchors.bottomMargin: 6
        visible: mouse.containsMouse && !tipDelay.running
        width: tip.implicitWidth + 14
        height: tip.implicitHeight + 6
        radius: 5
        color: Qt.rgba(40 / 255, 40 / 255, 42 / 255, 0.97)
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.13)
        Text {
            id: tip
            anchors.centerIn: parent
            text: "Open Make"
            textFormat: Text.PlainText
            color: "#ececec"
            font.pixelSize: 11
        }
    }
}
