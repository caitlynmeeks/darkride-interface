//
//  MakeButton.qml
//  qml/darkride/make
//
//  The Make dock's button: "primary" (the tint, for Allow and Save), "plain" (the fill, for most actions) or "stop"
//  (an outline in the destructive colour). A click or Space/Return while it has focus is real input in the dock's own
//  window, which is what MakeDock's real-input check looks for (MAKE-DOCK-CONTRACT.md); the button itself only
//  emits clicked.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick
import "../panel"

Rectangle {
    id: button

    property string text: ""
    property string kind: "plain"   // "primary", "plain" or "stop"
    property bool wide: false       // fills its row's width (the Allow and Don't Allow pair)
    signal clicked()

    implicitWidth: label.implicitWidth + 24 * Theme.unit
    implicitHeight: 30 * Theme.unit
    radius: 8 * Theme.unit
    color: kind === "primary" ? Theme.tint : kind === "stop" ? "transparent" : (mouse.pressed ? Theme.cardPressed : Theme.fill)
    border.width: kind === "stop" || activeFocus ? 1 : 0
    border.color: activeFocus ? Theme.tint : Theme.destructive
    opacity: !enabled ? 0.4 : (kind === "primary" && mouse.pressed ? 0.8 : 1)

    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.onPressAction: if (enabled) button.clicked()
    Keys.onSpacePressed: if (enabled) button.clicked()
    Keys.onReturnPressed: if (enabled) button.clicked()

    Text {
        id: label
        anchors.centerIn: parent
        text: button.text
        textFormat: Text.PlainText
        color: button.kind === "primary" ? "#ffffff" : button.kind === "stop" ? Theme.destructive : Theme.text
        font.pixelSize: Theme.controlTextSize
        font.weight: button.kind === "primary" ? Font.DemiBold : Font.Medium
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        enabled: button.enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }
}
