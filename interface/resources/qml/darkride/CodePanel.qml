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
    color: Theme.background

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
        dirty = CodeEditor.hasDraft();
        var change = CodeEditor.change();
        if (change.length === 2) {
            call("showDiff", [change[0], change[1]]);
        }
        loader.item.forceActiveFocus();
        call("focus", []);
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

    // ---- the title bar ----

    component BarButton: Rectangle {
        id: barButton
        property string text: ""
        property bool strong: false
        property string tip: ""
        signal pressed()
        implicitWidth: barButtonLabel.implicitWidth + 20 * Theme.unit
        implicitHeight: Theme.controlHeight
        radius: 7 * Theme.unit
        color: strong && enabled ? Theme.tint : Theme.fill
        opacity: enabled ? (barButtonMouse.pressed ? 0.7 : 1) : 0.45
        Text {
            id: barButtonLabel
            anchors.centerIn: parent
            text: barButton.text
            textFormat: Text.PlainText
            color: barButton.strong && barButton.enabled ? "#ffffff" : Theme.tint
            font.pixelSize: Theme.controlTextSize
            font.bold: barButton.strong
        }
        MouseArea {
            id: barButtonMouse
            anchors.fill: parent
            enabled: barButton.enabled
            hoverEnabled: true
            onClicked: barButton.pressed()
            ToolTip.visible: containsMouse && barButton.tip !== ""
            ToolTip.text: barButton.tip
            ToolTip.delay: 600
        }
    }

    Column {
        id: bar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: Theme.gap
        anchors.rightMargin: Theme.gap
        anchors.topMargin: 8 * Theme.unit
        spacing: 4 * Theme.unit
        visible: root.hasEditor && CodeEditor.hasDocument

        Item {
            width: parent.width
            height: Theme.controlHeight
            Text {
                id: title
                anchors.left: parent.left
                anchors.right: buttons.left
                anchors.rightMargin: 8 * Theme.unit
                anchors.verticalCenter: parent.verticalCenter
                text: root.hasEditor ? CodeEditor.fileName + (root.dirty ? " •" : "") : ""
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                color: Theme.text
                font.pixelSize: Theme.bodySize
                font.bold: true
            }
            Row {
                id: buttons
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6 * Theme.unit
                BarButton {
                    text: "Back to the Script"
                    visible: root.hasEditor && CodeEditor.showingChange
                    tip: "Claude's change side by side: back to the script itself"
                    onPressed: CodeEditor.closeChange()
                }
                BarButton {
                    text: root.hasEditor && CodeEditor.saving ? "Saving…" : "Save"
                    strong: true
                    visible: root.hasEditor && CodeEditor.problem === "" && CodeEditor.where !== "On the web, read only"
                    enabled: root.hasEditor && CodeEditor.canSave && root.dirty && !CodeEditor.saving &&
                        !CodeEditor.claudeWriting
                    tip: root.hasEditor && !CodeEditor.canSave ? CodeEditor.readOnlyReason : "Save (⌘S)"
                    onPressed: root.save()
                }
                BarButton {
                    text: "Reload"
                    visible: root.hasEditor && CodeEditor.problem === ""
                    enabled: root.hasEditor && CodeEditor.canSave
                    tip: "Load it again for everyone in the world"
                    onPressed: CodeEditor.reloadForEveryone()
                }
                BarButton {
                    id: moreButton
                    text: "•••"
                    tip: "More"
                    onPressed: moreMenu.popup(moreButton, 0, moreButton.height)
                }
            }
        }

        // the kind, where it lives, the entity it belongs to, and whether it's running
        Row {
            width: parent.width
            spacing: 8 * Theme.unit
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 7 * Theme.unit
                height: width
                radius: width / 2
                color: !root.hasEditor ? Theme.tertiaryText
                    : CodeEditor.statusState === "running" ? Theme.on
                    : CodeEditor.statusState === "error" ? Theme.destructive : Theme.tertiaryText
                visible: root.hasEditor && CodeEditor.status !== ""
            }
            Text {
                width: parent.width - 15 * Theme.unit
                text: {
                    if (!root.hasEditor) {
                        return "";
                    }
                    var parts = [CodeEditor.kind];
                    if (CodeEditor.where !== "") {
                        parts.push(CodeEditor.where);
                    }
                    if (CodeEditor.entityName !== "" && CodeEditor.entityName !== CodeEditor.fileName) {
                        parts.push(CodeEditor.entityName);
                    }
                    if (CodeEditor.claudeWriting) {
                        parts.push("Claude is writing…");
                    } else if (CodeEditor.status !== "") {
                        parts.push(CodeEditor.status);
                    }
                    return parts.join("  ·  ");
                }
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Theme.secondaryText
                font.pixelSize: Theme.detailSize
            }
        }

        // a save's answer, or why it can't be changed here
        Text {
            width: parent.width
            visible: text !== ""
            text: !root.hasEditor ? ""
                : CodeEditor.note !== "" ? CodeEditor.note
                : (!CodeEditor.canSave && CodeEditor.problem === "" ? CodeEditor.readOnlyReason : "")
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            maximumLineCount: 3
            color: root.hasEditor && CodeEditor.noteIsProblem ? Theme.destructive : Theme.secondaryText
            font.pixelSize: Theme.detailSize
        }
    }

    Menu {
        id: moreMenu
        MenuItem {
            text: "Show in Inspector"
            onTriggered: CodeEditor.showInInspector()
        }
        MenuItem {
            text: "Copy Link"
            enabled: root.hasEditor && CodeEditor.hasLink
            onTriggered: CodeEditor.copyLink()
        }
    }

    // ---- nothing open, or it couldn't open ----

    Text {
        anchors.centerIn: parent
        width: parent.width - 2 * Theme.pageMargin
        visible: !root.showPage
        text: !root.hasEditor ? ""
            : !CodeEditor.hasDocument ? "Open a script from the Inspector: press Edit beside its Script, or double-click it."
            : CodeEditor.problem
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        color: Theme.secondaryText
        font.pixelSize: Theme.bodySize
    }

    // ---- the editor ----

    Loader {
        id: loader
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: bar.bottom
        anchors.bottom: parent.bottom
        anchors.topMargin: 8 * Theme.unit
        active: root.showPage
        onActiveChanged: {
            if (!active) {
                root.pageReady = false;
            }
        }
        sourceComponent: WebEngineView {
            id: view
            Component.onCompleted: view.url = root.pageUrl
            backgroundColor: "#171b2d"   // the editor's own (code.css), behind it while it loads

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
