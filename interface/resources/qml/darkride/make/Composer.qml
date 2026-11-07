//
//  Composer.qml
//  qml/darkride/make
//
//  The foot of the Make dock. While Claude works: Stop, and how long and how much so far. On your API key route: the
//  text box (Return sends, Shift+Return is a new line, Esc gives the keyboard back to the world), the pictures you
//  add for it (the file chooser, or a drop on the Now view: MakeDock answers with picturesAdded), and Send; the only
//  place a request is sent from. On the Terminal route: where you talk to Claude instead. When a session has used its
//  spending limit (the `limit` state): MakeDock's line and Connect, which starts a fresh session.
//
//  The clock runs only while Claude works.
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

Rectangle {
    id: composer

    property var dock: null
    property var pictures: []   // waiting pictures' ids (picturesAdded), sent with the next request
    property string pictureNote: ""   // why a picture wasn't added (pictureRefused)
    property real workingSince: 0
    property real now: 0
    signal sent()

    readonly property bool connected: dock ? dock.state === "connected" : false
    readonly property bool atLimit: dock ? dock.state === "limit" : false
    readonly property bool working: dock ? dock.working : false
    readonly property bool terminalRoute: dock ? dock.route === "terminal" : false
    // the Claude-plan route chats here too, over the channel, unless the person chose to type in Terminal
    readonly property bool typesHere: !terminalRoute || (!!dock && dock.chat === true)
    readonly property bool canSend: connected && !working && (input.text.trim() !== "" || pictures.length > 0) &&
        (!terminalRoute || (!!dock && dock.channelFlag !== "off"))

    implicitHeight: column.implicitHeight + 16
    color: BuildTheme.panel

    function prefill(text) {
        input.text = text;
        input.cursorPosition = input.length;
        input.forceActiveFocus();
    }
    function send() {
        if (!canSend) {
            return;
        }
        // the request's number, or 0 when MakeDock refused it: then the text and the pictures stay
        if (dock.send(input.text.trim(), pictures) > 0) {
            input.text = "";
            pictures = [];
            pictureNote = "";
            sent();
        }
    }
    function addPictures() {
        pictureNote = "";
        if (dock) {
            dock.addPictures();   // the file chooser; the answer comes as picturesAdded or pictureRefused
        }
    }

    Connections {
        target: composer.dock
        ignoreUnknownSignals: true
        function onPicturesAdded(ids) {
            composer.pictureNote = "";
            composer.pictures = composer.pictures.concat(Array.prototype.slice.call(ids || []));
        }
        function onPictureRefused(reason) {
            composer.pictureNote = reason;
        }
    }

    onWorkingChanged: {
        if (working) {
            workingSince = Date.now();
            now = workingSince;
        }
    }
    Timer {
        interval: 1000
        repeat: true
        running: composer.working && composer.visible
        onTriggered: composer.now = Date.now()
    }

    Rectangle {
        width: parent.width
        height: 1
        color: BuildTheme.edge
    }

    ColumnLayout {
        id: column
        x: 10
        y: 8
        width: parent.width - 20
        spacing: 6

        // while Claude works: Stop, and how long and how much so far
        RowLayout {
            Layout.fillWidth: true
            visible: composer.working
            spacing: 10
            MakeButton {
                kind: "stop"
                text: "Stop"
                onClicked: if (composer.dock) composer.dock.stop()
            }
            Text {
                Layout.fillWidth: true
                text: {
                    var parts = ["Working for " + Format.duration(composer.now - composer.workingSince)];
                    var cost = Format.cost(composer.dock ? composer.dock.costSoFar : -1);
                    if (cost !== "") {
                        parts.push(cost + " so far");
                    }
                    return parts.join(" · ");
                }
                textFormat: Text.PlainText
                color: BuildTheme.mutedText
                font.pixelSize: BuildTheme.noteSize
                elide: Text.ElideRight
            }
        }

        // the pictures going with the next request, as thumbnails (MakeDock.pictureView), each with its ×
        Flow {
            Layout.fillWidth: true
            visible: composer.typesHere && !composer.atLimit && composer.pictures.length > 0
            spacing: 6
            Repeater {
                model: composer.pictures
                delegate: Rectangle {
                    id: thumb
                    required property string modelData
                    required property int index
                    width: 64
                    height: 48
                    radius: BuildTheme.radius
                    color: BuildTheme.field
                    clip: true
                    Image {
                        anchors.fill: parent
                        source: composer.dock ? Format.picture(composer.dock.pictureView(thumb.modelData)) : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize.width: 128
                        Accessible.role: Accessible.Graphic
                        Accessible.name: "Picture " + (thumb.index + 1)
                    }
                    // remove it: a round × in the corner
                    Rectangle {
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.margins: 3
                        width: 16
                        height: 16
                        radius: width / 2
                        color: "#99000000"
                        Accessible.role: Accessible.Button
                        Accessible.name: "Remove picture " + (thumb.index + 1)
                        Text {
                            anchors.centerIn: parent
                            text: "×"
                            textFormat: Text.PlainText
                            color: "#ffffff"
                            font.pixelSize: 12
                        }
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -4
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                var list = composer.pictures.slice();
                                list.splice(thumb.index, 1);
                                composer.pictures = list;
                            }
                        }
                    }
                }
            }
        }

        // the session's spending limit is used: MakeDock's line, and Connect for a fresh session
        RowLayout {
            Layout.fillWidth: true
            visible: composer.atLimit
            spacing: 10
            Text {
                Layout.fillWidth: true
                text: composer.dock ? composer.dock.reason : ""
                textFormat: Text.PlainText
                color: BuildTheme.text
                font.pixelSize: BuildTheme.textSize
                wrapMode: Text.Wrap
            }
            MakeButton {
                kind: "primary"
                text: "Connect"
                onClicked: if (composer.dock) composer.dock.connect()
            }
        }

        // the text box, add a picture, send: the API key route, and the Claude-plan route's chat
        RowLayout {
            Layout.fillWidth: true
            visible: composer.typesHere && !composer.atLimit
            spacing: 8

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(Math.max(input.implicitHeight, 23), 120)
                radius: BuildTheme.radius
                color: BuildTheme.field
                border.width: 1
                border.color: input.activeFocus ? BuildTheme.selection : BuildTheme.fieldEdge

                QQC2.ScrollView {
                    anchors.fill: parent
                    QQC2.TextArea {
                        id: input
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.Wrap
                        placeholderText: composer.connected ? "Make…" : "Make starts when your Claude is ready"
                        placeholderTextColor: BuildTheme.dimText
                        color: BuildTheme.text
                        selectionColor: BuildTheme.selection
                        selectedTextColor: "#ffffff"
                        font.pixelSize: BuildTheme.textSize
                        leftPadding: 7
                        rightPadding: 7
                        topPadding: 4
                        bottomPadding: 4
                        enabled: composer.connected
                        background: null
                        Accessible.name: "Ask Make"
                        Keys.onReturnPressed: function (event) {
                            if (event.modifiers & Qt.ShiftModifier) {
                                event.accepted = false;
                            } else {
                                composer.send();
                            }
                        }
                        Keys.onEnterPressed: function (event) { composer.send(); }
                        Keys.onEscapePressed: if (composer.dock) composer.dock.giveBackKeyboard()
                    }
                }
            }
            MakeButton {
                Layout.alignment: Qt.AlignBottom
                text: "+"
                tip: "Add a picture"
                Accessible.name: "Add a picture"
                enabled: composer.connected
                onClicked: composer.addPictures()
            }
            MakeButton {
                Layout.alignment: Qt.AlignBottom
                kind: "primary"
                text: "Make"
                enabled: composer.canSend
                onClicked: composer.send()
            }
        }
        Text {
            Layout.fillWidth: true
            visible: composer.typesHere && !composer.atLimit && (composer.pictures.length > 0 || composer.pictureNote !== "")
            text: composer.pictureNote !== "" ? composer.pictureNote : "Pictures you add are sent to Claude to guide the build."
            textFormat: Text.PlainText
            color: BuildTheme.dimText
            font.pixelSize: BuildTheme.noteSize
            wrapMode: Text.Wrap
        }

        // typing in Terminal (the fallback): you talk to Claude there
        Text {
            Layout.fillWidth: true
            visible: !composer.typesHere && !composer.atLimit
            text: "Ask Claude in Terminal. Its work shows here."
            textFormat: Text.PlainText
            color: BuildTheme.mutedText
            font.pixelSize: BuildTheme.noteSize
            wrapMode: Text.Wrap
        }
    }
}
