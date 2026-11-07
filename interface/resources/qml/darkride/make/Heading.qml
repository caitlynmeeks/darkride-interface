//
//  Heading.qml
//  qml/darkride/make
//
//  A small capitals heading over a card ("ON THIS MAC"), or inside one ("CLAUDE ASKS"), as the panels' sections do.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick
import QtQuick.Layouts
import "../panel"

Text {
    property bool accent: false

    Layout.fillWidth: true
    textFormat: Text.PlainText
    color: accent ? Theme.tint : Theme.secondaryText
    font.pixelSize: Theme.sectionSize
    font.weight: Font.DemiBold
    font.letterSpacing: 0.4 * Theme.unit
    font.capitalization: Font.AllUppercase
    elide: Text.ElideRight
}
