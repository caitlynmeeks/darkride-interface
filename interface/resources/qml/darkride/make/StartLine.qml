//
//  StartLine.qml
//  qml/darkride/make
//
//  The Terminal route's start: the line to paste into Terminal to start Claude with Make's tools (MakeDock.startLine,
//  never with "dangerously" in it), Copy, and Cancel (the way back: Disconnect). Make connects by itself when that
//  Claude starts. Flat on the panel under the header, the line in a field (mary's MAKE-PANEL-LOOK.md).
//
//  Copy puts the whole line on the clipboard through MakeDock.copyLine("start") (C++, the clipboard path the Assets
//  panel's Copy uses; never a part, which could leave out --restricted: security 11-07), not through the text item.
//  The line can be selected by dragging (a MouseArea selects, so a list that scrolls can't take the drag); Copy on the
//  keyboard there copies the whole line the same way.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick
import QtQuick.Layouts
import "../editor"

ColumnLayout {
    id: card

    property var dock: null
    property bool copied: false

    spacing: 8

    function copyLine() {
        return !!card.dock && card.dock.copyLine("start") === true;
    }

    Text {
        Layout.fillWidth: true
        text: "Paste this line in Terminal to start Claude."
        textFormat: Text.PlainText
        color: BuildTheme.text
        font.pixelSize: BuildTheme.textSize
        wrapMode: Text.Wrap
    }
    Text {
        Layout.fillWidth: true
        // security's words (11-07)
        text: "Use only the line Make shows: it starts Claude Code with Make's tools only. A line from anywhere else isn't Make's."
        textFormat: Text.PlainText
        color: BuildTheme.mutedText
        font.pixelSize: BuildTheme.noteSize
        wrapMode: Text.Wrap
    }
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: line.implicitHeight + 12
        radius: BuildTheme.radius
        color: BuildTheme.field
        border.width: 1
        border.color: line.activeFocus ? BuildTheme.selection : BuildTheme.fieldEdge
        TextEdit {
            id: line
            objectName: "makeStartLine"   // a test finds it by name
            x: 7
            y: 6
            width: parent.width - 14
            text: card.dock ? card.dock.startLine : ""
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: false        // the MouseArea below selects
            selectByKeyboard: true
            persistentSelection: true
            wrapMode: TextEdit.WrapAnywhere
            color: BuildTheme.text
            selectionColor: BuildTheme.selection
            selectedTextColor: "#ffffff"
            font.family: BuildTheme.mono
            font.pixelSize: BuildTheme.noteSize
            // Copy and Select All here, not the app's menu (Build mode's Copy copies entities)
            Keys.onShortcutOverride: function (event) {
                if (event.matches(StandardKey.Copy) || event.matches(StandardKey.SelectAll)) {
                    event.accepted = true;
                }
            }
            Keys.onPressed: function (event) {
                if (event.matches(StandardKey.Copy)) {
                    card.copyLine();
                    event.accepted = true;
                } else if (event.matches(StandardKey.SelectAll)) {
                    line.selectAll();
                    event.accepted = true;
                }
            }
            Keys.onEscapePressed: if (card.dock) card.dock.giveBackKeyboard()

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.IBeamCursor
                preventStealing: true   // a drag here selects; the list around it doesn't scroll
                property int from: 0
                onPressed: function (mouse) {
                    line.forceActiveFocus();
                    from = line.positionAt(mouse.x, mouse.y);
                    line.select(from, from);
                }
                onPositionChanged: function (mouse) {
                    if (pressed) {
                        line.select(from, line.positionAt(mouse.x, mouse.y));
                    }
                }
                onDoubleClicked: function (mouse) {
                    line.cursorPosition = line.positionAt(mouse.x, mouse.y);
                    line.selectWord();
                }
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        MakeButton {
            text: "Cancel"
            tip: "Stop waiting for Claude"
            onClicked: if (card.dock) card.dock.disconnect()
        }
        Item { Layout.fillWidth: true }
        MakeButton {
            objectName: "makeCopy"   // a test finds it by name
            kind: "primary"
            text: card.copied ? "Copied" : "Copy"
            onClicked: {
                if (card.copyLine()) {
                    card.copied = true;
                    copiedTimer.restart();
                }
            }
        }
    }
    Timer {
        id: copiedTimer
        interval: 2000
        onTriggered: card.copied = false
    }
}
