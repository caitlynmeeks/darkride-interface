//
//  ConnectView.qml
//  qml/darkride/make
//
//  Make's first view, until it's connected: what's on this computer (Claude Code, Blender), how Make reaches your
//  Claude (your API key, kept in the system's key store, or Terminal with your Claude plan), and the Allow, with what
//  Make sets up on this computer behind Details (open before the first Allow and whenever it changed: security A5).
//  After an Allow, while what Make sets up is unchanged, the Allow is remembered: one line and Connect (Caity's try
//  11-07, "do i have to see this every session"). Shown again after Disconnect or a failure, with the reason. Drawn
//  in Build mode's look (mary's MAKE-PANEL-LOOK.md).
//
//  The key field is cleared the moment Save is pressed, whatever MakeDock answers; only "Key saved" is ever shown.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import "../editor"

Rectangle {
    id: view

    property var dock: null

    readonly property bool claudeFound: dock ? dock.claudeFound : false
    readonly property bool blenderFound: dock ? dock.blenderFound : false
    // why Blender isn't offered, in one plain line ("" when it is): e.g. there, but it can't be closed off here
    readonly property string blenderWhy: dock && dock.blenderWhy ? dock.blenderWhy : ""
    readonly property string route: dock ? dock.route : "key"
    readonly property bool hasKey: dock ? dock.hasKey : false
    // what Make sets up changed since the last Allow (or there was none): MakeDock says which (setupChanged); then
    // the whole Allow shows, its Details open and unfoldable (security A5). Else the Allow is remembered.
    readonly property bool setupMustShow: !dock || dock.setupChanged === undefined || dock.setupChanged
    property bool detailsOpen: false
    readonly property bool canAllow: claudeFound && (route === "terminal" || hasKey)
    property string keyError: ""

    color: BuildTheme.panel

    function saveKey() {
        var text = keyField.text;
        keyField.text = "";   // at once, whatever the answer
        if (!dock || text.trim() === "") {
            return;
        }
        var answer = dock.storeKey(text);
        view.keyError = typeof answer === "string" ? answer : "";
    }

    onVisibleChanged: {
        keyField.text = "";
        keyError = "";
        detailsOpen = false;
    }

    // the bottom line, as the Now tab's and the Code panel's
    Rectangle {
        id: bottomLine
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        implicitHeight: bottomText.implicitHeight + 12
        color: BuildTheme.panel
        Rectangle { width: parent.width; height: 1; color: BuildTheme.edge }
        Text {
            id: bottomText
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 20
            text: "Nothing runs until you ask. Disconnect any time from the ••• menu."
            textFormat: Text.PlainText
            color: BuildTheme.mutedText
            font.pixelSize: BuildTheme.noteSize
            wrapMode: Text.WordWrap
        }
    }

    QQC2.ScrollView {
        id: scroll
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: bottomLine.top
        contentWidth: availableWidth
        clip: true
        Component.onCompleted: contentItem.boundsBehavior = Flickable.StopAtBounds

        ColumnLayout {
            width: scroll.availableWidth
            spacing: 0

            Text {
                Layout.fillWidth: true
                Layout.margins: 10
                text: "Build and script in this world with your own Claude."
                textFormat: Text.PlainText
                color: BuildTheme.text
                font.pixelSize: BuildTheme.textSize
                wrapMode: Text.WordWrap
            }
            // why the last Connect failed, when it did
            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 10
                Layout.rightMargin: 10
                Layout.bottomMargin: 10
                visible: !!view.dock && view.dock.state === "failed" && view.dock.reason !== ""
                text: view.dock ? view.dock.reason : ""
                textFormat: Text.PlainText
                color: BuildTheme.problem
                font.pixelSize: BuildTheme.textSize
                wrapMode: Text.WordWrap
            }

            // what's on this computer
            Heading { text: "On this computer" }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: 10
                spacing: 6
                FoundRow {
                    name: "Claude Code"
                    found: view.claudeFound
                    status: view.claudeFound ? "Found" : "Not found"
                    detail: view.claudeFound ? "" : "Install Claude Code, then come back to Make."
                }
                FoundRow {
                    name: "Blender"
                    found: view.blenderFound && view.blenderWhy === ""
                    optional: true
                    status: view.blenderWhy !== "" ? "Not available" : view.blenderFound ? "Found" : "Not found"
                    detail: view.blenderWhy !== "" ? view.blenderWhy
                        : view.blenderFound ? "" : "Needed for models; everything else works."
                }
            }

            // how Make reaches your Claude
            Heading { text: "How Make reaches your Claude" }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: 10
                spacing: 8
                RouteRow {
                    chosen: view.route === "key"
                    name: "With your API key"
                    detail: "You type here and pay per use; each ask shows its cost."
                    onPicked: if (view.dock) view.dock.setRoute("key")
                }
                // the key: a field until one is saved, then only that it's saved
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 24
                    visible: view.route === "key"
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        visible: !view.hasKey
                        spacing: 6
                        QQC2.TextField {
                            id: keyField
                            Layout.fillWidth: true
                            implicitHeight: 23
                            echoMode: TextInput.Password
                            placeholderText: "API key"
                            placeholderTextColor: BuildTheme.dimText
                            color: BuildTheme.text
                            selectionColor: BuildTheme.selection
                            selectedTextColor: "#ffffff"
                            font.pixelSize: BuildTheme.textSize
                            leftPadding: 6
                            rightPadding: 6
                            topPadding: 2
                            bottomPadding: 2
                            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                            Accessible.name: "API key"
                            background: Rectangle {
                                radius: BuildTheme.radius
                                color: BuildTheme.field
                                border.width: 1
                                border.color: keyField.activeFocus ? BuildTheme.selection : BuildTheme.fieldEdge
                            }
                            onAccepted: view.saveKey()
                            Keys.onEscapePressed: if (view.dock) view.dock.giveBackKeyboard()
                        }
                        MakeButton {
                            text: "Save"
                            enabled: keyField.text !== ""
                            onClicked: view.saveKey()
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: view.hasKey
                        spacing: 6
                        Text {
                            Layout.fillWidth: true
                            text: "Key saved"
                            textFormat: Text.PlainText
                            color: BuildTheme.text
                            font.pixelSize: BuildTheme.textSize
                        }
                        MakeButton {
                            text: "Forget"
                            onClicked: if (view.dock) view.dock.forgetKey()
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: view.keyError !== "" ? view.keyError
                            : "Kept in " + (view.dock ? view.dock.keyStoreName : "") + ", for this computer only."
                        textFormat: Text.PlainText
                        color: view.keyError !== "" ? BuildTheme.problem : BuildTheme.mutedText
                        font.pixelSize: BuildTheme.noteSize
                        wrapMode: Text.WordWrap
                    }
                }
                RouteRow {
                    chosen: view.route === "terminal"
                    name: "In Terminal, with your Claude plan"
                    detail: "You type in Terminal; the work shows here."
                    onPicked: if (view.dock) view.dock.setRoute("terminal")
                }
            }

            // the Allow: asked the first time and whenever what it covers changed (MakeDock.allowHash); else remembered:
            // the route and what Claude can't do in one line, Connect, and the rest under Details (security R5)
            Heading { text: view.setupMustShow ? "Allow Make to build here as you?" : "Allowed" }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: 10
                spacing: 8

                Text {
                    Layout.fillWidth: true
                    visible: !view.setupMustShow
                    text: "Make can build as you, with " + (view.route === "terminal" ? "your Claude plan" : "your API key")
                        + ". It can't open your files, run commands on this computer or browse the web."
                    textFormat: Text.PlainText
                    color: BuildTheme.text
                    font.pixelSize: BuildTheme.textSize
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    visible: !view.setupMustShow
                    text: (view.detailsOpen ? "▾  " : "▸  ") + "Details"
                    textFormat: Text.PlainText
                    color: BuildTheme.minorText
                    font.pixelSize: BuildTheme.textSize
                    Accessible.role: Accessible.Button
                    Accessible.name: "Details"
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: view.detailsOpen = !view.detailsOpen
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: view.setupMustShow || view.detailsOpen
                    // both routes, security's final words (11-07); the Blender clause only while MakeDock says Blender
                    // runs sealed off (blenderSealed). A change here: a new ALLOW_WORDS in MakeDock.cpp (security R2)
                    text: "Claude can add, change and remove things you could change yourself, and write their scripts. "
                        + "It can't open your files, run commands on your computer or browse the web. "
                        + (view.dock && view.dock.blenderSealed
                           ? "To make models it uses Blender, closed off from the internet and your files, and it asks "
                             + "before a script it writes uses the web."
                           : "It asks before a script it writes uses the web.")
                    textFormat: Text.PlainText
                    color: BuildTheme.mutedText
                    font.pixelSize: BuildTheme.textSize
                    wrapMode: Text.WordWrap
                    lineHeight: 1.1
                }

                // what Make writes on this computer (security A5): open before the first Allow and after any change
                Text {
                    Layout.fillWidth: true
                    visible: view.setupMustShow || view.detailsOpen
                    text: "What Make sets up on this computer"
                    textFormat: Text.PlainText
                    color: BuildTheme.minorText
                    font.pixelSize: BuildTheme.noteSize
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(setupText.implicitHeight + 12, 150)
                    visible: view.setupMustShow || view.detailsOpen
                    radius: BuildTheme.radius
                    color: BuildTheme.field
                    border.width: 1
                    border.color: BuildTheme.fieldEdge
                    clip: true

                    Flickable {
                        anchors.fill: parent
                        anchors.margins: 6
                        contentHeight: setupText.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds
                        QQC2.ScrollBar.vertical: QQC2.ScrollBar {}
                        TextEdit {
                            id: setupText
                            width: parent.width
                            text: view.dock ? view.dock.setupText : ""
                            textFormat: TextEdit.PlainText
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.WrapAnywhere
                            color: BuildTheme.mutedText
                            selectionColor: BuildTheme.selection
                            selectedTextColor: "#ffffff"
                            font.family: BuildTheme.mono
                            font.pixelSize: BuildTheme.noteSize
                            // Copy here copies the whole set-up text (MakeDock.copyLine), not the app's menu's Copy
                            Keys.onShortcutOverride: function (event) {
                                if (event.matches(StandardKey.Copy)) {
                                    event.accepted = true;
                                }
                            }
                            Keys.onPressed: function (event) {
                                if (event.matches(StandardKey.Copy)) {
                                    if (view.dock) view.dock.copyLine("setup");
                                    event.accepted = true;
                                }
                            }
                            Keys.onEscapePressed: if (view.dock) view.dock.giveBackKeyboard()
                        }
                    }
                }
                // forget the Allow (security R3): the next Connect asks with the whole card
                RowLayout {
                    Layout.fillWidth: true
                    visible: !view.setupMustShow && view.detailsOpen
                    Item { Layout.fillWidth: true }
                    MakeButton {
                        text: "Stop Allowing"
                        onClicked: {
                            view.detailsOpen = false;
                            if (view.dock) view.dock.stopAllowing();
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: !view.claudeFound || (view.route === "key" && !view.hasKey)
                    text: !view.claudeFound ? "Make needs Claude Code on this computer."
                        : "Save your API key, or choose Terminal."
                    textFormat: Text.PlainText
                    color: BuildTheme.mutedText
                    font.pixelSize: BuildTheme.noteSize
                    wrapMode: Text.WordWrap
                }

                // Don't Allow, then Allow (macOS's order), at their own width on the right; remembered: Connect
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 2
                    spacing: 6
                    Item { Layout.fillWidth: true }
                    MakeButton {
                        visible: view.setupMustShow
                        text: "Don't Allow"
                        onClicked: if (typeof Panels !== "undefined") Panels.close("Make")
                    }
                    MakeButton {
                        kind: "primary"
                        text: view.setupMustShow ? "Allow" : "Connect"
                        enabled: view.canAllow
                        onClicked: if (view.dock) view.dock.connect()
                    }
                }
            }
        }
    }

    // a found-or-not row in "On this computer": the mark, the name, what was found on the right; why, under it
    component FoundRow: ColumnLayout {
        id: foundRow
        property string name: ""
        property string status: ""
        property string detail: ""
        property bool found: false
        property bool optional: false
        Layout.fillWidth: true
        spacing: 1
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text {
                Layout.preferredWidth: 14   // the names line up whichever mark it is
                horizontalAlignment: Text.AlignHCenter
                text: foundRow.found ? "✓" : foundRow.optional ? "–" : "✕"
                textFormat: Text.PlainText
                color: foundRow.found ? BuildTheme.running : foundRow.optional ? BuildTheme.mutedText : BuildTheme.problem
                font.pixelSize: BuildTheme.textSize
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                text: foundRow.name
                textFormat: Text.PlainText
                color: BuildTheme.text
                font.pixelSize: BuildTheme.textSize
            }
            Text {
                text: foundRow.status
                textFormat: Text.PlainText
                color: BuildTheme.mutedText
                font.pixelSize: BuildTheme.textSize
            }
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 22
            visible: foundRow.detail !== ""
            text: foundRow.detail
            textFormat: Text.PlainText
            color: BuildTheme.mutedText
            font.pixelSize: BuildTheme.noteSize
            wrapMode: Text.WordWrap
        }
    }

    // one of the two ways to reach Claude: a radio, its name and what it means; the whole row picks it
    component RouteRow: RowLayout {
        id: routeRow
        property string name: ""
        property string detail: ""
        property bool chosen: false
        signal picked()
        Layout.fillWidth: true
        spacing: 8
        activeFocusOnTab: true
        Accessible.role: Accessible.RadioButton
        Accessible.name: name
        Accessible.checked: chosen
        Keys.onSpacePressed: routeRow.picked()

        Rectangle {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 1
            implicitWidth: 14
            implicitHeight: 14
            radius: width / 2
            // macOS's own radio, as the Inspector's check box beside it (mary): #067dff chosen, #4c4c4c not
            color: routeRow.chosen ? "#067dff" : "#4c4c4c"
            border.width: routeRow.activeFocus ? 1 : 0
            border.color: BuildTheme.selection
            Rectangle {
                anchors.centerIn: parent
                visible: routeRow.chosen
                width: 6
                height: 6
                radius: 3
                color: "#ffffff"
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            Text {
                text: routeRow.name
                textFormat: Text.PlainText
                color: BuildTheme.text
                font.pixelSize: BuildTheme.textSize
            }
            Text {
                Layout.fillWidth: true
                text: routeRow.detail
                textFormat: Text.PlainText
                color: BuildTheme.mutedText
                font.pixelSize: BuildTheme.noteSize
                wrapMode: Text.WordWrap
            }
        }
        TapHandler {
            onTapped: routeRow.picked()
        }
    }
}
