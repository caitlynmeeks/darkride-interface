import QtQuick 2.5
import QtWebChannel   // Qt 6: versionless
import QtWebEngine   // Qt 6: versionless (this +qt6 twin of the Qt 5 file beside it is what Qt 6 loads)

import controlsUit 1.0

WebView {
    id: webview
    url: "https://google.com"
    profile: FileTypeProfile;

    property var parentRoot: null

    userScripts.collection: [   // Qt 6: the scripts are values in the view's collection, not WebEngineScript objects
    {
      name: "createGlobalEventBridge",
      sourceCode: eventBridgeJavaScriptToInject,
      injectionPoint: WebEngineScript.Deferred,
      worldId: WebEngineScript.MainWorld },
    {
      name: "raiseAndLowerKeyboard",
      injectionPoint: WebEngineScript.Deferred,
      sourceUrl: resourceDirectoryUrl + "/html/raiseAndLowerKeyboard.js",
      worldId: WebEngineScript.MainWorld }
    ]

    onLoadingChanged: function (loadRequest) {   // our own name for the parameter: Qt 6 calls it loadingInfo
        if (loadRequest.status === WebEngineView.LoadSucceededStatus) {
            addressBar.text = loadRequest.url
        }
        parentRoot.loadingChanged(loadRequest.status);
    }

    onWindowCloseRequested: {
        parentRoot.destroy();
    }

    Component.onCompleted: {
        webChannel.registerObject("eventBridge", eventBridge);
        webChannel.registerObject("eventBridgeWrapper", eventBridgeWrapper);
        desktop.initWebviewProfileHandlers(webview.profile);
    }
}
