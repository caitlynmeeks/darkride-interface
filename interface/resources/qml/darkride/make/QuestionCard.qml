//
//  QuestionCard.qml
//  qml/darkride/make
//
//  Claude asks before something you didn't ask for by name (askPerson): "CLAUDE ASKS", the question, exactly which
//  things or addresses, and Don't Allow / Allow. The buttons take clicks only once the card has been on screen for
//  0.6 s, and again whenever it moves (a scroll, a new row above it) or its window comes to the front, so a click
//  meant for something else can't land on a card that just slid in under the pointer, or on the click that brought
//  Darkride forward (macOS's own prompts wait the same way; security and editor-ui, 11-07). A press held
//  while it re-arms is cancelled, not clicked (the buttons' MouseArea turns off under it). Allow never takes the first
//  keyboard focus: Return and Space work on it only once the person tabs there. Both go quiet the moment one is pressed; the answer then shows in their place
//  ("Allowed", "Not allowed", "No answer in time": MakeLink refuses after 100 s). MakeDock's answer() takes only real
//  input in this window; if it refuses, the buttons come back.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import "../panel"

Card {
    id: card

    property var dock: null
    property string askId: ""
    property string text: ""
    property string exactly: ""
    property string answered: ""   // "", "allowed", "refused" or "timed out"
    property bool pressed: false   // a button was pressed; waiting for MakeDock's answered
    property bool armed: false     // on screen, unmoved, for 0.6 s

    readonly property bool open: answered === "" && !pressed

    highlighted: open
    spacing: 8 * Theme.unit

    function answer(allow) {
        if (!open || !dock) {
            return;
        }
        pressed = true;
        if (!dock.answer(askId, allow)) {
            pressed = false;   // refused (not real input, or the link said no): the buttons come back
        }
    }

    // the window isn't in front: the click that brings it there is meant for something else (editor-ui review 2)
    readonly property bool windowActive: Window.active

    // the card moved in the view (its list scrolled, or a row above it changed), or the window came and went: wait
    // the whole 0.6 s again, from now (a countdown already running starts over)
    function moved() {
        armed = false;
        if (armTimer.running) {
            armTimer.restart();
        }
    }

    onVisibleChanged: if (!visible) armed = false
    onWindowActiveChanged: moved()
    Timer {
        id: armTimer
        interval: 600
        running: card.visible && card.windowActive && card.open && !card.armed
        onTriggered: card.armed = true
    }

    Heading {
        text: "Claude asks"
        accent: card.open
    }
    Text {
        Layout.fillWidth: true
        text: card.text
        textFormat: Text.PlainText
        color: Theme.text
        font.pixelSize: Theme.bodySize
        font.weight: Font.DemiBold
        wrapMode: Text.Wrap
        lineHeight: 1.15
    }
    // exactly which things or addresses: never shortened, so what's allowed is what's read
    Rectangle {
        Layout.fillWidth: true
        visible: card.exactly !== ""
        implicitHeight: exactlyText.implicitHeight + 12 * Theme.unit
        radius: 6 * Theme.unit
        color: Theme.background
        TextEdit {
            id: exactlyText
            x: 8 * Theme.unit
            y: 6 * Theme.unit
            width: parent.width - 16 * Theme.unit
            text: card.exactly
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.WrapAnywhere
            color: Theme.secondaryText
            selectionColor: Theme.tint
            font.family: "Menlo"
            font.pixelSize: 11 * Theme.unit
            Keys.onEscapePressed: if (card.dock) card.dock.giveBackKeyboard()
        }
    }
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 2 * Theme.unit
        visible: card.answered === ""
        spacing: 8 * Theme.unit
        MakeButton {
            Layout.fillWidth: true
            Layout.preferredWidth: 1   // the pair splits the row in equal halves
            text: "Don't Allow"
            enabled: card.open && card.armed
            onClicked: card.answer(false)
        }
        MakeButton {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            kind: "primary"
            text: "Allow"
            enabled: card.open && card.armed
            onClicked: card.answer(true)
        }
    }
    Text {
        Layout.fillWidth: true
        visible: card.answered !== ""
        text: card.answered === "allowed" ? "Allowed" : card.answered === "refused" ? "Not allowed" : "No answer in time, so not allowed"
        textFormat: Text.PlainText
        color: card.answered === "allowed" ? Theme.on : Theme.secondaryText
        font.pixelSize: Theme.detailSize
    }
}
