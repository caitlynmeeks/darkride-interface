//
//  MakePanel.qml
//  qml/darkride
//
//  The Make dock (Make v1, make-dock-1): build and script in this world with your own Claude. Connect first (what's
//  on this computer, how Make reaches Claude, the Allow); then one timeline of what you asked, what Claude does, what it
//  asks and what it made, with Stop and Undo; the Made tab keeps this session's things. Native QML on Qt 6 only (Make
//  ships with the Qt 6 app), drawn in the panels' Theme so the schemes and skins apply.
//
//  It sees one object, MakeDock (editor-ui's interface/src/make/MakeDock, set on this dock's own context only;
//  team/editor-ui/MAKE-DOCK-CONTRACT.md), and calls nothing else but Panels. MakeDock cleans every string before it
//  gets here, and every Text and TextEdit here is plain text. MakeDock checks that Allow, Deny, Send and Undo come
//  from real input in this window; this file only calls it.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import "panel"
import "make"

Rectangle {
    id: root

    signal sendToScript(var message)   // the dock's loader connects it; nothing is sent

    // the dock's object; a preview or test hands in a stand-in instead
    property var dock: typeof MakeDock !== "undefined" ? MakeDock : null
    property string tab: "now"   // "now" or "made"

    readonly property string linkState: dock ? dock.state : "off"
    readonly property bool showConnect: linkState === "off" || linkState === "failed"
    readonly property bool terminalRoute: dock ? dock.route === "terminal" : false

    anchors.fill: parent
    color: Theme.background

    ConnectView {
        anchors.fill: parent
        visible: root.showConnect
        dock: root.dock
    }

    ColumnLayout {
        anchors.fill: parent
        visible: !root.showConnect
        spacing: 0

        // the header: the name, how it's connected, Now or Made, and the more menu (Disconnect)
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 14 * Theme.unit
            Layout.rightMargin: 8 * Theme.unit
            Layout.topMargin: 8 * Theme.unit
            Layout.bottomMargin: 8 * Theme.unit
            spacing: 10 * Theme.unit

            Text {
                text: "Make"
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: 16 * Theme.unit
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                text: root.linkState === "connected" ? "● Connected"
                    : root.linkState === "starting" ? "Starting your Claude…"
                    : root.linkState === "listening" ? "Waiting for Claude in Terminal" : ""
                textFormat: Text.PlainText
                color: root.linkState === "connected" ? Theme.on : Theme.secondaryText
                font.pixelSize: Theme.detailSize
                elide: Text.ElideRight
            }
            Segmented {
                model: [{ text: "Now", value: "now" }, { text: "Made", value: "made" }]
                current: root.tab
                onChosen: function (value) { root.tab = value; }
            }
            MakeButton {
                id: moreButton
                text: "•••"
                implicitWidth: 34 * Theme.unit
                color: "transparent"
                Accessible.name: "More"
                onClicked: morePopup.open()

                QQC2.Popup {
                    id: morePopup
                    y: moreButton.height + 4 * Theme.unit
                    x: moreButton.width - width
                    padding: 6 * Theme.unit
                    background: Rectangle {
                        radius: Theme.cardRadius
                        color: Theme.card
                        border.width: 1
                        border.color: Theme.separator
                    }
                    contentItem: ColumnLayout {
                        spacing: 4 * Theme.unit
                        MakeButton {
                            Layout.fillWidth: true
                            text: "Disconnect"
                            onClicked: {
                                morePopup.close();
                                if (root.dock) {
                                    root.dock.disconnect();
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.separator
        }

        NowView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.tab === "now"
            dock: root.dock
            onExampleChosen: function (text) { composer.prefill(text); }
        }

        MadeView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.tab === "made"
            dock: root.dock
        }

        Composer {
            id: composer
            objectName: "makeComposer"   // a test or preview finds it by name
            Layout.fillWidth: true
            dock: root.dock
            onSent: root.tab = "now"
        }
    }
}
