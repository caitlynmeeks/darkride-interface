//
//  VrMenuEntry.qml
//  qml/desktop
//
//  One entry of the VR menu's tree (a copy of the app's menu bar for the headset, filled by VrMenu.cpp): a submenu,
//  an item or a separator. It has the parts of Qt Quick Controls 1's Menu, MenuItem and MenuSeparator that VrMenu.cpp
//  and the tablet's menu pages use (Qt 6 has no Controls 1), with the same type numbers. An item's checked state comes
//  from its QAction: trigger() only says triggered(), and the action toggles itself and sets checked here.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQml 2.12

QtObject {
    id: entry

    enum Type { Separator, Item, Menu }

    property int type: VrMenuEntry.Menu
    property string title: ""        // a submenu's
    property string text: ""         // an item's (VrMenu.cpp sets a submenu's too, from its menu's action)
    property bool enabled: true
    property bool visible: true
    property bool checkable: false
    property bool checked: false
    property string shortcut: ""
    property var exclusiveGroup: null  // set (to the group's name) when the action is one of a set of choices
    property var items: []             // a submenu's entries, in order

    signal triggered()

    function trigger() {
        if (type === VrMenuEntry.Item && enabled) {
            triggered();
        }
    }

    // VrMenu.cpp's calls; the new entry is a child of this one (VrMenu.cpp finds entries by name among the children,
    // and an item's menu is its parent)
    function addMenu(title) { return add({ type: VrMenuEntry.Menu, title: title, text: title }); }
    function addItem(text) { return add({ type: VrMenuEntry.Item, text: text }); }
    function addSeparator() { return add({ type: VrMenuEntry.Separator }); }

    property Component maker: null
    function add(properties) {
        if (!maker) {
            maker = Qt.createComponent("VrMenuEntry.qml");
        }
        var made = maker.createObject(entry, properties);
        items = items.concat([made]);
        return made;
    }
}
