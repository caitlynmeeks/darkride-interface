"use strict";
//
//  worldLoading.js
//  scripts/system
//
//  The loading screen's state and timing. It covers the way into a world: a hop (another script says one has started),
//  the first arrival after sign-in, and a world whose owner asks for it to load whole first (in clients built with
//  whole-world loading, Window.getWorldLoading). On a desktop the screen itself is
//  interface/resources/qml/hifi/WorldLoadingScreen.qml, which this drives by local messages; in a headset, where that
//  screen hides itself, this shows a card of its own: local entities that follow the camera. The desktop screen is
//  driven by Window.setLoadingScreen({up, world, author, picture, status}) (QML cannot listen to Messages).
//
//  The local message channel "Darkride-Hop":
//    in:  {"type": "start", "world": <name>, "author": <name or "">, "picture": <URL or "">} when a hop starts
//         (Explore's Hop or Join, "new instance", the address bar, a friend's Join), and {"type": "cancel"} should it
//         fail before the client leaves. A new instance's hop starts while the metaverse is still starting its server
//         ("starting": true, "id": <the start's id>), up to a few minutes on a fresh server: the screen says so and
//         waits, without its time limit, until {"type": "started", "id"} as the hop goes (Explore makes it, unless the
//         screen's Cancel was pressed by then), or {"type": "cancel", "id", "reason": <words for people>}, which it
//         says on the screen for a moment before it opens Explore again;
//    out: {"type": "screen", "up": <bool>, "world", "author", "picture", "status"} whenever any of it changes (for
//         scripts; the same goes to Window.setLoadingScreen), and "up": false fades it out.
//  The screen lifts once the client has left the old world (its physics or its connection gone), the new world is
//  connected, its physics is on and the avatar's own model has loaded
//  (MyAvatar.isSkeletonModelLoaded; at most 10 s more after physics). A cancel, a refused connection, or 90 s without
//  arriving lifts it anyway. Cancel is there on every way in after 3 s: back to the world it came from, or, with none
//  (the first arrival), to an empty world of the viewer's own with Explore open; so does the viewer giving up on a world that doesn't answer (its connection failure
//  window opens), or a hop's place not found. While it is up, the arrival waits for what is near the avatar to be
//  ready for physics and
//  drawn, even where the zone would let the avatar move at once (Window.setArrivalWaitsForNearby): no pop-in under the
//  avatar. At the first arrival, the launch screen (sign-in's navy cover) is released once this one covers the view.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

/* global Script, Entities, MyAvatar, Window, Messages, HMD, Account, Settings, location, AddressManager */

(function () {
    var CHANNEL = "Darkride-Hop";
    // One steady line, Caity's of 10-21: "Arriving…" for every way in, "Loading the world…" while a world that its
    // owner set to load whole first is loading (its bar then fills). No line per step: three lines in a second and a
    // half read as noise.
    var STATUS_ARRIVING = "Arriving…";
    var STATUS_WHOLE = "Loading the world…";
    var STATUS_STARTING = "Starting the world… A new server can take a minute or two.";
    var MIN_SHOWN = 2000;         // ms: the screen stays at least this long, so an arrival never flashes it (Caity, 10-21)
    var TIME_LIMIT = 90;          // s: a way in that never arrives lifts the screen anyway
    // s: a new instance's server that never comes. The metaverse answers within ~8 min (its front keeps the request
    // open 10 min), so this is only a backstop
    var STARTING_LIMIT = 660;
    var FAILED_SHOWN = 3000;      // ms: why a new instance couldn't start, on the screen before it lifts
    // the world a start-up went to (Settings > General > Start in) didn't answer, and the viewer goes to the entry world
    // instead: said until the arrival there, and at least NOTICE_SHOWN (Caity's 1.1.1, 10-30: her last world was an
    // instance stopped since; she got "Can't reach this world" and endless tries)
    var STATUS_FALLBACK = {
        last: "The world you were in last isn't answering. Taking you to Darkside.",
        home: "Your home world isn't answering. Taking you to Darkside."
    };
    var NOTICE_SHOWN = 4000;      // ms
    // ms: the screen's buttons (Cancel, and Show me now while a world loads whole) fade in once it has been up this
    // long, so a quick arrival never shows them (Caity, 10-22: "a cancel loading button to go back and abort a load")
    var BUTTONS_AFTER = 3000;
    var AVATAR_GRACE = 10;        // s after physics: an avatar that has not loaded by then no longer holds the screen
    var LAUNCH_RELEASE_DELAY = 600;   // ms: the desktop screen's fade-in, after which it covers the launch screen
    var TICK = 50;                // ms
    // Cancel with nowhere to go back to (the first world after sign-in didn't come, or isn't there): an empty world the
    // viewer carries, always there, and Explore open on it to choose where to go (Caity, 10-25: a first run on her
    // MacBook sat under this screen with no way out)
    var EMPTY_PLACE = "file:///~/serverless/empty.json";
    var EXPLORE_CHANNEL = "com.tivolicloud.explore";   // the bar's Explore opens on "open" (tivoli-scripts buttons.ts)
    // the headset card on a desktop as well (for testing, or a client without the desktop screen)
    var CARD_ON_DESKTOP = Settings.getValue("worldLoading/cardOnDesktop", false) === true ||
        Settings.getValue("worldLoading/cardOnDesktop", false) === "true";

    // Darkride's colours (the website's --dr-* tokens)
    var COLOR_NAVY = { red: 0x1c, green: 0x21, blue: 0x36 };
    var COLOR_TRACK = { red: 0x33, green: 0x3a, blue: 0x5a };
    var COLOR_CYAN = { red: 0x00, green: 0xaf, blue: 0xff };
    var COLOR_TEXT = { red: 0xf4, green: 0xf6, blue: 0xf8 };
    var COLOR_MUTED = { red: 0x9a, green: 0xa1, blue: 0xbd };
    var SPHERE_URL = Script.resolvePath("/~/system/assets/models/black-sphere.fbx");
    var LOGO_URL = Script.resourcesPath() + "images/darkride-logo.png";

    // the headset card, in the card's units (metres at 2.2 m), drawn at CARD_DISTANCE
    var CARD_DISTANCE = 1.1;
    var SCALE = CARD_DISTANCE / 2.2;
    var CARD_WIDTH = 1.6;
    var CARD_HEIGHT = 0.9;
    var PICTURE_WIDTH = 0.72;
    var PICTURE_HEIGHT = 0.405;   // 16:9
    var BAR_WIDTH = 1.36;
    var BAR_HEIGHT = 0.012;
    var GLIDE_WIDTH = 0.3;        // the gliding part of the bar
    var GLIDE_PERIOD = 1600;      // ms for one pass
    var SPHERE_SIZE = 6.0;

    // whole-world loading is in clients built with it; without it, hops and first arrivals only
    var NOT_LOADING = { active: false };
    function loadingState() {
        return Window.getWorldLoading ? Window.getWorldLoading() : NOT_LOADING;
    }

    function avatarLoaded() {
        return MyAvatar.isSkeletonModelLoaded ? MyAvatar.isSkeletonModelLoaded() : true;
    }

    function placeName() {
        return location.placename || location.hostname || "";
    }

    function placePicture() {
        var id = location.domainID;
        if (!id || id === "{00000000-0000-0000-0000-000000000000}" || !Account.metaverseServerURL) {
            return "";
        }
        return Account.metaverseServerURL + "/api/domain/" + id.replace(/[{}]/g, "") + "/image";
    }

    // The way in under way, or null: kind "hop", "first" (the first arrival after sign-in) or "whole" (a world that
    // loads whole, reached some other way)
    var way = null;
    var sent = null;          // the last screen message sent
    var timer = null;
    var card = {};            // the headset card's entities

    // startId: a new instance's start, whose server the hop waits for (its first line already says so)
    function begin(kind, world, author, picture, startId) {
        way = {
            kind: kind,
            world: world,
            author: author || "",
            picture: picture || "",
            started: Date.now(),
            waitFrom: Date.now(),           // the time limit counts from here: the start, or a new server's address
            starting: !!startId,            // a new instance's server is starting; the hop waits for its address
            startId: startId || "",
            failed: null,                   // why a new instance couldn't start: { text, at }
            notice: null,                   // why this way goes to the entry world instead: { text, at }
            left: kind !== "hop",           // a hop has gone for the old world once the domain changes
            lostOld: kind !== "hop",        // and has seen the old world's physics or connection drop (physics is
                                            // sometimes switched on again in the very frame of a domain change)
            physicsOnAt: 0,
            launchReleased: false,
            progress: -1,                   // the whole-world bar, 0 to 1; negative while it glides
            mostWaiting: 0,                 // the most entities the landing waited for once the world was all in
            // where Cancel goes back to: the world this way left (a hop starts while still in it), none for the first
            from: kind === "hop" ? { world: placeName(), picture: placePicture() } : null
        };
        setArrivalWaitsForNearby(true);
        if (!timer) {
            timer = Script.setInterval(tick, TICK);
        }
        tick();
    }

    function end() {
        way = null;
        if (timer) {
            Script.clearInterval(timer);
            timer = null;
        }
        hideCard();
        setArrivalWaitsForNearby(false);
        send(false, "");
        releaseLaunchScreen();
    }

    function setArrivalWaitsForNearby(wait) {
        if (Window.setArrivalWaitsForNearby) {
            Window.setArrivalWaitsForNearby(wait);
        }
    }

    function releaseLaunchScreen() {
        if (Window.releaseLaunchScreen) {
            Window.releaseLaunchScreen();
        }
    }

    function send(up, status, progress, canCancel, canShowNow) {
        var message = { type: "screen", up: up, world: way ? way.world : (sent ? sent.world : ""),
            author: way ? way.author : (sent ? sent.author : ""), picture: way ? way.picture : (sent ? sent.picture : ""),
            status: status, progress: progress === undefined ? -1 : progress,
            canCancel: !!(up && canCancel), canShowNow: !!(up && canShowNow) };
        if (sent && sent.up === message.up && sent.world === message.world && sent.author === message.author &&
                sent.picture === message.picture && sent.status === message.status &&
                Math.abs(sent.progress - message.progress) < 0.005 && sent.canCancel === message.canCancel &&
                sent.canShowNow === message.canShowNow) {
            return;
        }
        if (!sent && !up) {
            return;   // nothing was ever up
        }
        sent = message;
        if (Window.setLoadingScreen) {
            // with the headset card on a desktop (worldLoading/cardOnDesktop, to check the card without a headset), the
            // full-window page stays down: it would cover the card
            Window.setLoadingScreen({ up: message.up && !CARD_ON_DESKTOP, world: message.world, author: message.author,
                picture: message.picture, status: message.status, progress: message.progress,
                canCancel: message.canCancel, canShowNow: message.canShowNow });
        }
        Messages.sendLocalMessage(CHANNEL, JSON.stringify(message));
    }

    function tick() {
        if (!way) {
            return;
        }
        var now = Date.now();
        if (way.failed) {
            if (now - way.failed.at >= FAILED_SHOWN) {
                end();
                Messages.sendLocalMessage(EXPLORE_CHANNEL, "open");
            } else {
                send(true, way.failed.text, -1, false, false);
                if (HMD.active || CARD_ON_DESKTOP) {
                    showCard(way.failed.text, now);
                }
            }
            return;
        }
        if (way.starting && now - way.started > STARTING_LIMIT * 1000) {
            print("World loading: no server after " + STARTING_LIMIT + " s, the screen lifts");
            end();
            return;
        }
        var loading = loadingState();
        var physics = Window.isPhysicsEnabled();
        var connected = location.isConnected;
        if (way.left && (!physics || !connected)) {
            way.lostOld = true;
        }
        var landed = way.lostOld && connected && physics;
        if (!landed) {
            way.physicsOnAt = 0;
        } else if (!way.physicsOnAt) {
            way.physicsOnAt = now;
        }

        // arrived: in the new world with its physics on, and the avatar's own model in (or waited for long enough);
        // the screen then lifts once it has been up MIN_SHOWN
        var arrived = !loading.active && landed && (avatarLoaded() || now - way.physicsOnAt > AVATAR_GRACE * 1000);
        if ((arrived && now - way.started >= MIN_SHOWN && (!way.notice || now - way.notice.at >= NOTICE_SHOWN)) ||
                (!loading.active && !way.starting && now - way.waitFrom > TIME_LIMIT * 1000)) {
            end();
            return;
        }

        // (a hop's name only once it has left: before that, the world connected is still the one it is leaving)
        if (!way.world && way.left && location.isConnected) {
            way.world = placeName();
            way.picture = way.picture || placePicture();
        }
        var status = way.notice ? way.notice.text : way.starting ? STATUS_STARTING :
            loading.active ? STATUS_WHOLE : STATUS_ARRIVING;
        if (loading.active) {
            way.progress = Math.max(way.progress, wholeWorldProgress(loading));
        } else if (way.progress >= 0) {
            way.progress = 1;   // the whole world is in: the bar stays full until the screen lifts
        }
        // the buttons: Cancel on every way in (back where it came from, or an empty place), Show me now while a world
        // loads whole
        var offered = now - way.started >= BUTTONS_AFTER;
        send(true, status, way.progress, offered && canCancel(), offered && loading.active);

        if (HMD.active || CARD_ON_DESKTOP) {
            showCard(status, now);
        } else {
            hideCard();
        }
        if (!way.launchReleased && now - way.started > LAUNCH_RELEASE_DELAY) {
            way.launchReleased = true;
            releaseLaunchScreen();
        }
    }

    // The whole-world bar: while the entity server is still sending, it creeps towards RECEIVING_SHARE as entities
    // arrive (the world's total is not known until all of it has been sent: a total up front would be a protocol
    // change); then it fills with what the landing still waits for near the arrival, a model counting once it or the
    // impostor card drawn in its place has loaded (Caity, 10-21: "impostor cards count"). It never goes back.
    var RECEIVING_SHARE = 0.3;
    var RECEIVING_SCALE = 400;    // entities: the creep's scale (63 % of the share at this many)
    function wholeWorldProgress(loading) {
        var received = loading.received || 0;
        if (!loading.serverDone) {
            return RECEIVING_SHARE * (1 - Math.exp(-received / RECEIVING_SCALE));
        }
        var waiting = Math.max(0, received - (loading.ready || 0));
        way.mostWaiting = Math.max(way.mostWaiting, waiting);
        var loaded = way.mostWaiting > 0 ? 1 - waiting / way.mostWaiting : 1;
        return RECEIVING_SHARE + (1 - RECEIVING_SHARE) * loaded;
    }

    // The headset card: the world's picture, its name, "by <author>", a thin bar (gliding, or filling while a world
    // loads whole) and the status line, on navy

    function cardUnits(x, y, z) {
        return { x: SCALE * x, y: SCALE * y, z: SCALE * z };
    }

    function text(name, properties) {
        var base = {
            type: "Text",
            name: "World loading " + name,
            parentID: card.panel,
            unlit: true,
            backgroundAlpha: 0.0,
            textColor: COLOR_TEXT,
            leftMargin: 0.0,
            topMargin: 0.0,
            ignorePickIntersection: true,
            collisionless: true,
            drawInFront: true
        };
        Object.keys(properties).forEach(function (key) { base[key] = properties[key]; });
        return Entities.addEntity(base, "local");
    }

    function showCard(status, now) {
        if (!card.panel) {
            var cameraJoint = MyAvatar.getJointIndex("_CAMERA_MATRIX");
            card.sphere = Entities.addEntity({
                type: "Model",
                name: "World loading sphere",
                modelURL: SPHERE_URL,
                parentID: MyAvatar.SELF_ID,
                parentJointIndex: cameraJoint,
                localPosition: { x: 0, y: 0, z: 0 },
                dimensions: { x: SPHERE_SIZE, y: SPHERE_SIZE, z: SPHERE_SIZE },
                ignorePickIntersection: true,
                collisionless: true
            }, "local");
            card.panel = Entities.addEntity({
                type: "Text",
                name: "World loading card",
                parentID: MyAvatar.SELF_ID,
                parentJointIndex: cameraJoint,
                localPosition: { x: 0, y: 0, z: -CARD_DISTANCE },
                localRotation: { x: 0, y: 0, z: 0, w: 1 },
                dimensions: cardUnits(CARD_WIDTH, CARD_HEIGHT, 0.01),
                text: "",
                unlit: true,
                backgroundColor: COLOR_NAVY,
                backgroundAlpha: 1.0,
                ignorePickIntersection: true,
                collisionless: true,
                drawInFront: true
            }, "local");
            card.picture = Entities.addEntity({
                type: "Image",
                name: "World loading picture",
                parentID: card.panel,
                imageURL: way.picture || LOGO_URL,
                emissive: true,
                keepAspectRatio: false,
                localPosition: cardUnits(-CARD_WIDTH / 2 + 0.1 + PICTURE_WIDTH / 2, 0.08, 0.006),
                dimensions: cardUnits(way.picture ? PICTURE_WIDTH : PICTURE_HEIGHT, PICTURE_HEIGHT, 0.01),
                ignorePickIntersection: true,
                collisionless: true,
                drawInFront: true
            }, "local");
            card.shownPicture = way.picture;
            var right = -CARD_WIDTH / 2 + 0.1 + PICTURE_WIDTH + 0.08;   // the text column's left edge
            var columnWidth = CARD_WIDTH / 2 - 0.1 - right;
            card.world = text("world", {
                text: way.world,
                lineHeight: SCALE * 0.08,
                localPosition: cardUnits(right + columnWidth / 2, 0.22, 0.006),
                dimensions: cardUnits(columnWidth, 0.2, 0.01)
            });
            card.author = text("author", {
                text: way.author ? "by " + way.author : "",
                lineHeight: SCALE * 0.045,
                textColor: COLOR_MUTED,
                localPosition: cardUnits(right + columnWidth / 2, 0.06, 0.006),
                dimensions: cardUnits(columnWidth, 0.07, 0.01)
            });
            card.status = text("status", {
                text: status,
                lineHeight: SCALE * 0.045,
                textColor: COLOR_MUTED,
                localPosition: cardUnits(0, -0.25, 0.006),
                dimensions: cardUnits(BAR_WIDTH, 0.07, 0.01)
            });
            card.track = text("track", {
                text: "",
                backgroundColor: COLOR_TRACK,
                backgroundAlpha: 1.0,
                localPosition: cardUnits(0, -0.33, 0.006),
                dimensions: cardUnits(BAR_WIDTH, BAR_HEIGHT, 0.01)
            });
            card.glide = text("glide", {
                text: "",
                backgroundColor: COLOR_CYAN,
                backgroundAlpha: 1.0,
                localPosition: cardUnits(-BAR_WIDTH / 2 + GLIDE_WIDTH / 2, -0.33, 0.008),
                dimensions: cardUnits(GLIDE_WIDTH, BAR_HEIGHT, 0.01)
            });
            card.shown = { world: way.world, author: way.author, status: status };
        }
        if (way.picture !== card.shownPicture) {
            card.shownPicture = way.picture;
            Entities.editEntity(card.picture, {
                imageURL: way.picture || LOGO_URL,
                dimensions: cardUnits(way.picture ? PICTURE_WIDTH : PICTURE_HEIGHT, PICTURE_HEIGHT, 0.01)
            });
        }
        if (way.world !== card.shown.world) {
            card.shown.world = way.world;
            Entities.editEntity(card.world, { text: way.world });
        }
        if (way.author !== card.shown.author) {
            card.shown.author = way.author;
            Entities.editEntity(card.author, { text: way.author ? "by " + way.author : "" });
        }
        if (status !== card.shown.status) {
            card.shown.status = status;
            Entities.editEntity(card.status, { text: status });
        }
        if (way.progress >= 0) {
            // the bar fills from the left: no counts, no percentages
            var filled = Math.max(0.001, Math.min(1, way.progress)) * BAR_WIDTH;
            Entities.editEntity(card.glide, {
                localPosition: cardUnits(-BAR_WIDTH / 2 + filled / 2, -0.33, 0.008),
                dimensions: cardUnits(filled, BAR_HEIGHT, 0.01)
            });
        } else {
            // the bar glides
            var phase = ((now - way.started) % GLIDE_PERIOD) / GLIDE_PERIOD;
            var eased = 0.5 - 0.5 * Math.cos(Math.PI * phase);
            Entities.editEntity(card.glide, {
                localPosition: cardUnits(-BAR_WIDTH / 2 + GLIDE_WIDTH / 2 + eased * (BAR_WIDTH - GLIDE_WIDTH), -0.33, 0.008),
                dimensions: cardUnits(GLIDE_WIDTH, BAR_HEIGHT, 0.01)
            });
        }
    }

    function hideCard() {
        // the panel's children go with it
        if (card.panel) {
            Entities.deleteEntity(card.panel);
        }
        if (card.sphere) {
            Entities.deleteEntity(card.sphere);
        }
        card = {};
    }

    // a world's scripts may send a hop's start (a portal), but not a new instance's wait, which holds the screen for
    // minutes: only Explore starts an instance (fromOwnScripts false for a world's scripts on this computer)
    function onMessage(channel, message, sender, localOnly, fromOwnScripts) {
        if (channel !== CHANNEL || !localOnly) {
            return;
        }
        var data;
        try {
            data = JSON.parse(message);
        } catch (e) {
            return;
        }
        if (data.type === "start") {
            var keepLaunch = way && way.launchReleased;
            begin("hop", String(data.world || ""), String(data.author || ""), String(data.picture || ""),
                  data.starting === true && fromOwnScripts !== false ? String(data.id || "") : "");
            way.launchReleased = keepLaunch || way.launchReleased;
            if (!way.starting && Date.now() - lastViewpointAt < VIEWPOINT_BEFORE_START) {
                moveWithinWorld("its viewpoint came " + (Date.now() - lastViewpointAt) + " ms before the start");
            }
        } else if (data.type === "started") {
            if (way && way.starting && way.startId === String(data.id || "")) {
                print("World loading: the new instance's server is up after " + (Date.now() - way.started) + " ms");
                way.starting = false;
                way.waitFrom = Date.now();
            }
        } else if (data.type === "cancel" && way && way.kind === "hop" && !way.left &&
                   (!data.id || data.id === way.startId)) {
            if (way.starting && data.reason) {
                print("World loading: the new instance couldn't start: " + data.reason);
                way.starting = false;
                way.failed = { text: String(data.reason), at: Date.now() };
                tick();
            } else {
                end();
            }
        }
    }

    function canGoBack() {
        if (!way) {
            return false;
        }
        // a hop that has left goes back by the address history, which has the world it left
        return way.kind === "hop" ? !!way.from && (!way.left || location.canGoBack()) : location.canGoBack();
    }

    // a hop always has the world it left (going back, a hop's Cancel is taken away); the first arrival, or a world
    // loading whole, has the history's last world or else the empty place
    function canCancel() {
        return !!way && (way.kind !== "hop" || canGoBack());
    }

    // Cancel: back to the world this way left, under the screen as any hop (an arrival there, with its name and
    // picture); before the old world was left, the hop just stops. Show me now: stop waiting for the whole world.
    function onLoadingScreenButtonPressed(button) {
        if (!way) {
            return;
        }
        if (button === "showNow") {
            print("World loading: Show me now pressed");
            if (loadingState().active && Window.showWorldNow) {
                Window.showWorldNow();
            }
        } else if (button === "cancel" && canCancel()) {
            if (!canGoBack()) {
                print("World loading: Cancel pressed, going to an empty place and opening Explore");
                end();
                location.handleLookupString(EMPTY_PLACE);
                Messages.sendLocalMessage(EXPLORE_CHANNEL, "open");
                return;
            }
            var from = way.from;
            print("World loading: Cancel pressed, " + (way.kind === "hop" && !way.left ? "the hop stops" :
                "going back to \"" + (from ? from.world : "") + "\""));
            if (way.kind === "hop" && !way.left) {
                Messages.sendLocalMessage(CHANNEL, JSON.stringify({ type: "cancel" }));
                end();
                return;
            }
            var keepLaunch = way.launchReleased;
            begin("hop", from ? from.world : "", "", from ? from.picture : "");
            way.from = null;                  // no Cancel on the way back
            way.launchReleased = keepLaunch;
            location.goBack();
        }
    }

    // a hop to the world you're already in (Explore, picking it again): no world change follows, the address only
    // moves you to its viewpoint; a hop to another world gets its viewpoint after the world change. So a viewpoint
    // before any world change is a move within this world: it has arrived (the screen lifts after MIN_SHOWN), instead
    // of waiting out the time limit for a world change that never comes (Caity's hand test, 10-22: "about a minute")
    // The viewpoint and the hop's start reach this script by different ways (the address manager, the messages
    // client), in either order, so a viewpoint shortly before the start counts too.
    var VIEWPOINT_BEFORE_START = 1500;    // ms
    var lastViewpointAt = 0;
    function moveWithinWorld(why) {
        if (way && way.kind === "hop" && !way.left && !way.starting && location.isConnected) {
            print("World loading: the hop is a move within this world (" + why + ")");
            way.left = true;
            way.lostOld = true;
        }
    }
    function onLocationChangeRequired() {
        lastViewpointAt = Date.now();
        moveWithinWorld("its viewpoint came after the start");
    }

    function onDomainChanged() {
        if (way && !way.left) {
            way.left = true;
            way.lostOld = false;
        }
    }

    function onDomainConnectionRefused() {
        if (way) {
            end();
        }
    }

    // the world doesn't answer: the viewer gives up on it for now and opens its connection failure window, which the
    // screen would hide until its time limit (avatar-ai, 10-22: "Can't reach" hidden for 80 s), so the screen lifts
    function onDomainConnectionTimedOut() {
        if (way) {
            end();
        }
    }

    // the world this start-up went to didn't answer: the viewer goes to the entry world instead (without its failure
    // window), and the screen stays up saying why until the arrival there
    function onStartUpWorldUnreachable(from) {
        print("World loading: the start-up world (" + from + ") didn't answer, going to the entry world");
        if (!way) {
            begin("first", "", "", "");
        }
        way.notice = { text: STATUS_FALLBACK[from] || STATUS_FALLBACK.last, at: Date.now() };
        way.world = "";
        way.picture = "";
        way.waitFrom = Date.now();
        tick();
    }

    // a hop to a place that doesn't exist or is offline goes nowhere: the old world stays, so the screen lifts at once
    function onLookupFailed() {
        if (way && way.kind === "hop" && !way.left && !way.starting) {
            end();
        }
    }

    function onWorldLoadingChanged(loading) {
        if (loading && !way) {
            begin("whole", placeName(), "", placePicture());
        }
    }

    Messages.subscribe(CHANNEL);
    Messages.messageReceived.connect(onMessage);
    Window.domainChanged.connect(onDomainChanged);
    Window.domainConnectionRefused.connect(onDomainConnectionRefused);
    if (Window.loadingScreenButtonPressed) {
        Window.loadingScreenButtonPressed.connect(onLoadingScreenButtonPressed);
    }
    if (Window.domainConnectionTimedOut) {
        Window.domainConnectionTimedOut.connect(onDomainConnectionTimedOut);
    }
    if (Window.startUpWorldUnreachable) {
        Window.startUpWorldUnreachable.connect(onStartUpWorldUnreachable);
    }
    AddressManager.lookupResultIsNotFound.connect(onLookupFailed);
    AddressManager.lookupResultIsOffline.connect(onLookupFailed);
    AddressManager.locationChangeRequired.connect(onLocationChangeRequired);
    if (Window.worldLoadingChanged) {
        Window.worldLoadingChanged.connect(onWorldLoadingChanged);
    }

    // this starts at the end of sign-in: the first arrival is the way in, unless it is already done
    if (loadingState().active) {
        begin("whole", placeName(), "", placePicture());
    } else if (!Window.isPhysicsEnabled() || !avatarLoaded()) {
        begin("first", location.isConnected ? placeName() : "", "", location.isConnected ? placePicture() : "");
    }

    Script.scriptEnding.connect(function () {
        if (timer) {
            Script.clearInterval(timer);
            timer = null;
        }
        hideCard();
        setArrivalWaitsForNearby(false);
        if (sent && sent.up) {
            if (Window.setLoadingScreen) {
                Window.setLoadingScreen({ up: false });
            }
            Messages.sendLocalMessage(CHANNEL, JSON.stringify({ type: "screen", up: false, world: sent.world,
                author: sent.author, picture: sent.picture, status: "", progress: -1 }));
        }
        Messages.messageReceived.disconnect(onMessage);
        Messages.unsubscribe(CHANNEL);
        Window.domainChanged.disconnect(onDomainChanged);
        Window.domainConnectionRefused.disconnect(onDomainConnectionRefused);
        if (Window.loadingScreenButtonPressed) {
            Window.loadingScreenButtonPressed.disconnect(onLoadingScreenButtonPressed);
        }
        if (Window.domainConnectionTimedOut) {
            Window.domainConnectionTimedOut.disconnect(onDomainConnectionTimedOut);
        }
        if (Window.startUpWorldUnreachable) {
            Window.startUpWorldUnreachable.disconnect(onStartUpWorldUnreachable);
        }
        AddressManager.lookupResultIsNotFound.disconnect(onLookupFailed);
        AddressManager.lookupResultIsOffline.disconnect(onLookupFailed);
        AddressManager.locationChangeRequired.disconnect(onLocationChangeRequired);
        if (Window.worldLoadingChanged) {
            Window.worldLoadingChanged.disconnect(onWorldLoadingChanged);
        }
    });
}());
