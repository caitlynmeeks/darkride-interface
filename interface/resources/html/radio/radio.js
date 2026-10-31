//
//  radio.js
//  interface/resources/html/radio
//
//  The Radio page: Webamp (webamp.darkride.min.js, our copy without Winamp's art) starts on Darkride's skin
//  (darkride-skin.js), lists SomaFM's stations (stations.js) and the player's own, finds stations in the Radio Browser
//  directory (radio-browser.info, free and open) and plays any stream address; under the player, three tabs: SomaFM,
//  Find, Address. A station found or typed in joins the player's list (kept for next time); one that won't play here
//  is said so, the player stops, and it comes off the list again. A classic .wsz skin the player loads is kept for next time (IndexedDB); "Reset to
//  Darkride" and the skins menu's "Darkride" bring ours back. Kept cheap (performance, 10-30): the spectrum draws at
//  most 30 times a second, only while playing (Webamp's own rule) and shown (a hidden page gets no animation frames);
//  RadioPanel.qml tears the page down when the radio is closed.
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
    var TABS = ["soma", "find", "address"];
    // the Radio Browser directory (security's rules, 10-31): these hosts only, tried in order (the round-robin name,
    // then one server by its own name: each server has its own certificate); https; a capped answer, read as JSON;
    // its words shown as text, capped; no station pictures (each would tell a station's host who's searching)
    var DIRECTORY_HOSTS = ["all.api.radio-browser.info", "de1.api.radio-browser.info"];
    var DIRECTORY_MAX_BYTES = 256 * 1024;
    var DIRECTORY_ASK = 60;           // asked for, before the ones this engine can't play are left out
    var FOUND_MAX = 30;
    var FOUND_FEW = 10;               // fewer found by name: the same words as a kind of music (the directory's tags)
    var NAME_MAX = 80;
    var TAGS_MAX = 120;
    // the directory's codec names, as this engine is asked whether it plays them (an HLS stream never: Chrome doesn't)
    var CODEC_TYPES = {
        "MP3": "audio/mpeg",
        "AAC": "audio/aac",
        "AAC+": "audio/aac",
        "OGG": "audio/ogg; codecs=\"vorbis\"",
        "OPUS": "audio/ogg; codecs=\"opus\"",
        "FLAC": "audio/flac",
    };

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
                            return inflateWithin(zip);
                        });
                    });
                };
            },
        });
    };

    // the same caps again while each file inflates: a zip's own sizes can lie, and inflating doesn't hold to them
    // (security, 10-31); a skin over them doesn't go on (the middleware below)
    var tooBigToOpen = false;
    var READ_TYPES = ["text", "string", "uint8array", "arraybuffer", "blob"];
    function inflateWithin(zip) {
        var used = 0;
        tooBigToOpen = false;
        Object.keys(zip.files).forEach(function (fileName) {
            var entry = zip.files[fileName];
            entry.async = function (type) {
                if (READ_TYPES.indexOf(type) < 0) {
                    return Promise.reject(new Error("the radio doesn't read a skin's files as " + type));
                }
                return new Promise(function (resolve, reject) {
                    var parts = [];
                    var size = 0;
                    var over = false;
                    var stream = entry.internalStream("uint8array");
                    stream.on("data", function (chunk) {
                        if (over) {
                            return;
                        }
                        size += chunk.length;
                        used += chunk.length;
                        if (size > SKIN_MAX_FILE_BYTES || used > SKIN_MAX_UNZIPPED) {
                            over = true;
                            tooBigToOpen = true;
                            stream.pause();
                            reject(new Error("too big to open"));
                            return;
                        }
                        parts.push(chunk);
                    }).on("error", function (error) {
                        if (!over) {
                            over = true;
                            reject(error);
                        }
                    }).on("end", function () {
                        if (over) {
                            return;
                        }
                        var bytes = new Uint8Array(size);
                        var at = 0;
                        parts.forEach(function (part) {
                            bytes.set(part, at);
                            at += part.length;
                        });
                        resolve(type === "blob" ? new Blob([bytes]) : type === "uint8array" ? bytes
                            : type === "arraybuffer" ? bytes.buffer : new TextDecoder("utf-8").decode(bytes));
                    }).resume();
                });
            };
        });
        return zip;
    }

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
                // the sound has started (the media element's own "playing"): this station plays here
                if (action && action.type === "IS_PLAYING") {
                    var state = webamp.store.getState();
                    var track = state.tracks[state.playlist.currentTrack];
                    if (track) {
                        heard[track.url] = true;
                    }
                }
                if (action && action.type === "SET_SKIN_DATA" && tooBigToOpen && !loadingBuiltin) {
                    tooBigToOpen = false;
                    openedZip = null;
                    skinFailed("That skin is too big to open.");
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
    // the first stream in a .pls or .m3u, and its title when the file gives one
    function readPlaylist(address) {
        return withTimeout(fetch(address, { credentials: "omit" })).then(function (response) {
            if (!response.ok) {
                throw new Error(response.status);
            }
            return response.text();
        }).then(function (text) {
            var lines = text.slice(0, PLAYLIST_MAX_BYTES).split(/\r?\n/);
            var found = { url: "", title: "" };
            lines.forEach(function (line) {
                line = line.trim();
                var named = /^(?:Title1=|#EXTINF:[^,]*,)(.+)$/i.exec(line);
                if (named && !found.title) {
                    // "SomaFM: DEF CON Radio (#1): Music for Hacking." reads "SomaFM: DEF CON Radio"
                    found.title = named[1].replace(/\s*\(#\d+\).*$/, "").trim().slice(0, 80);
                }
                line = line.replace(/^File\d+=/i, "");
                if (/^https?:\/\//i.test(line) && !found.url) {
                    found.url = line;
                }
            });
            if (!found.url) {
                throw new Error("no stream");
            }
            return found;
        });
    }

    // a stream the player plays from its list: one not in it yet joins it (and is kept), and comes off again if it
    // won't play here (the media error below)
    var addedNow = {};   // stream addresses put in the list this visit
    var heard = {};      // stream addresses that have played this visit
    function playStream(url, title) {
        var trackWith = function () {
            var state = webamp.store.getState();
            return state.playlist.trackOrder.find(function (id) { return state.tracks[id].url === url; });
        };
        var id = trackWith();
        if (id == null) {
            ownStations.push({ url: url, title: title });
            save("ownStations", ownStations);
            addedNow[url] = true;
            webamp.appendTracks([ownTrack({ url: url, title: title })]);
            id = trackWith();
        }
        note("");
        webamp.store.dispatch({ type: "PLAY_TRACK", id: id });
    }

    // a stream that won't play here (gone, a format this engine can't play, or a server that doesn't let other sites'
    // pages play it), or one that dropped while playing: said under the player, which stops (make-bundle.py: Webamp
    // would go on to the next station)
    window.addEventListener("darkrideMediaError", function () {
        var state = webamp.store.getState();
        var id = state.playlist.currentTrack;
        var track = id == null ? null : state.tracks[id];
        webamp.stop();
        // one that played and dropped (the network, the station) may come back; one that never played here won't
        if (track && heard[track.url]) {
            note("The station stopped. Press Play to try again.", true);
            return;
        }
        note("That station didn't play here. Try another.", true);
        if (track && addedNow[track.url]) {
            delete addedNow[track.url];
            webamp.store.dispatch({ type: "REMOVE_TRACKS", ids: [id] });
        }
    });

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
        var stream = /\.(pls|m3u)$/i.test(address.pathname) ? readPlaylist(address.href)
            : Promise.resolve({ url: address.href, title: "" });
        note("Tuning in…");
        stream.then(function (found) {
            playStream(found.url, found.title || title);
            addressField.value = "";
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
        // the playlist's ADD > URL: the Address tab
        handleAddUrlEvent: function () {
            showTab("address", true);
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
    function layoutSoon() {
        clearTimeout(layoutTimer);
        layoutTimer = setTimeout(layout, 50);
    }
    window.addEventListener("resize", layoutSoon);

    // ---- Find: stations in the Radio Browser directory ----

    var probe = document.createElement("audio");
    function playable(codec) {
        var type = CODEC_TYPES[String(codec || "").toUpperCase()];
        return !!type && probe.canPlayType(type) !== "";
    }
    function clip(text, most) {
        return String(text == null ? "" : text).replace(/\s+/g, " ").trim().slice(0, most);
    }
    function streamAddress(text) {
        try {
            var address = new URL(String(text || ""));
            return address.protocol === "http:" || address.protocol === "https:" ? address.href : "";
        } catch (error) {
            return "";
        }
    }

    // one answer from the directory, read only up to the cap, within the time, without cookies
    function readCapped(address) {
        var abort = new AbortController();
        var timer = setTimeout(function () { abort.abort(); }, FETCH_TIMEOUT_MS);
        return fetch(address, { credentials: "omit", signal: abort.signal }).then(function (response) {
            if (!response.ok || !response.body) {
                throw new Error("the directory answered " + response.status);
            }
            var reader = response.body.getReader();
            var parts = [];
            var size = 0;
            function more() {
                return reader.read().then(function (part) {
                    if (part.done) {
                        return new Blob(parts).text();
                    }
                    size += part.value.byteLength;
                    if (size > DIRECTORY_MAX_BYTES) {
                        abort.abort();
                        throw new Error("the directory's answer is too big");
                    }
                    parts.push(part.value);
                    return more();
                });
            }
            return more();
        }).then(function (text) {
            clearTimeout(timer);
            return JSON.parse(text);
        }, function (error) {
            clearTimeout(timer);
            throw error;
        });
    }
    // our hosts in turn, never one named in an answer
    function askDirectory(path) {
        var hosts = DIRECTORY_HOSTS.slice();
        function next(why) {
            var host = hosts.shift();
            return host ? readCapped("https://" + host + path).catch(next) : Promise.reject(why);
        }
        return next();
    }

    // a stream on the listener's own network (this computer, the home network): never from the directory, so a click
    // there can't send a request into the listener's home (security, 10-31); one typed into Address is the listener's own
    function onOwnNetwork(url) {
        var host = new URL(url).hostname.toLowerCase().replace(/\.+$/, "");   // "localhost." is localhost too
        if (host === "localhost" || /\.(local|localhost)$/.test(host)) {
            return true;
        }
        var v4 = /^(\d+)\.(\d+)\.\d+\.\d+$/.exec(host);
        if (v4) {
            var a = Number(v4[1]);
            var b = Number(v4[2]);
            return a === 0 || a === 10 || a === 127 || (a === 169 && b === 254) || (a === 172 && b >= 16 && b <= 31) ||
                (a === 192 && b === 168) || (a === 100 && b >= 64 && b <= 127);
        }
        // IPv6: loopback, unique local (fc00::/7), link-local (fe80::/10), and IPv4 inside IPv6
        return /^\[(::1?|f[cd][0-9a-f]*:.*|fe[89ab][0-9a-f]*:.*|::ffff:.*)\]$/.test(host);
    }

    // a station from the directory's answer, only what the radio shows and plays, or null when this engine can't play it
    function stationFrom(row) {
        if (!row || typeof row !== "object" || Number(row.hls) === 1 || !playable(row.codec)) {
            return null;
        }
        var url = streamAddress(row.url_resolved) || streamAddress(row.url);
        if (!url || onOwnNetwork(url)) {
            return null;
        }
        var bitrate = Math.round(Number(row.bitrate)) || 0;
        var format = clip(row.codec, 8).toUpperCase() + (bitrate > 0 && bitrate < 10000 ? " " + bitrate + "k" : "");
        var id = clip(row.stationuuid, 36).toLowerCase();
        return {
            url: url,
            name: clip(row.name, NAME_MAX) || new URL(url).hostname,
            about: [clip(row.countrycode, 2).toUpperCase(), format].filter(Boolean).join(" · "),
            tags: clip(row.tags, TAGS_MAX).replace(/,\s*/g, ", "),
            id: /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(id) ? id : "",
        };
    }

    function searchPath(field, words) {
        return "/json/stations/search?" + field + "=" + encodeURIComponent(words) +
            "&hidebroken=true&order=clickcount&reverse=true&limit=" + DIRECTORY_ASK;
    }
    // by name, the most listened to first; with few found, the same words as a kind of music too
    function findStations(words) {
        var found = [];
        var seen = {};
        function take(rows) {
            (Array.isArray(rows) ? rows : []).forEach(function (row) {
                var station = found.length < FOUND_MAX ? stationFrom(row) : null;
                if (station && !seen[station.url]) {
                    seen[station.url] = true;
                    found.push(station);
                }
            });
        }
        return askDirectory(searchPath("name", words)).then(function (rows) {
            take(rows);
            if (found.length >= FOUND_FEW) {
                return found;
            }
            return askDirectory(searchPath("tag", words.toLowerCase())).then(function (more) {
                take(more);
                return found;
            }, function () {
                return found;
            });
        });
    }

    var findField = document.getElementById("find-field");
    var resultsElement = document.getElementById("results");
    function showFound(stations, status) {
        resultsElement.textContent = "";
        if (status) {
            var line = document.createElement("li");
            line.className = "status";
            line.textContent = status;
            resultsElement.appendChild(line);
        }
        stations.forEach(function (station) {
            var button = document.createElement("button");
            button.type = "button";
            button.className = "station";
            button.title = station.tags ? station.name + "\n" + station.tags : station.name;
            var name = document.createElement("span");
            name.className = "name";
            name.textContent = station.name;
            var about = document.createElement("span");
            about.className = "about";
            about.textContent = station.about;
            button.appendChild(name);
            button.appendChild(about);
            button.addEventListener("click", function () { playFound(station); });
            var item = document.createElement("li");
            item.appendChild(button);
            resultsElement.appendChild(item);
        });
        resultsElement.scrollTop = 0;
    }
    function playFound(station) {
        playStream(station.url, station.name);
        // the directory counts a station's listens when a player tells it (it asks players to); nothing comes back
        if (station.id) {
            fetch("https://" + DIRECTORY_HOSTS[0] + "/json/url/" + station.id, { credentials: "omit" })
                .catch(function () {});
        }
    }
    var searchNumber = 0;
    document.getElementById("find").addEventListener("submit", function (event) {
        event.preventDefault();
        var words = clip(findField.value, NAME_MAX);
        if (!words) {
            findField.focus();
            return;
        }
        var number = ++searchNumber;
        showFound([], "Looking…");
        findStations(words).then(function (found) {
            if (number === searchNumber) {
                showFound(found, found.length ? "" : "No stations found for “" + words + "”.");
            }
        }, function () {
            if (number === searchNumber) {
                showFound([], "The directory didn't answer. Try again in a moment.");
            }
        });
    });

    // ---- the tabs under the player: SomaFM, Find, Address (the last one open is open next time) ----

    var tabButtons = Array.from(document.querySelectorAll("#tabs [role=tab]"));
    function showTab(name, focus) {
        tabButtons.forEach(function (button) {
            var open = button.dataset.tab === name;
            button.setAttribute("aria-selected", open ? "true" : "false");
            document.getElementById("tab-" + button.dataset.tab).hidden = !open;
        });
        save("tab", name);
        if (focus) {
            var field = name === "find" ? findField : name === "address" ? addressField : null;
            if (field) {
                field.focus();
            }
        }
        // the station list makes room for the open tab
        layoutSoon();
    }
    tabButtons.forEach(function (button) {
        button.addEventListener("click", function () { showTab(button.dataset.tab, true); });
    });
    var firstTab = load("tab", "soma");
    showTab(TABS.indexOf(firstTab) >= 0 ? firstTab : "soma", false);

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
