//
//  BuildTheme.qml
//  qml/darkride/editor
//
//  Build mode's look for the QML docks that sit beside the Inspector (the Code panel, the Make panel): the greys of
//  interface/resources/styles/editor.qss, so a QML dock and the Qt widgets beside it read as one tool (Caity 11-07
//  "ok as drawn" for Code; mary's team/mary/brand/make-panel/MAKE-PANEL-LOOK.md for Make). The QML panels outside
//  Build mode (Settings, Explore, People, Avatar) keep panel/Theme.qml.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

pragma Singleton

import QtQuick

QtObject {
    // the panel and its parts
    readonly property color panel: "#383838"
    readonly property color field: "#2a2a2a"        // fields and insets
    readonly property color fieldEdge: "#1f1f1f"
    readonly property color fieldPressed: "#323232"
    readonly property color edge: "#232323"         // lines between rows and parts
    readonly property color hover: "#474747"
    readonly property color selection: "#2c5d87"    // editor.qss's selection and keyboard focus
    readonly property color sectionBar: "#3c3c3c"   // the Inspector's section header
    readonly property color sectionLine: "#2b2b2b"

    // text
    readonly property color text: "#d2d2d2"
    readonly property color strongText: "#e2e2e2"
    readonly property color minorText: "#b8b8b8"    // a minor section's disclosure
    readonly property color accentText: "#8fb6de"   // a mark or a small heading in blue (the Code list's script colour)
    readonly property color mutedText: "#8f8f8f"    // notes, waiting
    readonly property color dimText: "#6a6a6a"      // disabled
    readonly property color running: "#5fb760"      // connected, running, a tick
    readonly property color problem: "#ff6b6b"      // problems, deletes

    // sizes, in pixels
    readonly property int textSize: 12
    readonly property int titleSize: 13
    readonly property int noteSize: 11
    readonly property int radius: 3
    readonly property int padding: 8
    readonly property string mono: Qt.platform.os === "osx" ? "Menlo" : Qt.platform.os === "windows" ? "Consolas" : "monospace"
}
