//
//  TextField.qml
//
//  Created by David Rowe on 17 Feb 2016
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

import QtQuick 2.12
import QtQuick.Controls 2.12 as QQC2

import "../stylesUit"
import "." as HifiControls

// On Qt Quick Controls 2 (Qt 6 has no Controls 1); the look, the size and the properties are Controls 1's.
QQC2.TextField {
    id: textField

    property int colorScheme: hifi.colorSchemes.light
    readonly property bool isLightColorScheme: colorScheme == hifi.colorSchemes.light
    readonly property bool isFaintGrayColorScheme: colorScheme == hifi.colorSchemes.faintGray
    property bool isSearchField: false
    property string label: ""
    property real controlHeight: height + (textFieldLabel.visible ? textFieldLabel.height + 1 : 0)
    property bool hasDefocusedBorder: true;
    property bool hasRoundedBorder: false
    property int roundedBorderRadius: 4
    property bool error: false;
    property bool hasClearButton: false;
    property string leftPermanentGlyph: "";
    property string centerPlaceholderGlyph: "";
    property int styleRenderType: Text.NativeRendering

    font.family: "Roboto"
    font.pixelSize: hifi.fontSizes.textFieldInput
    // Controls 1's sizes: a box 1.2 lines high (25 at least), 8 lines wide, the text 4 px from the top and bottom
    implicitWidth: Math.round(contentHeight * 8)
    implicitHeight: Math.max(25, Math.round(contentHeight * 1.2))
    height: implicitHeight + 3  // Make surrounding box higher so that highlight is vertically centered.
    topPadding: 4
    bottomPadding: 4
    leftPadding: hasRoundedBorder ? textField.height / 2 : ((isSearchField || textField.leftPermanentGlyph !== "") ? textField.height - 2 : 0) + hifi.dimensions.textPadding
    rightPadding: (hasClearButton ? textField.height - 2 : 0) + hifi.dimensions.textPadding
    verticalAlignment: TextInput.AlignVCenter
    selectByMouse: true  // Controls 1's default
    renderType: textField.styleRenderType
    property alias textFieldLabel: textFieldLabel

    color: {
        if (isLightColorScheme) {
            if (textField.activeFocus) {
                hifi.colors.black
            } else {
                hifi.colors.lightGray
            }
        } else if (isFaintGrayColorScheme) {
            if (textField.activeFocus) {
                hifi.colors.black
            } else {
                hifi.colors.lightGray
            }
        } else {
            if (textField.activeFocus) {
                hifi.colors.white
            } else {
                hifi.colors.lightGrayText
            }
        }
    }
    placeholderTextColor: isFaintGrayColorScheme ? hifi.colors.lightGrayText : hifi.colors.lightGray
    selectedTextColor: hifi.colors.black
    selectionColor: hifi.colors.primaryHighlight

    y: textFieldLabel.visible ? textFieldLabel.height + textFieldLabel.anchors.bottomMargin : 0

    // Return and Enter end here: the field takes the key and says accepted() itself (QTBUG-49297 on Controls 1)
    Keys.onPressed: function (event) {
        switch (event.key) {
            case Qt.Key_Return:
            case Qt.Key_Enter:
                event.accepted = true;

                // emit accepted signal manually
                if (acceptableInput) {
                    accepted();
                }
        }
    }

    background: Rectangle {
        color: {
            if (isLightColorScheme) {
                if (textField.activeFocus) {
                    hifi.colors.white
                } else {
                    hifi.colors.textFieldLightBackground
                }
            } else if (isFaintGrayColorScheme) {
                if (textField.activeFocus) {
                    hifi.colors.white
                } else {
                    hifi.colors.faintGray50
                }
            } else {
                if (textField.activeFocus) {
                    hifi.colors.black
                } else {
                    hifi.colors.baseGrayShadow
                }
            }
        }
        border.color: textField.error ? hifi.colors.redHighlight :
        (textField.activeFocus ? hifi.colors.primaryHighlight : (hasDefocusedBorder ? (isFaintGrayColorScheme ? hifi.colors.lightGrayText : hifi.colors.lightGray) : color))
        border.width: textField.activeFocus || hasRoundedBorder || textField.error ? 1 : 0
        radius: isSearchField ? textField.height / 2 : (hasRoundedBorder ? roundedBorderRadius : 0)

        HiFiGlyphs {
            text: textField.leftPermanentGlyph;
            color: textField.color;
            size: hifi.fontSizes.textFieldSearchIcon;
            anchors.left: parent.left;
            anchors.verticalCenter: parent.verticalCenter;
            anchors.leftMargin: hifi.dimensions.textPadding - 2;
            visible: text;
        }

        HiFiGlyphs {
            text: textField.centerPlaceholderGlyph;
            color: textField.color;
            size: parent.height;
            anchors.horizontalCenter: parent.horizontalCenter;
            anchors.verticalCenter: parent.verticalCenter;
            visible: text && !textField.focus && textField.text === "";
        }

        HiFiGlyphs {
            text: hifi.glyphs.search
            color: textField.color
            size: hifi.fontSizes.textFieldSearchIcon
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: hifi.dimensions.textPadding - 2
            visible: isSearchField
        }
    }

    // The clear button sits above the text (the field covers its padding on Controls 2, so a button in the
    // background would never get the click)
    HiFiGlyphs {
        z: 1
        text: hifi.glyphs.error
        color: textField.color
        size: 40
        anchors.right: parent.right
        anchors.rightMargin: hifi.dimensions.textPadding - 2
        anchors.verticalCenter: parent.verticalCenter
        visible: hasClearButton && textField.text !== "";

        MouseArea {
            anchors.fill: parent;
            onClicked: {
                textField.text = "";
            }
        }
    }

    HifiControls.Label {
        id: textFieldLabel
        text: textField.label
        colorScheme: textField.colorScheme
        anchors.left: parent.left

        Binding on anchors.right {
            when: textField.right
            value: textField.right
        }
        Binding on wrapMode {
            when: textField.right
            value: Text.WordWrap
        }

        anchors.bottom: parent.top
        anchors.bottomMargin: 3
        visible: label != ""
    }
}
