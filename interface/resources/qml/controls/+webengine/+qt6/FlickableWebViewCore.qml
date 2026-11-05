import QtQuick 2.7
import QtWebEngine   // Qt 6: versionless (this +qt6 twin of the Qt 5 file beside it is what Qt 6 loads)
import QtWebChannel   // Qt 6: versionless

import QtQuick.Controls 2.2

import stylesUit 1.0 as StylesUIt
import controlsUit 1.0 as ControlsUit

Item {
    id: flick

    property alias url: webViewCore.url
    property alias canGoBack: webViewCore.canGoBack
    property alias webViewCore: webViewCore
    property alias webViewCoreProfile: webViewCore.profile
    property string webViewCoreUserAgent

    property string userScriptUrl: ""
    property string urlTag: "noDownload=false";

    signal newViewRequestedCallback(var request)
    signal loadingChangedCallback(var loadRequest)


    width: parent.width

    property bool interactive: false

    property bool blurOnCtrlShift: true

    property bool transparentBackground: false

    // the colour under the page until its first load has finished (the tablet's is Darkride's navy, so a dark page
    // doesn't open white in the headset); after it, the web's usual white under a page that sets no colour of its own
    property color loadingBackground: "white"
    property bool firstLoadDone: false

    // security finding #54: a page a world's script opened. The tablet shows it under its own screen type (WorldWeb),
    // so no app's handler wires to it; it gets no EventBridge: at every main-frame navigation of the view, the first
    // included, the web channel is taken off the view (no transport for the document that follows), see below. Before
    // the first navigation the view's engine isn't live and a change to its channel doesn't take, which is why this
    // is done at navigation and not when the roots mark the view. A window the page asks for opens nowhere (below).
    // The page runs and talks to nothing.
    property bool worldPage: false

    function unwireEventBridge() {
        webViewCore.webChannel = null;
        console.log("FlickableWebViewCore: a world's page, no EventBridge (security finding #54): " + webViewCore.url);
    }

    onUrlChanged: {
        permissionPopupBackground.visible = false;
    }

    StylesUIt.HifiConstants {
        id: hifi
    }

    function stop() {
        webViewCore.stop();
    }

    Timer {
        id: delayedUnfocuser
        repeat: false
        interval: 200
        onTriggered: {

            // The idea behind this is to delay unfocusing, so that fast lower/raise will not result actual unfocusing.
            // Fast lower/raise happens every time keyboard is being re-raised (see the code below in OffscreenQmlSurface::setKeyboardRaised)
            //
            // if (raised) {
            //    item->setProperty("keyboardRaised", QVariant(!raised));
            // }
            //
            // item->setProperty("keyboardRaised", QVariant(raised));
            //

            webViewCore.runJavaScript("if (document.activeElement) document.activeElement.blur();", function(result) {
                console.log('unfocus completed: ', result);
            });
        }
    }

    function unfocus() {
        delayedUnfocuser.start();
    }

    function stopUnfocus() {
        delayedUnfocuser.stop();
    }

    function onLoadingChanged(loadRequest) {
        if (WebEngineView.LoadStartedStatus === loadRequest.status) {

            // Required to support clicking on "hifi://" links
            var url = loadRequest.url.toString();
            url = (url.indexOf("?") >= 0) ? url + urlTag : url + "?" + urlTag;
            if (urlHandler.canHandleUrl(url)) {
                if (urlHandler.handleUrl(url)) {
                    webViewCore.stop();
                }
            }
        }

        if (WebEngineView.LoadFailedStatus === loadRequest.status) {
            console.log("Tablet WebEngineView failed to load url: " + loadRequest.url.toString());
        }

        if (WebEngineView.LoadSucceededStatus === loadRequest.status) {
            //disable Chromium's scroll bars
        }
    }

    WebEngineView {
        id: webViewCore

        width: parent.width
        height: parent.height

        profile: TivoliWebEngineProfile;
        settings.pluginsEnabled: false
        settings.touchIconsEnabled: true
        settings.allowRunningInsecureContent: false
        // pages loaded from file:// (the tablet apps) call the metaverse API; Qt blocks that unless this is on
        settings.localContentCanAccessRemoteUrls: true

        // https://doc.qt.io/qt-5/qml-qtwebengine-webengineview.html#backgroundColor-prop
        backgroundColor: flick.transparentBackground ? "transparent" : flick.firstLoadDone ? "white" : flick.loadingBackground

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
          sourceUrl: flick.userScriptUrl,
          injectionPoint: WebEngineScript.DocumentReady,   // DOM ready but page load may not be finished.
          worldId: WebEngineScript.MainWorld }
        ]

        Component.onCompleted: {
            webChannel.registerObject("eventBridge", eventBridge);
            webChannel.registerObject("eventBridgeWrapper", eventBridgeWrapper);

            if (webViewCoreUserAgent !== undefined) {
                webViewCore.profile.httpUserAgent = webViewCoreUserAgent
            } else {
                webViewCore.profile.httpUserAgent += " Darkride"; // TODO: add /<version>
            }
            // Ensure the JS from the web-engine makes it to our logging
            webViewCore.javaScriptConsoleMessage.connect(function(level, message, lineNumber, sourceID) {
                console.log("Web Entity JS message: " + sourceID + " " + lineNumber + " " +  message);
            });
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

        //disable popup
        onContextMenuRequested: {
            request.accepted = true;
        }

        onNewWindowRequested: function (request) {   // Qt 6: newViewRequested became newWindowRequested
            if (flick.worldPage) {
                return;   // a world's page gets no window: the request is opened nowhere
            }
            newViewRequestedCallback(request)
        }

        // security finding #54: a world's page gets no EventBridge: at each main-frame navigation (the first included)
        // the channel comes off before the document exists (urlChanged comes once its scripts have started, too late)
        onNavigationRequested: function (request) {
            if (flick.worldPage && request.isMainFrame) {
                flick.unwireEventBridge();
            }
        }

        // Prior to 5.10, the WebEngineView loading property is true during initial page loading and then stays false
        // as in-page javascript adds more html content. However, in 5.10 there is a bug such that adding html turns
        // loading true, and never turns it false again. safeLoading provides a workaround, but it should be removed
        // when QT fixes this.
        property bool safeLoading: false
        property bool loadingLatched: false
        property var loadingRequest: null
        onLoadingChanged: function (loadRequest) {   // our own name for the parameter: Qt 6 calls it loadingInfo
            webViewCore.loadingRequest = loadRequest;
            webViewCore.safeLoading = webViewCore.loading && !loadingLatched;
            webViewCore.loadingLatched |= webViewCore.loading;
            if (!webViewCore.loading && webViewCore.loadingLatched) {
                flick.firstLoadDone = true;
            }
         }
        onSafeLoadingChanged: {
            flick.onLoadingChanged(webViewCore.loadingRequest)
            loadingChangedCallback(webViewCore.loadingRequest)
        }
    }

    AnimatedImage {
        //anchoring doesnt works here when changing content size
        x: flick.width/2 - width/2
        y: flick.height/2 - height/2
        source: "qrc:///icons/loader-snake-64-w.gif"
        visible: webViewCore.safeLoading && /^(http.*|)$/i.test(webViewCore.url.toString())
        playing: visible
        z: 10000
    }

    Keys.onPressed: {
        if (blurOnCtrlShift && (event.modifiers & Qt.ShiftModifier) && (event.modifiers & Qt.ControlModifier)) {
            webViewCore.focus = false; 
        }
    }

    ControlsUit.PermissionPopupBackground {
        id: permissionPopupBackground
        onSendPermission: {
            webViewCore.grantFeaturePermission(securityOrigin, feature, shouldGivePermission);
        }
    }

}
