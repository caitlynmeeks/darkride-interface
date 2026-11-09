//
//  BuildToggle.qml
//  qml/darkride/editor
//
//  A toggle drawn as the Build bar's Grid and Gizmos: no fill, a 1 px transparent edge, 3 px corners; hover #474747;
//  the chosen one #505050 with a #232323 edge (mary's MAKE-PANEL-LOOK.md). Two side by side choose one of two (the
//  Make panel's Now and Made).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick

Rectangle {
    id: toggle

    property string text: ""
    property bool checked: false
    signal toggled()

    implicitWidth: label.implicitWidth + 18
    implicitHeight: label.implicitHeight + 6
    radius: BuildTheme.radius
    color: checked ? "#505050" : mouse.containsMouse ? BuildTheme.hover : "transparent"
    border.width: 1
    border.color: checked ? BuildTheme.edge : activeFocus ? BuildTheme.selection : "transparent"

    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.checkable: true
    Accessible.checked: checked
    Accessible.onPressAction: toggle.toggled()
    Keys.onSpacePressed: toggle.toggled()

    Text {
        id: label
        anchors.centerIn: parent
        text: toggle.text
        textFormat: Text.PlainText
        color: BuildTheme.text
        font.pixelSize: BuildTheme.textSize
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: toggle.toggled()
    }
}
