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
    // the Claude-plan route's chat (MAKE-CHANNEL-PLAN.md): a Claude Code started with another line was refused (C9);
    // the first message not taken up in 30 s; a Claude Code started without the chat line; an old Claude Code
    readonly property bool refusedLine: linkState === "listening" && !!dock && !!dock.reason
    readonly property bool chatting: terminalRoute && !!dock && dock.chat === true
    readonly property bool notGotIt: chatting && linkState === "connected" && dock.notGotIt === true
    readonly property bool plainClaude: chatting && linkState === "connected" && dock.channelFlag === "off"
    readonly property bool oldClaude: linkState === "connected" && !!dock && olderThan(dock.claudeVersion || "", "2.1.319")

    // a version older than the one the kits ran on ("2.1.248" < "2.1.319"); "" isn't older
    function olderThan(version, floor) {
        if (version === "") {
            return false;
        }
        var a = version.split(/[^0-9]+/).filter(function (part) { return part !== ""; }).map(Number);
        var b = floor.split(".").map(Number);
        for (var i = 0; i < b.length; i++) {
            var x = i < a.length ? a[i] : 0;
            if (x !== b[i]) {
                return x < b[i];
            }
        }
        return false;
    }

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
                // the status: a dot (green connected, grey waiting) and its words, in one place (mary's mock)
                Rectangle {
                    visible: status.text !== ""
                    implicitWidth: 7
                    implicitHeight: 7
                    radius: 3.5
                    color: root.refusedLine ? BuildTheme.problem
                        : root.linkState === "connected" ? BuildTheme.running : BuildTheme.dimText
                }
                Text {
                    id: status
                    Layout.fillWidth: true
                    text: root.refusedLine ? root.dock.reason
                        : root.linkState === "connected" && root.dock && root.dock.sentWaiting ? "Sent, waiting for Claude"
                        : root.linkState === "connected" ? "Connected"
                        : root.linkState === "starting" ? "Starting your Claude…"
                        : root.linkState === "listening" ? "Waiting for Claude in Terminal" : ""
                    textFormat: Text.PlainText
                    color: root.refusedLine ? BuildTheme.problem
                        : root.linkState === "connected" ? BuildTheme.text : BuildTheme.mutedText
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

        // what the chat needs, under the header: one line and its way on (plain words; security's and docs' paths)
        Rectangle {
            Layout.fillWidth: true
            visible: root.notGotIt || root.plainClaude || root.oldClaude
            implicitHeight: notice.implicitHeight + 16
            color: BuildTheme.panel
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: BuildTheme.edge }
            ColumnLayout {
                id: notice
                x: 10
                y: 8
                width: parent.width - 20
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: root.notGotIt
                        ? "Claude didn't get your message. On a Team or Enterprise plan an Owner turns on Channels "
                          + "(Organization settings > Claude Code > Channels). Or use your API key, or type in Terminal."
                        : root.plainClaude
                        ? "This Claude was started without the chat. Type in Terminal, or start it with Make's line again."
                        : "Update Claude Code: in Terminal, claude update"
                    textFormat: Text.PlainText
                    color: root.notGotIt || root.plainClaude ? BuildTheme.text : BuildTheme.mutedText
                    font.pixelSize: BuildTheme.textSize
                    wrapMode: Text.WordWrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.notGotIt || root.plainClaude
                    spacing: 6
                    Item { Layout.fillWidth: true }
                    MakeButton {
                        visible: root.notGotIt
                        text: "Use API Key"
                        onClicked: if (root.dock && root.dock.setRoute("key")) root.dock.disconnect()
                    }
                    MakeButton {
                        text: "Type in Terminal"
                        onClicked: if (root.dock && root.dock.setChat(false)) root.dock.disconnect()
                    }
                }
            }
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
