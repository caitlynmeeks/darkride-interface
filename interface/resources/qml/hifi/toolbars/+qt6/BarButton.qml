//
//  BarButton.qml
//  qml/hifi/toolbars
//
//  A button of the floating bar (Toolbar.qml) in look A, "plain icons" (Caity 10-28), as the docked bar draws its own
//  (ui/SystemToolbar.cpp): the bar's line icon for the button (SystemToolbar.iconFor), else the script's own icon in
//  the same colours, its label under it, in the cell the bar gives every button. White; Darkride cyan on a cyan plate
//  while its panel is open; a grey plate under the pointer; red for a muted microphone; the count waiting in a red
//  capsule. A skin's bar button frame (SKINS.md "barButton") and text colour take the plates' and the label's place.
//  The VR tablet's buttons stay ToolbarButton.qml.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.5
import Qt5Compat.GraphicalEffects   // Qt 6: QtGraphicalEffects lives in the Qt5Compat module (the Qt 5 file is beside this +qt6 folder)
import DarkridePanels 1.0

Item {
    id: button

    property real cellWidth: 68
    width: cellWidth
    height: 58
    readonly property real captionWidth: caption.implicitWidth   // the bar widens every cell to the widest label

    // the TabletButtonProxy.ButtonProperties a script may set (TabletScriptingInterface.cpp's JSDoc); the bar copies
    // each onto the button, so the ones it doesn't draw are here too
    property string text: ""
    property string hoverText: button.text
    property string activeText: button.text
    property string activeHoverText: button.activeText
    property string icon: ""
    property string hoverIcon: button.icon
    property string activeIcon: button.icon
    property string activeHoverIcon: button.activeIcon
    property int iconPadding: 8
    property color defaultCaptionColor: "#ffffff"
    property color captionColor: defaultCaptionColor
    property color backgroundColor: "#1d1f21"
    property bool buttonEnabled: true
    property bool isActive: false
    property bool isEntered: false
    property int sortOrder: 100
    property int stableOrder: 0
    property var uuid
    property bool inDebugMode: false
    property int badge: 0
    property bool muted: false

    signal clicked()

    readonly property var skinFrame: PanelTheme.skin && PanelTheme.skin.frames
        ? (PanelTheme.skin.frames.barButton || null) : null
    readonly property bool skinDrawsBehind: !!PanelTheme.skin && !!PanelTheme.skin.frames
        && !!(PanelTheme.skin.frames.barButton || PanelTheme.skin.frames.bar)
    readonly property var skinText: skinDrawsBehind && PanelTheme.skin.colors ? PanelTheme.skin.colors.text : undefined

    readonly property string barIcon: SystemToolbar.iconFor(button.text, button.muted)
    readonly property color iconColor: button.muted ? "#ff5555" : button.isActive ? "#00afff"
        : button.skinText !== undefined ? button.skinText : "#f4f6f8"

    function urlHelper(src) {
        if (src.match(/\bhttp/) || src.match(/\bfile:/) || src.match(/^data:/) || src.match(/^qrc:/)) {
            return src;
        }
        return "qrc:///" + src;
    }

    // a moment after the click, as the docked bar: the button may open a panel that takes the mouse
    Timer {
        id: asyncClickSender
        interval: 10
        onTriggered: button.clicked()
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: button.buttonEnabled
        onClicked: asyncClickSender.start()
        onEntered: button.isEntered = true
        onExited: button.isEntered = false
    }

    BorderImage {
        anchors.fill: parent
        visible: !!button.skinFrame
        source: !button.skinFrame ? ""
            : button.isActive && button.skinFrame.checked ? button.skinFrame.checked
            : button.isEntered && button.skinFrame.hover ? button.skinFrame.hover
            : button.skinFrame.source
        border.left: button.skinFrame ? button.skinFrame.slice[0] : 0
        border.top: button.skinFrame ? button.skinFrame.slice[1] : 0
        border.right: button.skinFrame ? button.skinFrame.slice[2] : 0
        border.bottom: button.skinFrame ? button.skinFrame.slice[3] : 0
    }

    // the plate: cyan while open, grey under the pointer
    Rectangle {
        anchors.fill: parent
        radius: 9
        visible: !button.skinFrame && (button.isActive || button.isEntered)
        color: mouseArea.pressed ? "#3d4568" : button.isActive ? Qt.rgba(0, 175 / 255, 1, 0.14) : "#333a5a"
    }

    Item {
        id: iconBox
        width: 24
        height: 24
        x: (button.width - width) / 2
        y: 8

        Image {
            id: glyph
            anchors.fill: parent
            visible: false   // drawn in the bar's colour below
            smooth: true
            fillMode: Image.PreserveAspectFit
            sourceSize.width: 48
            sourceSize.height: 48
            // none until the bar has copied the button's properties onto it (an empty path would load the resources'
            // folder as a picture)
            source: {
                var path = button.barIcon !== "" ? button.barIcon
                    : button.isActive ? (button.isEntered ? button.activeHoverIcon : button.activeIcon)
                    : (button.isEntered ? button.hoverIcon : button.icon);
                return path ? button.urlHelper(path) : "";
            }
        }
        ColorOverlay {
            anchors.fill: parent
            source: glyph
            color: button.iconColor
        }
    }

    // the count waiting, at the icon's top right, as the docked bar draws it
    Rectangle {
        visible: button.badge > 0
        x: Math.min(iconBox.x + 19, button.width - width)
        y: 3
        height: 15
        width: Math.max(height, badgeText.implicitWidth + 7)
        radius: height / 2
        color: "#ff3b30" // Apple's system red
        Text {
            id: badgeText
            anchors.centerIn: parent
            text: button.badge > 99 ? "99+" : button.badge
            color: "#ffffff"
            font.pixelSize: 10
            font.bold: true
        }
    }

    Text {
        id: caption
        anchors.horizontalCenter: parent.horizontalCenter
        y: iconBox.y + iconBox.height + 5
        horizontalAlignment: Text.AlignHCenter
        textFormat: Text.PlainText
        text: button.isActive ? (button.isEntered ? button.activeHoverText : button.activeText)
            : (button.isEntered ? button.hoverText : button.text)
        color: button.skinText !== undefined ? button.skinText : button.isActive ? "#00afff" : "#9aa1bd"
        font.pixelSize: 11
    }
}
