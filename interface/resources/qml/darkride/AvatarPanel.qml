//
//  AvatarPanel.qml
//  qml/darkride
//
//  Darkride's Avatar panel, in the panels' iOS look (darkride/panel), with three tabs (Caity, 10-23):
//  Avatar: the avatar worn (its picture and name); Favorites; the avatars in My Files as a collection (Caity, 10-24:
//    she has 100+), under the panels' search bar (search, Cards / Tiles / List, Name / Newest / Folder), each with its
//    picture when there is one beside it (<name>.preview.jpg or .png, or preview.jpg/.png in its folder: the
//    picture an .fst's `preview` field names); wearing one from a link (Wear from Link…: an .fst as it is, a .glb
//    imported first), the default, its scale, its collisions with the world, and putting it in My Files; for an avatar in your own My Files, a picture: taken of you wearing it
//    or chosen from this computer (AvatarThumbnails), saved beside its .fst as <name>.preview.jpg, private when the
//    avatar is (Caity, 10-24); the pictures load through the engine's loader (image://resource/), which asks the files
//    host again with your login for a private one;
//  Menu: the avatar's own menu, as its package made it (toggles, buttons held while pressed, radial sliders and
//    submenus; MyAvatar.getAvatarMenu()), drawn for whatever avatar is worn; a choice goes to avatarMenu.js on the
//    local channel io.darkride.avatarMenu, which sets the parameter, remembers the saved ones and plays emotes;
//  Nametag: the name and pronoun others see above your head, through the metaverse's API.
//  The bar's Avatar button opens it (tivoli-scripts' ButtonManager.addQmlButton). It replaces the web page of the
//  same name and the Avatar Menu app's web page.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.15
import QtQuick.Layouts 1.15
import "panel"

Page {
    id: root

    property real unit: 1
    signal sendToScript(var message)   // the tablet's loader connects it; nothing is sent

    // the tablet's loader sizes the page; a desktop window parents it to its content area
    anchors.fill: parent
    title: "Avatar"
    tabs: ["Avatar", "Menu", "Nametag"]

    readonly property string tabSetting: "io.darkride.avatarPanel.tab"

    header: [
        SearchBar {
            id: avatarBar
            visible: root.currentTab === 0
            settingsKey: "io.darkride.avatarPanel.files"
            placeholder: "Search your avatars"
            sorts: [
                { text: "Name", value: "name", by: function (a, b) { return a.title.toLowerCase().localeCompare(b.title.toLowerCase()); } },
                { text: "Newest", value: "newest", by: function (a, b) { return b.updated - a.updated; } },
                { text: "Folder", value: "folder", by: function (a, b) { return a.key.toLowerCase().localeCompare(b.key.toLowerCase()); } }
            ]
        }
    ]
    Component.onCompleted: currentTab = Math.max(0, Math.min(2, Settings.getValue(tabSetting, 0)))
    onCurrentTabChanged: Settings.setValue(tabSetting, currentTab)

    // a desktop window keeps the page between openings: read the values without a change signal again on each
    onVisibleChanged: {
        if (visible) {
            avatar.refresh();
            menu.reload();
            myFiles.load();
            nametag.load();
        }
    }

    Connections {
        target: MyAvatar
        function onSkeletonModelURLChanged() {
            if (avatar.linkGiven !== "") {
                avatar.linkWorn(String(MyAvatar.getFullAvatarURLFromPreferences()));
            }
            avatar.refresh();
            menu.reload();
        }
        function onAvatarMenuChanged() {
            menu.reload();
        }
    }

    // ==== Avatar =====================================================================================================

    QtObject {
        id: avatar

        property string url: ""
        property string name: ""
        property string pictureUrl: ""
        property var favorites: []           // the favourites' names, sorted
        property string favoriteName: ""     // the favourite worn, when the avatar came from one
        property bool collisions: true
        property real scale: 1
        property bool adding: false
        property bool linking: false         // Wear from Link…'s box is open
        property string linkGiven: ""        // a link being worn (after its license or the importer's questions)
        property string removing: ""         // the favourite whose Remove waits for a second press

        // the Avatar page's own setting, kept so a favourite worn there is still named here
        readonly property string favoriteNameSetting: "com.tivolicloud.defaultScripts.avatarBookmarkName"

        function refresh() {
            url = String(MyAvatar.getFullAvatarURLFromPreferences());
            name = MyAvatar.getFullAvatarModelName();
            pictureUrl = webUrl(String(MyAvatar.getAvatarPreviewImageURL()).trim());
            favorites = Object.keys(AvatarBookmarks.getBookmarks() || {}).sort(function (a, b) {
                return a.toLowerCase() < b.toLowerCase() ? -1 : 1;
            });
            favoriteName = Settings.getValue(favoriteNameSetting, "");
            collisions = MyAvatar.collisionsEnabled;
            scale = MyAvatar.scale;
        }

        // a picture's source: one in My Files (tea:// or the files host) through the engine's loader, which asks with
        // your login for a private one (ResourceImageProvider); anything else as it is
        function pictureSource(address) {
            address = String(address || "");
            if (/^tea:\/\//i.test(address) || /^https?:\/\/files\./i.test(address)) {
                return "image://resource/" + encodeURIComponent(address);
            }
            return address;
        }

        // tea://<user>/<path> is the files host's https://files.<metaverse host>/<user>/<path> (ResourceManager.cpp)
        function webUrl(address) {
            if (/^tea:\/\//i.test(address)) {
                var host = MetaverseRequests.metaverseUrl().replace(/^https?:\/\//, "").replace(/\/.*$/, "");
                return "https://files." + host + "/" + address.substring("tea://".length);
            }
            return address;
        }

        function wear(address, modelName) {
            linkGiven = "";
            var before = String(MyAvatar.getFullAvatarURLFromPreferences());
            MyAvatar.useFullAvatarURL(address, modelName);
            var after = String(MyAvatar.getFullAvatarURLFromPreferences());
            if (after === before && after !== String(address)) {
                // refused (someone else's private avatar, Caity 10-27): the look you have stays, its scale and name too
                refresh();
                return;
            }
            MyAvatar.scale = 1;
            Settings.setValue(favoriteNameSetting, "");
            refresh();
        }
        function wearDefault() {
            linkGiven = "";
            MyAvatar.randomizeDefaultAvatar();
            MyAvatar.useFullAvatarURL(MyAvatar.getDefaultFullAvatarURL(), "Default");
            MyAvatar.scale = 1;
            Settings.setValue(favoriteNameSetting, "");
            refresh();
        }
        function wearFavorite(favorite) {
            linkGiven = "";
            var bookmark = (AvatarBookmarks.getBookmarks() || {})[favorite] || {};
            var before = String(MyAvatar.getFullAvatarURLFromPreferences());
            AvatarBookmarks.loadBookmark(favorite);
            var after = String(MyAvatar.getFullAvatarURLFromPreferences());
            if (after === before && after !== String(bookmark.avatarUrl)) {
                // refused, as wear() can be: it isn't the favorite worn
                refresh();
                return;
            }
            Settings.setValue(favoriteNameSetting, favorite);
            refresh();
        }
        // the same asks as a link dropped on the window, but not "Wear it?" (Wear was pressed): an .fst's license, or a
        // .glb's import (Application::wearAvatarLink); the avatar changes once they're answered (linkWorn)
        function wearLink(link) {
            link = link.trim();
            if (link === "" || link === url) {
                return;
            }
            linking = false;
            linkGiven = link;
            ApplicationInterface.wearAvatarLink(link);
        }
        // a new avatar from the link given (a .glb's or .vrm's is the imported copy, on this computer) starts at scale
        // 1 and is no favourite; any other change only forgets the link (its license or import was turned down)
        function linkWorn(worn) {
            var fromLink = worn === linkGiven || (/\.(glb|vrm)$/i.test(linkGiven) && /^file:.*\/Avatars\//.test(worn));
            linkGiven = "";
            if (fromLink) {
                MyAvatar.scale = 1;
                Settings.setValue(favoriteNameSetting, "");
            }
        }
        function addFavorite(favorite) {
            favorite = favorite.trim();
            if (favorite === "") {
                return;
            }
            AvatarBookmarks.addBookmark(favorite);
            Settings.setValue(favoriteNameSetting, favorite);
            adding = false;
            refresh();
        }
        function removeFavorite(favorite) {
            AvatarBookmarks.removeBookmark(favorite);
            if (favoriteName === favorite) {
                Settings.setValue(favoriteNameSetting, "");
            }
            removing = "";
            refresh();
        }

        Component.onCompleted: refresh()
    }

    // the avatars (.fst files) in My Files; a tea-only (private) one is fetched with your login (the client asks again
    // with it when the files host refuses: main 3c9b0d3468, and the metaverse's private files, 10-24)
    QtObject {
        id: myFiles

        property var avatars: []     // [{ key, title, subtitle, picture, url, updated, isPrivate, badge }]
        property int privateCount: 0
        property bool busy: false
        property string status: ""

        // as the search bar arranges them, the one worn ticked
        readonly property var shown: avatarBar.arrange(avatars).map(function (entry) {
            return Object.assign({}, entry, { checked: entry.url === avatar.webUrl(avatar.url) });
        })
        // the avatar worn, when it is one of these
        readonly property var worn: avatars.filter(function (entry) { return entry.url === avatar.webUrl(avatar.url); })[0] || null

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

        function load() {
            if (!AccountServices.loggedIn || busy) {
                return;
            }
            busy = true;
            MetaverseRequests.send("GET", "/api/user/files", null, function (error, code, data) {
                busy = false;
                if (error || !data) {
                    console.log("Avatar: My Files failed: " + code + " " + error);
                    status = "Couldn't list My Files. Try again in a moment.";
                    return;
                }
                var found = [];
                var privates = 0;
                var keys = {};
                var updated = {};
                (data.files || []).forEach(function (file) {
                    var key = file.key.charAt(0) === "/" ? file.key : "/" + file.key;
                    keys[key.toLowerCase()] = key;
                    updated[key] = Date.parse(file.updated) || 0;
                });
                (data.files || []).forEach(function (file) {
                    if (!/\.fst$/i.test(file.key)) {
                        return;
                    }
                    var key = file.key.charAt(0) === "/" ? file.key : "/" + file.key;
                    var folder = key.substring(0, key.lastIndexOf("/") + 1);
                    var name = key.split("/").pop().replace(/\.fst$/i, "");
                    var picture = pictureFor(keys, folder, name);
                    privates += file.tea ? 1 : 0;
                    found.push({ key: key, title: name, subtitle: folder.replace(/^\/|\/$/g, "") || "My Files",
                                 // the picture's date in its address, so a new picture isn't the old one cached
                                 keywords: key, picture: picture !== "" ? avatar.pictureSource(data.url + picture + "?" + updated[picture]) : "",
                                 url: data.url + key, updated: Date.parse(file.updated) || 0, isPrivate: !!file.tea,
                                 badge: file.tea ? "private" : "" });
                });
                found.sort(function (a, b) { return a.key.toLowerCase() < b.key.toLowerCase() ? -1 : 1; });
                avatars = found;
                privateCount = privates;
                status = "";
            });
        }

        Component.onCompleted: load()
    }

    // the worn avatar's picture, when it is in your own My Files: taken or chosen, looked at, then saved beside it
    QtObject {
        id: thumb

        property string pending: ""     // a picture taken or chosen, not saved yet (a file:// address here)
        property bool fromFile: false   // the pending picture was chosen from this computer (not taken)
        property bool busy: false
        property bool choosing: false   // the file dialog is ours (Window answers every browseAsync)
        property string status: ""
        readonly property var place: AccountServices.loggedIn ? AvatarThumbnails.myFilesPlace(avatar.url) : ({})
        readonly property bool possible: place.folder !== undefined

        function take() {
            busy = true;
            status = "Taking a picture…";
            AvatarThumbnails.take(function (error, url) {
                busy = false;
                if (error) {
                    status = "Couldn't take a picture: " + error + ".";
                    return;
                }
                pending = url;
                fromFile = false;
                status = "";
            });
        }
        function choose() {
            choosing = true;
            WindowScriptingInterface.browseAsync("A picture for " + (avatar.name || "this avatar"), "", "Images (*.png *.jpg *.jpeg)");
        }
        function chosen(path) {
            choosing = false;
            if (path === "") {
                return;
            }
            AvatarThumbnails.fromFile(path, function (error, url) {
                if (error) {
                    status = "Couldn't use that picture: " + error + ".";
                    return;
                }
                pending = url;
                fromFile = true;
                status = "";
            });
        }
        function save() {
            busy = true;
            status = "Saving the picture…";
            var wantPrivate = !!(myFiles.worn && myFiles.worn.isPrivate);
            var key = AvatarThumbnails.pictureKey(avatar.url);
            AvatarThumbnails.upload(pending, avatar.url, function (error, url) {
                if (error) {
                    busy = false;
                    status = "Couldn't save the picture: " + error + ".";
                    return;
                }
                // the picture keeps its avatar's privacy: an upload keeps an old picture's setting, a new one is public
                var done = function (problem) {
                    busy = false;
                    pending = "";
                    status = problem !== "" ? problem
                        : wantPrivate ? "Saved beside the avatar, private as the avatar is." : "Saved beside the avatar in My Files.";
                    myFiles.load();
                };
                MetaverseRequests.send("GET", "/api/user/files", null, function (listError, code, data) {
                    var file = listError || !data ? null : (data.files || []).filter(function (f) {
                        return (f.key.charAt(0) === "/" ? f.key : "/" + f.key).toLowerCase() === key.toLowerCase();
                    })[0];
                    if (!file) {
                        done("Saved, but its privacy couldn't be checked.");
                        return;
                    }
                    if (!!file.tea === wantPrivate) {
                        done("");
                        return;
                    }
                    MetaverseRequests.send("POST", "/api/user/files/tea-only?path=" + encodeURIComponent(key), null,
                                           function (toggleError) {
                        done(toggleError ? "Saved, but its privacy couldn't be set: " + toggleError + "." : "");
                    });
                });
            });
        }
    }
    Connections {
        target: WindowScriptingInterface
        function onBrowseChanged(filename) {
            if (thumb.choosing) {
                thumb.chosen(filename);
            }
        }
    }

    Tab {
        visible: root.currentTab === 0

        Section {
            title: "Wearing"
            footer: thumb.status !== "" ? thumb.status
                : avatar.linking ? "A link to an avatar's .fst, .glb or .vrm file. A .glb or .vrm is set up for Darkride first, then you test it or keep it."
                : ""
            Item {
                width: parent ? parent.width : 0
                height: wornRow.implicitHeight + 2 * Theme.rowPadding + 4 * Theme.unit

                RowLayout {
                    id: wornRow
                    x: Theme.cardPadding
                    width: parent.width - 2 * Theme.cardPadding
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 12 * Theme.unit

                    RoundedImage {
                        Layout.preferredWidth: 56 * Theme.unit
                        Layout.preferredHeight: 56 * Theme.unit
                        radius: 10 * Theme.unit
                        placeholder: avatar.name !== "" ? avatar.name : "?"
                        source: myFiles.worn && myFiles.worn.picture !== "" ? myFiles.worn.picture
                            : avatar.pictureSource(avatar.pictureUrl)
                    }
                    Column {
                        Layout.fillWidth: true
                        spacing: 2 * Theme.unit
                        Text {
                            width: parent.width
                            text: avatar.favoriteName !== "" ? avatar.favoriteName : (avatar.name !== "" ? avatar.name : "Your avatar")
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.pixelSize: 16 * Theme.unit
                            font.bold: true
                            elide: Text.ElideMiddle
                        }
                        Text {
                            width: parent.width
                            text: avatar.url
                            textFormat: Text.PlainText
                            color: Theme.tertiaryText
                            font.pixelSize: Theme.detailSize
                            elide: Text.ElideMiddle
                        }
                    }
                }
            }
            ListRow {
                visible: thumb.possible && thumb.pending === ""
                action: true
                label: "Take Its Picture"
                enabled: !thumb.busy
                onClicked: thumb.take()
            }
            ListRow {
                visible: thumb.possible && thumb.pending === ""
                action: true
                label: "Choose a Picture…"
                enabled: !thumb.busy
                onClicked: thumb.choose()
            }
            // here, not below My Files, where hundreds of avatars hid it (Caity's bug batch, 10-29)
            ListRow {
                action: true
                label: "Use the Default Avatar"
                onClicked: avatar.wearDefault()
            }
            // here too: a player who wore avatars by their link didn't find the box below My Files (Caity, 10-29)
            ListRow {
                visible: !avatar.linking
                action: true
                label: "Wear from Link…"
                onClicked: {
                    // the link worn, selected: a paste replaces it, and it can be copied
                    linkField.text = avatar.url;
                    avatar.linking = true;
                    linkField.forceActiveFocus();
                    linkField.selectAll();
                }
            }
            ListRow {
                visible: avatar.linking
                label: "Link"
                fill: true
                Field {
                    id: linkField
                    objectName: "wearLinkField"   // a test fills it
                    Layout.fillWidth: true
                    horizontalAlignment: TextInput.AlignLeft
                    placeholderText: "https://…/avatar.fst"
                    onAccepted: avatar.wearLink(text)
                    Keys.onEscapePressed: avatar.linking = false
                }
            }
            ListRow {
                visible: avatar.linking
                action: true
                label: "Cancel"
                onClicked: avatar.linking = false
            }
            ListRow {
                visible: avatar.linking
                action: true
                label: "Wear"
                enabled: linkField.text.trim() !== "" && linkField.text.trim() !== avatar.url
                onClicked: avatar.wearLink(linkField.text)
            }
        }

        // a picture taken or chosen, to look at before it is saved
        Section {
            visible: thumb.pending !== ""
            title: "New picture"
            rightsLine: thumb.fromFile
            Item {
                width: parent ? parent.width : 0
                height: width * 0.75
                RoundedImage {
                    anchors.fill: parent
                    roundBottom: false
                    source: thumb.pending
                }
            }
            ListRow {
                action: true
                label: "Save as Its Picture"
                enabled: !thumb.busy
                onClicked: thumb.save()
            }
            ListRow {
                action: true
                label: "Take Another"
                enabled: !thumb.busy
                onClicked: thumb.take()
            }
            ListRow {
                action: true
                label: "Cancel"
                onClicked: { thumb.pending = ""; thumb.status = ""; }
            }
        }

        Section {
            title: "Favorites"
            footer: avatar.favorites.length === 0 && !avatar.adding ? "Add the avatar you wear to keep it a click away." : ""

            Repeater {
                model: avatar.favorites.filter(function (name) { return avatarBar.matches({ title: name }); })
                delegate: ListRow {
                    label: modelData
                    clickable: true
                    onClicked: avatar.wearFavorite(modelData)
                    Text {
                        text: "✓"
                        color: Theme.tint
                        font.pixelSize: Theme.bodySize
                        font.bold: true
                        visible: modelData === avatar.favoriteName
                    }
                    // a first press asks, a second removes
                    Text {
                        text: avatar.removing === modelData ? "Remove" : "−"   // −
                        color: avatar.removing === modelData ? Theme.destructive : Theme.tertiaryText
                        font.pixelSize: Theme.bodySize
                        font.bold: avatar.removing === modelData
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -8 * Theme.unit
                            onClicked: {
                                if (avatar.removing === modelData) {
                                    avatar.removeFavorite(modelData);
                                } else {
                                    avatar.removing = modelData;
                                }
                            }
                        }
                    }
                }
            }
            ListRow {
                visible: avatar.adding
                label: "Name"
                fill: true
                Field {
                    id: favoriteField
                    Layout.fillWidth: true
                    horizontalAlignment: TextInput.AlignLeft
                    onAccepted: avatar.addFavorite(text)
                }
            }
            ListRow {
                visible: avatar.adding
                action: true
                label: "Cancel"
                onClicked: avatar.adding = false
            }
            ListRow {
                visible: avatar.adding
                action: true
                label: "Save"
                enabled: favoriteField.text.trim() !== ""
                onClicked: avatar.addFavorite(favoriteField.text)
            }
            ListRow {
                visible: !avatar.adding
                action: true
                label: "Add This Avatar…"
                onClicked: {
                    favoriteField.text = avatar.name;
                    avatar.adding = true;
                    favoriteField.forceActiveFocus();
                    favoriteField.selectAll();
                }
            }
        }

        CollectionView {
            objectName: "avatarFiles"   // a test fills it
            visible: AccountServices.loggedIn
            title: "My Files" + (myFiles.avatars.length > 0 ? " · " + (avatarBar.search !== ""
                ? myFiles.shown.length + " of " + myFiles.avatars.length : myFiles.avatars.length) : "")
            items: myFiles.shown
            view: avatarBar.view
            empty: myFiles.status !== "" ? myFiles.status : myFiles.busy ? "…"
                : avatarBar.search !== "" ? "No avatar in My Files matches “" + avatarBar.search + "”."
                : "Avatars you put in My Files show here."
            footer: myFiles.privateCount > 0
                ? "Private: other players see it when they're on Darkride 1.2 or newer; it can't be downloaded from a browser."
                : ""
            onActivated: avatar.wear(item.url, item.title)
        }

        Section {
            title: "Body"
            footer: "Put This Avatar in My Files uploads an avatar that is on this computer, private: people see it on you in Darkride, but it has no public link."
            rightsLine: true

            ListRow {
                label: "Scale"
                fill: true
                labelWidth: 80 * Theme.unit
                scrubbable: true
                scrubValue: avatar.scale
                scrubFrom: 0.1
                scrubTo: 4
                scrubStep: 0.01
                onScrubbed: { MyAvatar.scale = Math.round(value * 100) / 100; avatar.refresh(); }

                ValueSlider {
                    from: 0.25; to: 3; stepSize: 0.05; decimals: 2; units: "×"
                    value: avatar.scale
                    onMoved: { MyAvatar.scale = value; avatar.refresh(); }
                }
                ResetButton {
                    needed: Math.abs(avatar.scale - 1) > 0.005
                    onClicked: { MyAvatar.scale = 1; avatar.refresh(); }
                }
            }
            ListRow {
                label: "Collide with the world"
                detail: "Off: walk through walls (noclip)"
                Toggle {
                    checked: avatar.collisions
                    onToggled: { MyAvatar.collisionsEnabled = checked; avatar.refresh(); }
                }
            }
            ListRow {
                action: true
                label: "Put This Avatar in My Files"
                onClicked: MenuInterface.triggerOption("Put Avatar in My Files")
            }
        }
    }

    // ==== Menu =======================================================================================================

    QtObject {
        id: menu

        property var avatarMenu: ({})   // MyAvatar.getAvatarMenu(): { menu: { controls }, parameters: [{ name, value, saved }] }
        property var stack: []          // the submenus opened, as indexes
        property var values: ({})       // each parameter's value, read ten times a second while the tab shows
        property string folder: ""      // the menu's icons sit beside the .fst

        readonly property bool hasMenu: !!(avatarMenu.menu && avatarMenu.menu.controls && avatarMenu.menu.controls.length)
        readonly property var controls: {
            var level = hasMenu ? avatarMenu.menu.controls : [];
            for (var i = 0; i < stack.length; i++) {
                level = (level[stack[i]] && level[stack[i]].controls) || [];
            }
            return level;
        }
        readonly property string path: {
            var names = [];
            var level = hasMenu ? avatarMenu.menu.controls : [];
            for (var i = 0; i < stack.length; i++) {
                if (!level[stack[i]]) {
                    break;
                }
                names.push(clean(level[stack[i]].name));
                level = level[stack[i]].controls || [];
            }
            return names.join(" › ");   // ›
        }

        function clean(text) {
            // a package's names may carry rich-text tags and escaped line breaks
            return String(text || "").replace(/<[^>]*>/g, "").replace(/\\n/g, " ").trim();
        }

        function reload() {
            avatarMenu = MyAvatar.getAvatarMenu() || {};
            stack = [];
            var address = String(MyAvatar.skeletonModelURL);
            folder = address.substring(0, address.lastIndexOf("/") + 1);
            poll();
        }

        function poll() {
            var read = {};
            (avatarMenu.parameters || []).forEach(function (parameter) {
                read[parameter.name] = MyAvatar.getAvatarParameter(parameter.name);
            });
            values = read;
        }

        function set(name, value) {
            // avatarMenu.js sets it, remembers it when the avatar saves it, and plays an emote it starts
            Messages.sendLocalMessage("io.darkride.avatarMenu", JSON.stringify({ type: "set", name: name, value: value }));
            var shown = Object.assign({}, values);
            shown[name] = value;
            values = shown;
        }

        function open(index) {
            stack = stack.concat([index]);
        }
        function back() {
            stack = stack.slice(0, -1);
        }

        function same(a, b) {
            return a !== undefined && Math.abs(a - b) < 0.001;
        }

        Component.onCompleted: reload()
    }

    Timer {
        interval: 100
        repeat: true
        running: root.visible && root.currentTab === 1 && menu.hasMenu
        onTriggered: menu.poll()
    }

    Tab {
        visible: root.currentTab === 1

        Section {
            title: menu.stack.length > 0 ? menu.path : "Avatar menu"
            footer: !menu.hasMenu ? "This avatar has no menu. Avatars imported from Unity packages bring theirs: "
                                    + "clothes to show or hide, expressions, emotes."
                  : "Buttons act while pressed. Choices the avatar saves are put back the next time you wear it."

            ListRow {
                visible: menu.stack.length > 0
                action: true
                label: "‹ Back"   // ‹
                onClicked: menu.back()
            }

            Repeater {
                model: menu.controls
                delegate: ListRow {
                    id: controlRow
                    readonly property string type: modelData.type || ""
                    readonly property string target: type === "radial"
                        ? ((modelData.subParameters && modelData.subParameters[0]) || modelData.parameter || "")
                        : (modelData.parameter || "")
                    readonly property bool known: type === "toggle" || type === "button" || type === "submenu"
                        || type === "radial"

                    label: menu.clean(modelData.name)
                    icon: modelData.icon ? menu.folder + encodeURI(modelData.icon) : ""
                    detail: known ? (type === "button" ? "Hold" : "") : type.charAt(0).toUpperCase() + type.slice(1) + ", not yet"
                    enabled: known
                    fill: type === "radial"
                    clickable: type === "button" || type === "submenu"
                    onClicked: if (type === "submenu") { menu.open(index); }
                    onPressed: if (type === "button") { menu.set(target, modelData.value); }
                    onReleased: if (type === "button") { menu.set(target, 0); }

                    Toggle {
                        visible: controlRow.type === "toggle"
                        checked: menu.same(menu.values[controlRow.target], modelData.value)
                        // a menu toggle: on sets its value, off puts the parameter back to 0
                        onToggled: menu.set(controlRow.target, checked ? modelData.value : 0)
                    }
                    Rectangle {
                        visible: controlRow.type === "button"
                        implicitWidth: 10 * Theme.unit
                        implicitHeight: implicitWidth
                        radius: width / 2
                        color: menu.same(menu.values[controlRow.target], modelData.value) ? Theme.on : Theme.fill
                    }
                    ValueSlider {
                        visible: controlRow.type === "radial"
                        from: 0; to: 1; stepSize: 0.01; decimals: 2
                        value: menu.values[controlRow.target] || 0
                        onMoved: menu.set(controlRow.target, value)
                    }
                    Text {
                        visible: controlRow.type === "submenu"
                        text: "›"
                        color: Theme.tertiaryText
                        font.pixelSize: 18 * Theme.unit
                    }
                }
            }
        }
    }

    // ==== Nametag ====================================================================================================

    QtObject {
        id: nametag

        readonly property bool signedIn: AccountServices.loggedIn
        readonly property string username: AccountServices.username
        property bool busy: false
        property string status: ""
        property bool rejoinNeeded: false
        property string pictureStamp: ""

        function load() {
            if (!signedIn || busy) {   // a first desktop opening asks twice: on creation, then on showing
                return;
            }
            busy = true;
            MetaverseRequests.send("GET", "/api/user/nametag-details", null, function (error, code, data) {
                busy = false;
                if (error) {
                    console.log("Avatar: nametag failed: " + code + " " + error);
                    status = "Couldn't read your nametag. Try again in a moment.";
                    return;
                }
                displayNameField.text = data && data.displayName ? data.displayName : "";
                pronounChoices.current = data && data.genderPronoun ? data.genderPronoun : "";
                status = "";
            });
            pictureStamp = String(Date.now());
        }

        function save() {
            busy = true;
            status = "Saving…";
            MetaverseRequests.send("POST", "/api/user/nametag-details",
                                   { displayName: displayNameField.text, genderPronoun: pronounChoices.current },
                                   function (error, code, data) {
                busy = false;
                if (error) {
                    console.log("Avatar: nametag save failed: " + code + " " + error);
                    status = "Couldn't save your nametag. Try again in a moment.";
                    return;
                }
                status = "Saved. Others see it after you rejoin the world.";
                rejoinNeeded = true;
                pictureStamp = String(Date.now());
            });
        }

        function rejoin() {
            rejoinTimer.target = String(AddressManager.href);
            AddressManager.handleLookupString("file:///");
            rejoinTimer.start();
            rejoinNeeded = false;
            status = "";
        }

        Component.onCompleted: load()
        onSignedInChanged: load()
    }

    Timer {
        id: rejoinTimer
        property string target: ""
        interval: 1000
        onTriggered: AddressManager.handleLookupString(target)
    }

    Tab {
        visible: root.currentTab === 2

        Section {
            title: "Nametag"
            footer: nametag.signedIn
                ? (nametag.status !== "" ? nametag.status : "Whether nametags show at all is in Settings, Displays.")
                : "Sign in to set the name and pronoun others see above your head."

            // the saved nametag as the metaverse draws it, in two pictures: the next loads unseen while the one shown
            // stays, so a Save swaps them without the section shrinking in between (Caity 10-28: "the UI crunches up")
            Item {
                id: nametagPreview
                readonly property string wanted: nametag.signedIn && nametag.username !== ""
                    ? MetaverseRequests.metaverseUrl() + "/api/user/" + nametag.username.toLowerCase()
                      + "/nametag?" + nametag.pictureStamp
                    : ""
                property Item shown: null

                function load() {
                    (shown === pictureA ? pictureB : pictureA).source = wanted;
                }
                function arrived(picture) {
                    if (picture.status === Image.Ready && String(picture.source) === wanted) {
                        shown = picture;
                    }
                }
                onWantedChanged: load()
                Component.onCompleted: load()

                visible: nametag.signedIn && shown !== null
                width: parent ? parent.width : 0
                height: visible ? shown.height + 2 * Theme.cardPadding : 0

                Image {
                    id: pictureA
                    visible: nametagPreview.shown === pictureA
                    anchors.centerIn: parent
                    width: Math.min(sourceSize.width, parent.width - 2 * Theme.cardPadding)
                    height: width * sourceSize.height / Math.max(1, sourceSize.width)
                    fillMode: Image.PreserveAspectFit
                    cache: false
                    onStatusChanged: nametagPreview.arrived(pictureA)
                }
                Image {
                    id: pictureB
                    visible: nametagPreview.shown === pictureB
                    anchors.centerIn: parent
                    width: Math.min(sourceSize.width, parent.width - 2 * Theme.cardPadding)
                    height: width * sourceSize.height / Math.max(1, sourceSize.width)
                    fillMode: Image.PreserveAspectFit
                    cache: false
                    onStatusChanged: nametagPreview.arrived(pictureB)
                }
            }
            ListRow {
                visible: nametag.signedIn
                label: "Display name"
                fill: true
                Field {
                    id: displayNameField
                    Layout.fillWidth: true
                    enabled: !nametag.busy
                    placeholderText: nametag.username
                    // Return saves, as on the Mac (Caity 10-28)
                    onAccepted: nametag.save()
                }
            }
            ListRow {
                visible: nametag.signedIn
                label: "Pronoun"
                Segmented {
                    id: pronounChoices
                    enabled: !nametag.busy
                    current: ""
                    model: [{ text: "None", value: "" }, { text: "She/Her", value: "she/her" },
                            { text: "He/Him", value: "he/him" }, { text: "They/Them", value: "they/them" }]
                    onChosen: current = value
                }
            }
            ListRow {
                visible: nametag.signedIn
                action: true
                label: "Save"
                enabled: !nametag.busy
                onClicked: nametag.save()
            }
            ListRow {
                visible: nametag.signedIn && nametag.rejoinNeeded
                action: true
                label: "Rejoin the World"
                onClicked: nametag.rejoin()
            }
            ListRow {
                visible: nametag.signedIn
                action: true
                label: "Change Picture…"
                onClicked: Qt.openUrlExternally(MetaverseRequests.metaverseUrl() + "/user/settings")
            }
        }
    }
}
