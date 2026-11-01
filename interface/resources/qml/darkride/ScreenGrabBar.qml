//
//  ScreenGrabBar.qml
//  interface/resources/qml/darkride
//
//  The VR screen's grab bar, under the middle panel, as under a visionOS window (ui/VrScreen.cpp moves the screen).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

import QtQuick 2.15
import "panel"

Rectangle {
    color: Theme.background

    Rectangle {
        anchors.centerIn: parent
        width: parent.width * 0.6
        height: parent.height * 0.3
        radius: height / 2
        color: Theme.text
    }
}
