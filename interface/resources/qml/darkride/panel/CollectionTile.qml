//
//  CollectionTile.qml
//  qml/darkride/panel
//
//  One thing as a tile in a CollectionView's Tiles: its picture across the top, its title, a line under it (rich text:
//  it may carry a link, linkActivated), then the page's own controls in a row at the bottom (content), and a status
//  line when there is one. A click anywhere else says clicked(); checked draws a tick in the picture's corner.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.15
import QtQuick.Layouts 1.15
import "."

Rectangle {
    id: tile

    property string title: ""
    property string subtitle: ""        // rich text
    property url picture: ""
    property string placeholder: title
    property real pictureRatio: 0.75
    property bool dim: false            // can't be used now (another version of Darkride)
    property bool checked: false
    property string badge: ""           // a word over the picture's corner ("private")
    property string status: ""
    default property alias content: controls.data

    signal clicked()
    signal linkActivated(string link)

    implicitHeight: column.implicitHeight
    radius: Theme.cardRadius
    color: area.pressed ? Theme.cardPressed : Theme.card

    MouseArea {
        id: area
        anchors.fill: parent
        onClicked: tile.clicked()
    }

    Column {
        id: column
        width: parent.width

        RoundedImage {
            width: parent.width
            height: width * tile.pictureRatio
            roundBottom: false
            placeholder: tile.placeholder
            source: tile.picture
            opacity: tile.dim ? Theme.dimmed : 1

            Rectangle {
                visible: tile.badge !== ""
                x: 8 * Theme.unit
                y: 8 * Theme.unit
                width: badgeText.implicitWidth + 12 * Theme.unit
                height: badgeText.implicitHeight + 4 * Theme.unit
                radius: height / 2
                color: "#99000000"
                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: tile.badge
                    textFormat: Text.PlainText
                    color: "#ffffff"
                    font.pixelSize: Theme.detailSize
                }
            }
            Rectangle {
                visible: tile.checked
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 8 * Theme.unit
                width: 22 * Theme.unit
                height: width
                radius: width / 2
                color: Theme.tint
                Text {
                    anchors.centerIn: parent
                    text: "✓"
                    color: "#ffffff"
                    font.pixelSize: 13 * Theme.unit
                    font.bold: true
                }
            }
        }

        Column {
            x: 10 * Theme.unit
            width: parent.width - 2 * x
            topPadding: 8 * Theme.unit
            bottomPadding: 10 * Theme.unit
            spacing: 2 * Theme.unit

            Text {
                width: parent.width
                text: tile.title
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.bodySize
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                visible: tile.subtitle !== ""
                width: parent.width
                text: tile.subtitle
                textFormat: Text.RichText
                color: Theme.secondaryText
                font.pixelSize: Theme.detailSize
                elide: Text.ElideRight
                onLinkActivated: tile.linkActivated(link)
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.NoButton
                    cursorShape: parent.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor
                }
            }
            Item {
                visible: controls.children.length > 0
                width: parent.width
                height: 6 * Theme.unit
            }
            RowLayout {
                id: controls
                visible: children.length > 0
                width: parent.width
                spacing: 8 * Theme.unit
            }
            Text {
                visible: tile.status !== ""
                width: parent.width
                topPadding: 4 * Theme.unit
                text: tile.status
                textFormat: Text.PlainText
                color: Theme.secondaryText
                font.pixelSize: Theme.detailSize
                wrapMode: Text.WordWrap
            }
        }
    }
}
