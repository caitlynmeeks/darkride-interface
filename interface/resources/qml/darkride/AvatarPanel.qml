//
//  AvatarPanel.qml
//  qml/darkride
//
//  Darkride's Avatar panel, in the panels' iOS look (darkride/panel), with three tabs (Caity, 10-23):
//  Avatars (mary's redo, Caity's yes 11-08: AVATAR-REDO.png): changing avatar on top; the avatar worn (Keep in My
//    Avatars for a web link, Favorite otherwise, its scale); the five worn last; My Avatars, one list of My Files,
//    favorites and the recently worn (darkride/AvatarLibrary.qml, avatars'), favorites first, ten then Show All.
//    Before the redo: the avatar worn (its picture and name); Favorites; the avatars in My Files as a collection (Caity, 10-24:
//    she has 100+), under the panels' search bar (search, Cards / Tiles / List, Name / Newest / Folder), each with its
//    picture when there is one beside it (<name>.preview.jpg or .png, or preview.jpg/.png in its folder: the
//    picture an .fst's `preview` field names); wearing one from a link (Paste an Avatar Link…: an .fst as it is, a .glb
//    imported first) or the default, in a card of their own above the avatar worn (mary's Apple pass, 11-08); saving
//    an avatar on this computer in My Files (Save in My Files, under it); its scale and its collisions with the world
//    (Body); for an avatar in your own My Files, a picture: taken of you wearing it
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
    tabs: ["Avatars", "Menu", "Nametag"]

    readonly property string tabSetting: "io.darkride.avatarPanel.tab"

    header: [
        SearchBar {
            id: avatarBar
            visible: root.currentTab === 0
            settingsKey: "io.darkride.avatarPanel.files"
            placeholder: "Search My Avatars"
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
            library.refresh();
            nametag.load();
        } else {
            list.forget();
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
        property string favoriteName: ""     // the favourite worn, when the avatar came from one
        property bool collisions: true
        property real scale: 1
        property bool linking: false         // Paste an Avatar Link…'s box is open
        // the avatar worn is a file on this computer: others see the default until it is saved in My Files
        // (AvatarImporter::putInMyFiles takes only a local file)
        readonly property bool onThisComputer: /^file:/i.test(url)
        property string linkGiven: ""        // a link being worn (after its license or the importer's questions)

        // the Avatar page's own setting, kept so a favourite worn there is still named here
        readonly property string favoriteNameSetting: "com.tivolicloud.defaultScripts.avatarBookmarkName"

        function refresh() {
            url = String(MyAvatar.getFullAvatarURLFromPreferences());
            name = MyAvatar.getFullAvatarModelName();
            pictureUrl = webUrl(String(MyAvatar.getAvatarPreviewImageURL()).trim());
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
        // a row of My Avatars: a favorite with a saved look (its scale and wearables) through the favorite, which
        // replaces the wearables worn; anything else by its link, as a My Files avatar is worn (avatars, 11-08)
        function wearEntry(entry) {
            if (entry.savedLook && entry.favoriteName) {
                wearFavorite(entry.favoriteName);
            } else {
                wear(entry.url, entry.title);
            }
        }

        Component.onCompleted: refresh()
    }

    // the avatars: My Files, favorites and the recently worn in one list (avatars' AvatarLibrary); it fetches My Files
    // once an opening (refresh), and leaves its lists as they are when nothing changed
    AvatarLibrary {
        id: library
    }

    // what My Avatars shows, and what it holds only until the panel closes or the search changes
    QtObject {
        id: list

        property bool showAll: false
        property bool showOlder: false
        property var ghosts: ({})        // key -> entry: a star just taken off; the row keeps its place, dimmed
        property string justKept: ""     // the avatar just kept: first in the list
        readonly property bool searching: avatarBar.search !== ""
        readonly property string wornKey: library.keyOf(avatar.url)
        readonly property var worn: library.entryFor(avatar.url)
        // My Avatars: My Files and favorites (the recently worn show only while searching)
        readonly property int total: library.items.filter(function (entry) { return entry.inMyFiles || entry.favorite; }).length

        // favorites first (one just kept before them, a star just taken off where it was), then as the bar sorts
        readonly property var ordered: {
            var held = ghosts;
            var all = library.items.filter(function (entry) {
                return searching || entry.inMyFiles || entry.favorite || held[entry.key] !== undefined;
            });
            var present = {};
            all.forEach(function (entry) { present[entry.key] = true; });
            Object.keys(held).forEach(function (key) {
                if (!present[key]) {
                    all.push(held[key]);
                }
            });
            var group = function (entry) {
                return entry.key === justKept ? 0 : entry.favorite || held[entry.key] !== undefined ? 1 : 2;
            };
            return avatarBar.arrange(all).map(function (entry, index) { return { entry: entry, index: index }; })
                .sort(function (a, b) { return group(a.entry) - group(b.entry) || a.index - b.index; })
                .map(function (pair) { return pair.entry; });
        }
        readonly property var current: ordered.filter(function (entry) { return !entry.legacy; })
        readonly property var older: ordered.filter(function (entry) { return entry.legacy; })
        readonly property bool folded: !searching && !showAll && current.length > 10
        readonly property var shown: (searching ? ordered : folded ? current.slice(0, 10)
                                      : current.concat(showOlder ? older : [])).map(decorate)
        readonly property var actions: searching ? []
            : folded ? [{ key: "all", label: "Show All " + ordered.length }]
            : older.length > 0 && !showOlder ? [{ key: "older", label: "Older files", value: String(older.length) }]
            : []
        readonly property bool keepsLinks: library.items.some(function (entry) { return entry.favorite && entry.source === "web"; })

        // a row's entry with the view's own state: worn, dimmed (a star just taken off), the grey line in words
        function decorate(entry) {
            var ghost = ghosts[entry.key] !== undefined;
            return Object.assign({}, entry, {
                subtitle: words(entry, true),
                badge: "",
                checked: entry.key === wornKey,
                dim: ghost,
                favorite: entry.favorite && !ghost
            });
        }
        // where an avatar lives, in words (mary's draft): My Files (yours everywhere), a web link (someone's site), or
        // this computer (only you see it)
        function words(entry, withLink) {
            if (entry.source === "myFiles") {
                return "My Files · " + (entry.legacy ? "older files" : entry.subtitle) + (entry.isPrivate ? " · private" : "");
            }
            if (entry.source === "local") {
                return "This computer · only you see it";
            }
            return "Web link · " + entry.host + (withLink && entry.fileName ? "/…/" + entry.fileName : "");
        }
        function toggleStar(entry) {
            var held = Object.assign({}, ghosts);
            if (entry.favorite) {
                // kept in place, dimmed, until the panel closes or the search changes: one more click undoes it
                held[entry.key] = Object.assign({}, library.items.filter(function (e) { return e.key === entry.key; })[0] || entry,
                                                { favorite: false });
                ghosts = held;
                library.unstar(entry.key);
            } else {
                delete held[entry.key];
                ghosts = held;
                library.star(entry.key, entry.url, entry.title);
            }
        }
        function forget() {
            ghosts = ({});
            justKept = "";
            showAll = false;
            showOlder = false;
        }
    }
    Connections {
        target: avatarBar
        function onSearchChanged() {
            list.ghosts = ({});
            list.justKept = "";
        }
    }

    // the avatar worn, as the Wearing card shows it
    QtObject {
        id: wearing

        readonly property var entry: list.worn
        readonly property bool isDefault: avatar.url === "" || /^qrc:/i.test(avatar.url)
            || avatar.url === String(MyAvatar.getDefaultFullAvatarURL())
        // someone's website (or someone else's files): kept by its link, not saved
        readonly property bool isWeb: !isDefault && !avatar.onThisComputer && !(entry && entry.inMyFiles)
        readonly property bool kept: !!(entry && entry.favorite)
        readonly property string title: entry ? entry.title
            : avatar.name !== "" ? avatar.name.replace(/-/g, " ") : "Your avatar"
        readonly property string words: isDefault ? "Darkride's default avatar"
            : entry ? list.words(entry, false)
            : avatar.onThisComputer ? "This computer · only you see it"
            : "Web link · " + avatar.url.replace(/^[a-z]+:\/\//i, "").replace(/\/.*$/, "")
        readonly property url picture: entry && entry.picture !== "" ? entry.picture : avatar.pictureSource(avatar.pictureUrl)

        // Keep in My Avatars / Favorite: kept, it's first in the list and brought into view
        function keep() {
            library.keep();
            list.justKept = list.wornKey;
            Qt.callLater(function () { root.reveal(myAvatars); });
        }
        function unkeep() {
            if (entry) {
                library.unstar(entry.key);
            }
        }
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
            var wantPrivate = !!(list.worn && list.worn.isPrivate);
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
                    library.refresh();
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

    // a row of My Avatars: its picture, name and where it lives; the tick on the one worn; its star
    Component {
        id: avatarRow
        ListRow {
            wrapLabel: true
            label: entry.title || ""
            detail: entry.subtitle || ""
            icon: entry.picture || ""
            iconSize: Theme.rowIconSize
            iconRadius: Theme.rowIconRadius
            iconPlaceholder: entry.title || "?"
            opacity: entry.dim ? Theme.dimmed : 1
            clickable: true
            onClicked: avatar.wearEntry(entry)
            Checkmark {
                checked: !!entry.checked
            }
            Star {
                on: !!entry.favorite
                onToggled: list.toggleStar(entry)
            }
        }
    }

    Tab {
        visible: root.currentTab === 0

        // changing avatar: its own card above the avatar worn (mary's Apple pass, 11-08; Caity's bug batch 10-29 first
        // brought these up from below My Files)
        Section {
            footer: avatar.linking ? "A link to an avatar's .fst, .glb or .vrm file. A .glb or .vrm is set up for Darkride first, then you test it or keep it."
                : ""

            ListRow {
                visible: !avatar.linking
                action: true
                label: "Paste an Avatar Link…"
                onClicked: {
                    // the link worn, selected: a paste replaces it, and it can be copied; a file on this computer is no
                    // link anyone can use, so then the box opens empty (mary, 11-08)
                    linkField.text = avatar.onThisComputer ? "" : avatar.url;
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
            // the open link box is one task: the card holds only it until Wear or Cancel (mary, 11-08)
            ListRow {
                visible: !avatar.linking
                action: true
                label: "Wear the Default Avatar"
                onClicked: avatar.wearDefault()
            }
        }

        // everything about the avatar worn, above the list (mary's redo: nothing buried under 250 avatars)
        Section {
            title: "Wearing"
            footer: thumb.status
            // the upload's rights line, with the row that uploads
            rightsLine: avatar.onThisComputer

            ListRow {
                large: true
                label: wearing.title
                detail: wearing.words
                icon: wearing.picture
                iconPlaceholder: wearing.title
            }
            // under the avatar it saves, and only when it would do something (mary's Apple pass, 11-08)
            ListRow {
                visible: avatar.onThisComputer
                action: true
                label: "Save in My Files"
                detail: "Others see the default avatar until it's saved."
                onClicked: MenuInterface.triggerOption("Put Avatar in My Files")
            }
            // a web avatar is kept by its link; any other is a favorite; either way it comes first in My Avatars
            ListRow {
                visible: !wearing.isDefault
                label: wearing.isWeb ? "Keep in My Avatars" : "Favorite"
                detail: !wearing.isWeb ? "Favorites come first in My Avatars."
                    : wearing.kept ? "Kept: it's first in My Avatars, below." : "It stays on the web, at its link."
                Toggle {
                    checked: wearing.kept
                    onToggled: checked ? wearing.keep() : wearing.unkeep()
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

        // the five worn last, a click away (the search finds the ten kept)
        Section {
            visible: !list.searching && library.recent.length > 0
            title: "Recent"

            Item {
                width: parent ? parent.width : 0
                height: recentRow.implicitHeight + 2 * Theme.rowPadding

                RowLayout {
                    id: recentRow
                    x: Theme.cardPadding
                    width: parent.width - 2 * Theme.cardPadding
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.gap

                    Repeater {
                        model: library.recent
                        delegate: CollectionTile {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.alignment: Qt.AlignTop
                            title: modelData.title
                            picture: modelData.picture
                            pictureRatio: 1
                            checked: modelData.key === list.wornKey
                            onClicked: avatar.wearEntry(modelData)
                        }
                    }
                    // fewer than five keep a fifth's width each
                    Repeater {
                        model: Math.max(0, 5 - library.recent.length)
                        delegate: Item {
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                        }
                    }
                }
            }
        }

        CollectionView {
            id: myAvatars
            objectName: "avatarFiles"   // a test fills it
            title: "My Avatars · " + (list.searching ? list.shown.length + " of " + list.total : list.total)
            items: list.shown
            view: avatarBar.view
            row: avatarRow
            actions: list.actions
            empty: library.status !== "" ? library.status : library.busy ? "…"
                : list.searching ? "No avatar in My Avatars matches “" + avatarBar.search + "”."
                : "Avatars you keep and put in My Files show here."
            footer: [library.privateCount > 0 ? "Private avatars are seen on you in Darkride 1.1 or newer, and have no public link." : "",
                     list.keepsLinks ? "Web links you keep are kept on this computer." : ""]
                .filter(function (line) { return line !== ""; }).join(" ")
            onActivated: avatar.wearEntry(item)
            onActionChosen: {
                if (key === "all") {
                    list.showAll = true;
                } else {
                    list.showOlder = true;
                }
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
