//
//  Checkmark.qml
//  qml/darkride/panel
//
//  The tick on a chosen row (a picker's choice, the avatar worn): ✓ in the tint, bold, at body size. It keeps its
//  place when not checked, so the row's other controls don't move.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.15
import "."

Text {
    property bool checked: true

    text: "✓"   // ✓
    color: Theme.tint
    font.pixelSize: Theme.bodySize
    font.bold: true
    opacity: checked ? 1 : 0
}
