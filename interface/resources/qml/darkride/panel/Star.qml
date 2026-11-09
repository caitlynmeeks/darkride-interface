//
//  Star.qml
//  qml/darkride/panel
//
//  A favorite's star on a row: ★ when on, ☆ when off, in the tint; a click says toggled(). One click undoes another,
//  so it needs no second press and no red (mary's Avatar redo, 11-08).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.15
import "."

Text {
    id: star

    property bool on: false
    signal toggled()

    text: on ? "★" : "☆"   // ★ ☆
    color: on ? Theme.tint : Theme.tertiaryText
    font.pixelSize: Theme.bodySize

    MouseArea {
        // larger than the glyph, so it is easy to hit
        anchors.fill: parent
        anchors.margins: -8 * Theme.unit
        cursorShape: Qt.PointingHandCursor
        onClicked: star.toggled()
    }
}
