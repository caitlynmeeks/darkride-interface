//
//  ExplorePanel.qml
//  qml/darkride
//
//  Darkride's Explore panel, in the panels' iOS look (darkride/panel): worlds to go to, in three lists (Popular, with
//  the worlds that open as instances; Liked; Private, your own), searchable; each a card with its picture, author,
//  likes, who is there, and Hop (where the people are, else somewhere running, else a new public instance), with the
//  places running now and new instances (public, friends, private) under More. The instance you are in has its own
//  card: its link, edit mode, save, close. Back and Forward walk your history of places. The worlds are a collection
//  under the panels' search bar, pinned at the top (Caity, 10-24): the search (the metaverse's), Cards / Tiles / List
//  (a tile's or a row's click opens its card alone), and the order: Relevance (the metaverse's own: people in it,
//  then the newest), Likes, People, Name; sorted here, over the worlds loaded. An author's name opens their profile
//  in the People panel (its Profile tab, Panels.showProfile).
//
//  The version check (with the metaverse's version-check, Caity's yes 10-24): the client's protocol signature goes with
//  a list's request and with an instance's start, so a world or an instance running another version of Darkride is
//  shown as such and can't be hopped to, and a start the servers can't serve is refused at once (409) instead of
//  after a burst server boots and the world turns the client away. An empty protocol counts as compatible.
//
//  A hop takes the screen at once (worldLoading.js, on the local "Darkride-Hop" channel) and Explore closes; the
//  join is AddressManager's. It replaces the web page of the same name and its handler (tivoli-scripts explore).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.15
import QtQuick.Layouts 1.15
import TabletScriptingInterface 1.0
import "panel"

Page {
    id: root

    property real unit: 1
    signal sendToScript(var message)   // the tablet's loader connects it; nothing is sent

    anchors.fill: parent
    title: "Explore"
    tabs: ["Popular", "Liked", "Private"]

    readonly property var kinds: ["popular", "liked", "private"]
    readonly property string tabSetting: "io.darkride.explorePanel.tab"
    readonly property string protocol: Window.protocolSignature()

    // what a failed metaverse request tells the player: the metaverse's own words where it wrote them for people (its
    // rule, 10-28: a 409, a 429, or a 400/403/404 whose message is a sentence, not "Forbidden" or "Not Found"; its
    // validation answers are lists), else ours by the status; its technical words go to the log, never the panel
    // (Caity 10-28: "amateur hour"). gone: the 404's sentence; serverDown: a 5xx's
    function plainly(code, data, failed, gone, serverDown) {
        var message = data && typeof data.message === "string" ? data.message.trim() : "";
        var forPeople = message !== "" && message !== "Forbidden" && message !== "Not Found";
        if (forPeople && [400, 403, 404, 409, 429].indexOf(code) >= 0) {
            return /[.!?]$/.test(message) ? message : message + ".";
        }
        if (data && data.message) {
            console.log("Explore: the metaverse answered " + code + " " + JSON.stringify(data.message));
        }
        return code === 0 ? "Couldn't reach Darkride. Check your connection and try again."
            : code === 401 ? "Sign in again, then try once more."
            : code === 403 ? "You can't do that here."
            : code === 404 ? gone
            : code >= 500 ? serverDown
            : failed;
    }
    Component.onCompleted: {
        currentTab = Math.max(0, Math.min(2, Settings.getValue(tabSetting, 0)));
        explore.load(true);
        here.refresh();
        history.refresh();
    }
    onCurrentTabChanged: {
        Settings.setValue(tabSetting, currentTab);
        explore.load(true);
    }
    onVisibleChanged: {
        if (visible) {
            explore.load(true);
            here.refresh();
            history.refresh();
        } else if (explore.startFailure.id !== "") {
            explore.startFailure = { id: "", text: "" };
        }
    }

    // ==== the worlds ==================================================================================================

    QtObject {
        id: explore

        property var worlds: []
        property int page: 1
        property bool loading: false
        property bool more: false          // the last page was full: there may be another
        property string status: ""
        property string search: ""
        property string hoppingWorld: ""   // a hop under way, so a second click doesn't start another
        readonly property int amount: 50
        property var focused: null         // a world opened from its tile or row, shown alone
        property var likesNow: ({})        // likes changed here since the list came, by world id: { liked, likes }
        // the last new instance that couldn't start, said on its world's card, tile or row once Explore is open again
        // (until it closes)
        property var startFailure: ({ id: "", text: "" })
        // the new instance being started, { world, visibility, id, cancelled, answered }: Cancel drops its late address;
        // the same start asked for again before its answer (Hop after Cancel) takes it up again rather than starting a
        // second server (Caity's hand test 10-30: two servers for one person)
        property var startTicket: null

        // the worlds in the search bar's order (the metaverse searched them); ties keep the metaverse's order
        readonly property var shown: worldBar.arrange(worlds, false)

        function query(extra) {
            var parts = ["page=" + page, "amount=" + amount, "search=" + encodeURIComponent(search),
                         "protocol=" + encodeURIComponent(root.protocol)];
            return "?" + parts.concat(extra || []).join("&");
        }

        function load(reset) {
            page = reset ? 1 : page + 1;
            loading = true;
            if (reset) {
                focused = null;
                likesNow = {};
            }
            var kind = root.kinds[root.currentTab];
            var path = kind === "liked" ? "/api/user/domains/liked" : kind === "private" ? "/api/user/domains/private"
                : "/api/domains";
            if (kind !== "popular" && !AccountServices.loggedIn) {
                loading = false;
                more = false;
                worlds = [];
                status = kind === "liked" ? "Sign in to see the worlds you've liked." : "Sign in to see your private worlds.";
                return;
            }
            var pending = kind === "popular" && page === 1 ? 2 : 1;
            var online = [];
            var withInstances = [];
            var done = function () {
                if (--pending > 0) {
                    return;
                }
                loading = false;
                var merged = merge(online, withInstances);
                more = online.length >= amount;
                worlds = reset ? merged : worlds.concat(merged);
            };
            MetaverseRequests.send("GET", path + query(), null, function (error, code, data) {
                if (error) {
                    console.log("Explore: worlds failed: " + code + " " + error);
                    status = "Couldn't list the worlds. Try again in a moment.";
                } else {
                    status = "";
                    online = Array.isArray(data) ? data : [];
                }
                done();
            });
            if (pending === 2) {
                // the worlds that open as instances are listed with the online ones, whether or not anyone is in them.
                // The protocol tells the metaverse this is a client that greys out other versions' worlds itself: a
                // list asked without it (1.0.2's Explore) leaves out the always-on worlds that run ahead, the beta's
                // Test Track first (Caity, 10-25: "hide it")
                MetaverseRequests.send("GET", "/api/worlds?search=" + encodeURIComponent(search) +
                                       "&protocol=" + encodeURIComponent(root.protocol), null,
                                       function (error, code, data) {
                    withInstances = !error && Array.isArray(data) ? data : [];
                    done();
                });
            }
        }

        function people(world) {
            return (world.numUsers || 0) + ((world.instances && world.instances.list) || []).reduce(function (n, i) {
                return n + (i.players || 0);
            }, 0);
        }

        // the online worlds as listed, each replaced by its record with instances when there is one; then the rest of
        // the instance worlds, those with people in them first. Every tab comes through here: an entry that isn't a
        // world is skipped (the online list gave a private world as null, 10-29, and the throw left Explore empty)
        function merge(online, withInstances) {
            var isWorld = function (world) { return !!world && typeof world === "object" && !!world.id; };
            online = online.filter(isWorld);
            withInstances = withInstances.filter(isWorld);
            var byId = {};
            withInstances.forEach(function (world) { byId[world.id] = world; });
            var merged = online.map(function (world) { return byId[world.id] || world; });
            var listed = {};
            merged.forEach(function (world) { listed[world.id] = true; });
            var rest = withInstances.filter(function (world) { return !listed[world.id]; })
                .sort(function (a, b) { return people(b) - people(a); });
            return merged.concat(rest.filter(function (w) { return people(w) > 0; }))
                .concat(rest.filter(function (w) { return people(w) === 0; }));
        }

        // a world's line under its name: who's there, or what the visitor must do first. Nothing about an empty world
        // (Caity 11-01, "quiet explore"): its picture and name invite; saying nobody's there makes Explore look deserted
        function summary(world, list) {
            var instances = world.instances || null;
            var here = people(world);
            if (!compatible(world.protocol)) return "Runs another version of Darkride.";
            if (here > 0) return here + (here === 1 ? " person here" : " people here");
            if (list.length === 0 && instances && !instances.canStart) return "Sign in to open this world.";
            return "";
        }

        function compatible(protocol) {
            return !protocol || !root.protocol || protocol === root.protocol;
        }

        // where a world can be joined: its own server when it is up, then its instances; the fullest first
        function places(world) {
            var instances = world.instances || null;
            var list = [];
            if (!!world.online && !(instances && instances.instanceOnly)) {
                list.push({ name: "Always on", detail: "The world's own server · " + (world.numUsers || 0) + " here",
                            people: world.numUsers || 0, instance: null, compatible: compatible(world.protocol) });
            }
            ((instances && instances.list) || []).forEach(function (instance) {
                var bits = [(instance.players || 0) + " here"];
                if (instance.startedBy) bits.push("Started by " + instance.startedBy);
                if (!instance.latest) bits.push("An older version");
                if (instance.editing) bits.push("Being edited");
                var ok = compatible(instance.protocol);
                if (!ok) bits.push("Another version of Darkride");
                list.push({ name: instance.visibility.charAt(0).toUpperCase() + instance.visibility.slice(1) + " instance",
                            detail: bits.join(" · "), people: instance.players || 0, instance: instance, compatible: ok });
            });
            return list.sort(function (a, b) { return b.people - a.people; });
        }
        function canHop(world, list) {
            var instances = world.instances || null;
            return compatible(world.protocol)
                && (list.some(function (p) { return p.compatible; }) || !!(instances && instances.canStart));
        }
        // where the people are; else somewhere running; else a new public instance
        function hop(world, list) {
            var instances = world.instances || null;
            var open = list.filter(function (p) { return p.compatible; });
            var best = open.filter(function (p) { return p.people > 0; })[0]
                || open.filter(function (p) { return p.instance === null; })[0]
                || open.filter(function (p) { return p.instance && p.instance.visibility === "public"; })[0]
                || (instances && instances.canStart ? null : open[0]);
            if (best) {
                join(world, best);
            } else if (instances && instances.canStart) {
                start(world, "public");
            }
        }
        function join(world, place) {
            if (place.instance) {
                joinInstance(world, place.instance);
            } else {
                joinWorld(world);
            }
        }
        function start(world, visibility) {
            startInstance(world, visibility);
        }

        // "by <author>", the name a link to their profile (a ListRow detail or a tile's line, as rich text)
        function escaped(text) {
            return String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
        }
        function byline(world, rest) {
            var parts = [];
            if (world.username) {
                parts.push("By <a href=\"" + encodeURIComponent(world.username) + "\">" + escaped(world.username) + "</a>");
            }
            if (rest) {
                parts.push(escaped(rest));
            }
            return "<style>a { color: " + Theme.tint + "; text-decoration: none; }</style>" + parts.join(" · ");
        }
        // someone's profile: the People panel opens on it, wherever it is (the card chat's names open too)
        function openProfile(username) {
            Panels.showProfile(username);
        }

        function picture(worldId) {
            return MetaverseRequests.metaverseUrl() + "/api/domain/" + worldId + "/image";
        }

        // a hop: the loading screen and the pop at once, Explore out of the way, then the join. startId: a new
        // instance's start, whose server the screen waits for (it then makes the hop itself)
        function hopStart(world, startId) {
            Tablet.playSound(TabletEnums.TabletOpen);
            Messages.sendLocalMessage("Darkride-Hop", JSON.stringify({
                type: "start", world: world.label || "", author: world.username || "",
                picture: world.id ? picture(world.id) : "", starting: !!startId, id: startId || ""
            }));
            root.closePanel();
        }
        // what was typed in the search, when it's an address rather than words to look for (Caity, 10-25: she typed
        // hifi://localhost:42102 and got "No world goes by that name"; Explore is where the address shortcut goes now):
        // hifi://..., a host with its port (localhost:42102, 10.0.0.5:40102/spot), or a /path in this world. "" if not
        function address(text) {
            var typed = String(text || "").trim();
            if (/^hifi:\/\//i.test(typed) || /^\/\S*$/.test(typed)) {
                return typed;
            }
            if (/^[A-Za-z0-9.-]+:\d{1,5}(\/\S*)?$/.test(typed)) {
                return "hifi://" + typed;
            }
            return "";
        }
        // goes to a typed address, with the loading screen as a hop has it
        function goToAddress(typed) {
            var where = address(typed);
            if (where === "") {
                return false;
            }
            hoppingWorld = "";
            hopStart({ label: where });
            AddressManager.handleLookupString(where);
            return true;
        }
        function joinWorld(world) {
            hoppingWorld = world.id;
            hopStart(world);
            AddressManager.handleLookupString(world.id + (world.path || ""));
        }
        function joinInstance(world, instance) {
            hoppingWorld = world.id;
            hopStart(world);
            AddressManager.handleLookupString(instance.address);
        }
        // a new instance: the loading screen goes up at once and waits while the metaverse starts a server (it answers
        // with the address once the server is up: a minute or more on a fresh server, Caity's 10-30 bug: "Hopping…" on
        // the card all that time). The hop goes when the address comes, unless the screen's Cancel was pressed by then
        // (the instance stays listed, to join later); a refusal (another version, the servers full) is said on the
        // screen, and Explore opens again with it on the world's card
        function startInstance(world, visibility) {
            if (hoppingWorld !== "") {
                return;
            }
            hoppingWorld = world.id;
            startFailure = { id: "", text: "" };
            var pending = startTicket;
            if (pending && pending.cancelled && !pending.answered && pending.world === world.id &&
                    pending.visibility === visibility) {
                pending.cancelled = false;
                hopStart(world, pending.id);
                return;
            }
            var startId = world.id + "@" + Date.now();
            var ticket = { world: world.id, visibility: visibility, id: startId, cancelled: false, answered: false };
            startTicket = ticket;
            // the answer may come after the page has gone (in the tablet, closing it goes to the home screen; the
            // headset's screen has no Cancel), so it keeps what it needs
            var messages = Messages;
            var addresses = AddressManager;
            var plainly = root.plainly;
            hopStart(world, startId);
            MetaverseRequests.send("POST", "/api/worlds/" + world.id + "/instances",
                                   { visibility: visibility, protocol: root.protocol }, function (error, code, data) {
                ticket.answered = true;
                if (ticket.cancelled) {
                    console.log("Explore: the new instance answered " + code + " after its hop was cancelled");
                    return;
                }
                if (!error && data && data.address) {
                    messages.sendLocalMessage("Darkride-Hop", JSON.stringify({ type: "started", id: startId }));
                    addresses.handleLookupString(String(data.address));
                    return;
                }
                var text = plainly(code, data, "Couldn't start an instance. Try again in a minute.",
                                   "That world isn't there any more.",
                                   "Couldn't start a server right now. Try again in a minute.");
                messages.sendLocalMessage("Darkride-Hop", JSON.stringify({ type: "cancel", id: startId, reason: text }));
                try {
                    if (hoppingWorld === world.id) {
                        hoppingWorld = "";
                    }
                    startFailure = { id: world.id, text: text };
                } catch (e) {
                    // the page has gone: the screen said it
                }
            });
        }
        function failureFor(world) {
            return startFailure.id !== "" && startFailure.id === world.id ? startFailure.text : "";
        }

        // a world from its tile, alone (an inline component can't reach the page's id, so the page scrolls from here)
        function openWorld(world) {
            focused = world;
            root.scrollToTop();
        }

        function likeState(world) {
            return likesNow[world.id] || { liked: !!world.liked, likes: world.likes || 0 };
        }
        function like(world, on) {
            MetaverseRequests.send("POST", "/api/domains/" + world.id + "/" + (on ? "like" : "unlike"), null,
                                   function (error) {
                var state = likeState(world);
                if (error || state.liked === on) {
                    return;
                }
                var next = Object.assign({}, likesNow);
                next[world.id] = { liked: on, likes: state.likes + (on ? 1 : -1) };
                likesNow = next;
            });
        }
    }

    // AddressManager lives on the network thread, and QML may not connect to an object on another thread (the client
    // aborts), so a world change is heard from Window and the history is read when it may have changed
    Connections {
        target: Window
        function onDomainChanged() { explore.hoppingWorld = ""; here.refresh(); history.refresh(); }
        // the loading screen's Cancel: a hop that hasn't left stops there (a new instance's late address is dropped by
        // the screen), one that has goes back, which changes the world anyway
        function onLoadingScreenButtonPressed(button) {
            if (button === "cancel") {
                explore.hoppingWorld = "";
                if (explore.startTicket) {
                    explore.startTicket.cancelled = true;
                }
            }
        }
    }
    Connections {
        target: AccountServices
        function onLoggedInChanged() { explore.load(true); }
    }

    // ==== the instance you are in ======================================================================================

    QtObject {
        id: here

        property var current: null     // { world, instance, editingBy } when the world you're in is an instance
        property bool busy: false
        property string status: ""
        property bool confirmClose: false

        function domainId() {
            return String(AddressManager.domainID).replace("{", "").replace("}", "");
        }

        function refresh() {
            var id = domainId();
            if (id === "" || id === "00000000-0000-0000-0000-000000000000") {
                current = null;
                return;
            }
            MetaverseRequests.send("GET", "/api/instances/by-domain/" + id, null, function (error, code, data) {
                current = !error && data && data.instance ? data : null;
            });
        }

        function editMode(on) {
            busy = true;
            MetaverseRequests.send("POST", "/api/instances/" + current.instance.id + "/edit", { on: on },
                                   function (error, code, data) {
                busy = false;
                if (error) {
                    status = root.plainly(code, data,
                                          on ? "Couldn't turn edit mode on." : "Couldn't turn edit mode off.",
                                          "That instance is gone.",
                                          "Couldn't reach the instance's server. Try again in a minute.");
                    return;
                }
                current = data;
                status = on ? "Edit mode is on: you can build here. Save to update the world." : "Edit mode is off.";
            });
        }

        function save() {
            busy = true;
            MetaverseRequests.send("POST", "/api/instances/" + current.instance.id + "/save", null,
                                   function (error, code, data) {
                busy = false;
                status = error ? root.plainly(code, data, "Couldn't save the world. Try again in a moment.",
                                              "That instance is gone.",
                                              "Couldn't reach the instance's server. Try again in a minute.")
                    : "Saved as version " + data.version + ", " + data.entities + " entities.";
                refresh();
            });
        }

        function close() {
            busy = true;
            confirmClose = false;
            MetaverseRequests.send("DELETE", "/api/instances/" + current.instance.id, null, function (error, code, data) {
                busy = false;
                status = error ? root.plainly(code, data, "Couldn't close the instance. Try again in a moment.",
                                              "That instance is already gone.",
                                              "Couldn't reach the instance's server. Try again in a minute.") : "";
                if (!error) {
                    current = null;
                }
            });
        }
    }

    // ==== the page ====================================================================================================

    // pinned: the panels' search bar (searching, the view and the order), and walking back and forth through the
    // places you've been
    header: [
        SearchBar {
            id: worldBar
            settingsKey: "io.darkride.explorePanel.worlds"
            placeholder: "Search worlds, or type an address"
            sorts: [
                { text: "Relevance", value: "relevance", by: null },
                { text: "Likes", value: "likes", by: function (a, b) {
                    return explore.likeState(b).likes - explore.likeState(a).likes; } },
                { text: "People", value: "people", by: function (a, b) { return explore.people(b) - explore.people(a); } },
                { text: "Name", value: "name", by: function (a, b) {
                    return String(a.label || "").toLowerCase().localeCompare(String(b.label || "").toLowerCase()); } }
            ]
            onSearchChanged: { explore.search = search; explore.load(true); }
            onAccepted: explore.goToAddress(text)
            onViewChanged: explore.focused = null
            onSortChanged: scrollToTop()

            CardButton {
                visible: history.canGoBack
                height: Theme.controlHeight
                text: "‹ Back"
                onPressed: AddressManager.goBack()
            }
            CardButton {
                visible: history.canGoForward
                height: Theme.controlHeight
                text: "Forward ›"
                onPressed: AddressManager.goForward()
            }
        }
    ]
    QtObject {
        id: history
        property bool canGoBack: false
        property bool canGoForward: false
        function refresh() {
            canGoBack = AddressManager.canGoBack();
            canGoForward = AddressManager.canGoForward();
        }
    }

    // the instance you're in
    Section {
        visible: here.current !== null
        title: here.current ? "You're in " + here.current.world.label : ""
        footer: here.status !== "" ? here.status
            : here.current && here.current.editingBy ? "Being edited by " + here.current.editingBy + "." : ""

        ListRow {
            label: here.current && here.current.instance
                ? (here.current.instance.visibility.charAt(0).toUpperCase() + here.current.instance.visibility.slice(1))
                  + " instance"
                : ""
            detail: here.current && here.current.instance
                ? here.current.instance.players + (here.current.instance.players === 1 ? " person" : " people")
                  + (here.current.instance.latest ? "" : " · An older version")
                : ""
            Text {
                text: here.current && here.current.instance ? here.current.instance.address : ""
                textFormat: Text.PlainText
                color: Theme.tertiaryText
                font.pixelSize: Theme.detailSize
                elide: Text.ElideMiddle
                Layout.maximumWidth: 200 * Theme.unit
            }
        }
        ListRow {
            action: true
            label: "Copy Link"
            onClicked: {
                Window.copyToClipboard(here.current.instance.address);
                here.status = "Copied " + here.current.instance.address + ".";
            }
        }
        ListRow {
            visible: here.current !== null && here.current.world.instances && here.current.world.instances.canEdit
            label: "Edit mode"
            detail: "Build here, then save to update the world"
            Toggle {
                checked: here.current !== null && here.current.instance && here.current.instance.editing
                enabled: !here.busy
                onToggled: here.editMode(checked)
            }
        }
        ListRow {
            visible: here.current !== null && here.current.instance && here.current.instance.editing
            action: true
            label: "Save the World"
            enabled: !here.busy
            onClicked: here.save()
        }
        ListRow {
            visible: here.current !== null && here.current.instance && here.current.instance.canStop && !here.confirmClose
            action: true
            destructive: true
            label: "Close This Instance…"
            onClicked: here.confirmClose = true
        }
        ListRow {
            visible: here.confirmClose
            action: true
            label: "Cancel"
            onClicked: here.confirmClose = false
        }
        ListRow {
            visible: here.confirmClose
            action: true
            destructive: true
            label: "Close It for Everyone"
            enabled: !here.busy
            onClicked: here.close()
        }
    }

    // a small button inside a row or a card
    component CardButton: Rectangle {
        id: cardButton
        property string text: ""
        property bool strong: false
        signal pressed()
        implicitWidth: cardButtonLabel.implicitWidth + 22 * Theme.unit
        implicitHeight: 28 * Theme.unit
        radius: 8 * Theme.unit
        color: strong ? Theme.tint : Theme.fill
        opacity: enabled ? 1 : 0.4
        Text {
            id: cardButtonLabel
            anchors.centerIn: parent
            text: cardButton.text
            color: cardButton.strong ? "#ffffff" : Theme.tint
            font.pixelSize: Theme.controlTextSize
            font.bold: cardButton.strong
        }
        MouseArea {
            anchors.fill: parent
            enabled: cardButton.enabled
            onClicked: cardButton.pressed()
        }
    }

    // likes: a heart and its count; a click likes or unlikes, signed in
    component Likes: Text {
        id: likes
        property var world: ({})
        readonly property var state: explore.likeState(world)
        text: (state.liked ? "♥ " : "♡ ") + state.likes
        color: state.liked ? Theme.destructive : Theme.secondaryText
        font.pixelSize: Theme.bodySize
        MouseArea {
            anchors.fill: parent
            anchors.margins: -6 * Theme.unit
            enabled: AccountServices.loggedIn
            onClicked: explore.like(likes.world, !likes.state.liked)
        }
    }

    // one world: its picture, name, author, likes, who's there, Hop; More opens the places and new instances
    component WorldCard: Section {
        id: card
        objectName: "worldCard"   // a test opens one by it
        property var world: ({})
        property bool open: false
        readonly property var instances: world.instances || null
        readonly property bool compatible: explore.compatible(world.protocol)
        readonly property int people: explore.people(world)
        readonly property var places: explore.places(world)
        readonly property bool canHop: explore.canHop(world, places)
        readonly property string summary: explore.summary(world, places)

        footer: explore.failureFor(world)

        Item {
            width: parent ? parent.width : 0
            height: worldPicture.ready ? width * 0.42 : 0
            visible: height > 0
            RoundedImage {
                id: worldPicture
                anchors.fill: parent
                roundBottom: false
                source: card.world.id ? explore.picture(card.world.id) : ""
                opacity: card.compatible ? 1 : 0.45
            }
        }
        ListRow {
            label: card.world.label || ""
            detail: explore.byline(card.world, card.summary)
            detailFormat: Text.RichText
            onLinkActivated: explore.openProfile(decodeURIComponent(link))
            clickable: true
            onClicked: card.open = !card.open

            Likes {
                world: card.world
            }
            CardButton {
                text: explore.hoppingWorld === card.world.id ? "Hopping…" : "Hop"
                strong: true
                enabled: card.canHop && explore.hoppingWorld === ""
                onPressed: explore.hop(card.world, card.places)
            }
        }
        ListRow {
            visible: !!card.world.description && card.open
            label: card.world.description || ""
            enabled: false
        }
        Repeater {
            model: card.open ? card.places : []
            delegate: ListRow {
                label: modelData.name
                detail: modelData.detail
                enabled: modelData.compatible
                CardButton {
                    text: "Join"
                    enabled: modelData.compatible && explore.hoppingWorld === ""
                    onPressed: explore.join(card.world, modelData)
                }
            }
        }
        ListRow {
            visible: card.open && !!(card.instances && card.instances.canStart) && card.compatible
            label: "New instance"
            detail: "Public: anyone · Friends: your friends · Private: only you"
            CardButton { text: "Public"; enabled: explore.hoppingWorld === ""; onPressed: explore.start(card.world, "public") }
            CardButton { text: "Friends"; enabled: explore.hoppingWorld === ""; onPressed: explore.start(card.world, "friends") }
            CardButton { text: "Private"; enabled: explore.hoppingWorld === ""; onPressed: explore.start(card.world, "private") }
        }
        ListRow {
            visible: !card.open && (card.places.length > 0 || !!(card.instances && card.instances.canStart))
            action: true
            label: "More ⌄"
            onClicked: card.open = true
        }
    }

    // one world as a tile, three a row: its picture, name, author, likes, who's there, Hop; a click opens its card
    component WorldTile: CollectionTile {
        id: tile
        objectName: "worldTile"
        property var world: ({})
        readonly property bool compatible: explore.compatible(world.protocol)
        readonly property int people: explore.people(world)
        readonly property var places: explore.places(world)
        readonly property bool canHop: explore.canHop(world, places)

        title: world.label || ""
        subtitle: explore.byline(world, !compatible ? "Another version" : people > 0 ? people + " here" : "")
        picture: world.id ? explore.picture(world.id) : ""
        pictureRatio: 0.6
        dim: !compatible
        status: explore.failureFor(world)
        onClicked: explore.openWorld(world)
        onLinkActivated: explore.openProfile(decodeURIComponent(link))

        Likes {
            world: tile.world
        }
        Item {
            Layout.fillWidth: true
        }
        CardButton {
            text: explore.hoppingWorld === tile.world.id ? "Hopping…" : "Hop"
            strong: true
            enabled: tile.canHop && explore.hoppingWorld === ""
            onPressed: explore.hop(tile.world, tile.places)
        }
    }

    // one world as a row of the List: a small picture, name, author and who's there, likes, Hop; a click opens its card
    component WorldRow: ListRow {
        id: worldRow
        property var world: ({})
        readonly property string failure: explore.failureFor(world)
        readonly property var places: explore.places(world)
        readonly property bool canHop: explore.canHop(world, places)

        label: world.label || ""
        detail: failure !== "" ? explore.escaped(failure) : explore.byline(world, explore.summary(world, places))
        detailFormat: Text.RichText
        onLinkActivated: explore.openProfile(decodeURIComponent(link))
        icon: world.id ? explore.picture(world.id) : ""
        iconSize: 40 * Theme.unit
        iconRadius: 8 * Theme.unit
        iconPlaceholder: world.label || "?"
        clickable: true
        onClicked: explore.openWorld(world)

        Likes {
            world: worldRow.world
        }
        CardButton {
            text: explore.hoppingWorld === worldRow.world.id ? "Hopping…" : "Hop"
            strong: true
            enabled: worldRow.canHop && explore.hoppingWorld === ""
            onPressed: explore.hop(worldRow.world, worldRow.places)
        }
    }

    // a world opened from its tile or row, alone, with the way back
    Section {
        visible: explore.focused !== null
        ListRow {
            action: true
            label: "‹ All Worlds"
            onClicked: explore.focused = null
        }
    }
    Repeater {
        model: explore.focused !== null ? [explore.focused] : []
        delegate: WorldCard { world: modelData; open: true }
    }

    CollectionView {
        visible: explore.focused === null
        items: explore.shown
        view: worldBar.view
        card: Component { WorldCard { world: entry } }
        tile: Component { WorldTile { world: entry } }
        row: Component { WorldRow { world: entry } }
        empty: explore.loading ? "…" : explore.status !== "" ? explore.status
            : explore.address(explore.search) !== "" ? "Press Return to go to " + explore.address(explore.search) + "."
            : explore.search !== "" ? "No world goes by that name."
            : root.currentTab === 1 ? "Worlds you like show here." : root.currentTab === 2 ? "Your private worlds show here."
            : "No worlds are open right now."
    }

    Section {
        visible: explore.more && explore.focused === null
        ListRow {
            action: true
            label: explore.loading ? "Loading…" : "Show More Worlds"
            enabled: !explore.loading
            onClicked: explore.load(false)
        }
    }
}
