//
//  ConnectView.qml
//  qml/darkride/make
//
//  Make's first view, until it's connected: what's on this computer (Claude Code, Blender), how Make reaches your
//  Claude (your API key, kept in the system's key store, or Terminal with your Claude plan), what Make sets up on this
//  computer (shown before Allow, security A5), and the Allow. Shown again after Disconnect or a failure, with the reason.
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
import "../panel"

Rectangle {
    id: view

    property var dock: null

    readonly property bool claudeFound: dock ? dock.claudeFound : false
    readonly property bool blenderFound: dock ? dock.blenderFound : false
    // why Blender isn't offered, in one plain line ("" when it is): e.g. there, but it can't be closed off here
    readonly property string blenderWhy: dock && dock.blenderWhy ? dock.blenderWhy : ""
    readonly property string route: dock ? dock.route : "key"
    readonly property bool hasKey: dock ? dock.hasKey : false
    // the set-up text is open, and can't be folded, the first time and whenever it changed since the last Allow
    // (editor-ui 11-06); MakeDock says which (setupChanged)
    readonly property bool setupMustShow: !dock || dock.setupChanged === undefined || dock.setupChanged
    property bool setupOpen: false
    readonly property bool canAllow: claudeFound && (route === "terminal" || hasKey)
    property string keyError: ""

    color: Theme.background

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
    }

    QQC2.ScrollView {
        id: scroll
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true
        Component.onCompleted: contentItem.boundsBehavior = Flickable.StopAtBounds

        ColumnLayout {
            x: 16 * Theme.unit
            width: scroll.availableWidth - 32 * Theme.unit
            spacing: 18 * Theme.unit

            Item { Layout.preferredHeight: 2 * Theme.unit }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4 * Theme.unit
                Text {
                    text: "Make"
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: 20 * Theme.unit
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    text: "Build and script in this world with your own Claude."
                    textFormat: Text.PlainText
                    color: Theme.secondaryText
                    font.pixelSize: Theme.bodySize
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    visible: !!view.dock && view.dock.state === "failed" && view.dock.reason !== ""
                    text: view.dock ? view.dock.reason : ""
                    textFormat: Text.PlainText
                    color: Theme.destructive
                    font.pixelSize: Theme.bodySize
                    wrapMode: Text.WordWrap
                    topPadding: 6 * Theme.unit
                }
            }

            // what's on this computer
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6 * Theme.unit
                Heading { text: "On this computer" }
                Card {
                    Layout.fillWidth: true
                    FoundRow {
                        name: "Claude Code"
                        found: view.claudeFound
                        detail: view.claudeFound ? "Found." : "Not found. Install Claude Code, then come back to Make."
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.separator }
                    FoundRow {
                        name: "Blender"
                        found: view.blenderFound && view.blenderWhy === ""
                        optional: true
                        detail: view.blenderWhy !== "" ? view.blenderWhy
                            : view.blenderFound ? "Found. Needed for models; building and scripts work without it."
                            : "Not found. Needed only for models; building and scripts work without it."
                    }
                }
            }

            // how Make reaches your Claude
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6 * Theme.unit
                Heading { text: "How Make reaches your Claude" }
                Card {
                    Layout.fillWidth: true
                    RouteRow {
                        chosen: view.route === "key"
                        name: "With your API key"
                        detail: "Everything happens here in Darkride. You pay Anthropic for what you use; each ask shows its cost."
                        onPicked: if (view.dock) view.dock.setRoute("key")
                    }
                    // the key: a field until one is saved, then only that it's saved
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 28 * Theme.unit
                        visible: view.route === "key"
                        spacing: 6 * Theme.unit

                        RowLayout {
                            Layout.fillWidth: true
                            visible: !view.hasKey
                            spacing: 8 * Theme.unit
                            Field {
                                id: keyField
                                Layout.fillWidth: true
                                echoMode: TextInput.Password
                                placeholderText: "API key"
                                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                                Accessible.name: "API key"
                                onAccepted: view.saveKey()
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
                            spacing: 8 * Theme.unit
                            Text {
                                Layout.fillWidth: true
                                text: "Key saved"
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.pixelSize: Theme.bodySize
                            }
                            MakeButton {
                                text: "Forget"
                                onClicked: if (view.dock) view.dock.forgetKey()
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: view.keyError !== "" ? view.keyError
                                : "Kept in " + (view.dock ? view.dock.keyStoreName : "") + ", for this computer only. Never shown again."
                            textFormat: Text.PlainText
                            color: view.keyError !== "" ? Theme.destructive : Theme.secondaryText
                            font.pixelSize: Theme.detailSize
                            wrapMode: Text.WordWrap
                        }
                    }
                    Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.separator }
                    RouteRow {
                        chosen: view.route === "terminal"
                        name: "In Terminal, with your Claude plan"
                        detail: "You talk to Claude in Terminal. Make shows the work here, with Stop and Undo."
                        onPicked: if (view.dock) view.dock.setRoute("terminal")
                    }
                }
            }

            // the Allow, with what Make sets up on this computer shown before it
            Card {
                Layout.fillWidth: true
                spacing: 10 * Theme.unit

                Text {
                    Layout.fillWidth: true
                    text: "Allow Make to build here as you?"
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: 15 * Theme.unit
                    font.weight: Font.DemiBold
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    // both routes, security's final words (11-07); the Blender clause only while MakeDock says Blender
                    // runs sealed off (blenderSealed)
                    text: "Claude can add, change and remove things you could change yourself, and write their scripts. "
                        + "It can't open your files, run commands on your computer or browse the web. "
                        + (view.dock && view.dock.blenderSealed
                           ? "To make models it uses Blender, closed off from the internet and your files, and it asks "
                             + "before a script it writes uses the web."
                           : "It asks before a script it writes uses the web.")
                    textFormat: Text.PlainText
                    color: Theme.secondaryText
                    font.pixelSize: Theme.bodySize
                    wrapMode: Text.WordWrap
                    lineHeight: 1.15
                }

                // what Make writes on this computer (security A5): open before the first Allow and after any change
                Text {
                    Layout.fillWidth: true
                    visible: !view.setupMustShow
                    text: (view.setupOpen ? "▾  " : "▸  ") + "What Make sets up on this computer"
                    textFormat: Text.PlainText
                    color: Theme.tint
                    font.pixelSize: Theme.detailSize
                    Accessible.role: Accessible.Button
                    Accessible.name: "What Make sets up on this computer"
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: view.setupOpen = !view.setupOpen
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: view.setupMustShow
                    text: "What Make sets up on this computer"
                    textFormat: Text.PlainText
                    color: Theme.secondaryText
                    font.pixelSize: Theme.detailSize
                    font.weight: Font.DemiBold
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(setupText.implicitHeight + 16 * Theme.unit, 150 * Theme.unit)
                    visible: view.setupMustShow || view.setupOpen
                    radius: 8 * Theme.unit
                    color: Theme.background
                    clip: true

                    Flickable {
                        anchors.fill: parent
                        anchors.margins: 8 * Theme.unit
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
                            color: Theme.secondaryText
                            selectionColor: Theme.tint
                            font.family: "Menlo"
                            font.pixelSize: 11 * Theme.unit
                            Keys.onEscapePressed: if (view.dock) view.dock.giveBackKeyboard()
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    visible: !view.claudeFound || (view.route === "key" && !view.hasKey)
                    text: !view.claudeFound ? "Make needs Claude Code on this computer."
                        : "Save your API key, or choose Terminal."
                    textFormat: Text.PlainText
                    color: Theme.secondaryText
                    font.pixelSize: Theme.detailSize
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 2 * Theme.unit
                    spacing: 8 * Theme.unit
                    MakeButton {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1   // the pair splits the row in equal halves
                        text: "Don't Allow"
                        onClicked: if (typeof Panels !== "undefined") Panels.close("Make")
                    }
                    MakeButton {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        kind: "primary"
                        text: "Allow"
                        enabled: view.canAllow
                        onClicked: if (view.dock) view.dock.connect()
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.bottomMargin: 16 * Theme.unit
                text: "Nothing runs until you ask. Disconnect any time from this panel."
                textFormat: Text.PlainText
                color: Theme.tertiaryText
                font.pixelSize: Theme.detailSize
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }

    // a found-or-not row in "On this computer"
    component FoundRow: RowLayout {
        id: foundRow
        property string name: ""
        property string detail: ""
        property bool found: false
        property bool optional: false
        Layout.fillWidth: true
        spacing: 10 * Theme.unit
        Text {
            Layout.alignment: Qt.AlignTop
            Layout.preferredWidth: 14 * Theme.unit   // the names line up whichever mark it is
            horizontalAlignment: Text.AlignHCenter
            text: foundRow.found ? "✓" : foundRow.optional ? "–" : "✕"
            textFormat: Text.PlainText
            color: foundRow.found ? Theme.on : foundRow.optional ? Theme.secondaryText : Theme.destructive
            font.pixelSize: Theme.bodySize
            font.weight: Font.DemiBold
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2 * Theme.unit
            Text {
                text: foundRow.name
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.bodySize
                font.weight: Font.Medium
            }
            Text {
                Layout.fillWidth: true
                text: foundRow.detail
                textFormat: Text.PlainText
                color: Theme.secondaryText
                font.pixelSize: Theme.detailSize
                wrapMode: Text.WordWrap
            }
        }
    }

    // one of the two ways to reach Claude: a round mark, its name and what it means; the whole row picks it
    component RouteRow: RowLayout {
        id: routeRow
        property string name: ""
        property string detail: ""
        property bool chosen: false
        signal picked()
        Layout.fillWidth: true
        spacing: 10 * Theme.unit
        activeFocusOnTab: true
        Accessible.role: Accessible.RadioButton
        Accessible.name: name
        Accessible.checked: chosen
        Keys.onSpacePressed: routeRow.picked()

        Rectangle {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 2 * Theme.unit
            implicitWidth: 16 * Theme.unit
            implicitHeight: 16 * Theme.unit
            radius: width / 2
            color: "transparent"
            border.width: routeRow.chosen ? 5 * Theme.unit : 1.5 * Theme.unit
            border.color: routeRow.chosen ? Theme.tint : Theme.secondaryText
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2 * Theme.unit
            Text {
                text: routeRow.name
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.bodySize
                font.weight: Font.Medium
            }
            Text {
                Layout.fillWidth: true
                text: routeRow.detail
                textFormat: Text.PlainText
                color: Theme.secondaryText
                font.pixelSize: Theme.detailSize
                wrapMode: Text.WordWrap
                lineHeight: 1.1
            }
        }
        TapHandler {
            onTapped: routeRow.picked()
        }
    }
}
