//
//  MicBar.qml
//  qml/hifi/audio
//
//  Created by Zach Pomerantz on 6/14/2017
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
//  Darkride's microphone pill (look A, Caity 10-28), in the bar's look: the mic alone in a small dark circle while it's
//  on and you're quiet; a short cyan level beside it while you speak; a red "Muted" pill while muted; "Hold T to talk"
//  with push to talk. A click mutes or unmutes, as before. At the view's top left (AvatarInputsBar.qml, which drags it)
//  and on the VR tablet's top line (TabletHome.qml).
//

import QtQuick 2.5
import Qt5Compat.GraphicalEffects   // Qt 6: QtGraphicalEffects lives in the Qt5Compat module (the Qt 5 file is beside this +qt6 folder)

import TabletScriptingInterface 1.0

Item {
    id: micBar

    property bool muted: AudioScriptingInterface.muted
    readonly property real level: AudioScriptingInterface.inputLevel
    readonly property bool clipping: AudioScriptingInterface.clipping
    property bool pushToTalk: AudioScriptingInterface.pushToTalk
    property bool pushingToTalk: AudioScriptingInterface.pushingToTalk
    property bool gated: false

    // the bar's own properties by its id: unqualified, a write from these handlers (called later, by C++ signals) went
    // to the global object instead ("Invalid write to global property gated", a flood on Windows as the noise gate
    // opens and closes; windowz, 10-28)
    Component.onCompleted: {
        AudioScriptingInterface.noiseGateOpened.connect(function() { micBar.gated = false; });
        AudioScriptingInterface.noiseGateClosed.connect(function() { micBar.gated = true; });
        // (the tablet's home page is made once before the HMD object is there)
        if (micBar.hasHMD) {
            HMD.displayModeChanged.connect(function() {
                micBar.muted = AudioScriptingInterface.muted;
                micBar.pushToTalk = AudioScriptingInterface.pushToTalk;
            });
        }
        AudioScriptingInterface.mutedChanged.connect(function() {
            micBar.muted = AudioScriptingInterface.muted;
        });
        AudioScriptingInterface.pushToTalkChanged.connect(function() {
            micBar.pushToTalk = AudioScriptingInterface.pushToTalk;
        });
        AudioScriptingInterface.pushingToTalkChanged.connect(function() {
            micBar.pushingToTalk = AudioScriptingInterface.pushingToTalk;
        });
    }

    readonly property bool hasHMD: typeof HMD !== "undefined" && HMD !== null
    property bool standalone: false   // the view's own (AvatarInputsBar.qml), not the tablet's
    property var dragTarget: null
    property real scale: 1

    // what it shows: "waiting" (push to talk, not held), "muted", "speaking" or "quiet"
    readonly property bool waitingToTalk: pushToTalk && !pushingToTalk
    readonly property bool voice: !muted && !waitingToTalk && !gated && level > 0.05
    // the level stays out 1.5 s after the voice stops, so the pill doesn't open and close between words
    property bool speaking: false
    onVoiceChanged: {
        if (voice) {
            quietTimer.stop();
            speaking = true;
        } else {
            quietTimer.restart();
        }
    }
    Timer {
        id: quietTimer
        interval: 1500
        onTriggered: micBar.speaking = false
    }
    readonly property string mode: waitingToTalk ? "waiting" : muted ? "muted" : speaking ? "speaking" : "quiet"

    readonly property color iconColor: mode === "muted" ? "#ff5555" : mode === "waiting" ? "#9aa1bd"
        : mode === "quiet" ? Qt.rgba(244 / 255, 246 / 255, 248 / 255, 0.75) : "#f4f6f8"

    width: pill.width
    height: pill.height

    Rectangle {
        id: pill
        height: 30 * micBar.scale
        width: micBar.mode === "quiet" ? height : content.width + 22 * micBar.scale
        radius: height / 2
        clip: true
        color: micBar.mode === "muted" ? Qt.rgba(60 / 255, 18 / 255, 28 / 255, 0.9) : Qt.rgba(23 / 255, 27 / 255, 45 / 255, 0.88)
        border.width: 1
        border.color: micBar.mode === "muted" ? (mouseArea.containsMouse ? "#ff8a8a" : Qt.rgba(1, 85 / 255, 85 / 255, 0.55))
            : (mouseArea.containsMouse ? "#4a5175" : "#262c47")
        Behavior on width { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

        Row {
            id: content
            anchors.verticalCenter: parent.verticalCenter
            x: micBar.mode === "quiet" ? (pill.height - icon.width) / 2 : 11 * micBar.scale
            spacing: 8 * micBar.scale

            Item {
                id: icon
                anchors.verticalCenter: parent.verticalCenter
                width: 18 * micBar.scale
                height: 18 * micBar.scale
                Image {
                    id: glyph
                    anchors.fill: parent
                    visible: false   // drawn in its colour below
                    smooth: true
                    sourceSize.width: 36 * micBar.scale
                    sourceSize.height: 36 * micBar.scale
                    source: micBar.mode === "muted" || micBar.mode === "waiting"
                        ? "qrc:///icons/bar/audio-muted.svg" : "qrc:///icons/bar/audio.svg"
                }
                ColorOverlay {
                    anchors.fill: parent
                    source: glyph
                    color: micBar.iconColor
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: micBar.mode === "muted" || micBar.mode === "waiting"
                text: micBar.mode === "muted" ? "Muted" : micBar.hasHMD && HMD.active ? "Hold to talk" : "Hold T to talk"
                color: micBar.mode === "muted" ? "#ff5555" : "#9aa1bd"
                font.pixelSize: 13 * micBar.scale
                font.weight: micBar.mode === "muted" ? Font.DemiBold : Font.Normal
            }

            // the level: seven bars lit up to it, cyan, the loudest two red while the microphone clips
            Row {
                anchors.verticalCenter: parent.verticalCenter
                visible: micBar.mode === "speaking"
                spacing: 2 * micBar.scale
                Repeater {
                    model: [5, 9, 13, 10, 8, 6, 5]
                    Rectangle {
                        readonly property bool lit: index < Math.ceil(micBar.level * 7)
                        anchors.verticalCenter: parent.verticalCenter
                        width: 3 * micBar.scale
                        height: modelData * micBar.scale
                        radius: width / 2
                        color: !lit ? "#3b4262" : micBar.clipping && index >= 5 ? "#ff5555" : "#00afff"
                    }
                }
            }
        }
    }

    MouseArea {
        id: mouseArea
        anchors.fill: pill
        hoverEnabled: true
        scrollGestureEnabled: false
        drag.target: micBar.dragTarget
        onClicked: {
            if (micBar.pushToTalk) {
                return;
            }
            AudioScriptingInterface.muted = !micBar.muted;
            Tablet.playSound(TabletEnums.ButtonClick);
        }
        onContainsMouseChanged: {
            if (containsMouse) {
                Tablet.playSound(TabletEnums.ButtonHover);
            }
        }
    }
}
