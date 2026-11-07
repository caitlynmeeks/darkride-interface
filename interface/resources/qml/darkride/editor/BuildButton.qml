//
//  BuildButton.qml
//  qml/darkride/editor
//
//  Build mode's one button, drawn as the Inspector's (Edit, Reload): flat grey, a 1 px bevel, square corners, 23 px,
//  12 px regular text. Every kind is the same button: the default is the one Return presses and sits on the right, not
//  a colour; a delete has its text in the problem red (mary's MAKE-PANEL-LOOK.md). Used by the Code and Make panels.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick
import QtQuick.Controls

Item {
    id: button

    property string text: ""
    property string tip: ""
    property bool down: false          // drawn pressed (a menu that is open)
    property bool destructive: false   // deletes only: the text in red
    signal pressed()

    implicitWidth: Math.max(text.length > 3 ? 64 : 30, label.implicitWidth + 24)
    implicitHeight: 23

    Rectangle {
        anchors.fill: parent
        color: buttonMouse.pressed || button.down ? BuildTheme.hover : BuildTheme.panel
    }
    Rectangle { width: parent.width; height: 1; color: "#404040" }
    Rectangle { width: 1; height: parent.height - 1; color: "#404040" }
    Rectangle { x: parent.width - 1; width: 1; height: parent.height - 1; color: "#252525" }
    Rectangle { y: parent.height - 2; width: parent.width; height: 1; color: "#252525" }
    Rectangle { y: parent.height - 1; width: parent.width; height: 1; color: "#131313" }
    Rectangle {   // keyboard focus
        anchors.fill: parent
        visible: button.activeFocus
        color: "transparent"
        border.width: 1
        border.color: BuildTheme.selection
    }
    Text {
        id: label
        anchors.centerIn: parent
        text: button.text
        textFormat: Text.PlainText
        color: !button.enabled ? BuildTheme.dimText : button.destructive ? BuildTheme.problem : BuildTheme.text
        font.pixelSize: BuildTheme.textSize
    }
    MouseArea {
        id: buttonMouse
        anchors.fill: parent
        enabled: button.enabled
        hoverEnabled: true
        onClicked: button.pressed()
        ToolTip.visible: containsMouse && button.tip !== ""
        ToolTip.text: button.tip
        ToolTip.delay: 600
    }
}
