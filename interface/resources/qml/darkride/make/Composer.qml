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
import "../panel"
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
    readonly property bool canSend: connected && !working && (input.text.trim() !== "" || pictures.length > 0)

    implicitHeight: column.implicitHeight + 20 * Theme.unit
    color: Theme.card

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
        color: Theme.separator
    }

    ColumnLayout {
        id: column
        x: 14 * Theme.unit
        y: 10 * Theme.unit
        width: parent.width - 28 * Theme.unit
        spacing: 8 * Theme.unit

        // while Claude works: Stop, and how long and how much so far
        RowLayout {
            Layout.fillWidth: true
            visible: composer.working
            spacing: 10 * Theme.unit
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
                color: Theme.secondaryText
                font.pixelSize: Theme.detailSize
                elide: Text.ElideRight
            }
        }

        // the pictures going with the next request, as thumbnails (MakeDock.pictureView), each with its ×
        Flow {
            Layout.fillWidth: true
            visible: !composer.terminalRoute && !composer.atLimit && composer.pictures.length > 0
            spacing: 6 * Theme.unit
            Repeater {
                model: composer.pictures
                delegate: Rectangle {
                    id: thumb
                    required property string modelData
                    required property int index
                    width: 64 * Theme.unit
                    height: 48 * Theme.unit
                    radius: 6 * Theme.unit
                    color: Theme.background
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
                        anchors.margins: 3 * Theme.unit
                        width: 16 * Theme.unit
                        height: 16 * Theme.unit
                        radius: width / 2
                        color: "#99000000"
                        Accessible.role: Accessible.Button
                        Accessible.name: "Remove picture " + (thumb.index + 1)
                        Text {
                            anchors.centerIn: parent
                            text: "×"
                            textFormat: Text.PlainText
                            color: "#ffffff"
                            font.pixelSize: 12 * Theme.unit
                        }
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -4 * Theme.unit
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
            spacing: 10 * Theme.unit
            Text {
                Layout.fillWidth: true
                text: composer.dock ? composer.dock.reason : ""
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Theme.detailSize + 0.5 * Theme.unit
                wrapMode: Text.Wrap
            }
            MakeButton {
                kind: "primary"
                text: "Connect"
                onClicked: if (composer.dock) composer.dock.connect()
            }
        }

        // your API key route: the text box, add a picture, send
        RowLayout {
            Layout.fillWidth: true
            visible: !composer.terminalRoute && !composer.atLimit
            spacing: 8 * Theme.unit

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(Math.max(input.implicitHeight, 36 * Theme.unit), 120 * Theme.unit)
                radius: 10 * Theme.unit
                color: Theme.background
                border.width: input.activeFocus ? 1 : 0
                border.color: Theme.tint

                QQC2.ScrollView {
                    anchors.fill: parent
                    QQC2.TextArea {
                        id: input
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.Wrap
                        placeholderText: composer.connected ? "Make…" : "Make starts when your Claude is ready"
                        placeholderTextColor: Theme.tertiaryText
                        color: Theme.text
                        selectionColor: Theme.tint
                        selectedTextColor: "#ffffff"
                        font.pixelSize: Theme.bodySize
                        leftPadding: 12 * Theme.unit
                        rightPadding: 12 * Theme.unit
                        topPadding: 9 * Theme.unit
                        bottomPadding: 9 * Theme.unit
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
                implicitWidth: 36 * Theme.unit
                implicitHeight: 36 * Theme.unit
                radius: 10 * Theme.unit
                text: "+"
                Accessible.name: "Add a picture"
                enabled: composer.connected
                onClicked: composer.addPictures()
            }
            MakeButton {
                Layout.alignment: Qt.AlignBottom
                implicitHeight: 36 * Theme.unit
                radius: 10 * Theme.unit
                kind: "primary"
                text: "Make"
                enabled: composer.canSend
                onClicked: composer.send()
            }
        }
        Text {
            Layout.fillWidth: true
            visible: !composer.terminalRoute && !composer.atLimit && (composer.pictures.length > 0 || composer.pictureNote !== "")
            text: composer.pictureNote !== "" ? composer.pictureNote : "Pictures you add are sent to Claude to guide the build."
            textFormat: Text.PlainText
            color: Theme.tertiaryText
            font.pixelSize: Theme.detailSize
            wrapMode: Text.Wrap
        }

        // the Terminal route: you talk to Claude there
        Text {
            Layout.fillWidth: true
            visible: composer.terminalRoute && !composer.atLimit
            text: "You talk to Claude in Terminal. What it does shows here."
            textFormat: Text.PlainText
            color: Theme.secondaryText
            font.pixelSize: Theme.detailSize
            wrapMode: Text.Wrap
        }
    }
}
