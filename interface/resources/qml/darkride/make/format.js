//
//  format.js
//  qml/darkride/make
//
//  How the Make dock writes times and costs, and reads a row's `views` (its pictures). Only MakeDock's own picture
//  provider's addresses (image://make/...) are ever shown: a file or web address in a row shows nothing.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

.pragma library

// "48 s", "3 min", "1 h 5 min"
function duration(ms) {
    var seconds = Math.max(0, Math.round(ms / 1000));
    if (seconds < 60) {
        return seconds + " s";
    }
    var minutes = Math.round(seconds / 60);
    if (minutes < 60) {
        return minutes + " min";
    }
    return Math.floor(minutes / 60) + " h " + (minutes % 60) + " min";
}

// "$0.52"; "" when the cost isn't known (-1: the Terminal route)
function cost(usd) {
    if (usd === undefined || usd === null || usd < 0) {
        return "";
    }
    return usd < 0.01 ? "under $0.01" : "$" + usd.toFixed(2);
}

var PROVIDER = "image://make/";

// one picture's address if it's MakeDock's own, else ""
function picture(url) {
    var text = url === undefined || url === null ? "" : String(url);
    return text.indexOf(PROVIDER) === 0 ? text : "";
}

// a row's pictures as a list of MakeDock's addresses, others left out: an ask row's list of strings, or a model
// row's one sheet; a QML ListModel (the preview's stand-in) turns a list into rows, read here by their `url`
function urls(views) {
    var list = [];
    if (!views) {
        return list;
    }
    if (typeof views === "string") {
        list = [views];   // a model's sheet: one address
    } else if (views.length !== undefined) {
        list = Array.prototype.slice.call(views).map(String);
    } else {
        for (var i = 0; views.count !== undefined && i < views.count; i++) {
            list.push(String(views.get(i).url));
        }
    }
    return list.filter(function (url) { return picture(url) !== ""; });
}
