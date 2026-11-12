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
            // size (a size of 0 means the picture's own): its layout size times the screen's pixel ratio times the scale
            // of the items it sits in. That scale is 1 on the desktop, so a 1x screen decodes at 1x; the VR screen draws a
            // page about 1.5x its layout (ScreenSlot.qml), and only there do its pictures decode bigger (performance,
            // 11-10). Read in halves, so a card's small hover scale doesn't decode it again; in steps of 32 px, so a window
            // being resized doesn't either. The binding reruns whenever an ancestor's scale changes: nothing animates a
            // page's scale today (the VR screen sets it once); one that eases a page full of pictures should read the scale
            // once its animation ends instead (performance, 11-10)
            readonly property real drawnScale: {
                var total = 1;
                for (var item = picture; item; item = item.parent) {
                    total *= item.scale;
                }
                return Math.max(1, Math.round(total * 2) / 2);
            }
            readonly property real decodeScale: Screen.devicePixelRatio * drawnScale
            sourceSize: Qt.size(Math.ceil(width * decodeScale / 32) * 32, Math.ceil(height * decodeScale / 32) * 32)
            source: width > 0 && height > 0 ? picture.source : ""
        }
    }
}
