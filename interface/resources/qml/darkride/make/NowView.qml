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
import "../editor"
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

    // before anything: the Terminal route's start line while Make waits for Claude (at the top, under the header),
    // else a welcome (in the middle). Above the list (z), which is empty then but still takes clicks and drags: under
    // it, Copy and the line never got them (Caity's try, 11-07).
    ColumnLayout {
        z: 1
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        // a y, not anchors: switching top and verticalCenter anchors by binding clashes and leaves it in the middle
        y: view.terminalRoute ? 12 : Math.max(12, (view.height - height) / 2)
        visible: view.empty
        spacing: 12

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
            color: BuildTheme.text
            font.pixelSize: BuildTheme.textSize
            font.weight: Font.Normal
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
        Repeater {
            model: view.terminalRoute ? [] : ["a neon cafe sign that lights up at night",
                "make the door open when I knock three times", "a little robot duck I can wear"]
            delegate: Rectangle {
                required property string modelData
                Layout.alignment: Qt.AlignHCenter
                Layout.maximumWidth: view.width - 36
                implicitWidth: example.implicitWidth + 24
                implicitHeight: example.implicitHeight + 12
                radius: BuildTheme.radius
                color: exampleMouse.pressed ? BuildTheme.fieldPressed : BuildTheme.field
                Accessible.role: Accessible.Button
                Accessible.name: modelData
                Text {
                    id: example
                    anchors.centerIn: parent
                    width: Math.min(implicitWidth, parent.Layout.maximumWidth - 24)
                    text: "“" + parent.modelData + "”"
                    textFormat: Text.PlainText
                    color: BuildTheme.mutedText
                    font.pixelSize: BuildTheme.noteSize
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
            visible: !view.terminalRoute   // the Claude-plan route's empty list says nothing (mary's mock)
            text: "Claude makes it in 3D, places it, makes it work and tests it."
            textFormat: Text.PlainText
            color: BuildTheme.dimText
            font.pixelSize: BuildTheme.noteSize
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.topMargin: 10
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
            height: shown ? footerLine.implicitHeight + 24 : 0
            visible: shown
            StartLine {
                id: footerLine
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 12
                anchors.leftMargin: 14
                anchors.rightMargin: 14
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
            readonly property real gap: (kind === "activity" && mode === "row")
                || (kind === "failed" && (mode === "row" || mode === "failedMore")) ? 5 : 12

            width: ListView.view.width
            height: mode === "hidden" ? 0 : loader.implicitHeight + gap
            visible: mode !== "hidden"

            Loader {
                id: loader
                x: 14
                width: parent.width - 28
                active: row.mode !== "hidden"
                sourceComponent: row.mode === "fold" ? foldLine
                    : row.mode === "steps" ? stepsLine
                    : row.kind === "ask" ? askBubble
                    : row.kind === "reply" ? replyBubble
                    : row.kind === "activity" ? activityLine
                    : row.kind === "failed" ? failedLine
                    : row.kind === "question" ? question
                    : row.kind === "done" ? doneLine
                    : (row.kind === "thing" || row.kind === "script" || row.kind === "model") ? madeCard
                    : null
            }

            // a folded request: "▸ what was asked · Done · 2 min · $0.52"
            Component {
                id: foldLine
                Rectangle {
                    implicitHeight: foldRow.implicitHeight + 18
                    radius: BuildTheme.radius
                    color: foldMouse.pressed ? BuildTheme.fieldPressed : BuildTheme.field
                    opacity: !!row.info && row.info.undone ? 0.6 : 1
                    Accessible.role: Accessible.Button
                    Accessible.name: foldTitle.text
                    RowLayout {
                        id: foldRow
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 8
                        Text {
                            text: "▸"
                            textFormat: Text.PlainText
                            color: BuildTheme.mutedText
                            font.pixelSize: BuildTheme.textSize
                        }
                        Text {
                            id: foldTitle
                            Layout.fillWidth: true
                            text: row.info ? (row.info.title || row.info.firstText) : ""
                            textFormat: Text.PlainText
                            color: BuildTheme.text
                            font.pixelSize: BuildTheme.textSize
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
                            color: BuildTheme.mutedText
                            font.pixelSize: BuildTheme.noteSize
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
                    implicitHeight: stepsText.implicitHeight + 4
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
                        color: BuildTheme.mutedText
                        font.pixelSize: BuildTheme.noteSize
                        leftPadding: 2
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
                    readonly property real picturesWidth: shown > 0 ? shown * 96 + (shown - 1) * 6 : 0
                    implicitHeight: bubble.height
                    Rectangle {
                        id: bubble
                        anchors.right: parent.right
                        width: Math.min(parent.width * 0.82, Math.max(askText.implicitWidth, ask.picturesWidth) + 24)
                        height: askColumn.implicitHeight + 18
                        radius: BuildTheme.radius
                        color: BuildTheme.field
                        ColumnLayout {
                            id: askColumn
                            x: 12
                            y: 9
                            width: parent.width - 24
                            spacing: 8
                            Flow {
                                id: pictures
                                Layout.fillWidth: true
                                visible: ask.views.length > 0
                                spacing: 6
                                Repeater {
                                    model: ask.views
                                    delegate: Image {
                                        required property string modelData
                                        width: 96
                                        height: 72
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
                                color: BuildTheme.text
                                font.pixelSize: BuildTheme.textSize
                                wrapMode: Text.Wrap
                                lineHeight: 1.15
                            }
                        }
                    }
                }
            }

            // Claude's words, on the left; selectable to copy. The box is at most 90% of the timeline and the text wraps
            // at the box's width less its 12 px sides, one number (mary's MAKE-POLISH.png: it wrapped at the row's
            // width before, past the box's edge)
            Component {
                id: replyBubble
                Item {
                    implicitHeight: replyBack.height
                    Rectangle {
                        id: replyBack
                        width: Math.min(parent.width * 0.9, replyText.implicitWidth + 24)
                        height: replyText.implicitHeight + 18
                        radius: BuildTheme.radius
                        color: BuildTheme.field
                        TextEdit {
                            id: replyText
                            x: 12
                            y: 9
                            width: replyBack.width - 24
                            text: row.model.text || ""
                            textFormat: TextEdit.PlainText
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.Wrap
                            color: BuildTheme.text
                            selectionColor: BuildTheme.selection
                            selectedTextColor: "#ffffff"
                            font.pixelSize: BuildTheme.textSize
                            Keys.onEscapePressed: if (view.dock) view.dock.giveBackKeyboard()
                        }
                    }
                }
            }

            // a step: a tick when done; the newest step of the running request turns while Claude works, until Claude's
            // reply comes after it ("sent to Claude" is done once Claude answers)
            Component {
                id: activityLine
                RowLayout {
                    id: step
                    readonly property bool current: !!row.info && !row.info.done && !!view.dock && view.dock.working
                        && row.request === requests.newestRequest
                        && row.info.activities[row.info.activities.length - 1] === row.index
                        && !(row.info.lastReply > row.index)
                    spacing: 8
                    Item {
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 1
                        implicitWidth: 14
                        implicitHeight: 14
                        Text {
                            anchors.centerIn: parent
                            visible: !step.current
                            text: "✓"
                            textFormat: Text.PlainText
                            color: BuildTheme.running
                            font.pixelSize: BuildTheme.noteSize
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
                        color: step.current ? BuildTheme.text : BuildTheme.mutedText
                        font.pixelSize: BuildTheme.noteSize + 0.5
                        wrapMode: Text.Wrap
                        lineHeight: 1.1
                    }
                }
            }

            // a step that didn't work (mary 12:28 11-08): a red ✕ and the line in the normal colour, "Couldn't <step>:
            // <reason>."; the second of more than three also says how many more are inside the fold
            Component {
                id: failedLine
                RowLayout {
                    spacing: 8
                    Item {
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 1
                        implicitWidth: 14
                        implicitHeight: 14
                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            textFormat: Text.PlainText
                            color: BuildTheme.problem
                            font.pixelSize: BuildTheme.noteSize
                            font.weight: Font.DemiBold
                        }
                    }
                    Column {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            width: parent.width
                            text: row.model.text || ""
                            textFormat: Text.PlainText
                            color: BuildTheme.text
                            font.pixelSize: BuildTheme.noteSize + 0.5
                            wrapMode: Text.Wrap
                            lineHeight: 1.1
                        }
                        Text {
                            width: parent.width
                            visible: row.mode === "failedMore" && !!row.info
                            text: row.info ? "and " + (row.info.failures.length - 2) + " more didn't work" : ""
                            textFormat: Text.PlainText
                            color: BuildTheme.mutedText
                            font.pixelSize: BuildTheme.noteSize
                        }
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
                    spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Text {
                            Layout.fillWidth: true
                            text: !row.info ? ""
                                : row.info.undone ? "Undone"
                                : ["✓  Done",
                                   row.info.endTime > 0 ? Format.duration(row.info.endTime - row.info.startTime) : "",
                                   Format.cost(row.model.cost)].filter(function (part) { return part !== ""; }).join(" · ")
                            textFormat: Text.PlainText
                            color: BuildTheme.mutedText
                            font.pixelSize: BuildTheme.noteSize
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
                        color: BuildTheme.dimText
                        font.pixelSize: BuildTheme.noteSize
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
        anchors.margins: 8
        visible: drop.containsDrag
        radius: BuildTheme.radius
        color: Qt.rgba(BuildTheme.panel.r, BuildTheme.panel.g, BuildTheme.panel.b, 0.88)
        border.width: 2
        border.color: BuildTheme.selection
        Text {
            anchors.centerIn: parent
            text: "Drop pictures to add them"
            textFormat: Text.PlainText
            color: BuildTheme.text
            font.pixelSize: BuildTheme.textSize
            font.weight: Font.Normal
        }
    }
}
