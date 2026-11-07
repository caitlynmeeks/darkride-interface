//
//  CodePanel.qml
//  qml/darkride
//
//  Darkride's Code panel (Make v1, team/editor-ui/CODE-EDITOR-PLAN.md): one script at a time in Monaco, the editor
//  inside VS Code, under a native title bar (its name and kind, where it lives, Save, Reload for everyone, and its
//  running status). Opened from the Inspector, the Hierarchy and Make's cards, through CodeEditor (ui/CodeEditor.h),
//  which only this panel's context has. Qt 6 only: Panels lists the panel on Qt 6 alone, as the page needs Qt 6's web
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
    readonly property bool showPage: panelOpen && hasEditor && CodeEditor.hasDocument && CodeEditor.problem === ""

    property bool pageReady: false   // the page said "ready": its calls work
    onPageReadyChanged: {
        if (hasEditor) {
            CodeEditor.setPageReady(pageReady);
        }
    }
    property bool dirty: false       // the page's text differs from the saved one

    onPanelOpenChanged: {
        if (hasEditor) {
            CodeEditor.setPanelOpen(panelOpen);
        }
    }
    Component.onCompleted: {
        if (hasEditor) {
            CodeEditor.setNearbyShown(true);   // the name's menu lists it whenever the panel is open
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
        var text = CodeEditor.note !== "" ? CodeEditor.note
            : (!CodeEditor.canSave && CodeEditor.problem === "" ? CodeEditor.readOnlyReason : "");
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
            root.call("text", [], function (text) {
                if (typeof text === "string" && root.dirty) {
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

    // ---- the header: one line ----

    Item {
        id: bar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 30
        visible: root.hasEditor

        // the name: its menu is the code near you (as Xcode's jump bar), so Code near you needs no row of its own
        Rectangle {
            id: jump
            x: 4
            anchors.verticalCenter: parent.verticalCenter
            width: jumpRow.width + 12
            height: 22
            radius: 3
            color: jumpMouse.pressed || jumpMouse.containsMouse ? root.hoverColor : "transparent"
            Row {
                id: jumpRow
                x: 6
                anchors.verticalCenter: parent.verticalCenter
                spacing: 5
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "{ }"
                    textFormat: Text.PlainText
                    color: root.mutedColor
                    font.family: "Menlo"
                    font.pixelSize: 11
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, bar.width * 0.4)
                    text: root.hasEditor && CodeEditor.hasDocument ? CodeEditor.fileName : "No script open"
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: root.hasEditor && CodeEditor.hasDocument ? root.textColor : root.mutedColor
                    font.pixelSize: 13
                    font.bold: root.hasEditor && CodeEditor.hasDocument
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "\u25be"
                    textFormat: Text.PlainText
                    color: root.mutedColor
                    font.pixelSize: 11
                }
            }
            MouseArea {
                id: jumpMouse
                anchors.fill: parent
                hoverEnabled: true
                onPressed: {
                    var at = jump.mapToGlobal(0, jump.height + 2);
                    CodeEditor.showNearbyMenu(at.x, at.y, root.nearbyUnfolded);
                }
            }
        }

        // the kind, where it lives, the entity it belongs to, whether it's running, and Edited
        Row {
            id: meta
            anchors.left: jump.right
            anchors.leftMargin: 6
            anchors.right: buttons.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            clip: true
            visible: root.hasEditor && CodeEditor.hasDocument
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    if (!root.hasEditor || !CodeEditor.hasDocument) {
                        return "";
                    }
                    var parts = [CodeEditor.kind];
                    if (CodeEditor.where !== "") {
                        parts.push(CodeEditor.where);
                    }
                    if (CodeEditor.entityName !== "" && CodeEditor.entityName !== CodeEditor.fileName) {
                        parts.push(CodeEditor.entityName);
                    }
                    return parts.join("  \u00b7  ") + "  \u00b7";
                }
                textFormat: Text.PlainText
                color: root.mutedColor
                font.pixelSize: 12
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 7
                height: 7
                radius: 3.5
                color: !root.hasEditor ? root.dimColor
                    : CodeEditor.statusState === "running" ? root.runningColor
                    : CodeEditor.statusState === "error" ? root.problemColor : root.dimColor
                visible: root.hasEditor && (CodeEditor.status !== "" || CodeEditor.claudeWriting)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: !root.hasEditor ? "" : CodeEditor.claudeWriting ? "Claude is writing\u2026" : CodeEditor.status
                textFormat: Text.PlainText
                color: root.mutedColor
                font.pixelSize: 12
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.dirty
                text: "\u00b7  Edited"
                textFormat: Text.PlainText
                color: root.mutedColor
                font.pixelSize: 12
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
                text: root.hasEditor && CodeEditor.saving ? "Saving\u2026" : "Save"
                visible: !root.hasEditor || CodeEditor.where !== "On the web, read only"
                enabled: root.hasEditor && CodeEditor.hasDocument && CodeEditor.problem === "" && CodeEditor.canSave &&
                    root.dirty && !CodeEditor.saving && !CodeEditor.claudeWriting
                tip: root.hasEditor && CodeEditor.hasDocument && !CodeEditor.canSave ? CodeEditor.readOnlyReason : "Save (\u2318S)"
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
                text: "\u2022\u2022\u2022"
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

    // ---- Code near you: every script within 50 m, nearest first, drawn as the Entities list. It fills the panel
    // while nothing is open; with a script open it shows above it when the name's menu says Show All ----

    property bool nearbyUnfolded: false
    readonly property bool listFills: root.hasEditor && !CodeEditor.hasDocument
    Connections {
        target: root.hasEditor ? CodeEditor : null
        function onDocumentChanged() {
            if (CodeEditor.hasDocument) {
                root.nearbyUnfolded = false;   // the editor gets the room
            }
        }
        function onNearbyUnfoldRequested() {
            root.nearbyUnfolded = true;
        }
        function onNearbyFoldRequested() {
            root.nearbyUnfolded = false;
        }
    }
    Rectangle {
        id: nearbySection
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: bar.bottom
        readonly property bool shown: root.listFills || (root.nearbyUnfolded && root.hasEditor)
        visible: shown
        height: !shown ? 0
            : root.listFills ? root.height - bar.height - statusLine.height
            : Math.min(nearbyColumn.implicitHeight + 12, 6 * 20 + 46)
        color: root.panelColor

        Column {
            id: nearbyColumn
            anchors.fill: parent
            anchors.margins: 6
            spacing: 6
            // the filter, drawn as the Entities list's Search field
            Rectangle {
                width: parent.width
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
                    text: "Filter by file or name"
                    textFormat: Text.PlainText
                    color: "#7a7a7a"
                    font.pixelSize: 12
                }
            }
            ListView {
                id: nearbyList
                width: parent.width
                height: root.listFills ? parent.height - 28 : Math.min(contentHeight, 6 * 20)
                clip: true
                model: root.hasEditor ? CodeEditor.nearby : null
                boundsBehavior: Flickable.StopAtBounds
                delegate: Rectangle {
                    readonly property string filter: nearbyFilter.text.trim().toLowerCase()
                    readonly property bool matches: filter === "" || model.file.toLowerCase().indexOf(filter) >= 0 ||
                        model.name.toLowerCase().indexOf(filter) >= 0
                    readonly property bool current: root.hasEditor && CodeEditor.hasDocument &&
                        CodeEditor.isOpenScript(model.entity, model.which)
                    width: ListView.view.width
                    height: matches ? 20 : 0
                    visible: matches
                    color: current ? root.selectionColor : rowMouse.containsMouse ? "#404040" : "transparent"
                    Rectangle {
                        id: rowDot
                        x: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: 7
                        height: 7
                        radius: 3.5
                        color: model.state === "running" ? root.runningColor
                            : model.state === "error" ? root.problemColor : root.dimColor
                    }
                    Row {
                        anchors.left: rowDot.right
                        anchors.leftMargin: 8
                        anchors.right: rowDistance.left
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8
                        clip: true
                        Text {
                            text: model.name !== "" ? model.name : model.file
                            textFormat: Text.PlainText
                            color: current ? "#ffffff" : root.textColor
                            font.pixelSize: 12
                        }
                        Text {
                            text: model.kind + "  \u00b7  " + model.file
                            textFormat: Text.PlainText
                            color: current ? "#cfe0ef" : root.mutedColor
                            font.pixelSize: 12
                        }
                    }
                    Text {
                        id: rowDistance
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: model.distance
                        textFormat: Text.PlainText
                        color: current ? "#cfe0ef" : root.mutedColor
                        font.pixelSize: 12
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: CodeEditor.open(model.entity, model.which)
                    }
                }
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: root.edgeColor
            visible: !root.listFills
        }
    }

    // ---- the bottom line while the page isn't there (the page has its own, drawn the same) ----

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
            id: statusLeft
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            text: !root.hasEditor ? "" : CodeEditor.nearby.count === 0 ? "No scripts within 50 m"
                : CodeEditor.nearby.count + (CodeEditor.nearby.count === 1 ? " script" : " scripts") + " within 50 m"
            textFormat: Text.PlainText
            color: root.mutedColor
            font.pixelSize: 11
        }
        Text {
            anchors.left: statusLeft.right
            anchors.leftMargin: 18
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideRight
            readonly property bool problem: root.hasEditor && CodeEditor.hasDocument && CodeEditor.problem !== ""
            text: !root.hasEditor ? "" : problem ? CodeEditor.problem
                : "Click one to open it, or Edit beside Script in the Inspector."
            textFormat: Text.PlainText
            color: problem ? root.problemColor : root.mutedColor
            font.pixelSize: 11
        }
    }

    // ---- the editor ----

    Loader {
        id: loader
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: nearbySection.visible ? nearbySection.bottom : bar.bottom
        anchors.bottom: parent.bottom
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
