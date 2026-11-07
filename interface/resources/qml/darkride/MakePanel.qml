//
//  MakePanel.qml
//  qml/darkride
//
//  The Make dock (Make v1, make-dock-1): build and script in this world with your own Claude. Connect first (what's
//  on this computer, how Make reaches Claude, the Allow); then one timeline of what you asked, what Claude does, what it
//  asks and what it made, with Stop and Undo; the Made tab keeps this session's things. Native QML on Qt 6 only (Make
//  ships with the Qt 6 app), drawn in Build mode's look (editor/BuildTheme.qml, shared with the Code panel; mary's
//  team/mary/brand/make-panel/MAKE-PANEL-LOOK.md), as it docks beside the Inspector.
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
import QtQuick.Layouts
import "editor"
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
    color: BuildTheme.panel

    ConnectView {
        anchors.fill: parent
        visible: root.showConnect
        dock: root.dock
    }

    ColumnLayout {
        anchors.fill: parent
        visible: !root.showConnect
        spacing: 0

        // the header, one line as the Code panel's: how it's connected on the left (no name: the tab and the dock's
        // title say "Make"), Now or Made and the more menu (the app's own menu: Disconnect) on the right
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 30
            color: BuildTheme.panel
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 6
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: root.linkState === "connected" ? "● Connected"
                        : root.linkState === "starting" ? "Starting your Claude…"
                        : root.linkState === "listening" ? "Waiting for Claude in Terminal" : ""
                    textFormat: Text.PlainText
                    color: root.linkState === "connected" ? BuildTheme.running : BuildTheme.mutedText
                    font.pixelSize: BuildTheme.textSize
                    elide: Text.ElideRight
                }
                BuildToggle {
                    text: "Now"
                    checked: root.tab === "now"
                    onToggled: root.tab = "now"
                }
                BuildToggle {
                    text: "Made"
                    checked: root.tab === "made"
                    onToggled: root.tab = "made"
                }
                MakeButton {
                    id: moreButton
                    text: "•••"
                    tip: "More"
                    Accessible.name: "More"
                    onClicked: {
                        if (root.dock && root.dock.showMoreMenu !== undefined) {
                            var at = moreButton.mapToGlobal(moreButton.width, moreButton.height + 2);
                            root.dock.showMoreMenu(at.x, at.y);
                        }
                    }
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: BuildTheme.edge }
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
