//
//  Requests.qml
//  qml/darkride/make
//
//  What the timeline knows per request, read from MakeDock's timeline model (any list model: its rows are read
//  through an Instantiator, so the model needs no get()). For each row it says how the row shows:
//  - "fold": the one line of a folded request (the `folded` role; the person can open it again);
//  - "steps": the one line for a request's activity ("6 steps · 3 min") once the request is done, or for the older
//    steps of a running one (the newest three stay in sight);
//  - "hidden": inside a fold or a steps line;
//  - "row": as itself.
//  And which request's Undo is on: the newest request, once it's done and until it's undone (MakeDock's rule).
//  It rebuilds once per change (Qt.callLater), and only while the model changes: nothing runs when Make is idle.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

pragma ComponentBehavior: Bound

import QtQuick
import QtQml.Models

QtObject {
    id: requests

    property var model: null
    readonly property int recentSteps: 3   // a running request's newest steps in sight

    // bumped after each rebuild: bind to it to follow changes
    property int revision: 0
    property int newestUndoable: -1
    property int newestRequest: -1

    property var _info: ({})
    property var _modes: []
    property var _opened: ({})       // request -> true: a folded request the person opened
    property var _stepsOpened: ({})  // request -> true: its steps line opened

    // a row not yet sorted (it arrived since the last rebuild, which runs before the next frame) stays hidden, so a
    // folded request's rows never show, or load their pictures, for a moment
    function mode(index) {
        return _modes[index] || "hidden";
    }
    function info(request) {
        return _info[request] || null;
    }
    function toggleFold(request) {
        var opened = Object.assign({}, _opened);
        opened[request] = !opened[request];
        _opened = opened;
        rebuild();
    }
    function toggleSteps(request) {
        var opened = Object.assign({}, _stepsOpened);
        opened[request] = !opened[request];
        _stepsOpened = opened;
        rebuild();
    }

    function changed() {
        Qt.callLater(rebuild);
    }

    function rebuild() {
        var count = rows.count;
        var rowsSeen = [];
        for (var i = 0; i < count; i++) {
            rowsSeen.push(rows.objectAt(i));
        }

        var info = {};
        var newest = -1;
        rowsSeen.forEach(function (row, index) {
            if (!row) {
                return;
            }
            var request = info[row.request];
            if (!request) {
                request = info[row.request] = {
                    first: index, activities: [], lastReply: -1, done: false, cost: -1, undone: false, folded: false,
                    startTime: row.time, endTime: 0, title: "", firstText: row.text
                };
            }
            if (row.kind === "activity") {
                request.activities.push(index);
            } else if (row.kind === "reply") {
                request.lastReply = index;   // Claude's reply: the step before it is done (NowView)
            } else if (row.kind === "ask" && request.title === "") {
                request.title = row.text;
            } else if (row.kind === "done") {
                request.done = true;
                request.cost = row.cost;
                request.endTime = row.time;
            }
            request.undone = request.undone || row.undone;
            request.folded = request.folded || row.folded;
            newest = Math.max(newest, row.request);
        });

        // the newest request, once it's done and while it isn't undone: MakeDock.undoRequest undoes only that one, so
        // an older request's Undo never comes on after the newest is undone (it would be refused; kit F, 11-08)
        var undoable = newest >= 0 && info[newest] && info[newest].done && !info[newest].undone ? newest : -1;

        var modes = [];
        rowsSeen.forEach(function (row, index) {
            if (!row) {
                modes.push("row");
                return;
            }
            var request = info[row.request];
            if (request.folded && !_opened[row.request]) {
                modes.push(index === request.first ? "fold" : "hidden");
                return;
            }
            if (row.kind === "activity" && !_stepsOpened[row.request]) {
                var rank = request.activities.indexOf(index);
                var steps = request.activities.length;
                if (request.done || (steps > recentSteps && rank < steps - recentSteps)) {
                    modes.push(rank === 0 ? "steps" : "hidden");
                    return;
                }
            }
            modes.push("row");
        });

        _info = info;
        _modes = modes;
        newestRequest = newest;
        newestUndoable = undoable;
        revision++;
    }

    property Instantiator rows: Instantiator {
        model: requests.model
        delegate: QtObject {
            required property var model
            readonly property string kind: model.kind || ""
            readonly property int request: model.request || 0
            readonly property string text: model.text || ""
            readonly property real cost: model.cost === undefined ? -1 : model.cost
            readonly property real time: model.time || 0
            readonly property bool folded: !!model.folded
            readonly property bool undone: !!model.undone
            readonly property string signature: [kind, request, cost, time, folded, undone].join("|")
            onSignatureChanged: requests.changed()
        }
        onObjectAdded: requests.changed()
        onObjectRemoved: requests.changed()
    }
}
