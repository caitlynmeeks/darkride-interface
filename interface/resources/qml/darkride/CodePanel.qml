//
//  CodePanel.qml
//  qml/darkride
//
//  Darkride's Code panel (Make v1, team/editor-ui/CODE-EDITOR-PLAN.md): on the left, Code near you, every script and
//  shader within 50 m (Caity 11-07, team/editor-ui/mockups/code-panel-list); on the right, one file at a time in
//  Monaco, the editor inside VS Code, under a two-line header (its path; its kind, running status and what uses it)
//  with Save, Reload for everyone and •••. Shaders are read only. Opened from the list, the Inspector, the Hierarchy
//  and Make's cards, through CodeEditor (ui/CodeEditor.h), which only this panel's context has. Qt 6 only: Panels lists the panel on Qt 6 alone, as the page needs Qt 6's web
//  engine, so this file uses Qt 6's versionless imports and has no Qt 5 twin.
//
//  The page (html/code/code.html) is the app's own, from qrc:, on its own profile with file and remote access off, and
//  it's made only while the panel is open on a script, so a closed panel costs nothing. The page has no bridge object:
//  this panel calls darkrideCode's fixed functions with JSON arguments and takes its events when its title says
//  "Code: events", as Radio's page (security, team/security/reviews/make-v1-editor-page.md). Saving is this panel's,
//  on the person's Save or Cmd+S. Every text from the world is shown as plain text.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

import QtQuick
import QtQuick.Controls
import QtWebEngine
import "panel"

Rectangle {
    id: root

    signal sendToScript(var message)   // the tablet's loader connects it; nothing is sent (desktop only in v1)

    anchors.fill: parent
    color: root.panelColor

    readonly property url pageUrl: "qrc:///html/code/code.html"
    readonly property bool hasEditor: typeof CodeEditor !== "undefined" && CodeEditor !== null
    property bool panelOpen: typeof Panels !== "undefined" && Panels.isOpen("Code")
    readonly property bool showPage: panelOpen && hasEditor && CodeEditor.hasDocument && CodeEditor.problem === "" &&
        !CodeEditor.loading

    property bool pageReady: false   // the page said "ready": its calls work
    onPageReadyChanged: {
        if (hasEditor) {
            CodeEditor.setPageReady(pageReady);
        }
    }
    property bool dirty: false       // the page's text differs from the saved one
    onDirtyChanged: showNote()

    onPanelOpenChanged: {
        if (hasEditor) {
            CodeEditor.setPanelOpen(panelOpen);
        }
    }
    Component.onCompleted: {
        if (hasEditor) {
            CodeEditor.setNearbyShown(true);   // the list looks at the world whenever the panel is open
            CodeEditor.setPanelOpen(panelOpen);
        }
    }
    function panelClosed() {
        // QmlDock: the panel was closed (its close box, View > Panels). The page goes with the loader; an unsaved
        // change was kept in CodeEditor as a draft, and is there when the panel opens again.
        panelOpen = false;
    }

    Connections {
        target: typeof Panels !== "undefined" ? Panels : null
        function onOpenChanged(name, open) {
            if (name === "Code") {
                root.panelOpen = open;
            }
        }
    }

    // ---- the page's fixed calls, every argument as JSON ----

    function call(name, args, answer) {
        var view = loader.item;
        if (!view || !pageReady) {
            return;
        }
        var quoted = (args || []).map(function (arg) { return JSON.stringify(arg); }).join(", ");
        var script = "window.darkrideCode && darkrideCode." + name + "(" + quoted + ")";
        if (answer) {
            view.runJavaScript(script, answer);
        } else {
            view.runJavaScript(script);
        }
    }

    function showDocument() {
        if (!pageReady || !hasEditor || !CodeEditor.hasDocument) {
            return;
        }
        var doc = {
            language: CodeEditor.language,
            readOnly: !CodeEditor.canSave,
            text: CodeEditor.text(),
            savedText: CodeEditor.savedText,
            errorLine: CodeEditor.errorLine,
            error: CodeEditor.errorText
        };
        if (CodeEditor.hasDraft()) {
            doc.draft = CodeEditor.draft();
        }
        call("open", [doc]);
        showNote();
        dirty = CodeEditor.hasDraft();
        var change = CodeEditor.change();
        if (change.length === 2) {
            call("showDiff", [change[0], change[1]]);
        }
        loader.item.forceActiveFocus();
        call("focus", []);
    }

    // a save's answer, or why it can't be changed here, on the page's bottom line (red when it's a problem)
    function showNote() {
        if (!hasEditor) {
            return;
        }
        // a file several things use: while there's a change, the bottom line says a save changes all of them
        var shared = root.dirty && CodeEditor.canSave && CodeEditor.sharedBy > 1
            ? "Saving changes it for all " + CodeEditor.sharedBy + " things that use it." : "";
        var text = CodeEditor.note !== "" ? CodeEditor.note
            : (!CodeEditor.canSave && CodeEditor.problem === "" ? CodeEditor.readOnlyReason : shared);
        call("setNote", [text, CodeEditor.note !== "" ? CodeEditor.noteIsProblem : false]);
    }

    // the person's Save or Cmd+S: the page's text to CodeEditor, which saves with the person's own rights
    function save() {
        if (!hasEditor || !CodeEditor.canSave || CodeEditor.saving) {
            return;
        }
        call("text", [], function (text) {
            if (typeof text === "string") {
                CodeEditor.save(text);
            }
        });
    }

    // the page's events, as JSON text, read with JSON.parse only and taken as data
    function takeEvents() {
        call("takeEvents", [], function (text) {
            var list;
            try {
                list = JSON.parse(String(text));
            } catch (error) {
                return;
            }
            if (!Array.isArray(list)) {
                return;
            }
            list.forEach(function (event) {
                if (!event || typeof event !== "object") {
                    return;
                }
                if (event.type === "ready") {
                    root.showDocument();
                } else if (event.type === "save") {
                    root.save();
                } else if (event.type === "changed") {
                    root.dirty = event.dirty === true;
                    CodeEditor.setDirty(root.dirty);
                    if (root.dirty) {
                        draftTimer.restart();
                    } else {
                        CodeEditor.setDraft("", false);
                    }
                } else if (event.type === "escape") {
                    CodeEditor.giveBackKeyboard();
                }
            });
        });
    }

    // an unsaved change kept in CodeEditor shortly after typing stops, so closing the panel loses nothing
    Timer {
        id: draftTimer
        interval: 700
        onTriggered: {
            if (!root.dirty) {
                return;
            }
            var key = CodeEditor.openKey;   // the answer comes later: only for the file it was asked about
            root.call("text", [], function (text) {
                if (typeof text === "string" && root.dirty && CodeEditor.openKey === key) {
                    CodeEditor.setDraft(text, true);
                }
            });
        }
    }

    Connections {
        target: root.hasEditor ? CodeEditor : null
        function onDocumentChanged() {
            root.dirty = false;
            root.showDocument();
        }
        function onRightsChanged() {
            root.call("setReadOnly", [!CodeEditor.canSave]);
            root.showNote();
        }
        function onNoteChanged() {
            root.showNote();
        }
        function onUsedByChanged() {
            root.showNote();
        }
        function onSavedChanged() {
            if (CodeEditor.savedText !== "") {
                root.dirty = false;
                root.call("saved", [CodeEditor.savedText]);
            }
        }
        // Claude writing this script (MakeDock): its text arriving, then old and new side by side
        function onStreamBegan() {
            root.call("streamBegin", []);
        }
        function onStreamed(chunk) {
            root.call("streamText", [chunk]);
        }
        function onStreamEnded(text) {
            root.dirty = false;
            root.call("streamEnd", [text]);
        }
        function onChangeShown(oldText, newText) {
            root.call("showDiff", [oldText, newText]);
        }
        function onChangeClosed() {
            root.call("closeDiff", []);
        }
        // Test only (scripted runs, --testScript): a probe run in the page, or Save pressed
        function onTestRequest(what, source, run) {
            if (what === "save") {
                root.save();
            } else if (what === "list") {
                root.listOpen = source === "open";
            } else if (what === "run" && loader.item && root.pageReady) {
                loader.item.runJavaScript(source, function (result) {
                    var json;
                    try {
                        json = JSON.stringify(result === undefined ? null : result);
                    } catch (error) {
                        json = JSON.stringify(String(result));
                    }
                    CodeEditor.testAnswer(run, json);
                });
            }
        }
        function onTestOpenRowRequested(entity, which) {
            root.openRow(entity, which);
        }
        function onStatusChanged() {
            if (CodeEditor.errorText !== "") {
                root.call("showError", [CodeEditor.errorLine, CodeEditor.errorText]);
            } else {
                root.call("clearError", []);
            }
        }
    }

    // ---- Build mode's look (interface/resources/styles/editor.qss; Caity 11-07 "ok as drawn", mockups/code-panel) ----

    readonly property color panelColor: "#383838"
    readonly property color textColor: "#d2d2d2"
    readonly property color mutedColor: "#8f8f8f"
    readonly property color dimColor: "#6a6a6a"
    readonly property color fieldColor: "#2a2a2a"
    readonly property color fieldEdge: "#1f1f1f"
    readonly property color edgeColor: "#232323"
    readonly property color hoverColor: "#474747"
    readonly property color selectionColor: "#2c5d87"
    readonly property color runningColor: "#5fb760"
    readonly property color problemColor: "#ff6b6b"

    // drawn as the Inspector's buttons (Edit, Reload): flat grey, a 1 px bevel, square corners, 23 px
    component BuildButton: Item {
        id: button
        property string text: ""
        property string tip: ""
        property bool down: false
        signal pressed()
        implicitWidth: Math.max(text.length > 3 ? 64 : 30, label.implicitWidth + 24)
        implicitHeight: 23
        Rectangle {
            anchors.fill: parent
            color: buttonMouse.pressed || button.down ? root.hoverColor : root.panelColor
        }
        Rectangle { width: parent.width; height: 1; color: "#404040" }
        Rectangle { width: 1; height: parent.height - 1; color: "#404040" }
        Rectangle { x: parent.width - 1; width: 1; height: parent.height - 1; color: "#252525" }
        Rectangle { y: parent.height - 2; width: parent.width; height: 1; color: "#252525" }
        Rectangle { y: parent.height - 1; width: parent.width; height: 1; color: "#131313" }
        Text {
            id: label
            anchors.centerIn: parent
            text: button.text
            textFormat: Text.PlainText
            color: button.enabled ? root.textColor : root.dimColor
            font.pixelSize: 12
        }
        MouseArea {
            id: buttonMouse
            anchors.fill: parent
            enabled: button.enabled
            hoverEnabled: true
            onClicked: button.pressed()
            ToolTip.visible: containsMouse && button.tip !== ""
            ToolTip.text: button.tip
            ToolTip.delay: 600
        }
    }

    // ---- the list on the left: Code near you, every script and shader within 50 m, nearest first, one row per file
    // (Caity 11-07 "EXCELLENT, as drawn", mockups/code-panel-list). It folds away when the panel is narrow; the
    // sidebar button brings it back. Clicking a row opens it on the right. ----

    readonly property bool narrow: width < 700
    property bool listOpen: !narrow
    onNarrowChanged: listOpen = !narrow
    readonly property var kindColors: ({ entity: "#8fb6de", server: "#c7a0dc", fragment: "#d8b06a", vertex: "#9cc79a" })

    // another file opened from the list: the person's typing in this one is kept first (CodeEditor's drafts, one per
    // file), so moving between files loses nothing
    function openRow(entity, which) {
        if (!hasEditor) {
            return;
        }
        draftTimer.stop();
        if (root.dirty && root.pageReady) {
            var key = CodeEditor.openKey;
            call("text", [], function (text) {
                if (typeof text === "string" && CodeEditor.openKey === key) {
                    CodeEditor.setDraft(text, true);
                }
                CodeEditor.open(entity, which);
            });
        } else {
            CodeEditor.open(entity, which);
        }
    }

    // the macOS sidebar button: the list folds and unfolds
    component SidebarButton: Rectangle {
        width: 22
        height: 20
        radius: 3
        color: sidebarMouse.pressed ? root.hoverColor : sidebarMouse.containsMouse ? "#404040" : "transparent"
        Rectangle {
            anchors.centerIn: parent
            width: 15
            height: 12
            radius: 2
            color: "transparent"
            border.color: "#c8c8c8"
            border.width: 1
            Rectangle { x: 5; y: 1; width: 1; height: parent.height - 2; color: "#c8c8c8" }
            Rectangle { x: 2; y: 3; width: 2; height: 1; color: "#c8c8c8" }
            Rectangle { x: 2; y: 5; width: 2; height: 1; color: "#c8c8c8" }
            Rectangle { x: 2; y: 7; width: 2; height: 1; color: "#c8c8c8" }
        }
        MouseArea {
            id: sidebarMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.listOpen = !root.listOpen
            ToolTip.visible: containsMouse
            ToolTip.text: root.listOpen ? "Hide the list" : "Show the code near you"
            ToolTip.delay: 600
        }
    }

    Item {
        id: side
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.listOpen ? 272 : 0
        visible: root.listOpen && root.hasEditor

        Item {
            id: sideHead
            width: parent.width
            height: 44
            SidebarButton {
                id: sideToggle
                x: 6
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                id: sideTitle
                anchors.left: sideToggle.right
                anchors.leftMargin: 7
                anchors.verticalCenter: parent.verticalCenter
                text: "Code near you"
                textFormat: Text.PlainText
                color: "#e2e2e2"
                font.pixelSize: 13
                font.bold: true
            }
            Text {
                anchors.left: sideTitle.right
                anchors.leftMargin: 7
                anchors.verticalCenter: parent.verticalCenter
                text: root.hasEditor ? String(CodeEditor.nearby.count) : ""
                textFormat: Text.PlainText
                color: root.mutedColor
                font.pixelSize: 12
            }
            BuildButton {
                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                text: "Refresh"
                tip: "Look at what's within 50 m now"
                onPressed: CodeEditor.refreshNearby()
            }
        }

        // the filter, drawn as the Entities list's Search field
        Rectangle {
            id: filterField
            anchors.top: sideHead.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            height: 22
            radius: 3
            color: root.fieldColor
            border.color: nearbyFilter.activeFocus ? root.selectionColor : root.fieldEdge
            TextInput {
                id: nearbyFilter
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                verticalAlignment: TextInput.AlignVCenter
                color: root.textColor
                selectionColor: root.selectionColor
                font.pixelSize: 12
                maximumLength: 80
                clip: true
                // Return is the field's: not on to the app, where it opens Chat
                Keys.onReturnPressed: function (event) { event.accepted = true; }
                Keys.onEnterPressed: function (event) { event.accepted = true; }
                Keys.onEscapePressed: function (event) { event.accepted = true; CodeEditor.giveBackKeyboard(); }
            }
            Text {
                anchors.fill: parent
                anchors.leftMargin: 6
                verticalAlignment: Text.AlignVCenter
                visible: nearbyFilter.text === ""
                text: "Filter by name or path"
                textFormat: Text.PlainText
                color: "#7a7a7a"
                font.pixelSize: 12
            }
        }

        ListView {
            id: nearbyList
            anchors.top: filterField.bottom
            anchors.topMargin: 6
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: sideFoot.top
            clip: true
            model: root.hasEditor ? CodeEditor.nearby : null
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            Rectangle {
                width: parent.width
                height: 1
                color: root.edgeColor
            }
            delegate: Rectangle {
                id: row
                readonly property string filter: nearbyFilter.text.trim().toLowerCase()
                readonly property bool matches: filter === "" || model.file.toLowerCase().indexOf(filter) >= 0 ||
                    model.path.toLowerCase().indexOf(filter) >= 0 || model.usedBy.toLowerCase().indexOf(filter) >= 0
                readonly property bool current: root.hasEditor && CodeEditor.hasDocument && CodeEditor.openKey === model.key
                readonly property bool edited: root.hasEditor && CodeEditor.draftKeys.indexOf(model.key) >= 0
                readonly property color kindColor: root.kindColors[model.kindKey] || root.mutedColor
                width: ListView.view.width
                height: matches ? rowColumn.implicitHeight + 12 : 0
                visible: matches
                color: current ? root.selectionColor : rowMouse.containsMouse ? "#404040" : "transparent"
                Column {
                    id: rowColumn
                    x: 10
                    y: 5
                    width: parent.width - 20
                    Item {
                        width: parent.width
                        height: 15
                        Rectangle {
                            id: rowDot
                            anchors.verticalCenter: parent.verticalCenter
                            width: model.kindKey === "entity" ? 7 : 0
                            height: 7
                            radius: 3.5
                            visible: model.kindKey === "entity"
                            color: model.state === "running" ? root.runningColor
                                : model.state === "error" ? root.problemColor : "#777777"
                        }
                        Text {
                            anchors.left: rowDot.right
                            anchors.leftMargin: rowDot.visible ? 6 : 0
                            anchors.right: rowDistance.left
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: model.file
                            textFormat: Text.PlainText
                            elide: Text.ElideMiddle
                            color: row.current ? "#ffffff" : "#e2e2e2"
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Text {
                            id: rowDistance
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: model.distance
                            textFormat: Text.PlainText
                            color: row.current ? "#cfe0ef" : root.mutedColor
                            font.pixelSize: 11
                        }
                    }
                    Text {
                        width: parent.width
                        height: 14
                        text: model.kind.toUpperCase() + (row.edited ? "  ·  EDITED" : "")
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: row.current ? Qt.lighter(row.kindColor, 1.25) : row.kindColor
                        font.pixelSize: 10
                        font.bold: true
                        font.letterSpacing: 0.5
                    }
                    Text {
                        width: parent.width
                        height: 15
                        text: model.path
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        color: row.current ? "#cfe0ef" : root.mutedColor
                        font.pixelSize: 11
                    }
                    Text {
                        width: parent.width
                        height: 15
                        text: model.usedBy
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: row.current ? "#cfe0ef" : "#7d7d7d"
                        font.pixelSize: 11
                    }
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: "#313131"
                    visible: row.matches
                }
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.openRow(model.entity, model.which)
                }
            }
        }

        Rectangle {
            id: sideFoot
            anchors.bottom: parent.bottom
            width: parent.width
            height: 22
            color: root.panelColor
            Rectangle {
                width: parent.width
                height: 1
                color: root.edgeColor
            }
            Text {
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                text: !root.hasEditor ? "" : CodeEditor.nearby.count === 0 ? "Nothing within 50 m"
                    : CodeEditor.nearby.count + " within 50 m"
                textFormat: Text.PlainText
                color: root.mutedColor
                font.pixelSize: 11
            }
        }

        Rectangle {
            anchors.right: parent.right
            width: 1
            height: parent.height
            color: root.edgeColor
        }
    }

    // ---- the right: the open file's header (two lines, Caity 11-07: its path, then its kind, Running, Edited and
    // what uses it), then the editor ----

    Item {
        id: main
        anchors.left: side.visible ? side.right : parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        visible: root.hasEditor

        Item {
            id: bar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 44

            SidebarButton {
                id: mainToggle
                x: 6
                anchors.verticalCenter: parent.verticalCenter
                visible: !root.listOpen
            }

            Column {
                id: titles
                anchors.left: mainToggle.visible ? mainToggle.right : parent.left
                anchors.leftMargin: mainToggle.visible ? 7 : 10
                anchors.right: buttons.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1
                Text {
                    width: parent.width
                    text: !root.hasEditor || !CodeEditor.hasDocument ? "No script open"
                        : CodeEditor.path !== "" ? CodeEditor.path : CodeEditor.fileName
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: root.hasEditor && CodeEditor.hasDocument ? "#e8e8e8" : root.mutedColor
                    font.pixelSize: 13
                    font.bold: root.hasEditor && CodeEditor.hasDocument
                }
                // the kind, where it lives (written in, on the web), whether it's running, Edited, then what uses it
                // and how far: cut short first, the list has it in full
                Row {
                    id: meta
                    width: parent.width
                    height: 14
                    spacing: 5
                    clip: true
                    visible: root.hasEditor && CodeEditor.hasDocument
                    readonly property string place: !root.hasEditor ? ""
                        : CodeEditor.where === "Written in" ? "Written in"
                        : CodeEditor.where === "On the web, read only" ? "On the web" : ""
                    readonly property bool hasStatus: root.hasEditor && (CodeEditor.status !== "" || CodeEditor.claudeWriting)
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.hasEditor ? CodeEditor.kind : ""
                        textFormat: Text.PlainText
                        color: root.mutedColor
                        font.pixelSize: 11
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: meta.place !== ""
                        text: "·  " + meta.place
                        textFormat: Text.PlainText
                        color: root.mutedColor
                        font.pixelSize: 11
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: meta.hasStatus
                        text: "·"
                        textFormat: Text.PlainText
                        color: root.mutedColor
                        font.pixelSize: 11
                    }
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 7
                        height: 7
                        radius: 3.5
                        visible: meta.hasStatus
                        color: !root.hasEditor ? root.dimColor
                            : CodeEditor.statusState === "running" ? root.runningColor
                            : CodeEditor.statusState === "error" ? root.problemColor : root.dimColor
                    }
                    // the status, Edited and what uses it in one line that ends in "…" when the header is narrow
                    // (never cut mid-word at the buttons)
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        readonly property var parts: {
                            var list = [];
                            if (meta.hasStatus) {
                                list.push(CodeEditor.claudeWriting ? "Claude is writing…" : CodeEditor.status);
                            }
                            if (root.dirty) {
                                list.push("Edited");
                            }
                            if (root.hasEditor && CodeEditor.usedBy !== "") {
                                list.push(CodeEditor.usedBy);
                            }
                            return list;
                        }
                        visible: parts.length > 0
                        width: Math.max(0, meta.width - x)
                        text: (meta.hasStatus ? "" : "·  ") + parts.join("  ·  ")
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: root.mutedColor
                        font.pixelSize: 11
                    }
                }
            }

            Row {
                id: buttons
                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6
                BuildButton {
                    text: "Back to the Script"
                    visible: root.hasEditor && CodeEditor.showingChange
                    tip: "Claude's change side by side: back to the script itself"
                    onPressed: CodeEditor.closeChange()
                }
                BuildButton {
                    text: root.hasEditor && CodeEditor.saving ? "Saving…" : "Save"
                    visible: !root.hasEditor || CodeEditor.where !== "On the web, read only"
                    enabled: root.hasEditor && CodeEditor.hasDocument && CodeEditor.problem === "" && CodeEditor.canSave &&
                        root.dirty && !CodeEditor.saving && !CodeEditor.claudeWriting
                    tip: root.hasEditor && CodeEditor.hasDocument && !CodeEditor.canSave ? CodeEditor.readOnlyReason : "Save (⌘S)"
                    onPressed: root.save()
                }
                BuildButton {
                    text: "Reload"
                    enabled: root.hasEditor && CodeEditor.hasDocument && CodeEditor.problem === "" && CodeEditor.canSave
                    tip: "Load it again for everyone in the world"
                    onPressed: CodeEditor.reloadForEveryone()
                }
                BuildButton {
                    id: moreButton
                    text: "•••"
                    tip: "More"
                    enabled: root.hasEditor && CodeEditor.hasDocument
                    onPressed: {
                        var at = moreButton.mapToGlobal(moreButton.width, moreButton.height + 2);
                        CodeEditor.showMoreMenu(at.x - 180, at.y);
                    }
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: root.edgeColor
            }
        }

        // while the page isn't there: a hint, why it can't open, or Loading; with its bottom line
        Rectangle {
            id: hint
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: bar.bottom
            anchors.bottom: statusLine.top
            visible: !root.showPage
            color: root.fieldColor
            Text {
                anchors.centerIn: parent
                width: Math.min(parent.width - 40, 520)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                readonly property bool problem: root.hasEditor && CodeEditor.hasDocument && CodeEditor.problem !== ""
                text: !root.hasEditor ? ""
                    : CodeEditor.hasDocument && CodeEditor.loading ? "Loading…"
                    : problem ? CodeEditor.problem
                    : root.listOpen ? "Click a script or shader on the left, or Edit beside Script in the Inspector."
                    : "Show the code near you with the button at the top left, or Edit beside Script in the Inspector."
                textFormat: Text.PlainText
                color: "#7a7a7a"
                font.pixelSize: 12
            }
        }
        Rectangle {
            id: statusLine
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 22
            visible: !root.showPage
            color: root.panelColor
            Rectangle {
                width: parent.width
                height: 1
                color: root.edgeColor
            }
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
                readonly property bool problem: root.hasEditor && CodeEditor.hasDocument && CodeEditor.problem !== ""
                text: problem ? CodeEditor.problem : ""
                textFormat: Text.PlainText
                color: root.problemColor
                font.pixelSize: 11
            }
        }
    }

    // ---- the editor ----

    Loader {
        id: loader
        anchors.left: main.left
        anchors.right: main.right
        anchors.top: main.top
        anchors.topMargin: bar.height
        anchors.bottom: main.bottom
        active: root.showPage
        onActiveChanged: {
            if (!active) {
                root.pageReady = false;
            }
        }
        sourceComponent: WebEngineView {
            id: view
            Component.onCompleted: view.url = root.pageUrl
            backgroundColor: "#2a2a2a"   // the editor's own (code.css), behind it while it loads

            // one profile for the run, a walled-off client's folders set before it opens them (ContextAwareProfile)
            profile: TivoliWebEngineProfile.codeProfile
            // the page is the app's own (qrc:) and reaches nothing: not the computer's files, not the network
            settings.localContentCanAccessRemoteUrls: false
            settings.localContentCanAccessFileUrls: false
            settings.javascriptCanOpenWindows: false
            settings.javascriptCanAccessClipboard: false
            settings.javascriptCanPaste: false
            settings.pluginsEnabled: false
            settings.pdfViewerEnabled: false
            settings.webGLEnabled: false
            settings.localStorageEnabled: false
            settings.autoLoadIconsForPage: false
            settings.errorPageEnabled: false
            settings.navigateOnDropEnabled: false
            settings.focusOnNavigationEnabled: false
            settings.unknownUrlSchemePolicy: WebEngineSettings.DisallowUnknownUrlSchemes

            // the page stays the editor: no link followed, no file dropped on it replaces it, in any frame
            onNavigationRequested: function (request) {
                if (request.url.toString() !== root.pageUrl.toString()) {
                    request.action = WebEngineNavigationRequest.IgnoreRequest;
                }
            }
            // no new windows, menus, dialogs or permissions: the page asks for none (a link in a hover goes nowhere)
            onNewWindowRequested: function (request) {
                console.log("Code: a new window refused");
            }
            onContextMenuRequested: function (request) {
                request.accepted = true;
            }
            onFileDialogRequested: function (request) {
                request.accepted = true;
                request.dialogReject();
            }
            onJavaScriptDialogRequested: function (request) {
                request.accepted = true;
                request.dialogReject();
            }
            onColorDialogRequested: function (request) {
                request.accepted = true;
                request.dialogReject();
            }
            onAuthenticationDialogRequested: function (request) {
                request.accepted = true;
                request.dialogReject();
            }
            onPermissionRequested: function (permission) {
                permission.deny();
            }
            onDesktopMediaRequested: function (request) {
                request.cancel();
            }
            onRegisterProtocolHandlerRequested: function (request) {
                request.reject();
            }
            onFileSystemAccessRequested: function (request) {
                request.reject();
            }
            onTitleChanged: {
                if (title.indexOf("Code: events") === 0) {
                    root.pageReady = true;   // the page posts "ready" first: its calls work from here
                    root.takeEvents();
                }
            }
            onRenderProcessTerminated: function (terminationStatus, exitCode) {
                console.warn("Code: the page's process ended", terminationStatus, exitCode);
                root.pageReady = false;
            }
        }
    }
}
