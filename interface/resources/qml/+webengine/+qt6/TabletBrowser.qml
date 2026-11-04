import QtQuick 2.5
import QtWebChannel   // Qt 6: versionless
import QtWebEngine   // Qt 6: versionless (this +qt6 twin of the Qt 5 file beside it is what Qt 6 loads)

import "controls"
import controlsUit 1.0 as HifiControls
import "styles" as HifiStyles
import stylesUit 1.0
import "windows"

Item {
    id: root
    HifiConstants { id: hifi }
    HifiStyles.HifiConstants { id: hifistyles }

    height: 600
    property alias url: webview.url

    property bool canGoBack: webview.canGoBack
    property bool canGoForward: webview.canGoForward


    signal loadingChanged(int status)

    x: 0
    y: 0

    function setProfile(profile) {
        webview.profile = profile;
    }

    onUrlChanged: {
        permissionPopupBackground.visible = false;
    }

    WebEngineView {
        id: webview
        objectName: "webEngineView"
        x: 0
        y: 0
        width: parent.width
        height: keyboardEnabled && keyboardRaised ? parent.height - keyboard.height : parent.height

        profile: TivoliWebEngineProfile;
        settings.localContentCanAccessRemoteUrls: true

        property string userScriptUrl: ""

        userScripts.collection: [   // Qt 6: the scripts are values in the view's collection, not WebEngineScript objects
        {
          name: "createGlobalEventBridge",
          sourceCode: eventBridgeJavaScriptToInject,
          injectionPoint: WebEngineScript.DocumentCreation,
          worldId: WebEngineScript.MainWorld },
        {
          name: "raiseAndLowerKeyboard",
          injectionPoint: WebEngineScript.Deferred,
          sourceUrl: resourceDirectoryUrl + "/html/raiseAndLowerKeyboard.js",
          worldId: WebEngineScript.MainWorld },
        {
          name: "userScript",
          sourceUrl: webview.userScriptUrl,
          injectionPoint: WebEngineScript.DocumentReady,   // DOM ready but page load may not be finished.
          worldId: WebEngineScript.MainWorld }
        ]

        property string newUrl: ""

        Component.onCompleted: {
            webChannel.registerObject("eventBridge", eventBridge);
            webChannel.registerObject("eventBridgeWrapper", eventBridgeWrapper);

            // Ensure the JS from the web-engine makes it to our logging
            webview.javaScriptConsoleMessage.connect(function(level, message, lineNumber, sourceID) {
                console.log("Web Entity JS message: " + sourceID + " " + lineNumber + " " +  message);
            });

            webview.profile.httpUserAgent = "Mozilla/5.0 (Linux; Android 6.0; Nexus 5 Build/MRA58N) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/56.0.2924.87 Mobile Safari/537.36";
            web.address = url;
        }

        onFeaturePermissionRequested: {
            if (permissionPopupBackground.visible === true) {
                console.log("Browser engine requested a new permission, but user is already being presented with a different permission request. Aborting request for new permission...");
                return;
            }
            permissionPopupBackground.securityOrigin = securityOrigin;
            permissionPopupBackground.feature = feature;

            permissionPopupBackground.visible = true;
        }

        onLoadingChanged: function (loadRequest) {   // our own name for the parameter: Qt 6 calls it loadingInfo
            keyboardRaised = false;
            punctuationMode = false;
            keyboard.resetShiftMode(false);

            // Required to support clicking on "hifi://" links
            if (WebEngineView.LoadStartedStatus == loadRequest.status) {
                urlAppend(loadRequest.url.toString())
                var url = loadRequest.url.toString();
                if (urlHandler.canHandleUrl(url)) {
                    if (urlHandler.handleUrl(url)) {
                        root.stop();
                    }
                }
            }
        }

        onNewWindowRequested: function (request) {   // Qt 6: newViewRequested became newWindowRequested
            request.openIn(webView);
        }

        onContextMenuRequested: {
            request.accepted = true;
        }

        HifiControls.WebSpinner { }
    }

    Keys.onPressed: {
        switch(event.key) {
        case Qt.Key_L:
            if (event.modifiers == Qt.ControlModifier) {
                event.accepted = true
                addressBar.selectAll()
                addressBar.forceActiveFocus()
            }
            break;
        }
    }

    HifiControls.PermissionPopupBackground {
        id: permissionPopupBackground
        onSendPermission: {
            webview.grantFeaturePermission(securityOrigin, feature, shouldGivePermission);
        }
    }
}
