//
//  TabletMessageBox.qml
//
//  Created by Bradley Austin Davis on 15 Jan 2016
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

import QtQuick 2.12
import "../js/StandardDialogs.js" as OriginalDialogs  // StandardButton and StandardIcon (Dialogs 1 needs Controls 1)

import "../windows"
import "alert"
import "alert/standardButtons.js" as StandardButtons

// The tablet's message box: the same alert as on the desktop (MessageDialog.qml), larger, over the dimmed tablet.
TabletModalWindow {
    id: root
    visible: true
    focus: true

    signal selected(int button);

    MouseArea {
        id: mouse;
        anchors.fill: parent
    }

    function click(button) {
        clickedButton = button;
        selected(button);
        destroy();
    }

    function exec() {
        return OffscreenUi.waitForMessageBoxResult(root);
    }

    // as MessageDialog's: Escape's answer for a world's box, and its answer as the visitor leaves the world
    function dismiss() {
        if (panel.cancelButton) {
            panel.reject();
        } else if (panel.defaultButton) {
            panel.accept();
        } else {
            click(OriginalDialogs.StandardButton.NoButton);
        }
    }

    property string text
    property string informativeText
    property string detailedText
    property int buttons: OriginalDialogs.StandardButton.Ok
    property int icon: OriginalDialogs.StandardIcon.NoIcon  // kept for callers: every alert shows Darkride's picture
    property int defaultButton: OriginalDialogs.StandardButton.NoButton
    property int clickedButton: OriginalDialogs.StandardButton.NoButton
    property var buttonTexts: ({})
    property int destructiveButton: OriginalDialogs.StandardButton.NoButton
    // a world's script's box, as MessageDialog's (interface's ui/WorldDialogs.h)
    property string world: ""
    property bool offerBlock: false
    readonly property bool blockWorld: blockCheck.checked

    AlertPanel {
        id: panel
        unit: 1.25
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.max(0, Math.round((parent.height - height) / 3))
        height: implicitHeight
        title: root.title
        text: StandardButtons.joined(root.text, root.informativeText)
        detailedText: root.detailedText
        buttons: StandardButtons.list(root.buttons, root.defaultButton, root.destructiveButton, root.buttonTexts)
        world: root.world
        onClicked: root.click(value)

        AlertCheck {
            id: blockCheck
            objectName: "blockWorldCheck"
            unit: panel.unit
            visible: root.offerBlock
            width: parent.width
            text: "Don't let this world show more dialogs"
        }
    }

    Keys.onPressed: {
        if (!visible) {
            return;
        }
        switch (event.key) {
            case Qt.Key_Return:
            case Qt.Key_Enter:
                event.accepted = true;
                panel.accept();
                break;
            case Qt.Key_Escape:
            case Qt.Key_Back:
                event.accepted = true;
                if (root.world !== "") {
                    root.dismiss();
                } else {
                    panel.reject();
                }
                break;
        }
    }
}
