//
//  BuildButton.qml
//  qml/darkride/editor
//
//  Build mode's one button, drawn as the Build toolbar's (Grid, Gizmos): a #505050 fill, a 1 px #232323 edge, 3 px
//  corners, no bevel and no shadow, 23 px tall, 12 px regular text with 9 px a side; hover #5a5a5a, pressed the
//  selection blue (mary's team/mary/brand/make-panel/MAKE-POLISH.png, after Caity's Make try 11-07). Every kind is the
//  same button: the default is the one Return presses and sits on the right, not a colour; a delete has its text in the
//  problem red. Used by the Code and Make panels; editor.qss draws the Inspector's push buttons (Edit, Reload) alike.
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

    implicitWidth: Math.max(text.length > 3 ? 64 : 30, label.implicitWidth + 18)
    implicitHeight: 23

    Rectangle {
        anchors.fill: parent
        radius: BuildTheme.radius
        color: !button.enabled ? "#424242"
            : buttonMouse.pressed || button.down ? BuildTheme.selection
            : buttonMouse.containsMouse ? "#5a5a5a" : "#505050"
        border.width: 1
        // the keyboard's focus: the edge in the selection blue
        border.color: button.activeFocus && button.enabled ? BuildTheme.selection
            : !button.enabled ? BuildTheme.sectionLine : BuildTheme.edge
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
