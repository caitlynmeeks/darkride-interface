//
//  MessageDialog.qml
//
//  Created by Bradley Austin Davis on 15 Jan 2016
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

import QtQuick 2.12
import "../js/StandardDialogs.js" as OriginalDialogs  // StandardButton and StandardIcon (Dialogs 1 needs Controls 1)

import stylesUit 1.0
import "../windows"
import "alert"
import "alert/standardButtons.js" as StandardButtons

// The desktop's message box, drawn as an alert (alert/AlertPanel.qml) a third of the way down the window, where Mac
// alerts appear. Callers pass the standard buttons and get the chosen one back; buttonTexts gives buttons their own
// words ({ yes: "Run" }), and destructiveButton names one that deletes or replaces something for good.
ModalWindow {
    id: root
    destroyOnCloseButton: true
    destroyOnHidden: true
    visible: true
    frame: AlertShadow { }

    anchors.centerIn: undefined
    anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
    anchors.top: parent ? parent.top : undefined
    anchors.topMargin: parent ? Math.max(0, Math.round((parent.height - height) / 3)) : 0

    signal selected(int button);

    function click(button) {
        clickedButton = button;
        selected(button);
        destroy();
    }

    function exec() {
        return OffscreenUi.waitForMessageBoxResult(root);
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
    property int titleWidth: 0  // ModalWindow's own frame sets it while the window is made

    width: panel.width
    height: panel.implicitHeight

    // A window holds one content item, its last child, so all it shows goes in this one. ModalWindow.qml reads hifi.
    Item {
        width: root.width
        height: root.height

        HifiConstants { id: hifi }

        AlertPanel {
            id: panel
            focus: true
            title: root.title
            text: StandardButtons.joined(root.text, root.informativeText)
            detailedText: root.detailedText
            buttons: StandardButtons.list(root.buttons, root.defaultButton, root.destructiveButton, root.buttonTexts)
            onClicked: root.click(value)
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
                panel.reject();
                break;
            case Qt.Key_Period:
                if (event.modifiers === Qt.ControlModifier && Qt.platform.os === "osx") {  // Command-period
                    event.accepted = true;
                    panel.reject();
                }
                break;
        }
    }
}
