//
//  Card.qml
//  qml/darkride/make
//
//  A rounded card on the dock's background, in the theme's card colour or a skin's card frame; its content goes in a
//  column with the card's padding. Make's own, not the panels' Section: Section carries the rich-text rights line,
//  and every text in the Make dock is plain (security, 11-06).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick
import QtQuick.Layouts
import "../panel"

Rectangle {
    id: card

    property bool padded: true
    property bool highlighted: false   // a question waiting for an answer
    property real spacing: 8 * Theme.unit
    default property alias content: column.data

    implicitHeight: column.implicitHeight + (padded ? 2 * Theme.cardPadding * 0.85 : 0)
    radius: Theme.cardRadius
    color: cardFrame.shown ? "transparent" : Theme.card
    border.width: highlighted ? 1 : 0
    border.color: Theme.tint

    SkinFrame {
        id: cardFrame
        part: "card"
        anchors.fill: parent
    }

    ColumnLayout {
        id: column
        x: card.padded ? Theme.cardPadding * 0.85 : 0
        y: card.padded ? Theme.cardPadding * 0.85 : 0
        width: card.width - 2 * x
        spacing: card.spacing
    }
}
