//
//  SavedScriptsReview.qml
//  interface/resources/qml/dialogs
//
//  The one review of the scripts that start with Darkride (security #30): a world could have put a script from the
//  network on the visitor's startup list, so the first start of an app with this review lists them, held, and the
//  visitor keeps only the ones they added. Each is unticked until they tick it. Done starts the ticked ones and takes
//  the rest off the list (ScriptEngines::finishSavedScriptsReview, through SavedScriptsReview.cpp); Ask Me Later
//  leaves them held and saved, and the review comes again at the next start.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

import QtQuick 2.12
import controlsUit 1.0
import stylesUit 1.0
import "../windows"
import "alert"
import "alert/standardButtons.js" as StandardButtons

ModalWindow {
    id: root
    objectName: "SavedScriptsReview"
    keyboardOverride: true
    frame: AlertShadow { }
    title: ""
    // as the app's other alerts (CustomQueryDialog.qml): its panel's size, a third of the way down the desktop
    anchors.centerIn: undefined
    anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
    anchors.top: parent ? parent.top : undefined
    anchors.topMargin: parent ? Math.max(0, Math.round((parent.height - height) / 3)) : 0
    width: panel.width
    height: panel.implicitHeight

    signal answered(var kept)   // Done: the addresses ticked

    // the held scripts' addresses, from the name SavedScriptsReview.cpp gives this dialog's context as it's made
    property var scripts: typeof savedScriptsHeld !== "undefined" ? savedScriptsHeld : []   // { address, main, full }
    property var ticked: ({})
    property int titleWidth: 0  // ModalWindow's own frame sets it while the window is made

    function finish() {
        var kept = [];
        for (var i = 0; i < root.scripts.length; i++) {
            if (root.ticked[root.scripts[i].address]) {
                kept.push(root.scripts[i].address);   // the exact address, as held
            }
        }
        root.answered(kept);
        root.destroy();
    }

    function later() {
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
            title: "These scripts start with Darkride."
            text: "Keep only the ones you added yourself. The ones left unticked are removed."
            buttons: StandardButtons.list(StandardButtons.OK | StandardButtons.CANCEL, StandardButtons.OK, 0,
                                          { ok: "Done", cancel: "Ask Me Later" })
            onClicked: {
                if (value === StandardButtons.OK) {
                    root.finish();
                } else {
                    root.later();
                }
            }

            // the panel lays out its own children, not a Repeater's: the list goes in one column
            Column {
                width: parent ? parent.width : 0
                spacing: 6
                Repeater {
                    model: root.scripts
                    // each: the site and the file as the main line (SavedScriptsReview.cpp), the whole address under it
                    delegate: Column {
                        width: parent ? parent.width : 0
                        AlertCheck {
                            width: parent.width
                            elide: Text.ElideMiddle   // the file's name, at the end, stays in sight
                            text: modelData.main
                            checked: false
                            onToggled: {
                                var ticked = root.ticked;
                                ticked[modelData.address] = checked;
                                root.ticked = ticked;
                            }
                        }
                        Text {
                            width: parent.width
                            leftPadding: 18
                            text: modelData.full
                            textFormat: Text.PlainText
                            elide: Text.ElideMiddle
                            color: Qt.rgba(1, 1, 1, 0.55)
                            font.pixelSize: 10
                        }
                    }
                }
            }
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
                root.later();
                break;
        }
    }
}
