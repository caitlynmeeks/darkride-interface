//
//  TabletQueryDialog.qml
//
//  Created by Bradley Austin Davis on 22 Jan 2016
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

import QtQuick 2.12

import controlsUit 1.0
import "../windows"
import "alert"
import "alert/standardButtons.js" as StandardButtons

// The tablet's text prompt: the same alert as on the desktop (QueryDialog.qml), larger, over the dimmed tablet, with
// the keyboard at the tablet's foot while the field is being typed in.
TabletModalWindow {
    id: root
    visible: true
    focus: true

    signal selected(var result);
    signal canceled();

    MouseArea {
        width: parent.width
        height: parent.height
    }

    property bool keyboardOverride: true
    property int icon: 0  // kept for callers: every alert shows Darkride's picture
    property var items;
    property string label: ""
    property var result;
    property var current
    property string placeholderText

    // For combo boxes
    property bool editable: true;

    property var buttonTexts: ({})
    property bool monospace: false
    // a world's script's prompt, as QueryDialog's (interface's ui/WorldDialogs.h)
    property string world: ""
    property bool offerBlock: false
    readonly property bool blockWorld: blockCheck.checked

    property bool keyboardEnabled: false
    property bool keyboardRaised: false
    property bool punctuationMode: false
    readonly property bool keyboardShown: keyboardEnabled && keyboardRaised

    function accept() {
        root.result = items ? menu.currentText : field.text;
        root.selected(root.result);
        root.destroy();
    }

    function cancel() {
        root.canceled();
        root.destroy();
    }

    AlertPanel {
        id: panel
        unit: 1.25
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.max(0, Math.round((parent.height - (root.keyboardShown ? keyboard.raisedHeight : 0) - height) / 3))
        height: implicitHeight
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
            unit: panel.unit
            visible: !root.items
            monospace: root.monospace
            placeholderText: root.placeholderText
        }

        AlertMenu {
            id: menu
            unit: panel.unit
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
            unit: panel.unit
            visible: root.offerBlock
            width: parent.width
            text: "Don't let this world show more dialogs"
        }
    }

    Keyboard {
        id: keyboard
        raised: root.keyboardShown
        numeric: root.punctuationMode
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
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
