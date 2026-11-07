//
//  MadeCard.qml
//  qml/darkride/make
//
//  Something Claude made, in the timeline: a 3D model (its sheet: one picture of four views, front and side above,
//  back and top below, on a 1 m grid beside a 1.75 m figure, under its row), a thing (a box, a light, a zone...) or a
//  script. One row (mary's mock): a 38 px box with its mark or picture, its name, what it is, and its next step: Show me
//  (Build mode, selected and framed:
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
import "../editor"

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

    spacing: 8
    padded: true
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

    // the Made tab's tile: its picture (a model's front view) or its mark across the top
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 96
        visible: card.tile
        radius: BuildTheme.radius
        color: BuildTheme.field
        border.width: 1
        border.color: BuildTheme.fieldEdge
        clip: true
        Text {
            anchors.centerIn: parent
            visible: card.kind !== "model"
            text: card.kind === "script" ? "{ }" : "◇"
            textFormat: Text.PlainText
            color: BuildTheme.accentText
            font.family: card.kind === "script" ? BuildTheme.mono : ""
            font.pixelSize: 22
        }
        Image {
            anchors.fill: parent
            anchors.margins: 1
            visible: card.kind === "model"
            source: card.kind === "model" && card.tile ? card.thumb : ""
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            sourceSize.width: 256
            Accessible.role: Accessible.Graphic
            Accessible.name: card.title
        }
    }

    // one row (mary's mock): a 38 px box with the mark or the model's front view, the name and what it is, the button
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Rectangle {
            visible: !card.tile
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: 38
            implicitHeight: 38
            radius: BuildTheme.radius
            color: BuildTheme.field
            border.width: 1
            border.color: BuildTheme.fieldEdge
            clip: true
            Text {
                anchors.centerIn: parent
                visible: card.kind !== "model"
                text: card.kind === "script" ? "{ }" : "◇"
                textFormat: Text.PlainText
                color: BuildTheme.accentText
                font.family: card.kind === "script" ? BuildTheme.mono : ""
                font.pixelSize: BuildTheme.textSize
            }
            Image {
                anchors.fill: parent
                anchors.margins: 1
                visible: card.kind === "model"
                source: card.kind === "model" && !card.tile ? card.thumb : ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                sourceSize.width: 76
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 1
            Text {
                Layout.fillWidth: true
                text: card.title
                textFormat: Text.PlainText
                color: BuildTheme.text
                font.pixelSize: card.tile ? BuildTheme.noteSize + 1 : BuildTheme.textSize
                elide: Text.ElideMiddle
            }
            Text {
                Layout.fillWidth: true
                text: card.undone ? card.kindName + ", undone"
                    : card.detail !== "" && card.text !== "" ? card.text : card.kindName
                textFormat: Text.PlainText
                color: BuildTheme.mutedText
                font.pixelSize: BuildTheme.noteSize
                elide: Text.ElideRight
                maximumLineCount: 1
            }
        }
        MakeButton {
            visible: !card.undone && !card.tile
            Layout.alignment: Qt.AlignVCenter
            text: card.kind === "script" ? "Open in Code" : "Show me"
            onClicked: card.act()
        }
    }

    // a model's sheet under its row in the timeline (four views: front and side above, back and top below, each named
    // in its corner), at most 240 points a side so the timeline keeps its words in sight
    Rectangle {
        id: sheetBox
        readonly property real side: Math.min(parent ? parent.width : 0, 240)
        Layout.preferredWidth: side
        Layout.preferredHeight: side
        visible: card.kind === "model" && !card.tile
        radius: BuildTheme.radius
        color: BuildTheme.field
        border.width: 1
        border.color: BuildTheme.fieldEdge
        clip: true
        Image {
            anchors.fill: parent
            anchors.margins: 1
            source: card.kind === "model" && !card.tile ? card.sheet : ""
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            sourceSize.width: 512   // 240 points at 2x; the sheet's 1024 px would be 4 MB a card
            Accessible.role: Accessible.Graphic
            Accessible.name: card.title + ": front, side, back and top"
        }
        Repeater {
            model: [{ name: "Front", right: false, bottom: false }, { name: "Side", right: true, bottom: false },
                    { name: "Back", right: false, bottom: true }, { name: "Top", right: true, bottom: true }]
            delegate: Text {
                required property var modelData
                x: modelData.right ? sheetBox.width / 2 + 6 : 6
                y: modelData.bottom ? sheetBox.height / 2 + 4 : 4
                text: modelData.name
                textFormat: Text.PlainText
                color: BuildTheme.mutedText
                font.pixelSize: 10
            }
        }
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
