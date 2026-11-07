//
//  StartLine.qml
//  qml/darkride/make
//
//  The Terminal route's start: the line to paste into Terminal to start Claude with Make's tools (MakeDock.startLine,
//  never with "dangerously" in it), and Copy. Make connects by itself when that Claude starts. Its own part so it can
//  go if Darkride learns to start Claude Code in the background on the plan route (scripting's check, 11-07).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick
import QtQuick.Layouts
import "../panel"

Card {
    id: card

    property var dock: null
    property bool copied: false

    spacing: 8 * Theme.unit

    Text {
        Layout.fillWidth: true
        text: "Start your Claude in Terminal"
        textFormat: Text.PlainText
        color: Theme.text
        font.pixelSize: Theme.bodySize
        font.weight: Font.DemiBold
    }
    Text {
        Layout.fillWidth: true
        text: "Open Terminal, paste this line and press Return. Make connects when Claude starts."
        textFormat: Text.PlainText
        color: Theme.secondaryText
        font.pixelSize: Theme.detailSize
        wrapMode: Text.Wrap
    }
    Text {
        Layout.fillWidth: true
        // security's words (11-07)
        text: "Use only the line Make shows: it starts Claude Code with Make's tools only. A line from anywhere else isn't Make's."
        textFormat: Text.PlainText
        color: Theme.secondaryText
        font.pixelSize: Theme.detailSize
        wrapMode: Text.Wrap
    }
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: line.implicitHeight + 14 * Theme.unit
        radius: 6 * Theme.unit
        color: Theme.background
        TextEdit {
            id: line
            x: 8 * Theme.unit
            y: 7 * Theme.unit
            width: parent.width - 16 * Theme.unit
            text: card.dock ? card.dock.startLine : ""
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.WrapAnywhere
            color: Theme.text
            selectionColor: Theme.tint
            selectedTextColor: "#ffffff"
            font.family: "Menlo"
            font.pixelSize: 11 * Theme.unit
            Keys.onEscapePressed: if (card.dock) card.dock.giveBackKeyboard()
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8 * Theme.unit
        MakeButton {
            kind: "primary"
            text: card.copied ? "Copied" : "Copy"
            onClicked: {
                line.selectAll();
                line.copy();
                line.deselect();
                card.copied = true;
                copiedTimer.restart();
            }
        }
        Text {
            Layout.fillWidth: true
            text: "Waiting for Claude…"
            textFormat: Text.PlainText
            color: Theme.tertiaryText
            font.pixelSize: Theme.detailSize
        }
    }
    Timer {
        id: copiedTimer
        interval: 2000
        onTriggered: card.copied = false
    }
}
