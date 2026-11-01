//
//  ScreenSlot.qml
//  interface/resources/qml/darkride
//
//  One slot of the VR screen (team/editor-ui/VR-SCREEN-PLAN.md): the host item in a surface of vr's, which the app's
//  panels' pages move into (Panels.screenSlotReady; Panels refuses a window that isn't the screen's). The middle slot
//  shows its panels' tabs along its top, the page in front under them. Pages are made once, in the desktop's QML
//  engine, and go back to their docks when the screen is hidden or the headset comes off.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

import QtQuick 2.15
import QtQuick.Window 2.15
import "panel"

Rectangle {
    id: root
    property string slot: ""
    property var tabs: []   // the middle slot's panels, the one in front last (Panels sets it)
    readonly property bool hasTabs: slot === "middle"

    color: Theme.background

    Row {
        id: tabRow
        visible: root.hasTabs
        height: root.hasTabs ? Theme.controlHeight + 12 * Theme.unit : 0
        anchors { left: parent.left; right: parent.right; top: parent.top; leftMargin: 12 * Theme.unit }
        spacing: 4 * Theme.unit

        Repeater {
            // in the order they were first opened would shuffle as they come to the front: a steady order, the bar's
            model: root.tabs.slice().sort()
            delegate: Rectangle {
                readonly property bool front: root.tabs.length > 0 && root.tabs[root.tabs.length - 1] === modelData
                anchors.verticalCenter: parent.verticalCenter
                width: tabLabel.implicitWidth + 20 * Theme.unit
                height: Theme.controlHeight
                radius: 7 * Theme.unit
                color: front ? Theme.fill : "transparent"
                Text {
                    id: tabLabel
                    anchors.centerIn: parent
                    text: modelData
                    textFormat: Text.PlainText
                    color: parent.front ? Theme.text : Theme.secondaryText
                    font.pixelSize: 13 * Theme.unit
                    font.weight: parent.front ? Font.DemiBold : Font.Normal
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: Panels.open(modelData)
                }
            }
        }
    }

    // the surface's pixels per layout pixel: vr sizes the surface from its own degrees-per-pixel number, the panels are
    // laid out in their own pixels (Panels.screenSlotSize), so a page is laid out at its size and drawn this much bigger
    readonly property real pageScale: {
        var layout = Panels.screenSlotSize(slot);
        return layout.width > 0 ? Math.max(1, width / layout.width) : 1;
    }
    onPageScaleChanged: content.layoutPages()

    Item {
        id: content
        objectName: "content"
        anchors { left: parent.left; right: parent.right; top: tabRow.bottom; bottom: parent.bottom }
        // a page in it is laid out at its own size and scaled up to fill it; Qt Quick text stays sharp scaled, and a
        // panel with a web view counter-scales it (its screenScale) so the page's picture has the pixels
        function layoutPages() {
            for (var i = 0; i < children.length; i++) {
                var page = children[i];
                page.transformOrigin = Item.TopLeft;
                page.scale = root.pageScale;
                page.width = width / root.pageScale;
                page.height = height / root.pageScale;
                if (page.screenScale !== undefined) {
                    page.screenScale = root.pageScale;
                }
            }
        }
        onWidthChanged: layoutPages()
        onHeightChanged: layoutPages()
        onChildrenChanged: layoutPages()
    }

    // the surface puts the item in its window after the item is made: Panels checks that window
    readonly property bool inWindow: root.Window.window !== null
    onInWindowChanged: {
        if (inWindow) {
            Panels.screenSlotReady(slot, root);
        }
    }
    Component.onCompleted: {
        if (inWindow) {
            Panels.screenSlotReady(slot, root);
        }
    }

    // what a Web entity's surface asks of its QML: nothing to send, nothing loading to stop
    signal sendToScript(var message)
    function stop() {}
}
