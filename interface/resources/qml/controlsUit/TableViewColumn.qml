//
//  TableViewColumn.qml
//  qml/controlsUit
//
//  One column of a Table or a Tree: which role of the model its cells show, its title in the header and its width.
//  The same shape as Qt Quick Controls 1's TableViewColumn, so the dialogs' columns read the same once their
//  Controls 1 import goes. `resizable` and `movable` are accepted and ignored: nobody drags these columns.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.7

QtObject {
    readonly property bool isTableViewColumn: true

    // the role the cells read: a ListModel role name (Table); a Qt role name ("display") or number (Tree)
    property var role: ""
    property string title: ""
    property real width: 160
    property bool visible: true
    property bool resizable: true
    property bool movable: true
    property int horizontalAlignment: Text.AlignLeft
    property int elideMode: Text.ElideRight
    // this column's own cell delegate; the view's itemDelegate when not set
    property Component delegate: null
}
