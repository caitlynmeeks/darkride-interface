//
//  jet/TaskList.qml
//
//  Created by Sam Gateau, 2018/03/28
//  Copyright 2018 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

import QtQuick 2.7
import QtQuick.Controls 2.12 as Original

import stylesUit 1.0
import controlsUit 1.0 as HifiControls

import "../jet.js" as Jet

Rectangle {   
    HifiConstants { id: hifi;}
    color: hifi.colors.baseGray;
    id: root
    property var rootConfig : Workload

    // Controls 2's TextArea doesn't scroll by itself: it fills a Flickable
    Flickable {
        anchors.fill: parent
        Original.TextArea.flickable: Original.TextArea {
            id: textArea
            text: ""
            wrapMode: TextEdit.WordWrap
            selectByMouse: true
        }
        Original.ScrollBar.vertical: Original.ScrollBar { }
    }

    Component.onCompleted: {
        var message = ""
        var functor = Jet.job_print_functor(function (line) { message += line + "\n"; }, false, true);
        Jet.task_traverseTree(rootConfig, functor);
        textArea.append(message);
    }

    function clearWindow() {
        textArea.remove(0,textArea.length);
    }
}