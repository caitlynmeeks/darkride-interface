//
//  MadeView.qml
//  qml/darkride/make
//
//  The Made tab: what Claude made this session (MakeDock.made, newest first: its models, things and scripts), as
//  tiles two or three to a row. A tile shows the thing (Show me) or opens the script (Open in Code).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import "../panel"
import "format.js" as Format

Item {
    id: view

    property var dock: null

    Text {
        anchors.centerIn: parent
        width: parent.width - 36 * Theme.unit
        visible: grid.count === 0
        text: "Nothing made yet. What Claude makes for you shows here."
        textFormat: Text.PlainText
        color: Theme.secondaryText
        font.pixelSize: Theme.bodySize
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
    }

    GridView {
        id: grid

        readonly property int columns: Math.max(2, Math.floor(width / (150 * Theme.unit)))   // width is inside the margins

        anchors.fill: parent
        anchors.margins: 14 * Theme.unit
        clip: true
        model: view.visible && view.dock ? view.dock.made : null   // nothing made or loaded while the tab is hidden
        cellWidth: Math.floor(width / columns)
        cellHeight: 172 * Theme.unit
        boundsBehavior: Flickable.StopAtBounds
        QQC2.ScrollBar.vertical: QQC2.ScrollBar {}

        delegate: Item {
            required property var model
            width: grid.cellWidth
            height: grid.cellHeight

            MadeCard {
                anchors.fill: parent
                anchors.margins: 4 * Theme.unit
                tile: true
                dock: view.dock
                kind: parent.model.kind || "thing"
                itemId: parent.model.itemId || ""
                text: parent.model.text || ""
                detail: parent.model.detail || ""
                thumb: Format.picture(parent.model.thumb)
                undone: !!parent.model.undone
            }
        }
    }
}
