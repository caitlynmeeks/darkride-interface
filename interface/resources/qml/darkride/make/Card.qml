//
//  Card.qml
//  qml/darkride/make
//
//  A box on the dock's panel, drawn as the Inspector's User Data box (the panel's grey, a 1 px #2b2b2b edge, 3 px
//  corners); its content goes in a column with the box's padding. Make's own, not the panels' Section: Section carries the rich-text rights line,
//  and every text in the Make dock is plain (security, 11-06).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick
import QtQuick.Layouts
import "../editor"

Rectangle {
    id: card

    property bool padded: true
    property bool highlighted: false   // a question waiting for an answer: the selection blue edge
    property real spacing: 8
    default property alias content: column.data

    // a box on the panel, as the Inspector's User Data box (mary's MAKE-PANEL-LOOK.md), not a raised card
    implicitHeight: column.implicitHeight + (padded ? 2 * BuildTheme.padding : 0)
    radius: BuildTheme.radius
    color: BuildTheme.panel
    border.width: 1
    border.color: highlighted ? BuildTheme.selection : BuildTheme.sectionLine

    ColumnLayout {
        id: column
        x: card.padded ? BuildTheme.padding : 0
        y: card.padded ? BuildTheme.padding : 0
        width: card.width - 2 * x
        spacing: card.spacing
    }
}
