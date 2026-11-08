//
//  ListRow.qml
//  qml/darkride/panel
//
//  One row of a section: its label (and a small detail under it) on the left, its control on the right; when the two
//  don't fit side by side, the control goes under the label. A row with a number can be scrubbed: dragging its label
//  sideways changes the number, as the build tools' labels do (editor/ui/PropertyEditors.cpp ScrubLabel: one step per
//  pixel, Shift ten, Alt a tenth). An action row is a whole-row button: its label in blue, or red when it deletes.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.15
import QtQuick.Layouts 1.15
import "."

Item {
    id: row

    property string label: ""
    property string detail: ""
    property int detailFormat: Text.PlainText   // Text.StyledText for a detail with a link (Explore's "by <author>")
    signal linkActivated(string link)
    property url icon: ""          // a small picture left of the label (an avatar menu's control)
    property real iconSize: 26 * Theme.unit
    property real iconRadius: 0    // rounded (a thing's picture in a collection's list), or square as it comes
    property string iconPlaceholder: ""
    // the control fills the room beside the label (a slider, a text field) rather than sitting at the right
    property bool fill: false
    // how far past the card's text the row starts, its separator with it (a picker's open choice: Theme.choiceIndent)
    property real indent: 0
    // a fixed width for the label, so the controls of neighbouring rows line up (0: the words' own width)
    property real labelWidth: 0
    default property alias content: contentRow.data

    // scrubbing: the value it starts from, its bounds, the change per pixel; scrubbed() gives each new value
    property bool scrubbable: false
    property real scrubValue: 0
    property real scrubFrom: -1e9
    property real scrubTo: 1e9
    property real scrubStep: 1
    signal scrubbed(real value)

    // an action row: the label in blue (red when it deletes), the whole row a button; a clickable row is a button
    // with its label and value as they are (a picker's row)
    property bool action: false
    property bool destructive: false
    property bool clickable: action
    signal clicked()
    signal pressed()               // a clickable row's press and release, for a button held while pressed
    signal released()

    width: parent ? parent.width : 0
    // from the parts themselves, not the grid's own implicit height, which changes with the layout it feeds
    implicitHeight: {
        var labelHeight = labelColumn.visible ? labelColumn.implicitHeight : 0;
        var contentHeight = contentRow.visible ? contentRow.implicitHeight : 0;
        var inner = stacked ? labelHeight + grid.rowSpacing + contentHeight : Math.max(labelHeight, contentHeight);
        return Math.max(Theme.rowHeight, inner + 2 * Theme.rowPadding);
    }

    // the item that sits in the card's column: the row itself, or the picker whose first row it is
    property Item cardItem: row

    // a row in the card's column: visible and with a height (a Repeater is a child too, of none)
    function shown(item) {
        return item.visible && item.height > 0;
    }

    // the separator runs above every row but a card's first, inset to the text as iOS draws it
    readonly property bool firstShown: {
        var column = cardItem ? cardItem.parent : null;
        if (!column) {
            return true;
        }
        for (var i = 0; i < column.children.length; i++) {
            if (shown(column.children[i])) {
                return column.children[i] === cardItem;
            }
        }
        return true;
    }
    // where the separator above starts: inset to the text, or the whole width under an open picker, which closes its
    // indented choices off from the rows that follow (the item above says so with fullWidthSeparatorBelow)
    readonly property real separatorX: {
        var column = cardItem ? cardItem.parent : null;
        if (column) {
            var above = null;
            for (var i = 0; i < column.children.length && column.children[i] !== cardItem; i++) {
                if (shown(column.children[i])) {
                    above = column.children[i];
                }
            }
            if (above && above.fullWidthSeparatorBelow === true) {
                return 0;
            }
        }
        return Theme.cardPadding + indent;
    }
    readonly property bool stacked: !action && width > 0 && contentRow.visibleChildrenWidth > 0
        && Math.max(labelWidth, labelColumn.implicitWidth) + Theme.gap + contentRow.visibleChildrenWidth
           > width - 2 * Theme.cardPadding - indent

    // the last shown item of its column
    function lastOf(item) {
        var column = item ? item.parent : null;
        if (!column) {
            return true;
        }
        for (var i = column.children.length - 1; i >= 0; i--) {
            if (shown(column.children[i])) {
                return column.children[i] === item;
            }
        }
        return true;
    }
    // the card's last row, so its pressed shade rounds the card's bottom corners: a picker's own row is not while its
    // choices show under it, and a picker's choice (its column has fullWidthSeparatorBelow) only when the picker is last
    readonly property bool lastShown: {
        if (!lastOf(cardItem)) {
            return false;
        }
        if (cardItem !== row && cardItem.fullWidthSeparatorBelow === true) {
            return false;
        }
        var column = cardItem ? cardItem.parent : null;
        return !column || column.fullWidthSeparatorBelow === undefined || lastOf(column);
    }

    // pressed: the row darkens, rounded where it meets the card's corners
    Rectangle {
        anchors.fill: parent
        visible: row.clickable && actionArea.pressed
        color: Theme.cardPressed
        radius: row.firstShown || row.lastShown ? Theme.cardRadius : 0
        Rectangle {
            visible: parent.radius > 0 && !(row.firstShown && row.lastShown)
            color: parent.color
            width: parent.width
            height: parent.radius
            y: row.firstShown ? parent.height - height : 0
        }
    }

    Rectangle {
        visible: !row.firstShown
        x: row.separatorX
        width: parent.width - x
        height: 1
        color: Theme.separator
    }

    MouseArea {
        id: actionArea
        anchors.fill: parent
        enabled: row.clickable && row.enabled
        onClicked: row.clicked()
        onPressed: row.pressed()
        onReleased: row.released()
        onCanceled: row.released()
    }

    GridLayout {
        id: grid
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.cardPadding + row.indent
        anchors.rightMargin: Theme.cardPadding
        anchors.verticalCenter: parent.verticalCenter
        columns: row.stacked ? 1 : 2
        columnSpacing: Theme.gap
        rowSpacing: 8 * Theme.unit

        Row {
            id: labelColumn
            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
            Layout.preferredWidth: row.labelWidth > 0 && !row.stacked ? row.labelWidth : implicitWidth
            visible: row.label !== ""
            spacing: 10 * Theme.unit

            RoundedImage {
                visible: row.icon.toString() !== "" || row.iconPlaceholder !== ""
                anchors.verticalCenter: parent.verticalCenter
                width: row.iconSize
                height: width
                radius: row.iconRadius
                fillMode: row.iconRadius > 0 ? Image.PreserveAspectCrop : Image.PreserveAspectFit
                placeholder: row.iconPlaceholder
                source: row.icon
            }

            Column {
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    id: labelText
                    text: row.label
                    textFormat: Text.PlainText
                    color: row.action ? (row.destructive ? Theme.destructive : Theme.tint) : Theme.text
                    opacity: row.enabled ? 1 : 0.4
                    font.pixelSize: Theme.bodySize

                    MouseArea {
                        id: scrubArea
                        // a little wider and taller than the words, so the label is easy to catch
                        anchors.fill: parent
                        anchors.margins: -4 * Theme.unit
                        enabled: row.scrubbable && row.enabled
                        cursorShape: enabled ? Qt.SizeHorCursor : Qt.ArrowCursor
                        preventStealing: true

                        property real pressX: 0
                        property real lastX: 0
                        property real accumulated: 0
                        property bool scrubbing: false

                        onPressed: {
                            pressX = lastX = mouse.x;
                            scrubbing = false;
                        }
                        onPositionChanged: {
                            if (!scrubbing) {
                                if (Math.abs(mouse.x - pressX) < Theme.scrubSlop) {
                                    return;
                                }
                                scrubbing = true;
                                lastX = mouse.x;
                                accumulated = row.scrubValue;
                                return;
                            }
                            var step = row.scrubStep;
                            if (mouse.modifiers & Qt.ShiftModifier) {
                                step *= Theme.scrubCoarse;
                            } else if (mouse.modifiers & Qt.AltModifier) {
                                step *= Theme.scrubFine;
                            }
                            accumulated = Math.max(row.scrubFrom, Math.min(row.scrubTo, accumulated + (mouse.x - lastX) * step));
                            lastX = mouse.x;
                            row.scrubbed(accumulated);
                        }
                        onReleased: scrubbing = false
                    }
                }

                Text {
                    visible: row.detail !== ""
                    text: row.detail
                    textFormat: row.detailFormat
                    color: Theme.secondaryText
                    linkColor: Theme.tint
                    font.pixelSize: Theme.detailSize
                    onLinkActivated: row.linkActivated(link)

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.NoButton   // only the pointer: the clicks go to the link or the row
                        cursorShape: parent.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor
                    }
                }
            }
        }

        RowLayout {
            id: contentRow
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 8 * Theme.unit
            visible: !row.action

            // what the controls need side by side (the spacer below is not counted)
            readonly property real visibleChildrenWidth: {
                var total = 0;
                var shown = 0;
                for (var i = 0; i < children.length; i++) {
                    var child = children[i];
                    if (child !== spacer && child.visible) {
                        total += child.implicitWidth;
                        shown++;
                    }
                }
                return total + Math.max(0, shown - 1) * spacing;
            }

            // pushes the controls to the right, unless the row fills or they sit under the label
            Item {
                id: spacer
                Layout.fillWidth: !row.fill && !row.stacked
                visible: Layout.fillWidth
            }
        }
    }
}
