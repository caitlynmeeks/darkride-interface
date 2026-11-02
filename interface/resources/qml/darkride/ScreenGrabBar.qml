//
//  ScreenGrabBar.qml
//  interface/resources/qml/darkride
//
//  The VR screen's grab bar, under the middle panel, as under a visionOS window (ui/VrScreen.cpp moves the screen): a
//  capsule only, no box around it (Caity, 11-02), brighter while a laser points at it (VrScreen sets hovered). The bar's
//  whole area still takes the laser, so it's no smaller a target than the capsule looks.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

import QtQuick 2.15
import "panel"

Item {
    property bool hovered: false

    Rectangle {
        anchors.centerIn: parent
        width: parent.width * 0.6
        height: parent.height * 0.3
        radius: height / 2
        color: Theme.text
        opacity: parent.hovered ? 1.0 : 0.6
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }
}
