//
//  RadioPanel.qml
//  qml/darkride
//
//  Darkride's Radio panel: internet radio in a classic skin player, the page html/radio/radio.html (Webamp wearing
//  Darkride's own skin; SomaFM's stations, any stream address, the player's own .wsz skins). The bar's Radio button
//  opens it (tivoli-scripts' ButtonManager.addQmlButton): on the desktop a dock panel (Panels' "Radio"), in a headset
//  the tablet. On the desktop the page lives from the panel's opening until it's closed, so the music goes on with the
//  panel tabbed behind another, through Build mode and a re-dock; it's torn down when the radio is closed (a web page
//  is a 100-200 MB process with its audio: performance, 10-30). The page gets no engine API: it plays streams straight
//  from the stations, opens its links (Support SomaFM) in the system's browser, and keeps what it remembers (volume,
//  the player's own stations and skin) in its own web profile.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.15
import QtWebEngine 1.10
import "panel"

Rectangle {
    id: root

    signal sendToScript(var message)   // the tablet's loader connects it; nothing is sent

    // the tablet's loader sizes the page; a dock or a desktop window parents it to its content area
    anchors.fill: parent
    color: Theme.background   // the native controls' panel; the player's page has its own (radio.css --navy)

    readonly property url pageUrl: "qrc:///html/radio/radio.html"
    // a dock on the desktop: alive while the panel is open (shown, or tabbed behind another); else (the tablet, the bar's
    // window) while shown
    readonly property bool docked: (typeof Panels !== "undefined" && Panels.names().indexOf("Radio") >= 0 &&
        !(typeof HMD !== "undefined" && HMD.active))
    property bool panelOpen: typeof Panels !== "undefined" && Panels.isOpen("Radio")
    // docked, the radio is on from the panel's first showing until it's closed (its close box, the player's, the bar's
    // button: QmlDock calls panelClosed()); a hide that isn't a close (Build mode's own layout, a re-dock, a tab behind
    // another) keeps the page and its music (Caity's hand test 10-31: Build mode stopped the radio)
    property bool on: false
    onPanelOpenChanged: {
        if (panelOpen) {
            on = true;
        }
    }
    function panelClosed() {
        console.log("Radio: closed");
        on = false;
    }
    readonly property bool live: docked ? on : visible

    Connections {
        target: typeof Panels !== "undefined" ? Panels : null
        function onOpenChanged(name, open) {
            if (name === "Radio") {
                root.panelOpen = open;
            }
        }
    }

    // a file the page asks for (Load a skin…, the player's own Play File): the app's file dialog (Window.browseAsync, as
    // the Avatar panel's pictures), its answer handed back to the page's request
    property var fileRequest: null
    Connections {
        target: typeof WindowScriptingInterface !== "undefined" ? WindowScriptingInterface : null
        function onBrowseChanged(filename) {
            var request = root.fileRequest;
            if (!request) {
                return;   // another panel's dialog
            }
            root.fileRequest = null;
            try {
                if (filename) {
                    request.dialogAccept([filename]);
                } else {
                    request.dialogReject();
                }
            } catch (error) {
                // the radio was closed while the dialog was open
            }
        }
    }

    // the player's own close box and its Exit close the radio
    Component.onCompleted: {
        on = panelOpen;
        console.log("Radio: made; docked:", docked, "panels:", typeof Panels !== "undefined" ? Panels.names() : "none",
            "headset:", typeof HMD !== "undefined" ? HMD.active : "none", "open:", panelOpen);
    }

    function closeRadio() {
        console.log("Radio: the player's close box; docked:", docked);
        if (docked) {
            Panels.close("Radio");
        }
    }

    // ---- the controls under the player: the app's own, in the panels' current skin (Caity 10-31: "only the player is
    // skinned ... make those bottom areas native UI. like explore or chat"); they call the page's fixed functions
    // (html/radio/radio.js darkrideRadioApi) and take its events when its title says "Radio: events" ----

    property string tab: "soma"
    property var stations: []            // the last search's: { name, about, tags }, shown as plain text only
    property string foundStatus: ""
    property string noteText: ""
    property bool noteProblem: false
    property bool pickingSkin: false

    // one of the page's fixed calls, every argument passed as JSON (security, 10-31)
    function call(name, args) {
        var view = loader.item;
        if (!view) {
            return;
        }
        var quoted = (args || []).map(function (arg) { return JSON.stringify(arg); }).join(", ");
        view.runJavaScript("window.darkrideRadioApi && darkrideRadioApi." + name + "(" + quoted + ")");
    }
    // the page's events, as JSON text, read with JSON.parse only and taken as data
    function takeEvents() {
        var view = loader.item;
        if (!view) {
            return;
        }
        view.runJavaScript("window.darkrideRadioApi ? darkrideRadioApi.takeEvents() : '[]'", function (text) {
            var list;
            try {
                list = JSON.parse(String(text));
            } catch (error) {
                return;
            }
            if (!Array.isArray(list)) {
                return;
            }
            list.forEach(function (event) {
                if (!event || typeof event !== "object") {
                    return;
                }
                if (event.type === "note") {
                    root.showNote(String(event.text || ""), event.problem === true);
                } else if (event.type === "found") {
                    root.foundStatus = String(event.status || "").slice(0, 120);
                    root.stations = (Array.isArray(event.stations) ? event.stations : []).slice(0, 30).map(
                        function (station) {
                            return {
                                name: String(station && station.name || "").slice(0, 80),
                                about: String(station && station.about || "").slice(0, 40),
                                tags: String(station && station.tags || "").slice(0, 120),
                            };
                        });
                } else if (event.type === "tab" && ["soma", "find", "address"].indexOf(event.tab) >= 0) {
                    root.tab = event.tab;
                } else if (event.type === "addressPlayed") {
                    addressField.text = "";
                }
            });
        });
    }
    function showNote(text, problem) {
        noteText = text.slice(0, 200);
        noteProblem = problem;
        noteTimer.interval = problem ? 8000 : 4000;
        noteTimer.restart();
    }
    Timer {
        id: noteTimer
        onTriggered: root.noteText = ""
    }
    function find() {
        if (findField.text.trim() !== "") {
            call("find", [findField.text]);
        } else {
            findField.forceActiveFocus();
        }
    }
    function playAddress() {
        if (addressField.text.trim() !== "") {
            call("playAddress", [addressField.text]);
        } else {
            addressField.forceActiveFocus();
        }
    }

    // Load a skin…: the app's own dialog (RadioSkinPicker), the picked skin's bytes into the page's skin gate
    Connections {
        target: typeof RadioSkins !== "undefined" ? RadioSkins : null
        function onPicked(name, base64, problem) {
            if (!root.pickingSkin) {
                return;
            }
            root.pickingSkin = false;
            if (problem !== "") {
                root.showNote(problem, true);
            } else if (base64 !== "") {
                root.call("loadSkin", [name, base64]);
            }
        }
    }

    component PanelButton: Rectangle {
        id: panelButton
        property string text: ""
        property bool strong: false
        signal pressed()
        implicitWidth: panelButtonLabel.implicitWidth + 22 * Theme.unit
        implicitHeight: Theme.controlHeight
        radius: 7 * Theme.unit
        color: strong ? Theme.tint : Theme.fill
        opacity: enabled ? (panelButtonMouse.pressed ? 0.7 : 1) : 0.4
        Text {
            id: panelButtonLabel
            anchors.centerIn: parent
            text: panelButton.text
            textFormat: Text.PlainText
            color: panelButton.strong ? "#ffffff" : Theme.tint
            font.pixelSize: Theme.controlTextSize
            font.bold: panelButton.strong
        }
        MouseArea {
            id: panelButtonMouse
            anchors.fill: parent
            enabled: panelButton.enabled
            onClicked: panelButton.pressed()
        }
    }

    Column {
        id: controls
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.gap
        spacing: 6 * Theme.unit
        visible: root.live

        Segmented {
            width: parent.width
            model: [{ text: "SomaFM", value: "soma" }, { text: "Find", value: "find" },
                { text: "Address", value: "address" }]
            current: root.tab
            onChosen: {
                root.tab = value;
                root.call("rememberTab", [value]);
                if (value === "find") {
                    findField.forceActiveFocus();
                } else if (value === "address") {
                    addressField.forceActiveFocus();
                }
            }
        }

        // SomaFM: their credit, and their donation page in the system's browser (a fixed https address)
        Item {
            visible: root.tab === "soma"
            width: parent.width
            height: Theme.controlHeight
            Text {
                anchors.left: parent.left
                anchors.right: supportButton.left
                anchors.rightMargin: 6 * Theme.unit
                anchors.verticalCenter: parent.verticalCenter
                text: "Radio from SomaFM"
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Theme.secondaryText
                font.pixelSize: Theme.controlTextSize
            }
            PanelButton {
                id: supportButton
                anchors.right: parent.right
                text: "Support SomaFM"
                onPressed: Qt.openUrlExternally("https://somafm.com/support/")
            }
        }

        // Find: the Radio Browser directory, searched by the page; its stations listed here as plain text
        Column {
            visible: root.tab === "find"
            width: parent.width
            spacing: 4 * Theme.unit
            Item {
                width: parent.width
                height: Theme.controlHeight
                Field {
                    id: findField
                    anchors.left: parent.left
                    anchors.right: findButton.left
                    anchors.rightMargin: 6 * Theme.unit
                    height: parent.height
                    placeholderText: "A station's name, or a kind of music"
                    maximumLength: 80
                    onAccepted: root.find()
                    // Return is the field's: not on to the app, where it opens Chat (windowz, 10-31)
                    Keys.onReturnPressed: { root.find(); event.accepted = true; }
                    Keys.onEnterPressed: { root.find(); event.accepted = true; }
                }
                PanelButton {
                    id: findButton
                    anchors.right: parent.right
                    text: "Find"
                    strong: true
                    onPressed: root.find()
                }
            }
            Rectangle {
                width: parent.width
                height: 4 * 28 * Theme.unit
                radius: 7 * Theme.unit
                color: Theme.fill
                clip: true
                Text {
                    anchors.centerIn: parent
                    width: parent.width - 16 * Theme.unit
                    visible: root.stations.length === 0
                    text: root.foundStatus
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    color: Theme.secondaryText
                    font.pixelSize: Theme.controlTextSize
                }
                ListView {
                    anchors.fill: parent
                    model: root.stations
                    boundsBehavior: Flickable.StopAtBounds
                    delegate: Rectangle {
                        width: ListView.view.width
                        height: 28 * Theme.unit
                        color: stationMouse.pressed ? Theme.cardPressed
                            : stationMouse.containsMouse ? Theme.card : "transparent"
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 8 * Theme.unit
                            anchors.right: stationAbout.left
                            anchors.rightMargin: 6 * Theme.unit
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: Theme.text
                            font.pixelSize: Theme.controlTextSize
                        }
                        Text {
                            id: stationAbout
                            anchors.right: parent.right
                            anchors.rightMargin: 8 * Theme.unit
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.about
                            textFormat: Text.PlainText
                            color: Theme.secondaryText
                            font.pixelSize: Theme.detailSize
                        }
                        MouseArea {
                            id: stationMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: root.call("playFound", [index])
                        }
                    }
                }
            }
            Text {
                width: parent.width
                text: "Searches go to radio-browser.info, a free directory."
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.tertiaryText
                font.pixelSize: Theme.detailSize
            }
        }

        // Address: any stream, or a .pls or .m3u
        Item {
            visible: root.tab === "address"
            width: parent.width
            height: Theme.controlHeight
            Field {
                id: addressField
                anchors.left: parent.left
                anchors.right: playButton.left
                anchors.rightMargin: 6 * Theme.unit
                height: parent.height
                placeholderText: "Any stream address (or .pls, .m3u)"
                // Return is the field's: not on to the app, where it opens Chat (windowz, 10-31)
                Keys.onReturnPressed: { root.playAddress(); event.accepted = true; }
                Keys.onEnterPressed: { root.playAddress(); event.accepted = true; }
            }
            PanelButton {
                id: playButton
                anchors.right: parent.right
                text: "Play"
                strong: true
                onPressed: root.playAddress()
            }
        }

        // the player's skin
        Item {
            width: parent.width
            height: Theme.controlHeight
            PanelButton {
                anchors.left: parent.left
                width: (parent.width - 6 * Theme.unit) / 2
                text: "Load a skin…"
                enabled: typeof RadioSkins !== "undefined"
                onPressed: {
                    root.pickingSkin = true;
                    RadioSkins.pick();
                }
            }
            PanelButton {
                anchors.right: parent.right
                width: (parent.width - 6 * Theme.unit) / 2
                text: "Reset to Darkride"
                onPressed: root.call("resetSkin", [])
            }
        }

        Text {
            width: parent.width
            visible: root.noteText !== ""
            text: root.noteText
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: root.noteProblem ? Theme.destructive : Theme.secondaryText
            font.pixelSize: Theme.controlTextSize
        }
    }

    Loader {
        id: loader
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: controls.top
        anchors.bottomMargin: Theme.gap
        active: root.live
        sourceComponent: WebEngineView {
            id: view
            Component.onCompleted: view.url = root.pageUrl
            backgroundColor: "#0c1022"   // the page's own background (radio.css --navy), behind it while it loads
            // the skin's pixels at a whole size: twice once both of the player's windows fit twice; on Windows always 1x,
            // where the switch to 2x froze the page's picture (its web engine is Chrome 83) while the music played on
            // (windowz, 10-31)
            zoomFactor: Qt.platform.os !== "windows" && width >= 600 && height >= 500 ? 2 : 1

            // one profile for the whole run, a walled-off client's folders set before it opens them (ContextAwareProfile)
            profile: TivoliWebEngineProfile.radioProfile
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
            onFileDialogRequested: function (request) {
                request.accepted = true;
                if (typeof WindowScriptingInterface === "undefined") {
                    request.dialogReject();
                    return;
                }
                // one still waiting (its dialog never answered) gives way: asking again always opens a dialog
                if (root.fileRequest) {
                    try {
                        root.fileRequest.dialogReject();
                    } catch (error) {
                        // already answered
                    }
                }
                root.fileRequest = request;
                var skin = request.acceptedMimeTypes.join(" ").indexOf("wsz") >= 0;
                console.log("Radio: a file dialog for", skin ? "a skin" : "a sound file");
                WindowScriptingInterface.browseAsync(skin ? "Load a skin" : "Play a file", "",
                    skin ? "Classic skins (*.wsz *.zip)" : "Sound files (*.mp3 *.ogg *.wav *.flac *.m4a *.aac)");
            }
            onJavaScriptDialogRequested: function (request) {
                request.accepted = true;
                request.dialogReject();
            }
            onTitleChanged: {
                if (title === "Radio: close") {
                    root.closeRadio();
                } else if (title.indexOf("Radio: events") === 0) {
                    root.takeEvents();
                }
            }
            onRenderProcessTerminated: function (terminationStatus, exitCode) {
                console.warn("Radio: the page's process ended", terminationStatus, exitCode);
            }
        }
    }
}
