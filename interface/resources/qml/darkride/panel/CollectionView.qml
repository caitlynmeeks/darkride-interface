//
//  CollectionView.qml
//  qml/darkride/panel
//
//  Things a SearchBar arranged, in the view it chose (Caity, 10-24): Cards, a big picture over each, one a row;
//  Tiles, three a row (two when narrow); List, one card of rows, each with a small picture. An item:
//  { key, title, subtitle, picture, checked, badge, dim }; a click on one says activated(item). A page with more to
//  show on each passes its own card, tile and row components: each is loaded with the item as `entry`. Rows after
//  the things (actions: "Show All 256", "Older files · 181 ›") sit in the list's card and say actionChosen(key).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.15
import QtQuick.Layouts 1.15
import "."

ColumnLayout {
    id: collection

    property var items: []
    property string view: "cards"
    property string title: ""           // a heading over the things, as a Section's
    property string empty: ""           // said when there are none
    property string footer: ""          // said under them
    property Component card: defaultCard
    property Component tile: defaultTile
    property Component row: defaultRow
    property int columns: width < 480 * Theme.unit ? 2 : 3

    // [{ key, label, value }]: an action row in blue, or with a value, a row that opens more (its value and › at the right)
    property var actions: []

    signal activated(var item)
    signal actionChosen(string key)

    Layout.fillWidth: true
    spacing: 0

    function plain(text) {
        return String(text || "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }

    // one of `actions`, loaded with it as `more`
    Component {
        id: actionRow
        ListRow {
            label: more.label || ""
            action: !more.value
            clickable: true
            onClicked: collection.actionChosen(more.key)
            Text {
                text: (more.value || "") + "  ›"   // ›
                textFormat: Text.PlainText
                color: Theme.secondaryText
                font.pixelSize: Theme.bodySize
            }
        }
    }

    Component {
        id: defaultCard
        Section {
            Item {
                width: parent ? parent.width : 0
                height: width * 0.42
                RoundedImage {
                    anchors.fill: parent
                    roundBottom: false
                    placeholder: entry.title || ""
                    source: entry.picture || ""
                    opacity: entry.dim ? Theme.dimmed : 1
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: collection.activated(entry)
                }
            }
            ListRow {
                label: entry.title || ""
                detail: entry.subtitle || ""
                clickable: true
                onClicked: collection.activated(entry)
                Text {
                    visible: !!entry.badge
                    text: entry.badge || ""
                    textFormat: Text.PlainText
                    color: Theme.secondaryText
                    font.pixelSize: Theme.detailSize
                }
                Checkmark {
                    checked: !!entry.checked
                }
            }
        }
    }
    Component {
        id: defaultTile
        CollectionTile {
            title: entry.title || ""
            subtitle: collection.plain(entry.subtitle)
            picture: entry.picture || ""
            dim: !!entry.dim
            checked: !!entry.checked
            badge: entry.badge || ""
            onClicked: collection.activated(entry)
        }
    }
    Component {
        id: defaultRow
        ListRow {
            label: entry.title || ""
            detail: entry.subtitle || ""
            icon: entry.picture || ""
            iconSize: Theme.rowIconSize
            iconRadius: Theme.rowIconRadius
            iconPlaceholder: entry.title || "?"
            clickable: true
            onClicked: collection.activated(entry)
            Text {
                visible: !!entry.badge
                text: entry.badge || ""
                textFormat: Text.PlainText
                color: Theme.secondaryText
                font.pixelSize: Theme.detailSize
            }
            Checkmark {
                checked: !!entry.checked
            }
        }
    }

    // the heading, for Cards and Tiles (List's card carries its own)
    Text {
        Layout.fillWidth: true
        visible: collection.title !== "" && collection.view !== "list"
        text: collection.title.toUpperCase()
        textFormat: Text.PlainText
        color: Theme.secondaryText
        font.pixelSize: Theme.sectionSize
        font.letterSpacing: 0.3 * Theme.unit
        leftPadding: Theme.cardPadding
        topPadding: 22 * Theme.unit
        bottomPadding: collection.view === "tiles" ? 6 * Theme.unit : 0
    }

    Section {
        visible: collection.items.length === 0 && collection.empty !== ""
        title: collection.view === "list" ? collection.title : ""
        footer: collection.empty
    }

    // Cards
    Repeater {
        model: collection.view === "cards" ? collection.items : []
        delegate: Loader {
            property var entry: modelData
            Layout.fillWidth: true
            sourceComponent: collection.card
        }
    }

    // Tiles
    GridLayout {
        visible: collection.view === "tiles" && collection.items.length > 0
        Layout.fillWidth: true
        Layout.topMargin: collection.title === "" ? 18 * Theme.unit : 0
        columns: collection.columns
        columnSpacing: Theme.gap
        rowSpacing: Theme.gap

        Repeater {
            model: collection.view === "tiles" ? collection.items : []
            delegate: Loader {
                property var entry: modelData
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                sourceComponent: collection.tile
            }
        }
    }

    // List
    Section {
        visible: collection.view === "list" && collection.items.length > 0
        title: collection.title

        Repeater {
            model: collection.view === "list" ? collection.items : []
            delegate: Loader {
                id: rowLoader
                property var entry: modelData
                width: parent ? parent.width : 0
                sourceComponent: collection.row
                onLoaded: if (item.hasOwnProperty("cardItem")) item.cardItem = rowLoader
            }
        }
        Repeater {
            model: collection.view === "list" ? collection.actions : []
            delegate: Loader {
                id: actionLoader
                property var more: modelData
                width: parent ? parent.width : 0
                sourceComponent: actionRow
                onLoaded: item.cardItem = actionLoader
            }
        }
    }

    // Cards and Tiles: the action rows in a card of their own under them
    Section {
        visible: collection.view !== "list" && collection.actions.length > 0
        Repeater {
            model: collection.view !== "list" ? collection.actions : []
            delegate: Loader {
                id: moreLoader
                property var more: modelData
                width: parent ? parent.width : 0
                sourceComponent: actionRow
                onLoaded: item.cardItem = moreLoader
            }
        }
    }

    Text {
        Layout.fillWidth: true
        visible: collection.footer !== "" && collection.items.length > 0
        text: collection.footer
        textFormat: Text.PlainText
        color: Theme.secondaryText
        font.pixelSize: Theme.detailSize
        wrapMode: Text.WordWrap
        leftPadding: Theme.cardPadding
        rightPadding: Theme.cardPadding
        topPadding: 6 * Theme.unit
    }
}
