import QtQuick 2.5
import QtWebEngine   // Qt 6: versionless (this +qt6 twin of the Qt 5 file beside it is what Qt 6 loads)
import QtWebChannel   // Qt 6: versionless

import controlsUit 1.0 as Controls
import stylesUit 1.0

Controls.WebView {
    id: webview
    url: "about:blank"
    anchors.fill: parent
    focus: true
    profile: TivoliWebEngineProfile;

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

    Component.onCompleted: {
        webChannel.registerObject("eventBridge", eventBridge);
        webChannel.registerObject("eventBridgeWrapper", eventBridgeWrapper);
    }
}
