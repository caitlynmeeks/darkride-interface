//
//  Busy.qml
//  qml/darkride/make
//
//  The small mark on the step Claude is doing: twelve spokes, the bright one stepping round, as macOS's own spinner.
//  It steps on an 80 ms timer, about twelve redraws a second, never at the display's rate, and only while it's in
//  sight (editor-ui's review 1: a RotationAnimator kept the dock drawing at 72 or 144 Hz for minutes).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

pragma ComponentBehavior: Bound

import QtQuick
import "../editor"

Item {
    id: busy

    property int step: 0

    implicitWidth: 13
    implicitHeight: 13
    Accessible.role: Accessible.Animation
    Accessible.name: "Working"

    Repeater {
        model: 12
        delegate: Rectangle {
            id: spoke
            required property int index
            // each spoke a short rounded bar from the middle out, turned to its place on the dial
            x: (busy.width - width) / 2
            y: 0
            width: 1.6
            height: busy.height * 0.3
            radius: width / 2
            color: BuildTheme.mutedText
            // the spoke at `step` is brightest; those behind it fade
            opacity: 0.25 + 0.75 * (((spoke.index - busy.step + 12) % 12) === 0 ? 1 : Math.max(0, 1 - ((busy.step - spoke.index + 12) % 12) / 6))
            transform: Rotation {
                origin.x: 0.8
                origin.y: busy.height / 2
                angle: spoke.index * 30
            }
        }
    }

    Timer {
        interval: 80
        repeat: true
        running: busy.visible
        onTriggered: busy.step = (busy.step + 1) % 12
    }
}
