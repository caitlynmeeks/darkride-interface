//
//  Tree.qml
//
//  Created by David Rowe on 17 Feb 2016
//  Copyright 2016 High Fidelity, Inc.
//  Rebuilt on a ListView, without Qt Quick Controls 1, by Darkride in 2026.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
//  A tree of a QAbstractItemModel (treeModel), shown as a flat list of its visible rows: the top level, and the
//  children of every expanded row, indented by depth. The flat list is rebuilt from the model (rowCount, index,
//  hasChildren) on every expand and collapse and on the model's own changes; a cell reads its value with
//  treeModel.data(index, role). It keeps the surface the dialogs used on Controls 1's TreeView: selection (an
//  ItemSelectionModel: currentIndex, selectedIndexes, hasSelection), selectionMode, headerVisible, the sort indicator,
//  isExpanded / expand / collapse(index), indexAt(x, y), clicked / doubleClicked / activated(index), expanded /
//  collapsed(index), itemDelegate, rowDelegate, headerDelegate and branchDelegate (each loaded with a `styleData`
//  object: row, column, value, selected, alternate, depth, isExpanded, hasChildren, index), contentItem /
//  flickableItem (the ListView). A column's role is a Qt role name ("display") or a role number.
//

import QtQml.Models 2.2
import QtQuick 2.7
import QtQuick.Controls 2.3 as QQC2

import "../stylesUit"

FocusScope {
    id: treeView

    enum SelectionMode { NoSelection, SingleSelection, MultiSelection, ExtendedSelection }

    property var treeModel: null
    property int selectionMode: Tree.SingleSelection
    property bool centerHeaderText: false
    property int colorScheme: hifi.colorSchemes.light
    readonly property bool isLightColorScheme: colorScheme == hifi.colorSchemes.light
    readonly property real headerSpacing: 3
    property real indentation: hifi.dimensions.tablePadding

    property bool headerVisible: false
    property bool sortIndicatorVisible: false
    property int sortIndicatorColumn: 0
    property int sortIndicatorOrder: Qt.AscendingOrder
    property bool backgroundVisible: true
    property bool alternatingRowColors: true
    property bool frameVisible: false   // the border below is drawn either way, as before

    property var modifyEl: function(index, data) { return false; }

    // a column's role by name; a number is taken as it is
    property var roleNames: ({
        "display": Qt.DisplayRole, "decoration": Qt.DecorationRole, "edit": Qt.EditRole,
        "toolTip": Qt.ToolTipRole, "statusTip": Qt.StatusTipRole, "whatsThis": Qt.WhatsThisRole
    })

    property Component itemDelegate: defaultItemDelegate
    property Component rowDelegate: defaultRowDelegate
    property Component headerDelegate: defaultHeaderDelegate
    property Component branchDelegate: defaultBranchDelegate

    // the TableViewColumn children, in order (gathered once the children exist)
    property var columns: []
    readonly property int columnCount: columns.length
    readonly property int rowCount: flatRows.count

    property alias flickableItem: listView
    property alias contentItem: listView
    property alias selection: selectionModel

    // bump: the rows' indexes or values changed (cells re-read them); the selection changed
    property int dataRevision: 0
    property int selectionRevision: 0

    signal clicked(var index)
    signal doubleClicked(var index)
    signal activated(var index)
    signal expanded(var index)
    signal collapsed(var index)

    activeFocusOnTab: true
    anchors.margins: 2  // the border below lies outside; the view inside it, as before

    HifiConstants { id: hifi }

    ItemSelectionModel {
        id: selectionModel
        model: treeView.treeModel
        onSelectionChanged: treeView.selectionRevision++
    }

    // the visible rows, in order; d.indexes holds each row's QModelIndex (kept in step with this model)
    ListModel {
        id: flatRows
    }

    QtObject {
        id: d
        property var indexes: []
        property var expandedKeys: ({})     // key (the row's names from the root) -> true

        function roleNumber(role) {
            if (typeof role === "number") {
                return role;
            }
            var number = treeView.roleNames[role];
            return number === undefined ? Qt.DisplayRole : number;
        }

        function sameIndex(a, b) {
            return a !== undefined && b !== undefined && a !== null && b !== null
                && a.row === b.row && a.column === b.column && a.internalId === b.internalId;
        }

        // the visible rows under parentIndex (undefined = the root), in order
        function build(parentIndex, parentKey, depth, out) {
            var model = treeView.treeModel;
            var count = parentIndex === undefined ? model.rowCount() : model.rowCount(parentIndex);
            var seen = {};
            for (var r = 0; r < count; r++) {
                var index = parentIndex === undefined ? model.index(r, 0) : model.index(r, 0, parentIndex);
                var key = parentKey + "/" + String(model.data(index, Qt.DisplayRole));
                if (seen[key] !== undefined) {
                    seen[key]++;
                    key += "#" + seen[key];
                } else {
                    seen[key] = 0;
                }
                var children = model.hasChildren(index);
                var open = children && expandedKeys[key] === true;
                out.push({ key: key, depth: depth, hasChildren: children, expanded: open, index: index });
                if (open) {
                    build(index, key, depth + 1, out);
                }
            }
        }

        // brings flatRows and indexes to the model's current shape, touching only the rows that changed
        // (indexes first, then flatRows: a row's delegate re-reads its index as soon as its position changes)
        function sync() {
            var fresh = [];
            if (treeView.treeModel) {
                build(undefined, "", 0, fresh);
            }
            var i = 0;
            while (i < fresh.length) {
                var f = fresh[i];
                if (i < flatRows.count) {
                    var current = flatRows.get(i);
                    if (current.key === f.key) {
                        indexes[i] = f.index;
                        if (current.depth !== f.depth || current.hasChildren !== f.hasChildren
                                || current.expanded !== f.expanded) {
                            flatRows.set(i, { depth: f.depth, hasChildren: f.hasChildren, expanded: f.expanded });
                        }
                        i++;
                        continue;
                    }
                    var j = i + 1;
                    while (j < flatRows.count && flatRows.get(j).key !== f.key) {
                        j++;
                    }
                    if (j < flatRows.count) {
                        indexes.splice(i, j - i);
                        flatRows.remove(i, j - i);
                        continue;
                    }
                }
                indexes.splice(i, 0, f.index);
                flatRows.insert(i, { key: f.key, depth: f.depth, hasChildren: f.hasChildren, expanded: f.expanded });
                i++;
            }
            if (flatRows.count > fresh.length) {
                indexes.length = fresh.length;
                flatRows.remove(fresh.length, flatRows.count - fresh.length);
            }
            treeView.dataRevision++;
        }
    }

    Connections {
        target: treeView.treeModel
        ignoreUnknownSignals: true
        function onModelReset() { d.sync(); }
        function onRowsInserted() { d.sync(); }
        function onRowsRemoved() { d.sync(); }
        function onRowsMoved() { d.sync(); }
        function onLayoutChanged() { d.sync(); }
        function onDataChanged() { treeView.dataRevision++; }
    }

    onTreeModelChanged: {
        d.expandedKeys = {};
        d.sync();
    }
    Component.onCompleted: {
        gatherColumns();
        d.sync();
    }

    // ours, or Controls 1's TableViewColumn (the same properties) from a file not yet off Controls 1
    function gatherColumns() {
        var found = [];
        for (var i = 0; i < data.length; i++) {
            var child = data[i];
            if (child && (child.isTableViewColumn === true
                          || (typeof child.role === "string" && child.title !== undefined && child.width !== undefined))) {
                found.push(child);
            }
        }
        if (found.length !== columns.length) {
            columns = found;
        }
    }

    function valueAt(row, role) {
        var index = d.indexes[row];
        if (!treeModel || index === undefined) {
            return "";
        }
        var value = treeModel.data(index, d.roleNumber(role));
        return value === undefined || value === null ? "" : value;
    }

    function rowOf(index) {
        for (var i = 0; i < d.indexes.length; i++) {
            if (d.sameIndex(d.indexes[i], index)) {
                return i;
            }
        }
        return -1;
    }

    function indexOfRow(row) {
        return d.indexes[row];
    }

    function isExpanded(index) {
        var row = rowOf(index);
        return row >= 0 && flatRows.get(row).expanded;
    }

    function expand(index) {
        var row = rowOf(index);
        if (row < 0) {
            return;
        }
        var info = flatRows.get(row);
        if (!info.hasChildren || info.expanded) {
            return;
        }
        d.expandedKeys[info.key] = true;
        d.sync();
        expanded(index);
    }

    function collapse(index) {
        var row = rowOf(index);
        if (row < 0 || !flatRows.get(row).expanded) {
            return;
        }
        delete d.expandedKeys[flatRows.get(row).key];
        d.sync();
        collapsed(index);
    }

    function toggleRow(row) {
        var index = d.indexes[row];
        if (index !== undefined) {
            if (isExpanded(index)) {
                collapse(index);
            } else {
                expand(index);
            }
        }
    }

    // the model index of the row under the point (in the tree's coordinates); an invalid index when none
    function indexAt(x, y) {
        var p = treeView.mapToItem(listView, x, y);
        var row = listView.indexAt(p.x + listView.contentX, p.y + listView.contentY);
        if (row >= 0 && row < d.indexes.length) {
            return d.indexes[row];
        }
        return treeModel ? treeModel.index(-1, -1) : undefined;
    }

    function setCurrentRow(row) {
        if (row >= 0 && row < d.indexes.length) {
            selectionModel.setCurrentIndex(d.indexes[row], ItemSelectionModel.ClearAndSelect);
            listView.positionViewAtIndex(row, ListView.Contain);
        }
    }

    function currentRow() {
        return rowOf(selectionModel.currentIndex);
    }

    onDoubleClicked: isExpanded(index) ? collapse(index) : expand(index)

    Keys.onUpPressed: {
        var row = currentRow();
        setCurrentRow(row < 0 ? 0 : Math.max(0, row - 1));
    }
    Keys.onDownPressed: {
        setCurrentRow(Math.min(flatRows.count - 1, currentRow() + 1));
    }
    Keys.onRightPressed: {
        var row = currentRow();
        if (row >= 0) {
            expand(d.indexes[row]);
        }
    }
    Keys.onLeftPressed: {
        var row = currentRow();
        if (row < 0) {
            return;
        }
        if (flatRows.get(row).expanded) {
            collapse(d.indexes[row]);
        } else {
            var parentRow = rowOf(d.indexes[row].parent);
            if (parentRow >= 0) {
                setCurrentRow(parentRow);
            }
        }
    }
    Keys.onReturnPressed: {
        if (selectionModel.currentIndex.valid) {
            activated(selectionModel.currentIndex);
        }
    }
    Keys.onEnterPressed: {
        if (selectionModel.currentIndex.valid) {
            activated(selectionModel.currentIndex);
        }
    }

    Rectangle {
        // the border, around the view
        color: "#00000000"
        anchors.fill: parent
        radius: hifi.dimensions.borderRadius
        border.color: isLightColorScheme ? hifi.colors.lightGrayText : hifi.colors.baseGrayHighlight
        border.width: 2
        anchors.margins: -2
    }

    Row {
        id: header
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: treeView.headerVisible ? hifi.dimensions.tableHeaderHeight : 0
        visible: treeView.headerVisible
        clip: true

        Repeater {
            model: treeView.columns

            Loader {
                readonly property var col: modelData
                readonly property int columnIndex: index
                width: col.visible ? (treeView.columnCount === 1 ? listView.width : col.width) : 0  // one column fills the view, as Controls 1's did
                height: header.height
                visible: col.visible
                sourceComponent: treeView.headerDelegate

                property QtObject styleData: QtObject {
                    readonly property string value: col.title
                    readonly property int column: columnIndex
                    readonly property int textAlignment: col.horizontalAlignment
                    readonly property bool pressed: headerMouse.pressed
                    readonly property bool containsMouse: headerMouse.containsMouse
                }

                MouseArea {
                    id: headerMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        if (treeView.sortIndicatorColumn === columnIndex) {
                            treeView.sortIndicatorOrder = treeView.sortIndicatorOrder === Qt.AscendingOrder
                                ? Qt.DescendingOrder : Qt.AscendingOrder;
                        } else {
                            treeView.sortIndicatorColumn = columnIndex;
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        // rows keep their colour below the last entry, as before
        anchors.fill: listView
        visible: treeView.backgroundVisible
        color: isLightColorScheme ? hifi.colors.tableRowLightEven : hifi.colors.tableRowDarkEven
    }

    ListView {
        id: listView
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: parent.bottom }
        clip: true
        model: flatRows
        keyNavigationEnabled: false     // the Keys handlers above move the current row
        boundsBehavior: Flickable.StopAtBounds
        highlightFollowsCurrentItem: false
        delegate: rowComponent
        QQC2.ScrollBar.vertical: scrollbar
    }

    QQC2.ScrollBar {
        id: scrollbar
        policy: QQC2.ScrollBar.AsNeeded
        orientation: Qt.Vertical
        visible: size < 1.0
        anchors.top: listView.top
        anchors.left: treeView.right
        anchors.bottom: listView.bottom

        background: Item {
            implicitWidth: hifi.dimensions.scrollbarBackgroundWidth
            Rectangle {
                anchors.fill: parent
                color: isLightColorScheme ? hifi.colors.tableScrollBackgroundLight
                                          : hifi.colors.tableScrollBackgroundDark
            }
        }

        contentItem: Item {
            implicitWidth: hifi.dimensions.scrollbarHandleWidth
            Rectangle {
                anchors.fill: parent
                radius: (width - 4)/2
                color: isLightColorScheme ? hifi.colors.tableScrollHandleLight : hifi.colors.tableScrollHandleDark
            }
        }
    }

    Component {
        id: rowComponent

        Item {
            id: rowItem
            width: listView.width
            height: rowBackground.item ? rowBackground.item.height : hifi.dimensions.tableRowHeight

            readonly property int row: index
            readonly property int depth: model.depth
            readonly property bool hasChildren: model.hasChildren
            readonly property bool expanded: model.expanded
            readonly property var modelIndex: (treeView.dataRevision, d.indexes[index])
            readonly property bool selected: (treeView.selectionRevision, treeView.dataRevision,
                                              modelIndex !== undefined && selectionModel.isSelected(modelIndex))
            readonly property bool alternate: treeView.alternatingRowColors && index % 2 === 1
            readonly property bool pressed: rowMouse.pressed

            Loader {
                id: rowBackground
                width: parent.width
                sourceComponent: treeView.rowDelegate

                property QtObject styleData: QtObject {
                    readonly property int row: rowItem.row
                    readonly property bool selected: rowItem.selected
                    readonly property bool alternate: rowItem.alternate
                    readonly property bool pressed: rowItem.pressed
                    readonly property bool hasActiveFocus: treeView.activeFocus
                    readonly property int depth: rowItem.depth
                    readonly property bool isExpanded: rowItem.expanded
                    readonly property bool hasChildren: rowItem.hasChildren
                    readonly property var index: rowItem.modelIndex
                }
            }

            MouseArea {
                // under the cells, so a cell's own controls get their clicks first
                id: rowMouse
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onPressed: {
                    treeView.forceActiveFocus();
                    var index = rowItem.modelIndex;
                    if (index === undefined || treeView.selectionMode === Tree.NoSelection) {
                        return;
                    }
                    if (treeView.selectionMode === Tree.ExtendedSelection && (mouse.modifiers & Qt.ControlModifier)) {
                        selectionModel.setCurrentIndex(index, ItemSelectionModel.Toggle);
                    } else if (treeView.selectionMode === Tree.ExtendedSelection && (mouse.modifiers & Qt.ShiftModifier)
                               && selectionModel.currentIndex.valid) {
                        var from = treeView.rowOf(selectionModel.currentIndex);
                        var to = rowItem.row;
                        for (var r = Math.min(from, to); r <= Math.max(from, to); r++) {
                            selectionModel.select(d.indexes[r], ItemSelectionModel.Select);
                        }
                        selectionModel.setCurrentIndex(index, ItemSelectionModel.Select);
                    } else {
                        selectionModel.setCurrentIndex(index, ItemSelectionModel.ClearAndSelect);
                    }
                }
                onClicked: treeView.clicked(rowItem.modelIndex)
                onDoubleClicked: {
                    treeView.doubleClicked(rowItem.modelIndex);
                    treeView.activated(rowItem.modelIndex);
                }
            }

            Row {
                anchors.fill: parent

                Repeater {
                    model: treeView.columns

                    Loader {
                        readonly property var col: modelData
                        readonly property int columnIndex: index
                        width: col.visible ? (treeView.columnCount === 1 ? listView.width : col.width) : 0
                        height: rowItem.height
                        visible: col.visible
                        clip: true
                        sourceComponent: col.delegate ? col.delegate : treeView.itemDelegate

                        property QtObject styleData: QtObject {
                            readonly property int row: rowItem.row
                            readonly property int column: columnIndex
                            readonly property var value: (treeView.dataRevision, treeView.valueAt(rowItem.row, col.role))
                            readonly property bool selected: rowItem.selected
                            readonly property bool alternate: rowItem.alternate
                            readonly property int depth: rowItem.depth
                            readonly property bool isExpanded: rowItem.expanded
                            readonly property bool hasChildren: rowItem.hasChildren
                            readonly property var index: rowItem.modelIndex
                            readonly property int elideMode: col.elideMode
                            readonly property int textAlignment: col.horizontalAlignment
                            readonly property bool pressed: rowItem.pressed
                            readonly property bool hasActiveFocus: treeView.activeFocus
                        }
                    }
                }
            }

            Loader {
                // the branch mark, in the indent before the first column's text; a click opens or closes the row
                id: branch
                x: rowItem.depth * treeView.indentation
                width: 2 * treeView.indentation
                height: rowItem.height
                visible: rowItem.hasChildren
                sourceComponent: treeView.branchDelegate

                property QtObject styleData: QtObject {
                    readonly property int row: rowItem.row
                    readonly property bool selected: rowItem.selected
                    readonly property int depth: rowItem.depth
                    readonly property bool isExpanded: rowItem.expanded
                    readonly property bool hasChildren: rowItem.hasChildren
                    readonly property var index: rowItem.modelIndex
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: treeView.toggleRow(rowItem.row)
                    onDoubleClicked: { }    // not the row's: one toggle per click
                }
            }
        }
    }

    Component {
        id: defaultHeaderDelegate

        Rectangle {
            height: hifi.dimensions.tableHeaderHeight
            color: isLightColorScheme ? hifi.colors.tableBackgroundLight : hifi.colors.tableBackgroundDark

            RobotoRegular {
                id: titleText
                text: styleData.value
                size: hifi.fontSizes.tableHeading
                font.capitalization: Font.AllUppercase
                color: hifi.colors.baseGrayHighlight
                horizontalAlignment: (treeView.centerHeaderText ? Text.AlignHCenter : Text.AlignLeft)
                elide: Text.ElideRight
                anchors {
                    left: parent.left
                    leftMargin: hifi.dimensions.tablePadding
                    right: treeView.sortIndicatorVisible && treeView.sortIndicatorColumn === styleData.column
                           ? titleSort.left : parent.right
                    rightMargin: hifi.dimensions.tablePadding
                    verticalCenter: parent.verticalCenter
                }
            }

            HiFiGlyphs {
                id: titleSort
                text: treeView.sortIndicatorOrder == Qt.AscendingOrder ? hifi.glyphs.caratUp : hifi.glyphs.caratDn
                color: isLightColorScheme ? hifi.colors.darkGray : hifi.colors.baseGrayHighlight
                opacity: 0.6;
                size: hifi.fontSizes.tableHeadingIcon
                anchors {
                    right: parent.right
                    verticalCenter: titleText.verticalCenter
                }
                visible: treeView.sortIndicatorVisible && treeView.sortIndicatorColumn === styleData.column
            }

            Rectangle {
                width: 1
                anchors {
                    left: parent.left
                    top: parent.top
                    topMargin: 1
                    bottom: parent.bottom
                    bottomMargin: 2
                }
                color: isLightColorScheme ? hifi.colors.lightGrayText : hifi.colors.baseGrayHighlight
                visible: styleData.column > 0
            }

            Rectangle {
                height: 1
                anchors {
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                }
                color: isLightColorScheme ? hifi.colors.lightGrayText : hifi.colors.baseGrayHighlight
            }
        }
    }

    Component {
        id: defaultBranchDelegate

        HiFiGlyphs {
            // centred in the indent slot (2 x indentation wide) before the first column's text
            anchors.fill: parent
            text: styleData.isExpanded ? hifi.glyphs.caratDn : hifi.glyphs.caratR
            size: hifi.fontSizes.carat
            horizontalAlignment: Text.AlignHCenter
            color: treeView.isLightColorScheme
                   ? (styleData.selected ? hifi.colors.black : hifi.colors.baseGrayHighlight)
                   : (styleData.selected ? hifi.colors.black : hifi.colors.lightGrayText)
        }
    }

    Component {
        id: defaultRowDelegate

        Rectangle {
            height: hifi.dimensions.tableRowHeight
            color: styleData.selected
                   ? hifi.colors.primaryHighlight
                   : treeView.isLightColorScheme
                     ? (styleData.alternate ? hifi.colors.tableRowLightEven : hifi.colors.tableRowLightOdd)
                     : (styleData.alternate ? hifi.colors.tableRowDarkEven : hifi.colors.tableRowDarkOdd)
        }
    }

    Component {
        id: defaultItemDelegate

        RobotoMedium {
            anchors {
                left: parent ? parent.left : undefined
                leftMargin: (2 + styleData.depth) * hifi.dimensions.tablePadding
                right: parent ? parent.right : undefined
                rightMargin: hifi.dimensions.tablePadding
                verticalCenter: parent ? parent.verticalCenter : undefined
            }

            text: styleData.value
            size: hifi.fontSizes.tableText
            color: treeView.isLightColorScheme
                   ? (styleData.selected ? hifi.colors.black : hifi.colors.baseGrayHighlight)
                   : (styleData.selected ? hifi.colors.black : hifi.colors.lightGrayText)

            elide: Text.ElideRight
        }
    }
}
