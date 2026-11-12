//
//  QueryDialog.qml
//
//  Created by Bradley Austin Davis on 22 Jan 2016
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

import QtQuick 2.12

import controlsUit 1.0
import stylesUit 1.0
import "../windows"
import "alert"
import "alert/standardButtons.js" as StandardButtons

// The desktop's text prompt, drawn as an alert: a title, the label as a line under it, then a text field, or a list
// when items are given (current is then the index of the item chosen at first). buttonTexts gives the buttons their
// own words ({ ok: "Rename" }); monospace shows the field in a fixed-width face, for paths and addresses.
ModalWindow {
    id: root
    visible: true
    keyboardOverride: true  // Disable ModalWindow's keyboard.
    frame: AlertShadow { }

    anchors.centerIn: undefined
    anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
    anchors.top: parent ? parent.top : undefined
    anchors.topMargin: parent ? Math.max(0, Math.round((parent.height - height) / 3)) : 0

    signal selected(var result);
    signal canceled();

    property int icon: 0  // kept for callers: every alert shows Darkride's picture
    property var items;
    property string label
    property var result;
    property var current
    property string placeholderText

    // For combo boxes
    property bool editable: true;

    property var buttonTexts: ({})
    property bool monospace: false
    property int titleWidth: 0  // ModalWindow's own frame sets it while the window is made
    // a world's script's prompt (interface's ui/WorldDialogs.h): its world's name; offerBlock shows the tick that stops
    // the world's dialogs until the visitor leaves it, read as blockWorld when the prompt is answered
    property string world: ""
    property bool offerBlock: false
    readonly property bool blockWorld: blockCheck.checked

    property bool keyboardEnabled: false
    property bool keyboardRaised: false
    property bool punctuationMode: false
    readonly property bool keyboardShown: keyboardEnabled && keyboardRaised

    width: keyboardShown ? Math.max(panel.width, keyboard.keyboardWidth) : panel.width
    height: panel.implicitHeight + (keyboardShown ? keyboard.raisedHeight + 8 : 0)

    function accept() {
        root.result = items ? menu.currentText : field.text;
        root.selected(root.result);
        root.destroy();
    }

    function cancel() {
        root.canceled();
        root.destroy();
    }

    // A window holds one content item, its last child, so all it shows goes in this one. ModalWindow.qml reads hifi.
    Item {
        width: root.width
        height: root.height

        HifiConstants { id: hifi }

        AlertPanel {
            id: panel
            x: parent ? Math.round((parent.width - width) / 2) : 0
            title: root.title
            text: root.label
            world: root.world
            buttons: StandardButtons.list(StandardButtons.OK | StandardButtons.CANCEL, StandardButtons.OK, 0, root.buttonTexts)
            onClicked: {
                if (value === StandardButtons.OK) {
                    root.accept();
                } else {
                    root.cancel();
                }
            }

            AlertField {
                id: field
                visible: !root.items
                monospace: root.monospace
                placeholderText: root.placeholderText
            }

            AlertMenu {
                id: menu
                visible: !!root.items
                model: root.items ? root.items : []
            }

            Text {
                visible: root.world !== ""
                width: parent.width
                text: "Darkride never asks for your password in a world's dialog."
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: panel.worldAmber
                font.pixelSize: 11 * panel.unit
            }

            AlertCheck {
                id: blockCheck
                objectName: "blockWorldCheck"
                visible: root.offerBlock
                width: parent.width
                text: "Don't let this world show more dialogs"
            }
        }

        Keyboard {
            id: keyboard
            raised: root.keyboardShown
            numeric: root.punctuationMode
            y: panel.implicitHeight + 8
        }
    }

    Keys.onPressed: {
        if (!visible) {
            return;
        }
        switch (event.key) {
            case Qt.Key_Escape:
            case Qt.Key_Back:
                event.accepted = true;
                root.cancel();
                break;
            case Qt.Key_Return:
            case Qt.Key_Enter:
                event.accepted = true;
                root.accept();
                break;
        }
    }

    Component.onCompleted: {
        keyboardEnabled = HMD.active;
        if (items) {
            menu.currentIndex = Math.max(0, Number(current) || 0);
            menu.forceActiveFocus();
        } else {
            field.text = current !== undefined && current !== null ? String(current) : "";
            field.selectAll();
            field.forceActiveFocus();
        }
    }
}
