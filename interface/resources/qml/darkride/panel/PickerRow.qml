//
//  PickerRow.qml
//  qml/darkride/panel
//
//  iOS's choice in a list: a row showing the chosen value; a click opens the choices under it as rows, the chosen one
//  ticked, and picking one closes them. The open choices are indented past the label, and a full-width separator
//  closes them off, so the card's next row doesn't read as another choice. model: [{ text, value, swatches }]
//  (swatches: colours shown beside the name, for a theme); current: the chosen value; chosen(value).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.15
import QtQuick.Layouts 1.15
import "."

Column {
    id: picker

    property string label: ""
    property var model: []
    property var current
    property bool open: false
    signal chosen(var value)
    // the card's next row draws its separator the whole width while the choices show (ListRow.separatorX)
    readonly property bool fullWidthSeparatorBelow: open && model.length > 0

    readonly property string currentText: {
        for (var i = 0; i < model.length; i++) {
            if (model[i].value === current) {
                return model[i].text;
            }
        }
        return "";
    }

    width: parent ? parent.width : 0

    ListRow {
        label: picker.label
        cardItem: picker
        clickable: true
        onClicked: picker.open = !picker.open

        Text {
            text: picker.currentText
            textFormat: Text.PlainText
            color: Theme.secondaryText
            font.pixelSize: Theme.bodySize
        }
        Text {
            text: picker.open ? "⌃" : "⌄"   // ⌃ ⌄
            color: Theme.tertiaryText
            font.pixelSize: Theme.bodySize
        }
    }

    Repeater {
        model: picker.open ? picker.model : []
        delegate: ListRow {
            id: choiceRow
            label: modelData.text
            indent: Theme.choiceIndent
            clickable: true
            onClicked: {
                picker.chosen(modelData.value);
                picker.open = false;
            }

            Row {
                spacing: 4 * Theme.unit
                visible: !!modelData.swatches
                Repeater {
                    model: modelData.swatches || []
                    delegate: Rectangle {
                        width: 14 * Theme.unit
                        height: width
                        radius: width / 2
                        color: modelData
                        border.color: Theme.separator
                    }
                }
            }
            Text {
                text: "✓"   // ✓
                color: Theme.tint
                font.pixelSize: Theme.bodySize
                font.bold: true
                opacity: modelData.value === picker.current ? 1 : 0
            }
        }
    }
}
