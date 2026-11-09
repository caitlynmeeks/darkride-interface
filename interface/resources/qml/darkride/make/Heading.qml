//
//  Heading.qml
//  qml/darkride/make
//
//  A section heading on Make's set-up page, drawn as the Inspector's section header: a #3c3c3c bar with a 1 px line on
//  top, 12 px bold, sentence case (mary's MAKE-PANEL-LOOK.md). "accent" is a small heading inside a card ("Claude
//  asks"): no bar, the selection blue.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick
import QtQuick.Layouts
import "../editor"

Rectangle {
    id: heading

    property alias text: label.text
    property bool accent: false

    Layout.fillWidth: true
    implicitHeight: label.implicitHeight + (accent ? 0 : 8)
    color: accent ? "transparent" : BuildTheme.sectionBar
    Rectangle { visible: !heading.accent; width: parent.width; height: 1; color: BuildTheme.sectionLine }

    Text {
        id: label
        x: heading.accent ? 0 : 6
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width - 2 * x
        textFormat: Text.PlainText
        color: heading.accent ? BuildTheme.accentText : BuildTheme.text
        font.pixelSize: BuildTheme.textSize
        font.bold: true
        elide: Text.ElideRight
    }
}
