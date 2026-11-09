//
//  Page.qml
//  qml/darkride/panel
//
//  A panel's page: the large title, its tabs (a segmented control under the title, when it has more than one) and
//  its header (rows that stay put: Explore's search) pinned at the top; then its sections on the grouped background,
//  scrolling under them, stopping at either end without a bounce (Caity, 10-24). A tab's sections sit in a Tab whose
//  index says which it is.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

import QtQuick 2.15
import QtQuick.Controls 2.15 as QQC2
import QtQuick.Layouts 1.15
import "."

Rectangle {
    id: page

    property string title: ""
    property var tabs: []          // the tabs' names; none or one: no tab bar
    property int currentTab: 0
    default property alias content: column.data
    property alias header: headerColumn.data

    color: Theme.background

    // a skin's panel frame behind it all, and its padding around the page (ui/Skins.h)
    readonly property var skinPadding: Theme.frame("panel") ? Theme.frame("panel").padding : [0, 0, 0, 0]
    SkinFrame {
        part: "panel"
        anchors.fill: parent
    }

    // closes the panel the page is on, as its close box does: a desktop window hides (the bar's button follows it);
    // the tablet goes back to its home screen. Explore closes itself so a hop isn't watched from behind it.
    function closePanel() {
        for (var item = page.parent; item; item = item.parent) {
            if (item.hasOwnProperty("shown") && item.windowClosed !== undefined) {
                item.shown = false;
                item.windowClosed();
                return;
            }
        }
        var tablet = Tablet.getTablet("com.highfidelity.interface.tablet.system");
        if (tablet) {
            tablet.gotoHomeScreen();
        }
    }

    function scrollToTop() {
        scroll.contentItem.contentY = 0;
    }

    // brings an item of the page into view, its top a margin under the pinned top; nothing moves when it shows already
    function reveal(item) {
        var flick = scroll.contentItem;
        var at = item.mapToItem(holder, 0, 0).y;
        if (at < flick.contentY || at + Math.min(item.height, flick.height) > flick.contentY + flick.height) {
            flick.contentY = Math.max(0, Math.min(at - Theme.pageMargin, flick.contentHeight - flick.height));
        }
    }

    ColumnLayout {
        id: top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: Theme.pageMargin + page.skinPadding[0]
        anchors.rightMargin: Theme.pageMargin + page.skinPadding[2]
        anchors.topMargin: page.skinPadding[1]
        spacing: 0

        Text {
            visible: page.title !== ""
            text: page.title
            textFormat: Text.PlainText
            color: Theme.text
            font.pixelSize: Theme.largeTitleSize
            font.bold: true
            topPadding: 14 * Theme.unit
            bottomPadding: 2 * Theme.unit
            leftPadding: 4 * Theme.unit
        }

        Segmented {
            visible: page.tabs.length > 1
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 10 * Theme.unit
            model: page.tabs.map(function (name, index) { return { text: name, value: index }; })
            current: page.currentTab
            onChosen: {
                page.currentTab = value;
                page.scrollToTop();
            }
        }

        ColumnLayout {
            id: headerColumn
            Layout.fillWidth: true
            spacing: 0
        }

        Item {
            Layout.preferredHeight: 4 * Theme.unit
        }
    }

    // a hairline under the pinned top once something has scrolled under it, as iOS draws one
    Rectangle {
        anchors.top: top.bottom
        width: parent.width
        height: 1
        z: 1
        color: Theme.separator
        visible: scroll.contentItem.contentY > 1
    }

    QQC2.ScrollView {
        id: scroll
        objectName: "pageScroll"   // a test can scroll the page by it
        anchors.top: top.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: page.skinPadding[0]
        anchors.rightMargin: page.skinPadding[2]
        anchors.bottomMargin: page.skinPadding[3]
        clip: true
        contentWidth: availableWidth
        contentHeight: holder.implicitHeight
        Component.onCompleted: contentItem.boundsBehavior = Flickable.StopAtBounds

        Item {
            id: holder
            width: scroll.availableWidth
            // room under the last section, so it doesn't sit on the window's edge
            implicitHeight: column.implicitHeight + Theme.pageMargin * 1.5

            ColumnLayout {
                id: column
                x: Theme.pageMargin
                width: parent.width - 2 * Theme.pageMargin
                spacing: 0
            }
        }
    }

    // a skin's flowers, grass or badges on the panel, over everything and never taking a click
    SkinDecorations {
        anchors.fill: parent
        z: 2
    }
}
