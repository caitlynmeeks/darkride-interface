//
//  AvatarLibrary.qml
//  qml/darkride
//
//  The Avatar panel's avatars as one list (avatars, 2026-11-08, for the panel's redo: mary's AVATAR-REDO.png from
//  Caity's report of 11-08). One entry per avatar link, from three places, matched by the link (keyOf):
//    My Files: the .fst files in the person's files on the metaverse, each with the picture beside it when there is one;
//    favourites: AvatarBookmarks, kept on this computer: a star on any avatar (star), or "Keep in My Avatars" on a web
//      link worn (keep), which keeps it by its link (copying it into My Files is for later);
//    Recent: the ten avatars worn last, kept in a private setting per account (recentKey), written when the avatar
//      changes; the newest five are `recent`, the default avatar among them, and the ten (without it) are in `items`
//      for the search.
//  Each entry's picture is the one beside it in My Files, else the one the app took by itself when it was worn
//  (AvatarThumbnails.autoPictures, kept on this computer). The panel (AvatarPanel.qml) draws it: it works out what's
//  worn and ticked (with keyOf), puts favourites first, folds the old files (`legacy`) into one row and shows a
//  Recent-only entry only while the search has text. Nothing here runs while the panel is closed but the one setting
//  written when the avatar changes, and the lists' one change when a picture is taken. My Files is asked for once per
//  opening (refresh), and `items` and `recent` are replaced only when what they hold changed, so an opening with nothing
//  new, or wearing an avatar that is listed already, rebuilds no row. My Files' last list is kept per account (a cache,
//  filesSetting) and shown at once while My Files is asked again, so a slow answer never shows an empty list.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.15

QtObject {
    id: library

    // One entry per avatar link, in no particular order (the panel sorts): {
    //   key           the link as keyOf gives it: unique, and how the panel finds the worn one
    //   url           what to wear
    //   link          the link to show (a tea:// address as the files host's https one)
    //   title         its favourite's name, else its name in My Files, else the name it was worn under, else its file name
    //   fileName      the link's file name ("female-full-body-cyberpunk-themed-avatar.fst")
    //   folder        its folder in My Files ("made/avatars/a-cute-duck-robot-2ib2hk"), "" when it isn't there
    //   host          the web link's host ("darkride.jollysoft.com"), "" for a file on this computer
    //   source        where it lives, for the grey line: "myFiles", "local" (a file on this computer) or "web"
    //   subtitle      the detail of where it lives, for the panel's grey line (which adds "My Files · " and the rest):
    //                 its folder in My Files without the avatar's own folder ("made/avatars"), the web link's host, or
    //                 "On this computer"
    //   keywords      what the search looks in: title, file name, folder and link
    //   picture       ready to draw, or "": its picture in My Files (through the engine's loader, so a private one
    //                 loads), else the one taken by itself when it was worn (a file:// address on this computer)
    //   updated       its date in My Files (ms), 0 when it isn't there
    //   isPrivate, badge   as My Files' list had them ("private" or "")
    //   inMyFiles, favorite, favoriteName, local, legacy (in My Files under legacy/: the old files)
    //   savedLook     its favourite holds a saved scale and wearables (AvatarBookmarks.addBookmark): wear it with
    //                 AvatarBookmarks.loadBookmark; any other entry by its url, which keeps the wearables worn
    //   recentOnly    in the list only because it was worn lately (not in My Files, not a favourite)
    // }
    property var items: []
    // The five avatars worn last, newest first: copies of their entries with lastWorn (ms); the default avatar is one
    // with source "default" and the title "Default"
    property var recent: []
    property bool busy: false            // My Files is being asked for
    property bool filesKnown: false      // My Files' avatars are listed: from its answer, or its last list kept
    property string status: ""           // why My Files couldn't be listed, in words, or ""
    property int myFilesCount: 0
    property int privateCount: 0
    property int legacyCount: 0

    // Recent is kept per account (security, 11-08: on a shared computer each person sees only their own), under
    // private/, which a world's scripts can neither read nor write (security's rule 28: the links someone wore are
    // theirs, and only her own lists may give star() a link): this, then "/" and the signed-in account's name hashed,
    // or "/signed-out" (recentKey)
    readonly property string recentSetting: "private/avatarPanel/recent"
    readonly property int recentKept: 10
    readonly property int recentShown: 5
    // My Files' last list (a cache), kept per account as Recent is, under private/ (scripts can't read it) and only for
    // a signed-in account: { url: the files host's base, avatars: [[path, picture path?date or "", date, 1 if private]] }.
    // Written only when it changed; at most filesKept avatars (an account with more shows the rest when the answer comes).
    readonly property string filesSetting: "private/avatarPanel/myFiles"
    readonly property int filesKept: 500
    property string filesKeptText: ""    // what the setting holds, so an unchanged answer writes nothing

    // what the three places held when last read
    property var filesRead: []           // My Files' avatars: [{ key, url, title, folder, picture, updated, isPrivate, legacy }]
    property var favoritesRead: ({})     // AvatarBookmarks.getBookmarks()
    property var recentRead: []          // [{ url, name, worn, isDefault }], newest first
    property var autoPicturesRead: ({})  // the pictures taken by themselves, by key: "file:///…jpg?<taken>"
    property var entriesByKey: ({})      // the entries of `items` and `recent` by key, for entryFor
    property string itemsSignature: ""
    property string recentSignature: ""

    // ---- links ------------------------------------------------------------------------------------------------------

    // https://files.<metaverse host>: tea://<user>/<path> is <files host>/<user>/<path> (ResourceManager.cpp)
    function filesHost() {
        return "https://files." + String(MetaverseRequests.metaverseUrl()).replace(/^https?:\/\//i, "").replace(/\/.*$/, "");
    }

    // The link as one key: a tea:// address becomes the files host's https one, the scheme and host are lower case,
    // and the rest is read as decoded ("TEST%20AVATAR.fst" and "TEST AVATAR.fst" are one avatar; decodeURI leaves
    // the characters that shape an address, such as %2F, as they are). A web path's case matters, so it is kept.
    function keyOf(url) {
        var address = String(url || "").trim();
        var decoded = function (rest) {
            try {
                return decodeURI(rest);
            } catch (e) {
                return rest;   // a stray %
            }
        };
        var tea = /^tea:\/\/([^\/?#]*)(.*)$/i.exec(address);
        if (tea) {
            return filesHost() + "/" + tea[1].toLowerCase() + decoded(tea[2]);
        }
        var parts = /^([a-z][a-z0-9+.\-]*):\/\/([^\/?#]*)(.*)$/i.exec(address);
        if (parts) {
            return parts[1].toLowerCase() + "://" + parts[2].toLowerCase() + decoded(parts[3]);
        }
        return address;
    }

    // the link to show: a tea:// address as the files host's https one
    function webUrl(url) {
        var address = String(url || "").trim();
        return /^tea:\/\//i.test(address) ? keyOf(address) : address;
    }

    // a picture's source: one in My Files (tea:// or the files host) through the engine's loader, which asks with the
    // login for a private one (ResourceImageProvider); anything else as it is
    function pictureSource(address) {
        address = String(address || "");
        if (/^tea:\/\//i.test(address) || /^https?:\/\/files\./i.test(address)) {
            return "image://resource/" + encodeURIComponent(address);
        }
        return address;
    }

    function fileNameOf(url) {
        var path = String(url || "").replace(/[?#].*$/, "");
        var name = path.substring(path.lastIndexOf("/") + 1);
        try {
            return decodeURIComponent(name);
        } catch (e) {
            return name;   // a stray % in a name
        }
    }

    // a name made from a file's: no extension, dashes and underscores as spaces ("female full body cyberpunk ...")
    function nameFromFile(url) {
        return fileNameOf(url).replace(/\.[A-Za-z0-9]{1,5}$/, "").replace(/[-_]+/g, " ").replace(/\s+/g, " ").trim();
    }

    function isLocalLink(url) {
        return /^file:/i.test(String(url || ""));
    }

    function hostOf(url) {
        var parts = /^[a-z][a-z0-9+.\-]*:\/\/([^\/?#]*)/i.exec(webUrl(url));
        return parts && !isLocalLink(url) ? parts[1].replace(/^[^@]*@/, "").toLowerCase() : "";
    }

    // ---- reading the three places -----------------------------------------------------------------------------------

    function readFavorites() {
        favoritesRead = AvatarBookmarks.getBookmarks() || {};
    }

    // the pictures the app took by itself of the avatars worn, by key
    function readAutoPictures() {
        var taken = typeof AvatarThumbnails !== "undefined" && AvatarThumbnails.autoPictures ? AvatarThumbnails.autoPictures() || {} : {};
        var byKey = {};
        Object.keys(taken).forEach(function (url) {
            byKey[keyOf(url)] = String(taken[url]);
        });
        autoPicturesRead = byKey;
    }

    // the signed-in account's name hashed, or "" (signed out, or its name not known yet)
    function accountHash() {
        var name = AccountServices.loggedIn ? String(AccountServices.username || "").toLowerCase() : "";
        return name !== "" ? Qt.md5(name) : "";
    }

    function recentKey() {
        var hash = accountHash();
        return recentSetting + "/" + (hash !== "" ? hash : "signed-out");
    }

    // My Files' last list's setting for the signed-in account, or "" when there is none to keep
    function filesKey() {
        var hash = accountHash();
        return hash !== "" ? filesSetting + "/" + hash : "";
    }

    // the signed-in account's last list of My Files, shown at once (an opening, another account); none: an empty list
    function readKeptFiles() {
        var key = filesKey();
        var kept = null;
        if (key !== "") {
            var stored = Settings.getValue(key, "");
            try {
                kept = typeof stored === "string" && stored !== "" ? JSON.parse(stored) : null;
            } catch (e) {
                kept = null;
            }
        }
        var ok = !!kept && typeof kept.url === "string" && Array.isArray(kept.avatars);
        filesRead = ok ? avatarsFrom({ url: kept.url, avatars: kept.avatars.filter(function (a) {
            return Array.isArray(a) && typeof a[0] === "string" && /^\/.*\.fst$/i.test(a[0]) && typeof a[1] === "string"
                && typeof a[2] === "number";
        }) }) : [];
        filesKnown = ok;
        filesKeptText = ok ? String(Settings.getValue(key, "")) : "";
    }

    function readRecent() {
        var stored = Settings.getValue(recentKey(), "[]");
        var list = [];
        try {
            list = typeof stored === "string" ? JSON.parse(stored) : stored;
        } catch (e) {
            list = [];
        }
        // each avatar once, by its link (the default avatar once, whichever default it was)
        var seen = {};
        recentRead = (Array.isArray(list) ? list : []).filter(function (r) {
            if (!r || typeof r.url !== "string" || r.url === "") {
                return false;
            }
            var key = r.isDefault ? "default" : keyOf(r.url);
            if (seen[key]) {
                return false;
            }
            seen[key] = true;
            return true;
        }).slice(0, recentKept);
    }

    // the avatar worn now, at the head of Recent, once (Recent read first: the setting is the list, not what was read)
    function remember() {
        var url = String(MyAvatar.getFullAvatarURLFromPreferences() || "").trim();
        if (url === "") {
            return;
        }
        readRecent();
        var name = String(MyAvatar.getFullAvatarModelName() || "");
        var isDefault = name === "Default" || url === String(MyAvatar.getDefaultFullAvatarURL()) || /^qrc:/i.test(url);
        var key = isDefault ? "default" : keyOf(url);
        var keyOfRecord = function (r) { return r.isDefault ? "default" : keyOf(r.url); };
        if (recentRead.length > 0 && keyOfRecord(recentRead[0]) === key && recentRead[0].url === url) {
            return;
        }
        var list = [{ url: url, name: name, worn: Date.now(), isDefault: isDefault }].concat(recentRead.filter(function (r) {
            return keyOfRecord(r) !== key;
        })).slice(0, recentKept);
        Settings.setValue(recentKey(), JSON.stringify(list));
        recentRead = list;
    }

    // My Files' avatars (.fst files) with their pictures; a private (tea-only) one loads with the login
    function fetchMyFiles() {
        if (!AccountServices.loggedIn) {
            if (filesRead.length > 0 || status !== "" || filesKnown) {
                filesRead = [];
                filesKnown = false;
                status = "";
                rebuild();
            }
            return;
        }
        if (busy) {
            return;
        }
        busy = true;
        var asked = filesKey();
        var started = Date.now();
        MetaverseRequests.send("GET", "/api/user/files", null, function (error, code, data) {
            busy = false;
            if (filesKey() !== asked) {
                fetchMyFiles();   // another account since it was asked: never one account's list under another's
                return;
            }
            if (error || !data) {
                console.log("Avatar: My Files failed: " + code + " " + error);
                status = "Couldn't list My Files. Try again in a moment.";
                return;   // the last list stays
            }
            var list = listed(data);
            // counts and time only, never names (qa, 11-10: a slow list on one person's computer)
            console.log("Avatar: My Files listed " + (data.files || []).length + " files, " + list.avatars.length
                        + " avatars, in " + (Date.now() - started) + " ms");
            status = "";
            filesRead = avatarsFrom(list);
            filesKnown = true;
            if (asked !== "") {
                var text = JSON.stringify({ url: list.url, avatars: list.avatars.slice(0, filesKept) });
                if (text !== filesKeptText) {
                    Settings.setValue(asked, text);
                    filesKeptText = text;
                }
            }
            rebuild();
        });
    }

    // the picture beside an .fst: <name>.preview.jpg/.png (also as an upload names it: "Auxi Day" is stored as
    // Auxi_Day), then preview.jpg/.png in its folder
    function pictureFor(keys, folder, name) {
        var safe = name.replace(/[^A-Za-z0-9._-]/g, "_");
        var candidates = [name + ".preview.jpg", name + ".preview.jpeg", name + ".preview.png", safe + ".preview.jpg",
                          "preview.jpg", "preview.jpeg", "preview.png"];
        for (var i = 0; i < candidates.length; i++) {
            var found = keys[(folder + candidates[i]).toLowerCase()];
            if (found) {
                return found;
            }
        }
        return "";
    }

    // My Files' answer as only what the list needs, in the form its last list is kept: the files host's base, and per
    // .fst its path, its picture's path with the picture's date ("" when none), its date (ms), 1 when private
    function listed(data) {
        var files = data.files || [];
        var pathOf = function (file) { return file.key.charAt(0) === "/" ? file.key : "/" + file.key; };
        var keys = {};
        var updated = {};
        files.forEach(function (file) {
            var path = pathOf(file);
            keys[path.toLowerCase()] = path;
            updated[path] = Date.parse(file.updated) || 0;
        });
        var avatars = [];
        files.forEach(function (file) {
            if (!/\.fst$/i.test(file.key)) {
                return;
            }
            var path = pathOf(file);
            var picture = pictureFor(keys, path.substring(0, path.lastIndexOf("/") + 1), path.split("/").pop().replace(/\.fst$/i, ""));
            // the picture's date in its address, so a new picture isn't the old one cached
            avatars.push([path, picture !== "" ? picture + "?" + updated[picture] : "", Date.parse(file.updated) || 0, file.tea ? 1 : 0]);
        });
        avatars.sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; });
        return { url: String(data.url || "").replace(/\/$/, ""), avatars: avatars };
    }

    // the entries of My Files' avatars from what listed() gives (or the last list kept)
    function avatarsFrom(list) {
        var found = list.avatars.map(function (a) {
            var path = a[0];
            var folder = path.substring(0, path.lastIndexOf("/") + 1);
            var url = list.url + path;
            return { key: keyOf(url), url: url, title: path.split("/").pop().replace(/\.fst$/i, ""),
                     folder: folder.replace(/^\/|\/$/g, ""), picture: a[1] !== "" ? pictureSource(list.url + a[1]) : "",
                     updated: a[2], isPrivate: !!a[3], legacy: /^\/legacy\//i.test(path) };
        });
        found.sort(function (a, b) { return a.key < b.key ? -1 : a.key > b.key ? 1 : 0; });
        return found;
    }

    // ---- the list -----------------------------------------------------------------------------------------------------

    // a folder for the grey line: without its last part when that is the avatar's own folder
    // ("made/avatars/a-cute-duck-robot-2ib2hk" for a-cute-duck-robot.fst reads "made/avatars")
    function shortFolder(folder, title) {
        var parts = folder.split("/").filter(function (p) { return p !== ""; });
        if (parts.length > 1 && parts[parts.length - 1].toLowerCase().indexOf(title.toLowerCase()) === 0) {
            parts.pop();
        }
        return parts.join("/");
    }

    function rebuild() {
        var byKey = {};
        var order = [];
        var entryAt = function (key, url) {
            if (!byKey[key]) {
                byKey[key] = { key: key, url: url, filesTitle: "", wornName: "", folder: "", picture: "", updated: 0,
                               isPrivate: false, legacy: false, inMyFiles: false, favorite: false, favoriteName: "",
                               savedLook: false, inRecent: false };
                order.push(key);
            }
            return byKey[key];
        };

        var privates = 0;
        var legacies = 0;
        filesRead.forEach(function (file) {
            var entry = entryAt(file.key, file.url);
            entry.inMyFiles = true;
            entry.filesTitle = file.title;
            entry.folder = file.folder;
            entry.picture = file.picture;
            entry.updated = file.updated;
            entry.isPrivate = file.isPrivate;
            entry.legacy = file.legacy;
            privates += file.isPrivate ? 1 : 0;
            legacies += file.legacy ? 1 : 0;
        });

        // the favourites by name, so that two of one avatar give it the first name
        Object.keys(favoritesRead).sort(function (a, b) {
            return a.toLowerCase() < b.toLowerCase() ? -1 : a.toLowerCase() > b.toLowerCase() ? 1 : 0;
        }).forEach(function (name) {
            var bookmark = favoritesRead[name];
            var url = typeof bookmark === "string" ? bookmark : String((bookmark && bookmark.avatarUrl) || "");
            if (url.trim() === "") {
                return;
            }
            var entry = entryAt(keyOf(url), url.trim());
            if (!entry.favorite) {
                entry.favorite = true;
                entry.favoriteName = name;
                entry.savedLook = !!bookmark && typeof bookmark === "object" && bookmark.avatarEntites !== undefined;
            }
        });

        recentRead.forEach(function (record) {
            if (record.isDefault) {
                return;
            }
            var entry = entryAt(keyOf(record.url), record.url);
            entry.inRecent = true;
            if (entry.wornName === "" && record.name && record.name !== "Default") {
                entry.wornName = record.name;
            }
        });

        var newItems = order.map(function (key) {
            var raw = byKey[key];
            var local = isLocalLink(raw.url);
            var title = raw.favoriteName || raw.filesTitle || raw.wornName || nameFromFile(raw.url) || webUrl(raw.url);
            var source = raw.inMyFiles ? "myFiles" : local ? "local" : "web";
            var link = webUrl(raw.url);
            var subtitle = source === "myFiles" ? shortFolder(raw.folder, raw.filesTitle)
                : source === "local" ? "On this computer" : hostOf(raw.url);
            var fileName = fileNameOf(raw.url);
            return {
                key: key, url: raw.url, link: link, title: title, fileName: fileName, folder: raw.folder,
                host: hostOf(raw.url), source: source, subtitle: subtitle,
                keywords: [title, fileName, raw.folder, link].join(" "),
                picture: raw.picture || autoPicturesRead[key] || "", updated: raw.updated, isPrivate: raw.isPrivate, badge: raw.isPrivate ? "private" : "",
                inMyFiles: raw.inMyFiles, favorite: raw.favorite, favoriteName: raw.favoriteName, local: local,
                legacy: raw.legacy, savedLook: raw.savedLook, recentOnly: !raw.inMyFiles && !raw.favorite
            };
        });

        var index = {};
        newItems.forEach(function (entry) { index[entry.key] = entry; });
        var newRecent = recentRead.slice(0, recentShown).map(function (record) {
            if (record.isDefault) {
                return { key: "default", url: record.url, link: webUrl(record.url), title: "Default", fileName: fileNameOf(record.url),
                         folder: "", host: hostOf(record.url), source: "default", subtitle: "Default", keywords: "Default",
                         picture: autoPicturesRead[keyOf(record.url)] || "", updated: 0, isPrivate: false, badge: "", inMyFiles: false, favorite: false,
                         favoriteName: "", local: false, legacy: false, savedLook: false, recentOnly: false,
                         lastWorn: record.worn || 0 };
            }
            return Object.assign({}, index[keyOf(record.url)], { lastWorn: record.worn || 0 });
        });
        newRecent.forEach(function (entry) {
            if (entry.key === "default") {
                index["default"] = entry;
            }
        });

        var itemsText = JSON.stringify(newItems);
        if (itemsText !== itemsSignature) {
            itemsSignature = itemsText;
            items = newItems;
        }
        var recentText = JSON.stringify(newRecent);
        if (recentText !== recentSignature) {
            recentSignature = recentText;
            recent = newRecent;
        }
        entriesByKey = index;
        myFilesCount = filesRead.length;
        privateCount = privates;
        legacyCount = legacies;
    }

    // ---- what the panel calls -----------------------------------------------------------------------------------------

    // when the panel shows: the worn avatar into Recent, favourites and Recent read again, My Files' last list at once
    // (a page made new for this opening), then My Files asked for once
    function refresh() {
        readFavorites();
        readAutoPictures();
        remember();
        readRecent();
        if (!filesKnown) {
            readKeptFiles();
        }
        rebuild();
        fetchMyFiles();
    }

    // the entry for a link (the worn avatar's, for its Keep or Favorite switch), or null
    function entryFor(url) {
        return entriesByKey[keyOf(url)] || null;
    }

    // a favourite's name not in use yet: the name, else "name 2", "name 3", ...
    function freeName(name) {
        name = String(name || "").trim() || "Avatar";
        var taken = favoritesRead || {};
        if (taken[name] === undefined) {
            return name;
        }
        for (var n = 2; ; n++) {
            if (taken[name + " " + n] === undefined) {
                return name + " " + n;
            }
        }
    }

    // Keep in My Avatars (a web link) or Favorite (any other) on the avatar worn: kept with its scale and wearables,
    // named after its file; its favourite's name when it is one already
    function keep() {
        var url = String(MyAvatar.getFullAvatarURLFromPreferences() || "").trim();
        if (url === "") {
            return "";
        }
        var entry = entryFor(url);
        if (entry && entry.favorite) {
            return entry.favoriteName;
        }
        var name = freeName(nameFromFile(url) || String(MyAvatar.getFullAvatarModelName() || ""));
        AvatarBookmarks.addBookmark(name);
        readFavorites();
        rebuild();
        return name;
    }

    // the star on any entry: the worn avatar as keep() keeps it, any other by its link (no wearables; it is worn by its
    // link). url and title name one that has left `items` (a web link whose star was just taken off, still shown).
    // Its favourite's name, or "" when it can't be one (the default avatar)
    function star(key, url, title) {
        var entry = entriesByKey[key] || (url ? { key: keyOf(url), url: String(url), title: String(title || ""),
                                                   source: "", favorite: false } : null);
        if (!entry || entry.source === "default" || entry.key !== key) {
            return "";
        }
        if (entry.favorite) {
            return entry.favoriteName;
        }
        if (key === keyOf(MyAvatar.getFullAvatarURLFromPreferences())) {
            return keep();
        }
        var name = freeName(entry.title || nameFromFile(entry.url));
        var added = AvatarBookmarks.addBookmarkForLink(name, entry.url);
        readFavorites();
        rebuild();
        return added ? name : "";
    }

    // the star taken off: every favourite of that link goes; an entry in My Files or Recent stays in `items`
    function unstar(key) {
        var names = Object.keys(favoritesRead).filter(function (name) {
            var bookmark = favoritesRead[name];
            var url = typeof bookmark === "string" ? bookmark : String((bookmark && bookmark.avatarUrl) || "");
            return url.trim() !== "" && keyOf(url) === key;
        });
        names.forEach(function (name) { AvatarBookmarks.removeBookmark(name); });
        readFavorites();
        rebuild();
    }

    // ---- what changes them --------------------------------------------------------------------------------------------

    // an avatar change (one setting written; nothing on a timer). This lives while the panel's page does; refresh()
    // puts the worn avatar in Recent at each opening for the rest.
    property Connections avatarChanges: Connections {
        target: MyAvatar
        function onSkeletonModelURLChanged() {
            library.remember();
            library.rebuild();
        }
    }
    // a favourite added or removed elsewhere (a script, the old Avatar page)
    property Connections favoriteChanges: Connections {
        target: AvatarBookmarks
        ignoreUnknownSignals: true
        function onBookmarkAdded() {
            library.readFavorites();
            library.rebuild();
        }
        function onBookmarkDeleted() {
            library.readFavorites();
            library.rebuild();
        }
    }
    // a picture taken by itself of the avatar worn (once per avatar): the lists change once
    property Connections pictureChanges: Connections {
        target: typeof AvatarThumbnails !== "undefined" ? AvatarThumbnails : null
        ignoreUnknownSignals: true
        function onAutoPicturesChanged() {
            library.readAutoPictures();
            library.rebuild();
        }
    }
    // another account, or none: its own Recent, pictures and My Files' last list (the next opening asks My Files);
    // signed out, My Files' avatars (some of them private) leave the list
    property Connections accountChanges: Connections {
        target: AccountServices
        ignoreUnknownSignals: true
        function onLoggedInChanged() {
            library.accountChanged();
        }
        function onMyUsernameChanged() {
            library.accountChanged();
        }
    }
    function accountChanged() {
        status = "";
        readRecent();
        readKeptFiles();
        readAutoPictures();
        rebuild();
    }
}
