//
//  MadeCard.qml
//  qml/darkride/make
//
//  Something Claude made, in the timeline: a 3D model (its sheet: one picture of four views, front and side above,
//  back and top below, on a 1 m grid beside a 1.75 m figure), a thing (a box, a light, a zone...) or a script. Its name, what it is, and its next step: Show me (Build mode, selected and framed:
//  MakeDock.showThing) or, for a script, Open in Code (MakeDock.openInCode). A request's Undo is on its end line;
//  an undone card fades and says so. In "tile" form it's the Made tab's square.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../panel"

Card {
    id: card

    property var dock: null
    property string kind: "thing"   // "thing", "script" or "model"
    property string itemId: ""
    property string text: ""        // what it is ("Entity script on Front door")
    property string detail: ""      // its file's name, for a script or a model
    property string sheet: ""       // a model's sheet of four views, MakeDock's image:// address
    property string thumb: ""       // the Made tab's picture: the front view alone, cropped by MakeDock
    property bool undone: false
    property bool tile: false       // the Made tab's form: smaller, one picture or a mark

    readonly property string title: detail !== "" ? detail : text
    readonly property string kindName: kind === "model" ? "3D model" : kind === "script" ? "Script" : "Thing"

    spacing: 8 * Theme.unit
    opacity: undone ? 0.55 : 1

    function act() {
        if (!dock || undone) {
            return;
        }
        if (kind === "script") {
            dock.openInCode(itemId);
        } else {
            dock.showThing(itemId);
        }
    }

    // a model's sheet, square, at most 240 points a side so the timeline keeps its words in sight, each view named in
    // its corner; a tile shows the front alone, across the tile
    Rectangle {
        id: sheetBox
        readonly property real side: Math.min(parent ? parent.width : 0, 240 * Theme.unit)
        Layout.fillWidth: card.tile
        Layout.preferredWidth: card.tile ? -1 : side
        Layout.preferredHeight: card.tile ? 96 * Theme.unit : side
        visible: card.kind === "model" || card.tile   // in a tile, a script or a thing gets the same box, with its mark
        radius: 8 * Theme.unit
        color: Theme.background
        clip: true
        Text {
            anchors.centerIn: parent
            visible: card.kind !== "model"
            text: card.kind === "script" ? "{ }" : "◇"
            textFormat: Text.PlainText
            color: Theme.tint
            font.family: card.kind === "script" ? "Menlo" : ""
            font.pixelSize: 26 * Theme.unit
        }
        Image {
            anchors.fill: parent
            visible: card.kind === "model"
            source: card.kind !== "model" ? "" : card.tile ? card.thumb : card.sheet
            fillMode: card.tile ? Image.PreserveAspectCrop : Image.PreserveAspectFit
            asynchronous: true
            sourceSize.width: card.tile ? 256 : 512   // 240 points at 2x; the sheet's 1024 px would be 4 MB a card
            Accessible.role: Accessible.Graphic
            Accessible.name: card.tile ? card.title : card.title + ": front, side, back and top"
        }
        Repeater {
            model: card.tile ? [] : [{ name: "Front", right: false, bottom: false }, { name: "Side", right: true, bottom: false },
                                    { name: "Back", right: false, bottom: true }, { name: "Top", right: true, bottom: true }]
            delegate: Text {
                required property var modelData
                x: modelData.right ? sheetBox.width / 2 + 6 * Theme.unit : 6 * Theme.unit
                y: modelData.bottom ? sheetBox.height / 2 + 4 * Theme.unit : 4 * Theme.unit
                text: modelData.name
                textFormat: Text.PlainText
                color: Theme.secondaryText
                font.pixelSize: 10 * Theme.unit
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 10 * Theme.unit
        // a mark for a script or a thing
        Text {
            visible: card.kind !== "model" && !card.tile
            Layout.alignment: Qt.AlignTop
            text: card.kind === "script" ? "{ }" : "◇"
            textFormat: Text.PlainText
            color: Theme.tint
            font.family: card.kind === "script" ? "Menlo" : ""
            font.pixelSize: Theme.bodySize
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2 * Theme.unit
            Text {
                Layout.fillWidth: true
                text: card.title
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: card.tile ? Theme.detailSize + 1 * Theme.unit : Theme.bodySize
                font.weight: Font.DemiBold
                elide: Text.ElideMiddle
            }
            Text {
                Layout.fillWidth: true
                text: card.undone ? card.kindName + ", undone"
                    : card.detail !== "" && card.text !== "" ? card.text : card.kindName
                textFormat: Text.PlainText
                color: Theme.secondaryText
                font.pixelSize: Theme.detailSize
                wrapMode: card.tile ? Text.NoWrap : Text.Wrap
                elide: Text.ElideRight
                maximumLineCount: card.tile ? 1 : 3
            }
        }
    }

    MakeButton {
        visible: !card.undone && !card.tile
        text: card.kind === "script" ? "Open in Code" : "Show me"
        onClicked: card.act()
    }

    // a tile is one button as a whole
    MouseArea {
        parent: card
        anchors.fill: parent
        enabled: card.tile && !card.undone
        visible: enabled
        cursorShape: Qt.PointingHandCursor
        onClicked: card.act()
    }
    Accessible.role: tile ? Accessible.Button : Accessible.Grouping
    Accessible.name: title
}
