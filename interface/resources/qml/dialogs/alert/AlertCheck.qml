//
//  AlertCheck.qml
//  interface/resources/qml/dialogs/alert
//
//  Created on 2026-10-19 for Darkride's popups.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

import QtQuick 2.12
import QtQuick.Controls 2.12 as QQC2

// An alert's checkbox, at the left under its fields.
QQC2.CheckBox {
    id: check

    property real unit: 1
    property int elide: Text.ElideRight   // given a width, a long label is cut rather than running out of the alert
    readonly property AlertLook look: AlertLook {}

    padding: 0
    spacing: 6 * unit
    font.pixelSize: 12 * unit
    focusPolicy: Qt.TabFocus

    indicator: Rectangle {
        x: check.leftPadding
        y: (check.height - height) / 2
        width: 12 * check.unit
        height: 12 * check.unit
        radius: 3 * check.unit
        color: check.checked ? (check.enabled ? check.look.tint : check.look.button) : check.look.well
        border.width: check.checked ? 0 : 1
        border.color: check.look.checkBorder

        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: 5 * check.unit
            color: "transparent"
            border.width: 2
            border.color: check.look.focus
            visible: check.visualFocus
        }

        Canvas {
            anchors.fill: parent
            visible: check.checked
            onPaint: {
                var context = getContext("2d");
                context.reset();
                context.scale(width / 12, height / 12);
                context.strokeStyle = "#ffffff";
                context.lineWidth = 1.6;
                context.lineCap = "round";
                context.lineJoin = "round";
                context.beginPath();
                context.moveTo(3, 6.2);
                context.lineTo(5.2, 8.4);
                context.lineTo(9, 3.8);
                context.stroke();
            }
        }
    }

    contentItem: Text {
        leftPadding: check.indicator.width + check.spacing
        text: check.text
        textFormat: Text.PlainText
        elide: check.elide
        font: check.font
        color: check.enabled ? check.look.text : check.look.dimText
        verticalAlignment: Text.AlignVCenter
    }
}
