//
//  MakeButton.qml
//  qml/darkride/make
//
//  The Make dock's button: Build mode's one button (editor/BuildButton.qml, as the Inspector's and the Code panel's).
//  Every kind is drawn the same (mary's MAKE-PANEL-LOOK.md): "primary" is the one Return presses and sits on the
//  right, "plain" is most actions, "stop" is Stop (not a delete, so not red). A click or Space/Return while it has
//  focus is real input in the dock's own window, which is what MakeDock's real-input check looks for
//  (MAKE-DOCK-CONTRACT.md); the button itself only emits clicked.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick
import "../editor"

BuildButton {
    id: button

    property string kind: "plain"   // "primary", "plain" or "stop": all drawn alike
    signal clicked()

    onPressed: button.clicked()

    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: text
    Accessible.onPressAction: if (enabled) button.clicked()
    Keys.onSpacePressed: if (enabled) button.clicked()
    Keys.onReturnPressed: if (enabled) button.clicked()
}
