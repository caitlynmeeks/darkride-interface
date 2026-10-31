//
//  radio.js
//  interface/resources/html/radio
//
//  The Radio page: Webamp (webamp.darkride.min.js, our copy without Winamp's art) starts on Darkride's skin
//  (darkride-skin.js), lists SomaFM's stations (stations.js) and the player's own, and plays any stream address. A
//  classic .wsz skin the player loads is kept for next time (IndexedDB); "Reset to Darkride" and the skins menu's
//  "Darkride" bring ours back. Kept cheap (performance, 10-30): the spectrum draws at most 30 times a second, only while
//  playing (Webamp's own rule) and shown (a hidden page gets no animation frames); RadioPanel.qml tears the page down
//  when the radio is closed.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

(function () {
    "use strict";

    var SKIN_MAX_BYTES = 4 * 1024 * 1024;        // a .wsz as loaded (classic skins are 20-500 KB)
    var SKIN_MAX_FILES = 256;
    var SKIN_MAX_FILE_BYTES = 8 * 1024 * 1024;   // one file in it, unzipped
    var SKIN_MAX_UNZIPPED = 32 * 1024 * 1024;    // all of it, unzipped (a zip bomb stops here, before it's inflated)
    var PLAYLIST_MAX_BYTES = 64 * 1024;          // a .pls or .m3u
    var FETCH_TIMEOUT_MS = 8000;
    var FRAME_MS = 1000 / 30;
    var MAIN_HEIGHT = 116;                       // Webamp's main window
    var PLAYLIST_HEIGHT = 116;                   // its playlist at its smallest; it grows in steps of 29
    var PLAYLIST_STEP = 29;
    var KEY = "darkride.radio.";

    // ---- at most 30 frames a second, for every animation in the page (the spectrum is the only one that runs on) ----

    var nativeFrame = window.requestAnimationFrame.bind(window);
    var frameCallbacks = new Map();
    var nextFrameId = 1;
    var frameWaiting = false;
    var lastFrame = 0;
    function runFrame(time) {
        frameWaiting = false;
        if (time - lastFrame < FRAME_MS - 2) {
            waitForFrame();
            return;
        }
        lastFrame = time;
        var callbacks = Array.from(frameCallbacks.values());
        frameCallbacks.clear();
        callbacks.forEach(function (callback) {
            try {
                callback(time);
            } catch (error) {
                console.error(error);
            }
        });
    }
    function waitForFrame() {
        if (frameWaiting || frameCallbacks.size === 0) {
            return;
        }
        frameWaiting = true;
        var wait = Math.max(0, lastFrame + FRAME_MS - 2 - performance.now());
        if (wait > 1) {
            setTimeout(function () { nativeFrame(runFrame); }, wait);
        } else {
            nativeFrame(runFrame);
        }
    }
    window.requestAnimationFrame = function (callback) {
        var id = nextFrameId++;
        frameCallbacks.set(id, callback);
        waitForFrame();
        return id;
    };
    window.cancelAnimationFrame = function (id) {
        frameCallbacks.delete(id);
    };

    // ---- a line under the buttons instead of the browser's alert boxes ----

    var noteElement = document.getElementById("note");
    var noteTimer = null;
    function note(text, problem) {
        noteElement.textContent = text;
        noteElement.className = problem ? "problem" : "";
        noteElement.hidden = !text;
        clearTimeout(noteTimer);
        if (text) {
            noteTimer = setTimeout(function () { noteElement.hidden = true; }, problem ? 8000 : 4000);
        }
    }
    var skinProblem = null;   // why the gate refused the last skin, said instead of Webamp's own words
    window.alert = function (message) {
        message = String(message);
        if (/skin/i.test(message)) {
            skinFailed(skinProblem || "That skin couldn't be read. Classic skins are .wsz files.");
            skinProblem = null;
        } else if (/not supported/i.test(message)) {
            note("The radio doesn't do that.", true);
        } else {
            note(message, true);
        }
    };
    window.confirm = function () { return false; };
    window.prompt = function () { return null; };

    // ---- what's kept between visits ----

    function load(name, otherwise) {
        try {
            var value = localStorage.getItem(KEY + name);
            return value == null ? otherwise : JSON.parse(value);
        } catch (error) {
            return otherwise;
        }
    }
    function save(name, value) {
        try {
            localStorage.setItem(KEY + name, JSON.stringify(value));
        } catch (error) {
            // no storage in this profile: the radio still plays, it just doesn't remember
        }
    }
    function skinStore(mode, work) {
        return new Promise(function (resolve) {
            try {
                var open = indexedDB.open("darkride-radio", 1);
                open.onupgradeneeded = function () { open.result.createObjectStore("files"); };
                open.onerror = function () { resolve(null); };
                open.onsuccess = function () {
                    var request = work(open.result.transaction("files", mode).objectStore("files"));
                    request.onsuccess = function () { resolve(request.result || null); };
                    request.onerror = function () { resolve(null); };
                };
            } catch (error) {
                resolve(null);
            }
        });
    }
    function savedSkin() {
        return skinStore("readonly", function (files) { return files.get("skin"); });
    }
    function keepSkin(blob) {
        return skinStore("readwrite", function (files) {
            return blob ? files.put(blob, "skin") : files.delete("skin");
        });
    }

    // ---- skins: every zip Webamp opens comes through here first (make-bundle.py's darkrideZipGate) ----

    function base64Blob(text, type) {
        var bytes = atob(text);
        var array = new Uint8Array(bytes.length);
        for (var i = 0; i < bytes.length; i++) {
            array[i] = bytes.charCodeAt(i);
        }
        return new Blob([array], { type: type });
    }
    var builtinSkinUrl = URL.createObjectURL(base64Blob(window.DARKRIDE_SKIN_WSZ, "application/zip"));
    var loadingBuiltin = false;   // the skin coming in is ours
    var restoring = false;        // the skin coming in is the one kept from last time
    var openedZip = null;         // the last zip that passed the gate, kept if its skin goes on
    var builtinImages = null;     // our skin's sprites, filling in any a player's skin lacks

    function refuse(why) {
        skinProblem = why;
        throw new Error(why);
    }
    window.darkrideZipGate = function (JSZip) {
        return new Proxy(JSZip, {
            get: function (target, name) {
                if (name !== "loadAsync") {
                    return target[name];
                }
                return function (data, options) {
                    return Promise.resolve(data).then(function (zipData) {
                        var size = zipData && (zipData.size != null ? zipData.size : zipData.byteLength);
                        if (!(size > 0)) {
                            refuse("That skin is empty.");
                        }
                        if (size > SKIN_MAX_BYTES) {
                            refuse("That skin is too big (the radio takes skins up to 4 MB).");
                        }
                        return target.loadAsync(zipData, options).then(function (zip) {
                            var names = Object.keys(zip.files);
                            if (names.length > SKIN_MAX_FILES) {
                                refuse("That skin has too many files in it.");
                            }
                            var total = 0;
                            names.forEach(function (fileName) {
                                var entry = zip.files[fileName]._data;
                                var unzipped = entry && entry.uncompressedSize || 0;
                                if (unzipped > SKIN_MAX_FILE_BYTES) {
                                    refuse("That skin has a file too big to open.");
                                }
                                total += unzipped;
                            });
                            if (total > SKIN_MAX_UNZIPPED) {
                                refuse("That skin is too big to open.");
                            }
                            openedZip = zipData instanceof Blob ? zipData : new Blob([zipData]);
                            return zip;
                        });
                    });
                };
            },
        });
    };

    function useBuiltinSkin() {
        loadingBuiltin = true;
        webamp.setSkinFromUrl(builtinSkinUrl);
    }
    function resetSkin() {
        keepSkin(null);
        useBuiltinSkin();
        note("Darkride's skin is back on.");
    }
    function skinFailed(why) {
        if (restoring) {
            // last time's skin no longer opens: ours, and forget it
            restoring = false;
            keepSkin(null);
            playerReady();
            return;
        }
        loadingBuiltin = false;
        note(why, true);
    }
    function loadSkinFile(file) {
        if (!file) {
            return;
        }
        if (file.size > SKIN_MAX_BYTES) {
            note("That skin is too big (the radio takes skins up to 4 MB).", true);
            return;
        }
        webamp.setSkinFromUrl(URL.createObjectURL(file));
    }

    // Webamp's actions on their way in: its skins menu's "Darkride" (its base skin) brings ours back; a skin going on is
    // filled in from ours where it lacks a sprite, and kept for next time
    function radioMiddleware() {
        return function (next) {
            return function (action) {
                if (action && action.type === "LOAD_DEFAULT_SKIN") {
                    resetSkin();
                    return undefined;
                }
                if (action && action.type === "SET_SKIN_DATA") {
                    if (loadingBuiltin) {
                        builtinImages = action.data.skinImages;
                        loadingBuiltin = false;
                    } else {
                        if (builtinImages) {
                            var data = Object.assign({}, action.data, {
                                skinImages: Object.assign({}, builtinImages, action.data.skinImages),
                            });
                            action = Object.assign({}, action, { data: data });
                        }
                        if (restoring) {
                            restoring = false;
                        } else if (openedZip) {
                            keepSkin(openedZip);
                            note("Skin on. The radio keeps it until you reset it.");
                        }
                    }
                    openedZip = null;
                    var result = next(action);
                    playerReady();
                    return result;
                }
                return next(action);
            };
        };
    }

    // ---- stations ----

    var builtinUrls = {};
    var stationTracks = (window.RADIO_STATIONS || []).map(function (station) {
        builtinUrls[station.streams[0]] = true;
        // a length of 0: Webamp shows none, and doesn't open the stream to find one
        return { url: station.streams[0], duration: 0, metaData: { artist: "SomaFM", title: station.name } };
    });
    function ownTrack(station) {
        return { url: station.url, duration: 0, metaData: { artist: "", title: station.title } };
    }
    var ownStations = load("ownStations", []).filter(function (station) {
        return station && typeof station.url === "string" && /^https?:\/\//i.test(station.url);
    });

    function withTimeout(promise) {
        return Promise.race([promise, new Promise(function (resolve, reject) {
            setTimeout(function () { reject(new Error("timeout")); }, FETCH_TIMEOUT_MS);
        })]);
    }
    // the first stream in a .pls or .m3u
    function readPlaylist(address) {
        return withTimeout(fetch(address, { credentials: "omit" })).then(function (response) {
            if (!response.ok) {
                throw new Error(response.status);
            }
            return response.text();
        }).then(function (text) {
            text = text.slice(0, PLAYLIST_MAX_BYTES);
            var lines = text.split(/\r?\n/);
            for (var i = 0; i < lines.length; i++) {
                var line = lines[i].trim().replace(/^File\d+=/i, "");
                if (/^https?:\/\//i.test(line)) {
                    return line;
                }
            }
            throw new Error("no stream");
        });
    }

    var addressField = document.getElementById("address-field");
    function playAddress(text) {
        var address;
        try {
            address = new URL(text.trim());
        } catch (error) {
            note("That isn't a web address. Stream addresses start with http:// or https://", true);
            return;
        }
        if (address.protocol !== "http:" && address.protocol !== "https:") {
            note("The radio plays http:// and https:// addresses only.", true);
            return;
        }
        var title = address.hostname + (address.pathname.length > 1 ? address.pathname : "");
        var stream = /\.(pls|m3u)$/i.test(address.pathname) ? readPlaylist(address.href) : Promise.resolve(address.href);
        note("Tuning in…");
        stream.then(function (url) {
            var trackWith = function () {
                var state = webamp.store.getState();
                return state.playlist.trackOrder.find(function (id) { return state.tracks[id].url === url; });
            };
            var id = trackWith();
            if (id == null) {
                ownStations.push({ url: url, title: title });
                save("ownStations", ownStations);
                webamp.appendTracks([ownTrack({ url: url, title: title })]);
                id = trackWith();
            }
            webamp.store.dispatch({ type: "PLAY_TRACK", id: id });
            addressField.value = "";
            note("");
        }, function () {
            note("Couldn't read that playlist. Try the stream's own address.", true);
        });
    }
    document.getElementById("address").addEventListener("submit", function (event) {
        event.preventDefault();
        if (addressField.value.trim()) {
            playAddress(addressField.value);
        } else {
            addressField.focus();
        }
    });

    // the player's own stations follow its list: one taken out (REM) stays out
    function keepOwnStations() {
        var inList = {};
        webamp.getPlaylistTracks().forEach(function (track) { inList[track.url] = true; });
        var kept = ownStations.filter(function (station) { return inList[station.url]; });
        if (kept.length !== ownStations.length) {
            ownStations = kept;
            save("ownStations", ownStations);
        }
    }

    // ---- the player ----

    var playerElement = document.getElementById("player");
    playerElement.className = "waiting";
    var shown = false;
    function playerReady() {
        if (!shown && !restoring && !loadingBuiltin) {
            shown = true;
            playerElement.className = "";
        }
    }

    var webamp = new window.Webamp({
        initialSkin: { url: builtinSkinUrl },
        windowLayout: {
            main: { position: { top: 0, left: 0 } },
            playlist: { position: { top: MAIN_HEIGHT, left: 0 }, size: { extraHeight: 0, extraWidth: 0 } },
        },
        // the playlist's ADD > URL: the address field
        handleAddUrlEvent: function () {
            addressField.focus();
            note("Type or paste a stream address, then Play.");
            return Promise.resolve(null);
        },
        __customMiddlewares: [radioMiddleware],
    });
    // for RadioPanel.qml (runJavaScript) and page checks
    window.darkrideRadio = webamp;
    loadingBuiltin = true;
    // listed, not loaded: nothing connects to a station until it's played (Webamp's initialTracks would load the first)
    webamp.appendTracks(stationTracks.concat(ownStations.map(ownTrack)));

    // the playlist as tall as the panel allows, under the main window
    function layout() {
        var controls = document.getElementById("controls").offsetHeight;
        var room = window.innerHeight - 16 - controls - MAIN_HEIGHT - PLAYLIST_HEIGHT;
        var steps = Math.max(0, Math.floor(room / PLAYLIST_STEP));
        playerElement.style.height = MAIN_HEIGHT + PLAYLIST_HEIGHT + steps * PLAYLIST_STEP + "px";
        webamp.store.dispatch({ type: "WINDOW_SIZE_CHANGED", windowId: "playlist", size: [0, steps] });
        webamp.store.dispatch({
            type: "UPDATE_WINDOW_POSITIONS",
            positions: { main: { x: 0, y: 0 }, playlist: { x: 0, y: MAIN_HEIGHT } },
            absolute: false,
        });
    }
    var layoutTimer = null;
    window.addEventListener("resize", function () {
        clearTimeout(layoutTimer);
        layoutTimer = setTimeout(layout, 50);
    });

    // the volume, kept
    var volume = load("volume", null);
    var lastVolume = null;
    var lastTracks = null;
    webamp.store.subscribe(function () {
        var state = webamp.store.getState();
        if (state.media.volume !== lastVolume) {
            lastVolume = state.media.volume;
            save("volume", lastVolume);
        }
        if (state.playlist && state.playlist.trackOrder !== lastTracks) {
            lastTracks = state.playlist.trackOrder;
            keepOwnStations();
        }
    });

    // Webamp's own close box and Exit close the radio (RadioPanel.qml reads the page's title)
    webamp.onClose(function () {
        document.title = "Radio: close";
        setTimeout(function () {
            document.title = "Radio";
            webamp.reopen();
        }, 500);
    });

    webamp.renderInto(playerElement).then(function () {
        if (typeof volume === "number") {
            webamp.setVolume(volume);
        }
        layout();
        return savedSkin();
    }).then(function (blob) {
        if (blob) {
            restoring = true;
            webamp.setSkinFromUrl(URL.createObjectURL(blob));
        }
        playerReady();
    });

    document.getElementById("load-skin").addEventListener("click", function () {
        var input = document.getElementById("skin-file");
        input.value = "";
        input.click();
    });
    document.getElementById("skin-file").addEventListener("change", function (event) {
        loadSkinFile(event.target.files[0]);
    });
    document.getElementById("reset-skin").addEventListener("click", resetSkin);

    // a file dropped anywhere outside the player doesn't open in the page; a skin dropped there goes on
    window.addEventListener("dragover", function (event) {
        event.preventDefault();
    });
    window.addEventListener("drop", function (event) {
        event.preventDefault();
        var file = event.dataTransfer && event.dataTransfer.files[0];
        if (file && /\.(wsz|zip)$/i.test(file.name)) {
            loadSkinFile(file);
        }
    });
}());
