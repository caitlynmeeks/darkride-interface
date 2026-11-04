//
//  Table.qml
//
//  Created by David Rowe on 18 Feb 2016
//  Copyright 2016 High Fidelity, Inc.
//  Rebuilt on a ListView, without Qt Quick Controls 1, by Darkride in 2026.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
//  A table of the rows of a ListModel (or a JS array), its TableViewColumn children naming the roles. It keeps the
//  surface the dialogs used on Controls 1's TableView: model, headerVisible, the sort indicator (sortIndicatorVisible,
//  sortIndicatorColumn, sortIndicatorOrder; a header click changes them), currentRow, selection (select, deselect,
//  clear, selectAll, contains, forEach, count, selectionChanged), clicked / doubleClicked / activated(row),
//  itemDelegate, rowDelegate and headerDelegate (each loaded with a `styleData` object and the row's `model`, as
//  Controls 1 gave them), contentItem / flickableItem (the ListView). Up and Down move currentRow; Return and Enter
//  activate it; in SingleSelection the selection follows currentRow.
//

import QtQuick 2.7
import QtQuick.Controls 2.3 as QQC2

import "../stylesUit"

FocusScope {
    id: tableView

    enum SelectionMode { NoSelection, SingleSelection, MultiSelection, ExtendedSelection }

    property var model: ListModel { }
    property int colorScheme: hifi.colorSchemes.light
    readonly property bool isLightColorScheme: colorScheme == hifi.colorSchemes.light
    property bool expandSelectedRow: false
    property bool centerHeaderText: false
    readonly property real headerSpacing: 3 // spacing between sort indicator and table header title

    property bool headerVisible: false
    property bool sortIndicatorVisible: false
    property int sortIndicatorColumn: 0
    property int sortIndicatorOrder: Qt.AscendingOrder
    property int currentRow: -1
    property int selectionMode: Table.SingleSelection
    readonly property int rowCount: listView.count
    property bool backgroundVisible: true
    property bool alternatingRowColors: true
    property bool frameVisible: false   // the border below is drawn either way, as before

    property Component itemDelegate: defaultItemDelegate
    property Component rowDelegate: defaultRowDelegate
    property Component headerDelegate: defaultHeaderDelegate

    // the TableViewColumn children, in order (gathered once the children exist)
    property var columns: []
    readonly property int columnCount: columns.length

    property alias flickableItem: listView
    property alias contentItem: listView
    property alias selection: selectionObject

    signal clicked(int row)
    signal doubleClicked(int row)
    signal activated(int row)

    activeFocusOnTab: true
    anchors.margins: 2  // the border below lies outside; the view inside it, as before
    implicitWidth: 240  // Controls 1's TableView was a ScrollView with this implicit size: a Column gives the
    implicitHeight: 150 // table this height when no one sets one

    HifiConstants { id: hifi }

    Component.onCompleted: gatherColumns()

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

    // the value a cell shows: the row's role for a ListModel, else from a plain value or object (modelData)
    function cellValue(rowModel, role) {
        if (!rowModel || role === undefined || role === "") {
            return "";
        }
        var value = rowModel[role];
        if (value !== undefined) {
            return value;
        }
        var data = rowModel.modelData;
        if (data === undefined || data === null) {
            return "";
        }
        if (typeof data === "object" && data[role] !== undefined) {
            return data[role];
        }
        return data;
    }

    function positionViewAtRow(row, mode) {
        listView.positionViewAtIndex(row, mode === undefined ? ListView.Contain : mode);
    }

    function rowAt(x, y) {
        var p = tableView.mapToItem(listView, x, y);
        return listView.indexAt(p.x + listView.contentX, p.y + listView.contentY);
    }

    onCurrentRowChanged: {
        if (selectionMode === Table.SingleSelection) {
            selectionObject.setOnly(currentRow);
        }
        if (currentRow >= 0 && currentRow < listView.count) {
            listView.positionViewAtIndex(currentRow, ListView.Contain);
        }
    }

    Keys.onUpPressed: {
        if (listView.count > 0) {
            currentRow = currentRow < 0 ? 0 : Math.max(0, currentRow - 1);
        }
    }
    Keys.onDownPressed: {
        if (listView.count > 0) {
            currentRow = Math.min(listView.count - 1, currentRow + 1);
        }
    }
    Keys.onReturnPressed: {
        if (currentRow >= 0) {
            activated(currentRow);
        }
    }
    Keys.onEnterPressed: {
        if (currentRow >= 0) {
            activated(currentRow);
        }
    }

    // the selected rows, the way Controls 1's TableView.selection gave them
    QtObject {
        id: selectionObject
        property var rows: ({})     // row -> true
        property int count: 0
        property int revision: 0    // bumps on every change; bindings read it to follow the selection
        signal selectionChanged()

        function select(from, to) {
            if (to === undefined) {
                to = from;
            }
            var changed = false;
            for (var r = Math.min(from, to); r <= Math.max(from, to); r++) {
                if (r >= 0 && r < listView.count && rows[r] !== true) {
                    rows[r] = true;
                    changed = true;
                }
            }
            if (changed) {
                touched();
            }
        }
        function deselect(from, to) {
            if (to === undefined) {
                to = from;
            }
            var changed = false;
            for (var r = Math.min(from, to); r <= Math.max(from, to); r++) {
                if (rows[r] === true) {
                    delete rows[r];
                    changed = true;
                }
            }
            if (changed) {
                touched();
            }
        }
        function clear() {
            if (count > 0) {
                rows = {};
                touched();
            }
        }
        function selectAll() {
            select(0, listView.count - 1);
        }
        function contains(row) {
            return rows[row] === true;
        }
        function forEach(callback) {
            var keys = Object.keys(rows).map(Number).sort(function (a, b) { return a - b; });
            for (var i = 0; i < keys.length; i++) {
                callback(keys[i]);
            }
        }
        function toggle(row) {
            if (contains(row)) {
                deselect(row);
            } else {
                select(row);
            }
        }
        // exactly this row (none when row < 0)
        function setOnly(row) {
            if (row >= 0 && count === 1 && rows[row] === true) {
                return;
            }
            rows = {};
            if (row >= 0 && row < listView.count) {
                rows[row] = true;
            }
            touched();
        }
        // rows that left the model are no longer selected
        function prune() {
            var changed = false;
            for (var key in rows) {
                if (Number(key) >= listView.count) {
                    delete rows[key];
                    changed = true;
                }
            }
            if (changed) {
                touched();
            }
        }
        function touched() {
            count = Object.keys(rows).length;
            revision++;
            selectionChanged();
        }
    }

    Rectangle {
        // the border, around the view
        color: "#00000000"
        anchors { fill: parent; margins: -2 }
        border.color: isLightColorScheme ? hifi.colors.lightGrayText : hifi.colors.baseGrayHighlight
        border.width: 2
    }

    Row {
        id: header
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: tableView.headerVisible ? hifi.dimensions.tableHeaderHeight : 0
        visible: tableView.headerVisible
        clip: true

        Repeater {
            model: tableView.columns

            Loader {
                readonly property var col: modelData
                readonly property int columnIndex: index
                width: col.visible ? (tableView.columnCount === 1 ? listView.width : col.width) : 0  // one column fills the view, as Controls 1's did
                height: header.height
                visible: col.visible
                sourceComponent: tableView.headerDelegate

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
                        if (tableView.sortIndicatorColumn === columnIndex) {
                            tableView.sortIndicatorOrder = tableView.sortIndicatorOrder === Qt.AscendingOrder
                                ? Qt.DescendingOrder : Qt.AscendingOrder;
                        } else {
                            tableView.sortIndicatorColumn = columnIndex;
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        // rows keep their colour below the last entry, as before
        anchors.fill: listView
        visible: tableView.backgroundVisible
        color: isLightColorScheme ? hifi.colors.tableBackgroundLight : hifi.colors.tableBackgroundDark
    }

    ListView {
        id: listView
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: parent.bottom }
        clip: true
        model: tableView.model
        keyNavigationEnabled: false     // the Keys handlers above move currentRow
        boundsBehavior: Flickable.StopAtBounds
        highlightFollowsCurrentItem: false
        delegate: rowComponent
        onCountChanged: {
            selectionObject.prune();
            if (tableView.currentRow >= count) {
                tableView.currentRow = -1;
            }
        }
        QQC2.ScrollBar.vertical: scrollbar
    }

    QQC2.ScrollBar {
        id: scrollbar
        policy: QQC2.ScrollBar.AsNeeded
        orientation: Qt.Vertical
        visible: size < 1.0
        anchors.top: listView.top
        anchors.left: tableView.right
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
            readonly property var rowModel: model
            readonly property bool selected: (selectionObject.revision, selectionObject.contains(index))
            readonly property bool alternate: tableView.alternatingRowColors && index % 2 === 1
            readonly property bool pressed: rowMouse.pressed

            Loader {
                id: rowBackground
                width: parent.width
                sourceComponent: tableView.rowDelegate

                property QtObject styleData: QtObject {
                    readonly property int row: rowItem.row
                    readonly property bool selected: rowItem.selected
                    readonly property bool alternate: rowItem.alternate
                    readonly property bool pressed: rowItem.pressed
                    readonly property bool hasActiveFocus: tableView.activeFocus
                }
                property var model: rowItem.rowModel
            }

            MouseArea {
                // under the cells, so a cell's own buttons get their clicks first
                id: rowMouse
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                onPressed: {
                    tableView.forceActiveFocus();
                    var row = rowItem.row;
                    if (tableView.selectionMode === Table.ExtendedSelection && (mouse.modifiers & Qt.ControlModifier)) {
                        selectionObject.toggle(row);
                    } else if (tableView.selectionMode === Table.ExtendedSelection && (mouse.modifiers & Qt.ShiftModifier)
                               && tableView.currentRow >= 0) {
                        selectionObject.clear();
                        selectionObject.select(tableView.currentRow, row);
                    } else if (tableView.selectionMode !== Table.NoSelection) {
                        selectionObject.setOnly(row);
                    }
                    tableView.currentRow = row;
                }
                onClicked: tableView.clicked(rowItem.row)
                onDoubleClicked: {
                    tableView.doubleClicked(rowItem.row);
                    tableView.activated(rowItem.row);
                }
            }

            Row {
                anchors.fill: parent

                Repeater {
                    model: tableView.columns

                    Loader {
                        readonly property var col: modelData
                        readonly property int columnIndex: index
                        width: col.visible ? (tableView.columnCount === 1 ? listView.width : col.width) : 0
                        height: rowItem.height
                        visible: col.visible
                        clip: true
                        sourceComponent: col.delegate ? col.delegate : tableView.itemDelegate

                        property QtObject styleData: QtObject {
                            readonly property int row: rowItem.row
                            readonly property int column: columnIndex
                            readonly property var value: tableView.cellValue(rowItem.rowModel, col.role)
                            readonly property bool selected: rowItem.selected
                            readonly property bool alternate: rowItem.alternate
                            readonly property int elideMode: col.elideMode
                            readonly property int textAlignment: col.horizontalAlignment
                            readonly property bool pressed: rowItem.pressed
                            readonly property bool hasActiveFocus: tableView.activeFocus
                        }
                        property var model: rowItem.rowModel
                    }
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
                x: tableView.centerHeaderText ? (parent.width - paintedWidth -
                                      ((tableView.sortIndicatorVisible &&
                                        tableView.sortIndicatorColumn === styleData.column) ?
                                           (titleSort.paintedWidth / 5 + tableView.headerSpacing) : 0)) / 2 :
                                      hifi.dimensions.tablePadding
                text: styleData.value
                size: hifi.fontSizes.tableHeading
                font.capitalization: Font.AllUppercase
                color: hifi.colors.baseGrayHighlight
                horizontalAlignment: (tableView.centerHeaderText ? Text.AlignHCenter : Text.AlignLeft)
                anchors.verticalCenter: parent.verticalCenter
            }

            // actual image of sort indicator in glyph font only 20% of real font size
            // i.e. if the character size set to 60 pixels, actual image is 12 pixels
            HiFiGlyphs {
                id: titleSort
                text: tableView.sortIndicatorOrder == Qt.AscendingOrder ? hifi.glyphs.caratUp : hifi.glyphs.caratDn
                color: hifi.colors.darkGray
                opacity: 0.6;
                size: hifi.fontSizes.tableHeadingIcon
                anchors.verticalCenter: titleText.verticalCenter
                anchors.left: titleText.right
                anchors.leftMargin: -(hifi.fontSizes.tableHeadingIcon / 2.5) + tableView.headerSpacing
                visible: tableView.sortIndicatorVisible && tableView.sortIndicatorColumn === styleData.column
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
        id: defaultRowDelegate

        Rectangle {
            height: (styleData.selected && tableView.expandSelectedRow ? 1.8 : 1) * hifi.dimensions.tableRowHeight
            color: styleData.selected
                   ? hifi.colors.primaryHighlight
                   : tableView.isLightColorScheme
                     ? (styleData.alternate ? hifi.colors.tableRowLightEven : hifi.colors.tableRowLightOdd)
                     : (styleData.alternate ? hifi.colors.tableRowDarkEven : hifi.colors.tableRowDarkOdd)
        }
    }

    Component {
        id: defaultItemDelegate

        Item {
            clip: true
            RobotoMedium {
                anchors {
                    left: parent.left
                    leftMargin: hifi.dimensions.tablePadding
                    right: parent.right
                    rightMargin: hifi.dimensions.tablePadding
                    verticalCenter: parent.verticalCenter
                }
                text: styleData.value !== undefined ? styleData.value : ""
                size: hifi.fontSizes.tableText
                elide: styleData.elideMode
                horizontalAlignment: styleData.textAlignment
                color: tableView.isLightColorScheme
                       ? (styleData.selected ? hifi.colors.black : hifi.colors.baseGrayHighlight)
                       : (styleData.selected ? hifi.colors.black : hifi.colors.lightGrayText)
            }
        }
    }
}
