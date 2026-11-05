//
//  WebView.qml
//
//  Created by Bradley Austin Davis on 12 Jan 2016
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

import QtQuick 2.7
import QtWebEngine   // Qt 6: versionless (this +qt6 twin of the Qt 5 file beside it is what Qt 6 loads)
import controlsUit 1.0 as ControlsUit

WebEngineView {
    settings.localContentCanAccessRemoteUrls: true
    id: root
    Component.onCompleted: {
        console.log("Connecting JS messaging to Hifi Logging")
        // Ensure the JS from the web-engine makes it to our logging
        root.javaScriptConsoleMessage.connect(function(level, message, lineNumber, sourceID) {
            console.log("Web Window JS message: " + sourceID + " " + lineNumber + " " +  message);
        });
    }

    onUrlChanged: {
        permissionPopupBackground.visible = false;
    }

    onLoadingChanged: function (loadRequest) {   // our own name for the parameter: Qt 6 calls it loadingInfo
        if (WebEngineView.LoadStartedStatus === loadRequest.status) {
            permissionPopupBackground.withdraw();
        }
        // Required to support clicking on "hifi://" links
        if (WebEngineView.LoadStartedStatus == loadRequest.status) {
            var url = loadRequest.url.toString();
            if (urlHandler.canHandleUrl(url)) {
                if (urlHandler.handleUrl(url)) {
                    root.stop();
                }
            }
        }
    }

    WebSpinner { }

    onPermissionRequested: function (permission) {
        permissionPopupBackground.ask(permission);
    }

    ControlsUit.PermissionPopupBackground {
        id: permissionPopupBackground
        z: 100
        unit: 1
    }

    onContextMenuRequested: {
        request.accepted = true;
    }
}
