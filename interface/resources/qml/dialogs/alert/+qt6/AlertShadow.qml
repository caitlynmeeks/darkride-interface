//
//  AlertShadow.qml
//  interface/resources/qml/dialogs/alert
//
//  Created on 2026-10-19 for Darkride's popups.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

import QtQuick 2.12
import Qt5Compat.GraphicalEffects   // Qt 6: QtGraphicalEffects lives in the Qt5Compat module (the Qt 5 file is beside this +qt6 folder)

// A desktop alert's window frame: no decoration, only the soft shadow that lifts the panel off the world behind it.
Item {
    anchors.fill: parent

    RectangularGlow {
        anchors.fill: parent
        anchors.topMargin: 16
        anchors.bottomMargin: -16
        glowRadius: 28
        spread: 0.05
        cornerRadius: 12 + glowRadius
        color: Qt.rgba(0, 0, 0, 0.5)
        cached: true
    }
}
