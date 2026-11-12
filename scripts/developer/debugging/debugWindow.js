//
//  debugWindow.js
//
//  Brad Hefta-Gaub, created on 12/19/2016.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

(function() { // BEGIN LOCAL_SCOPE

//check if script already running.
var scriptData = ScriptDiscoveryService.getRunning();
var scripts = scriptData.filter(function (datum) { return datum.name === 'debugWindow.js'; });
if (scripts.length >= 2) {
    //2nd instance of the script is too much
    ScriptDiscoveryService.stopScript(scripts[1].url);
    return;
}

var SUPPRESS_DEFAULT_SCRIPTS_MENU_NAME  = "Developer"
// the default scripts' lines (the app's own) are hidden until the person shows them: a checkmark means "shown" (mary's
// words, 11-11). A new key: the old one hid nothing by default, and the default scripts drown a scripter's lines.
var SUPPRESS_DEFAULT_SCRIPTS_ITEM_NAME = "Script Log: Show Default Scripts";
// under Developer/: a world's script may not write it (ScriptSettingsInterface), the person's own scripts may
var DEBUG_WINDOW_SUPPRESS_DEFAULTS_SCRIPTS = 'Developer/ScriptLog/showDefaultScripts';
var savedShow = Settings.getValue(DEBUG_WINDOW_SUPPRESS_DEFAULTS_SCRIPTS, false);
var suppressDefaultScripts = !(savedShow === true || savedShow === "true");
Menu.addMenuItem({
    menuName: SUPPRESS_DEFAULT_SCRIPTS_MENU_NAME,
    menuItemName: SUPPRESS_DEFAULT_SCRIPTS_ITEM_NAME,
    isCheckable: true,
    isChecked: !suppressDefaultScripts
});

Menu.menuItemEvent.connect(function(menuItem) {
    if (menuItem === SUPPRESS_DEFAULT_SCRIPTS_ITEM_NAME) {
        suppressDefaultScripts = !Menu.isOptionChecked(SUPPRESS_DEFAULT_SCRIPTS_ITEM_NAME);
    }
});

// Set up the qml ui
var qml = Script.resolvePath('debugWindow.qml');

var HMD_DEBUG_WINDOW_GEOMETRY_KEY = 'Developer/ScriptLog/geometry';   // under Developer/ too
var hmdDebugWindowGeometryValue = Settings.getValue(HMD_DEBUG_WINDOW_GEOMETRY_KEY);

var windowWidth = 400;
var windowHeight = 900;

var hasPosition = false;
var windowX = 0;
var windowY = 0;

// a saved place that can't be read, or isn't on the window, is no place: the window opens at its default
function clamp(value, low, high) {
    return Math.max(low, Math.min(high, value));
}
var screenWidth = Math.max(400, Window.innerWidth);
var screenHeight = Math.max(300, Window.innerHeight);
windowHeight = Math.min(windowHeight, screenHeight);   // the default's 900 is taller than a small laptop's window
if (hmdDebugWindowGeometryValue !== '') {
    var geometry = null;
    try {
        geometry = JSON.parse(hmdDebugWindowGeometryValue);
    } catch (e) {
        geometry = null;
    }
    var numbers = geometry && [geometry.x, geometry.y, geometry.width, geometry.height].every(function (n) {
        return typeof n === "number" && isFinite(n);
    });
    if (numbers && (geometry.x !== 0) && (geometry.y !== 0)) {
        windowWidth = clamp(geometry.width, 200, screenWidth);
        windowHeight = clamp(geometry.height, 150, screenHeight);
        windowX = clamp(geometry.x, 0, screenWidth - 100);
        windowY = clamp(geometry.y, 0, screenHeight - 100);
        hasPosition = true;
    }
}

var window = new OverlayWindow({
    title: 'Script Log',
    source: qml,
    width: windowWidth, height: windowHeight,
});

if (hasPosition) {
    window.setPosition(windowX, windowY);
};

function recenterWindow() {
    window.setPosition(100, 100);
}

window.visibleChanged.connect(function() {
    if (!window.visible) {
        window.setVisible(true);
        recenterWindow();
    }
});

HMD.displayModeChanged.connect(function(isHMDMode) {
    recenterWindow();
});

window.closed.connect(function () { Script.stop(); });

// the default scripts: every running script loaded from the app's scripts folder ("file:///~/...", as the running list
// names them), by file name, the name each message carries (its engine's). Read again whenever the scripts change, and
// at a name not seen before (a new script's first lines can come before the change); a name once the app's stays so,
// so a stopped one's last lines stay hidden too
var APP_SCRIPTS_PREFIX = "file:///~/";
var appScriptNames = {};
var seenNames = {};
function readAppScripts() {
    ScriptDiscoveryService.getRunning().forEach(function (script) {
        if (String(script.url).indexOf(APP_SCRIPTS_PREFIX) === 0 && script.name !== "debugWindow.js") {
            appScriptNames[script.name] = true;
        }
    });
}
readAppScripts();
ScriptDiscoveryService.scriptCountChanged.connect(readAppScripts);

function shouldLogMessage(scriptFileName) {
    if (!seenNames[scriptFileName]) {
        seenNames[scriptFileName] = true;
        readAppScripts();
    }
    return !suppressDefaultScripts || !appScriptNames[scriptFileName];
}

function twoDigits(number) {
    return (number < 10 ? "0" : "") + number;
}
// the time of day, 24-hour: a log is read as it runs (getMonth() counts from 0: the old date was a month behind)
var getFormattedDate = function() {
    var date = new Date();
    return twoDigits(date.getHours()) + ":" + twoDigits(date.getMinutes()) + ":" + twoDigits(date.getSeconds());
};

// the lines go to the window together, at most ten times a second (performance, 11-11): a script printing every frame
// costs ten re-draws of the window a second, not one a frame, and no line is lost. The timer runs only while lines wait.
var FLUSH_MS = 100;
var waitingLines = [];
var flushTimer = null;
function flushLines() {
    flushTimer = null;
    if (waitingLines.length > 0) {
        window.sendToQml(waitingLines.join("\n"));
        waitingLines = [];
    }
}

// every line in the window starts with its own script's label: a message's further lines are indented under it, so no
// script can write a line that reads as another's; a message is cut at MAX_MESSAGE_LENGTH characters
var MAX_MESSAGE_LENGTH = 2000;
var CONTINUATION = "\n    ";
function asLines(message) {
    var text = String(message);
    if (text.length > MAX_MESSAGE_LENGTH) {
        text = text.slice(0, MAX_MESSAGE_LENGTH) + "… (" + (text.length - MAX_MESSAGE_LENGTH) + " more characters)";
    }
    return text.replace(/\r\n|\r|\n/g, CONTINUATION);
}
// the entity scripts' engine ("about:Entities N"): every entity's client script, a world's among them
function labelOf(scriptFileName) {
    return String(scriptFileName).indexOf("about:Entities") === 0 ? "entity scripts" : asLines(scriptFileName);
}

var sendToLogWindow = function(type, message, scriptFileName) {
    if (shouldLogMessage(scriptFileName)) {
        var typeFormatted = "";
        if (type) {
            typeFormatted = type + " - ";
        }
        waitingLines.push("[" + getFormattedDate() + "] " + "[" + labelOf(scriptFileName) + "] " + typeFormatted +
                          asLines(message));
        if (flushTimer === null) {
            flushTimer = Script.setTimeout(flushLines, FLUSH_MS);
        }
    }
};

ScriptDiscoveryService.printedMessage.connect(function(message, scriptFileName) {
    sendToLogWindow("", message, scriptFileName);
});

ScriptDiscoveryService.warningMessage.connect(function(message, scriptFileName) {
    sendToLogWindow("WARNING", message, scriptFileName);
});

ScriptDiscoveryService.errorMessage.connect(function(message, scriptFileName) {
    sendToLogWindow("ERROR", message, scriptFileName);
});

ScriptDiscoveryService.infoMessage.connect(function(message, scriptFileName) {
    sendToLogWindow("INFO", message, scriptFileName);
});

ScriptDiscoveryService.clearDebugWindow.connect(function() {
    waitingLines = [];
    window.clearDebugWindow();
});

Script.scriptEnding.connect(function () {
    if (flushTimer !== null) {
        Script.clearTimeout(flushTimer);
        flushTimer = null;
    }
    Settings.setValue(DEBUG_WINDOW_SUPPRESS_DEFAULTS_SCRIPTS,
                      Menu.isOptionChecked(SUPPRESS_DEFAULT_SCRIPTS_ITEM_NAME));
    Menu.removeMenuItem(SUPPRESS_DEFAULT_SCRIPTS_MENU_NAME, SUPPRESS_DEFAULT_SCRIPTS_ITEM_NAME);

    var geometry = JSON.stringify({
        x: window.position.x,
        y: window.position.y,
        width: window.size.x,
        height: window.size.y
    });

    Settings.setValue(HMD_DEBUG_WINDOW_GEOMETRY_KEY, geometry);
    window.close();
});

}());
