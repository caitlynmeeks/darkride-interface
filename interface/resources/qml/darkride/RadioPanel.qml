//
//  RadioPanel.qml
//  qml/darkride
//
//  Darkride's Radio panel: internet radio in a classic skin player, the page html/radio/radio.html (Webamp wearing
//  Darkride's own skin; SomaFM's stations, any stream address, the player's own .wsz skins). The bar's Radio button
//  opens it (tivoli-scripts' ButtonManager.addQmlButton): on the desktop a dock panel (Panels' "Radio"), in a headset
//  the tablet. On the desktop the page lives while the panel is open, so the music goes on with the panel tabbed behind
//  another; it's torn down when the radio is closed (a web page is a 100-200 MB process with its audio: performance,
//  10-30). Twice the size once the panel has the room, so the skin's pixels stay sharp. The page gets no engine API:
//  it plays streams straight from the stations, opens its links (Support SomaFM) in the system's browser, and keeps
//  what it remembers (volume, the player's own stations and skin) in its own web profile.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.15
import QtWebEngine 1.10

Rectangle {
    id: root

    signal sendToScript(var message)   // the tablet's loader connects it; nothing is sent

    // the tablet's loader sizes the page; a dock or a desktop window parents it to its content area
    anchors.fill: parent
    color: "#0c1022"   // the page's own background (radio.css --navy), behind it while it loads

    readonly property url pageUrl: "qrc:///html/radio/radio.html"
    // a dock on the desktop: alive while the panel is open (shown, or tabbed behind another); else (the tablet, the bar's
    // window) while shown
    readonly property bool docked: typeof Panels !== "undefined" && Panels.names().indexOf("Radio") >= 0
        && !(typeof HMD !== "undefined" && HMD.active)
    property bool panelOpen: typeof Panels !== "undefined" && Panels.isOpen("Radio")
    readonly property bool live: docked ? panelOpen : visible

    Connections {
        target: typeof Panels !== "undefined" ? Panels : null
        function onOpenChanged(name, open) {
            if (name === "Radio") {
                root.panelOpen = open;
            }
        }
    }

    // the player's own close box and its Exit close the radio
    function closeRadio() {
        if (docked) {
            Panels.close("Radio");
        }
    }

    Loader {
        anchors.fill: parent
        active: root.live
        sourceComponent: WebEngineView {
            url: root.pageUrl
            backgroundColor: root.color
            // the skin's pixels at a whole size: twice once both of the player's windows and its buttons fit twice
            zoomFactor: root.width >= 600 && root.height >= 820 ? 2 : 1

            profile: WebEngineProfile {
                storageName: "DarkrideRadio"
                offTheRecord: false
                httpCacheType: WebEngineProfile.MemoryHttpCache
                persistentCookiesPolicy: WebEngineProfile.NoPersistentCookies
            }
            // the page is the app's own (qrc): it may reach the stations, never the computer's files
            settings.localContentCanAccessRemoteUrls: true
            settings.localContentCanAccessFileUrls: false
            settings.javascriptCanOpenWindows: false
            settings.pluginsEnabled: false

            // Support SomaFM and the player's About link: in the system's browser
            onNewViewRequested: function (request) {
                var link = request.requestedUrl.toString();
                if (/^https?:\/\//i.test(link)) {
                    Qt.openUrlExternally(link);
                }
            }
            // the page stays the radio: a file dropped on it or a link followed in it doesn't replace it
            onNavigationRequested: function (request) {
                if (request.isMainFrame && request.url.toString() !== root.pageUrl.toString()) {
                    request.action = WebEngineNavigationRequest.IgnoreRequest;
                }
            }
            // no browser menu (Back, Reload, Inspect): the player has its own
            onContextMenuRequested: function (request) {
                request.accepted = true;
            }
            onJavaScriptDialogRequested: function (request) {
                request.accepted = true;
                request.dialogReject();
            }
            onTitleChanged: {
                if (title === "Radio: close") {
                    root.closeRadio();
                }
            }
            onRenderProcessTerminated: function (terminationStatus, exitCode) {
                console.warn("Radio: the page's process ended", terminationStatus, exitCode);
            }
        }
    }
}
