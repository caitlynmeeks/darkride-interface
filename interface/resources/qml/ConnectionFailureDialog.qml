import QtQuick 2.12
import "js/StandardDialogs.js" as OriginalDialogs  // StandardButton and StandardIcon (Dialogs 1 needs Controls 1)

import "dialogs"

MessageDialog {
    id: root
    objectName: "ConnectionFailureDialog"

    title: "Can't reach this world"
    text: "Open Explore to choose another world."
    buttons: OriginalDialogs.StandardButton.Ok | OriginalDialogs.StandardButton.Cancel
    defaultButton: OriginalDialogs.StandardButton.Ok
    buttonTexts: ({ ok: "Open Explore", cancel: "OK" })

    // Explore's button listens for this (tivoli-scripts buttons.ts)
    onSelected: {
        if (button === OriginalDialogs.StandardButton.Ok) {
            Messages.sendLocalMessage("com.tivolicloud.explore", "open");
        }
    }

    Component.onCompleted: {
        var world = AddressManager.worldName.trim();
        if (world !== "") {
            title = "Can't reach " + world;
        }
    }
}
