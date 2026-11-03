//
//  TabletConnectionFailureDialog.qml
//
//  Created by Vlad Stelmahovsky on 29 Mar 2017
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

import Hifi 1.0
import QtQuick 2.5
import "../js/StandardDialogs.js" as OriginalDialogs  // StandardButton and StandardIcon (Dialogs 1 needs Controls 1)

Item {
    Component.onCompleted: {
        var world = AddressManager.worldName.trim();
        var object = tabletRoot.messageBox({
                                            buttons: OriginalDialogs.StandardButton.Ok | OriginalDialogs.StandardButton.Cancel,
                                            defaultButton: OriginalDialogs.StandardButton.Ok,
                                            buttonTexts: { ok: "Open Explore", cancel: "OK" },
                                            title: "Can't reach " + (world !== "" ? world : "this world"),
                                            text: "Open Explore to choose another world."
                                        });
        object.selected.connect(function(button) {
            var tablet = Tablet.getTablet("com.highfidelity.interface.tablet.system");
            tablet.gotoHomeScreen();
            if (button === OriginalDialogs.StandardButton.Ok) {
                // Explore's button listens for this (tivoli-scripts buttons.ts)
                Messages.sendLocalMessage("com.tivolicloud.explore", "open");
            }
        });
    }
}
