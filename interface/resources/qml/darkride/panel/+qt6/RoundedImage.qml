//
//  RoundedImage.qml
//  qml/darkride/panel
//
//  A picture cropped to fill, with rounded corners (a radius of half the width makes a circle: a person's picture);
//  roundBottom false keeps the bottom corners square, for a picture across the top of a card. While it loads, or when
//  there is none, a plain fill with the placeholder's first letter.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.15
import QtQuick.Window 2.15
import Qt5Compat.GraphicalEffects   // Qt 6: QtGraphicalEffects lives in the Qt5Compat module (the Qt 5 file is beside this +qt6 folder)
import "qrc:///qml/darkride/panel"

Item {
    id: picture

    property url source: ""
    property real radius: Theme.cardRadius
    property bool roundBottom: true
    property string placeholder: ""
    property int fillMode: Image.PreserveAspectCrop
    readonly property bool ready: image.status === Image.Ready

    Item {
        id: content
        anchors.fill: parent
        layer.enabled: picture.radius > 0
        layer.effect: OpacityMask {
            maskSource: Item {
                width: content.width
                height: content.height
                Rectangle {
                    width: parent.width
                    height: parent.height + (picture.roundBottom ? 0 : picture.radius)
                    radius: picture.radius
                }
            }
        }

        Rectangle {
            anchors.fill: parent
            visible: !picture.ready && (picture.placeholder !== "" || picture.source.toString() !== "")
            color: Theme.fill
            Text {
                anchors.centerIn: parent
                text: picture.placeholder.charAt(0).toUpperCase()
                color: Theme.secondaryText
                font.pixelSize: Math.min(parent.width, parent.height) * 0.42
                font.bold: true
            }
        }

        Image {
            id: image
            anchors.fill: parent
            fillMode: picture.fillMode
            asynchronous: true
            // decoded at about the size it's drawn, not the file's (a hundred avatars' pictures at full size cost hundreds
            // of MB, avatars 11-08; a full-size upload was a ~24 ms draw in the headset, vr 11-01), and only once it has a
            // size (a size of 0 means the picture's own). At least twice the layout size: that covers the VR screen, which
            // draws a page about 1.5x its layout (ScreenSlot.qml), as well as Retina. In steps of 64 px, so a window being
            // resized doesn't decode it again at every pixel
            readonly property real decodeScale: Math.max(2, Screen.devicePixelRatio)
            sourceSize: Qt.size(Math.ceil(width * decodeScale / 64) * 64, Math.ceil(height * decodeScale / 64) * 64)
            source: width > 0 && height > 0 ? picture.source : ""
        }
    }
}
