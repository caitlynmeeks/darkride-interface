//
//  debugWindow.qml
//
//  Brad Hefta-Gaub, created on 12/19/2016.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

import QtQuick 2.5
import QtQuick.Controls 2.12
import Hifi 1.0 as Hifi
// Darkride's panel colours (Theme: the colour scheme and skin the person chose)
import "qrc:///qml/darkride/panel"


Rectangle {
    id: root
    width: parent ? parent.width : 100
    height: parent ? parent.height : 100
    color: Theme.background

    signal moved(vector2d position);
    signal resized(size size);

    property var channel;

    // Controls 2's TextArea doesn't scroll by itself: it fills a Flickable (Controls 1's wrapped and selected by mouse)
    Flickable {
        id: flick
        anchors.fill: parent
        TextArea.flickable: TextArea {
            id: textArea
            text: ""
            // every line as written: a script's output is never read as formatting (or as pictures to fetch)
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.WordWrap
            selectByMouse: true
            readOnly: true
            color: Theme.text
            selectionColor: Theme.tint
            selectedTextColor: Theme.background
            font.family: Qt.platform.os === "osx" ? "Menlo" : "Consolas"
            font.pixelSize: 12
            background: null
        }
        ScrollBar.vertical: ScrollBar { }
    }

    function fromScript(message) {
        var MAX_LINE_COUNT = 2000;
        var TRIM_LINES = 500;
        if (textArea.lineCount > MAX_LINE_COUNT) {
            var lines = textArea.text.split('\n');
            lines.splice(0, TRIM_LINES);
            textArea.text = lines.join('\n');
        }
        var following = flick.atYEnd;  // the log follows new lines only while it's at its end
        textArea.append(message);
        if (following) {
            textArea.cursorPosition = textArea.length;
        }
    }

    function clearWindow() {
        textArea.remove(0,textArea.length);
    }
}


