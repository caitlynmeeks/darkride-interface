//
//  NowView.qml
//  qml/darkride/make
//
//  Make's timeline, newest at the bottom: what you asked (with your pictures), Claude's words, its steps (a tick when
//  done, a turning mark on the one it's doing), its questions with Allow and Don't Allow, what it made as cards, and
//  each request's end line with its time, cost and Undo. A finished request folds to one line; a done request's
//  steps fold to "6 steps · 3 min". It follows the newest row unless the person has scrolled up.
//
//  Before anything is asked: a welcome with examples (your API key route), or the line to start Claude in Terminal
//  (the Terminal route, while Make waits for it). Pictures dropped here go with the next request: MakeDock checks the
//  drop was real, reads and cleans the files itself, and answers with picturesAdded (the composer shows them).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import "../panel"
import "format.js" as Format

Item {
    id: view

    property var dock: null
    signal exampleChosen(string text)

    readonly property bool terminalRoute: dock ? dock.route === "terminal" : false
    readonly property bool empty: list.count === 0

    Requests {
        id: requests
        model: view.dock ? view.dock.timeline : null
    }

    // before anything: the Terminal route's start line while Make waits for Claude, else a welcome
    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 18 * Theme.unit
        anchors.rightMargin: 18 * Theme.unit
        visible: view.empty
        spacing: 12 * Theme.unit

        StartLine {
            Layout.fillWidth: true
            visible: view.terminalRoute && !!view.dock && view.dock.state === "listening"
            dock: view.dock
        }

        Text {
            Layout.fillWidth: true
            visible: !view.terminalRoute
            text: "Say what you want, in your own words."
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.bodySize
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
        Repeater {
            model: view.terminalRoute ? [] : ["a neon cafe sign that lights up at night",
                "make the door open when I knock three times", "a little robot duck I can wear"]
            delegate: Rectangle {
                required property string modelData
                Layout.alignment: Qt.AlignHCenter
                Layout.maximumWidth: view.width - 36 * Theme.unit
                implicitWidth: example.implicitWidth + 24 * Theme.unit
                implicitHeight: example.implicitHeight + 12 * Theme.unit
                radius: height / 2
                color: exampleMouse.pressed ? Theme.cardPressed : Theme.card
                Accessible.role: Accessible.Button
                Accessible.name: modelData
                Text {
                    id: example
                    anchors.centerIn: parent
                    width: Math.min(implicitWidth, parent.Layout.maximumWidth - 24 * Theme.unit)
                    text: "“" + parent.modelData + "”"
                    textFormat: Text.PlainText
                    color: Theme.secondaryText
                    font.pixelSize: Theme.detailSize
                    elide: Text.ElideRight
                }
                MouseArea {
                    id: exampleMouse
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: view.exampleChosen(parent.modelData)
                }
            }
        }
        Text {
            Layout.fillWidth: true
            text: view.terminalRoute ? "Ask Claude in Terminal. What it does shows here, with Stop and Undo."
                : "Claude makes it in 3D, places it, makes it work and tests it."
            textFormat: Text.PlainText
            color: Theme.tertiaryText
            font.pixelSize: Theme.detailSize
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.topMargin: 10 * Theme.unit
        clip: true
        model: view.dock ? view.dock.timeline : null
        boundsBehavior: Flickable.StopAtBounds
        QQC2.ScrollBar.vertical: QQC2.ScrollBar {
            onPressedChanged: if (!pressed) list.stickToEnd = list.atYEnd
        }

        // follow the newest row, unless the person scrolled up to read (a drag, a flick, the wheel or the scroll bar)
        property bool stickToEnd: true
        onMovementEnded: stickToEnd = atYEnd
        onContentHeightChanged: if (stickToEnd) Qt.callLater(toEnd)
        function toEnd() { positionViewAtEnd(); }

        // the Terminal route waiting for Claude after earlier work in this list: the start line below the newest row,
        // so a Connect again always shows the line to paste (the empty list shows it in the middle, above)
        footer: Item {
            width: list.width
            readonly property bool shown: view.terminalRoute && !view.empty && !!view.dock
                && view.dock.state === "listening"
            height: shown ? footerLine.implicitHeight + 24 * Theme.unit : 0
            visible: shown
            StartLine {
                id: footerLine
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 12 * Theme.unit
                anchors.leftMargin: 14 * Theme.unit
                anchors.rightMargin: 14 * Theme.unit
                dock: view.dock
            }
        }

        delegate: Item {
            id: row

            required property int index
            required property var model

            readonly property string kind: model.kind || ""
            readonly property int request: model.request || 0
            readonly property string mode: requests.revision >= 0 ? requests.mode(index) : "row"
            readonly property var info: requests.revision >= 0 ? requests.info(request) : null
            readonly property real gap: (kind === "activity" && mode === "row" ? 5 : 12) * Theme.unit

            width: ListView.view.width
            height: mode === "hidden" ? 0 : loader.implicitHeight + gap
            visible: mode !== "hidden"

            Loader {
                id: loader
                x: 14 * Theme.unit
                width: parent.width - 28 * Theme.unit
                active: row.mode !== "hidden"
                sourceComponent: row.mode === "fold" ? foldLine
                    : row.mode === "steps" ? stepsLine
                    : row.kind === "ask" ? askBubble
                    : row.kind === "reply" ? replyBubble
                    : row.kind === "activity" ? activityLine
                    : row.kind === "question" ? question
                    : row.kind === "done" ? doneLine
                    : (row.kind === "thing" || row.kind === "script" || row.kind === "model") ? madeCard
                    : null
            }

            // a folded request: "▸ what was asked · Done · 2 min · $0.52"
            Component {
                id: foldLine
                Rectangle {
                    implicitHeight: foldRow.implicitHeight + 18 * Theme.unit
                    radius: 10 * Theme.unit
                    color: foldMouse.pressed ? Theme.cardPressed : Theme.card
                    opacity: !!row.info && row.info.undone ? 0.6 : 1
                    Accessible.role: Accessible.Button
                    Accessible.name: foldTitle.text
                    RowLayout {
                        id: foldRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 12 * Theme.unit
                        anchors.rightMargin: 12 * Theme.unit
                        spacing: 8 * Theme.unit
                        Text {
                            text: "▸"
                            textFormat: Text.PlainText
                            color: Theme.secondaryText
                            font.pixelSize: Theme.bodySize
                        }
                        Text {
                            id: foldTitle
                            Layout.fillWidth: true
                            text: row.info ? (row.info.title || row.info.firstText) : ""
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: Theme.bodySize
                            elide: Text.ElideRight
                            maximumLineCount: 1
                        }
                        Text {
                            text: !row.info ? ""
                                : row.info.undone ? "Undone"
                                : [row.info.done ? "Done" : "Working",
                                   row.info.done && row.info.endTime > 0 ? Format.duration(row.info.endTime - row.info.startTime) : "",
                                   Format.cost(row.info.cost)].filter(function (part) { return part !== ""; }).join(" · ")
                            textFormat: Text.PlainText
                            color: Theme.secondaryText
                            font.pixelSize: Theme.detailSize
                        }
                    }
                    MouseArea {
                        id: foldMouse
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: requests.toggleFold(row.request)
                    }
                }
            }

            // a request's steps, folded: "▸ 6 steps · 3 min", or "▸ 4 earlier steps" while it runs
            Component {
                id: stepsLine
                Item {
                    implicitHeight: stepsText.implicitHeight + 4 * Theme.unit
                    Text {
                        id: stepsText
                        width: parent.width
                        text: {
                            if (!row.info) {
                                return "";
                            }
                            var steps = row.info.activities.length;
                            if (row.info.done) {
                                var parts = ["▸  " + steps + (steps === 1 ? " step" : " steps")];
                                if (row.info.endTime > 0) {
                                    parts.push(Format.duration(row.info.endTime - row.info.startTime));
                                }
                                return parts.join(" · ");
                            }
                            var earlier = steps - requests.recentSteps;
                            return "▸  " + earlier + (earlier === 1 ? " earlier step" : " earlier steps");
                        }
                        textFormat: Text.PlainText
                        color: Theme.secondaryText
                        font.pixelSize: Theme.detailSize
                        leftPadding: 2 * Theme.unit
                        Accessible.role: Accessible.Button
                        Accessible.name: text
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: requests.toggleSteps(row.request)
                    }
                }
            }

            // what the person asked, on the right, with their pictures above the words
            Component {
                id: askBubble
                Item {
                    id: ask
                    readonly property var views: Format.urls(row.model.views)
                    readonly property int shown: Math.min(views.length, 3)
                    readonly property real picturesWidth: shown > 0 ? shown * 96 * Theme.unit + (shown - 1) * 6 * Theme.unit : 0
                    implicitHeight: bubble.height
                    Rectangle {
                        id: bubble
                        anchors.right: parent.right
                        width: Math.min(parent.width * 0.82, Math.max(askText.implicitWidth, ask.picturesWidth) + 24 * Theme.unit)
                        height: askColumn.implicitHeight + 18 * Theme.unit
                        radius: 14 * Theme.unit
                        color: Theme.fill
                        ColumnLayout {
                            id: askColumn
                            x: 12 * Theme.unit
                            y: 9 * Theme.unit
                            width: parent.width - 24 * Theme.unit
                            spacing: 8 * Theme.unit
                            Flow {
                                id: pictures
                                Layout.fillWidth: true
                                visible: ask.views.length > 0
                                spacing: 6 * Theme.unit
                                Repeater {
                                    model: ask.views
                                    delegate: Image {
                                        required property string modelData
                                        width: 96 * Theme.unit
                                        height: 72 * Theme.unit
                                        source: modelData
                                        fillMode: Image.PreserveAspectCrop
                                        asynchronous: true
                                        sourceSize.width: 192
                                        Accessible.role: Accessible.Graphic
                                        Accessible.name: "Your picture"
                                    }
                                }
                            }
                            Text {
                                id: askText
                                Layout.fillWidth: true
                                text: row.model.text || ""
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: Theme.bodySize
                                wrapMode: Text.Wrap
                                lineHeight: 1.15
                            }
                        }
                    }
                }
            }

            // Claude's words, on the left; selectable to copy
            Component {
                id: replyBubble
                Item {
                    implicitHeight: replyBack.height
                    Rectangle {
                        id: replyBack
                        width: Math.min(parent.width * 0.9, replyText.implicitWidth + 24 * Theme.unit)
                        height: replyText.implicitHeight + 18 * Theme.unit
                        radius: 14 * Theme.unit
                        color: Theme.card
                        TextEdit {
                            id: replyText
                            x: 12 * Theme.unit
                            y: 9 * Theme.unit
                            width: Math.min(implicitWidth, row.width * 0.9 - 24 * Theme.unit)
                            text: row.model.text || ""
                            textFormat: TextEdit.PlainText
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.Wrap
                            color: Theme.text
                            selectionColor: Theme.tint
                            selectedTextColor: "#ffffff"
                            font.pixelSize: Theme.bodySize
                            Keys.onEscapePressed: if (view.dock) view.dock.giveBackKeyboard()
                        }
                    }
                }
            }

            // a step: a tick when done; the newest step of the running request turns while Claude works
            Component {
                id: activityLine
                RowLayout {
                    id: step
                    readonly property bool current: !!row.info && !row.info.done && !!view.dock && view.dock.working
                        && row.request === requests.newestRequest
                        && row.info.activities[row.info.activities.length - 1] === row.index
                    spacing: 8 * Theme.unit
                    Item {
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 1 * Theme.unit
                        implicitWidth: 14 * Theme.unit
                        implicitHeight: 14 * Theme.unit
                        Text {
                            anchors.centerIn: parent
                            visible: !step.current
                            text: "✓"
                            textFormat: Text.PlainText
                            color: Theme.on
                            font.pixelSize: Theme.detailSize
                            font.weight: Font.DemiBold
                        }
                        Busy {
                            anchors.centerIn: parent
                            visible: step.current
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: row.model.text || ""
                        textFormat: Text.PlainText
                        color: step.current ? Theme.text : Theme.secondaryText
                        font.pixelSize: Theme.detailSize + 0.5 * Theme.unit
                        wrapMode: Text.Wrap
                        lineHeight: 1.1
                    }
                }
            }

            Component {
                id: question
                QuestionCard {
                    id: questionCard
                    dock: view.dock
                    // a scroll or a row above it moving re-arms its buttons (handlers, not a binding: no loop
                    // while the list moves under a held press)
                    Connections {
                        target: list
                        function onContentYChanged() { questionCard.moved(); }
                    }
                    Connections {
                        target: row
                        function onYChanged() { questionCard.moved(); }
                    }
                    askId: row.model.itemId || ""
                    text: row.model.text || ""
                    exactly: row.model.detail || ""
                    answered: row.model.answered || ""
                }
            }

            Component {
                id: madeCard
                MadeCard {
                    dock: view.dock
                    kind: row.kind
                    itemId: row.model.itemId || ""
                    text: row.model.text || ""
                    detail: row.model.detail || ""
                    sheet: Format.urls(row.model.views)[0] || ""
                    undone: !!row.model.undone
                }
            }

            // a request's end: "✓ Done · 3 min · $0.61" and its Undo (on for the newest request not undone)
            Component {
                id: doneLine
                ColumnLayout {
                    spacing: 4 * Theme.unit
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8 * Theme.unit
                        Text {
                            Layout.fillWidth: true
                            text: !row.info ? ""
                                : row.info.undone ? "Undone"
                                : ["✓  Done",
                                   row.info.endTime > 0 ? Format.duration(row.info.endTime - row.info.startTime) : "",
                                   Format.cost(row.model.cost)].filter(function (part) { return part !== ""; }).join(" · ")
                            textFormat: Text.PlainText
                            color: Theme.secondaryText
                            font.pixelSize: Theme.detailSize
                            elide: Text.ElideRight
                        }
                        MakeButton {
                            visible: !!row.info && !row.info.undone
                            text: "Undo"
                            enabled: row.request === requests.newestUndoable && !(view.dock && view.dock.working)
                            onClicked: if (view.dock) view.dock.undoRequest(row.request)
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: text !== "" && text !== "Done"
                        text: row.model.text || ""
                        textFormat: Text.PlainText
                        color: Theme.tertiaryText
                        font.pixelSize: Theme.detailSize
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }

    DropArea {
        id: drop
        anchors.fill: parent
        enabled: !view.terminalRoute && !!view.dock && view.dock.state === "connected"
        keys: ["text/uri-list"]
        onDropped: function (event) {
            if (view.dock && view.dock.dropPictures(event.urls)) {
                event.accept(Qt.CopyAction);
            }
        }
    }
    Rectangle {
        anchors.fill: parent
        anchors.margins: 8 * Theme.unit
        visible: drop.containsDrag
        radius: Theme.cardRadius
        color: Qt.rgba(Theme.background.r, Theme.background.g, Theme.background.b, 0.88)
        border.width: 2 * Theme.unit
        border.color: Theme.tint
        Text {
            anchors.centerIn: parent
            text: "Drop pictures to add them"
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.bodySize
            font.weight: Font.Medium
        }
    }
}
