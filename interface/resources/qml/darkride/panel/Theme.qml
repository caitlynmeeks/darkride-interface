//
//  Theme.qml
//  qml/darkride/panel
//
//  The look of Darkride's QML panels (Settings first, then Friends, Explore and Avatar): iOS's grouped lists, the
//  same on the desktop and on the tablet (Caity, 10-23: "classy ... kinda like iOS design language"), in one of
//  four colour schemes or a loaded theme file (PanelTheme, which every engine shares and which saves the choice).
//  Graphite, soft greys, is the default (Caity, 10-23: the black first look was "a little too harsh").
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

pragma Singleton
import QtQuick 2.15
import DarkridePanels 1.0

QtObject {
    // 1 on the desktop; a surface seen from further away may scale everything up
    property real unit: 1

    // the built-in schemes; a theme file sets any of these keys over its base scheme (PanelTheme::COLOR_KEYS)
    readonly property var schemes: ({
        graphite: {
            name: "Graphite", background: "#1c1c1f", card: "#2a2a2e", cardPressed: "#35353a",
            separator: "#17ffffff", text: "#ececf0", secondaryText: "#a1a1a9", tertiaryText: "#6c6c74",
            tint: "#5aa2ff", on: "#34c759", destructive: "#ff6961", fill: "#3a3a40", segmentThumb: "#5c5c64",
            sliderTrack: "#46464d"
        },
        midnight: {
            name: "Midnight", background: "#000000", card: "#1c1c1e", cardPressed: "#2c2c2e",
            separator: "#99545458", text: "#ffffff", secondaryText: "#99ebebf5", tertiaryText: "#4debebf5",
            tint: "#0a84ff", on: "#30d158", destructive: "#ff453a", fill: "#3d767680", segmentThumb: "#636366",
            sliderTrack: "#5c787880"
        },
        dusk: {
            name: "Dusk", background: "#1b1828", card: "#272338", cardPressed: "#322d47",
            separator: "#1affffff", text: "#eeeaf7", secondaryText: "#a9a3bf", tertiaryText: "#6e6884",
            tint: "#a78bfa", on: "#9b7bf7", destructive: "#ff6b81", fill: "#3a3550", segmentThumb: "#5b5478",
            sliderTrack: "#45405c"
        },
        daylight: {
            name: "Daylight", background: "#f2f2f7", card: "#ffffff", cardPressed: "#e5e5ea",
            separator: "#4a3c3c43", text: "#1c1c1e", secondaryText: "#6c6c70", tertiaryText: "#aeaeb2",
            tint: "#007aff", on: "#34c759", destructive: "#ff3b30", fill: "#33787880", segmentThumb: "#ffffff",
            sliderTrack: "#e5e5ea"
        }
    })
    readonly property var schemeOrder: ["graphite", "midnight", "dusk", "daylight"]

    // the palette in use: the chosen scheme, or a loaded file's colours over its base scheme; then the colours of a
    // skin that is on (ui/Skins.h) over that
    readonly property var palette: {
        var custom = PanelTheme.custom || {};
        var chosen = PanelTheme.scheme === "custom" ? (custom.base || "graphite") : PanelTheme.scheme;
        var base = schemes[chosen] || schemes.graphite;
        var colors = PanelTheme.scheme === "custom" ? custom : {};
        var skinColors = (PanelTheme.skin && PanelTheme.skin.colors) || {};
        var merged = {};
        for (var key in base) {
            merged[key] = skinColors[key] !== undefined ? skinColors[key]
                : colors[key] !== undefined ? colors[key] : base[key];
        }
        return merged;
    }

    // a skin's frame for a part: { source, hover, pressed, checked, disabled, slice: [l, t, r, b], padding }, or null
    function frame(part) {
        var frames = PanelTheme.skin && PanelTheme.skin.frames;
        return frames && frames[part] ? frames[part] : null;
    }
    // a skin's decorations on the panels: [{ source, at, offset: [x, y], size: [w, h] or [], stretch }]
    readonly property var decorations: (PanelTheme.skin && PanelTheme.skin.decorations) || []
    readonly property string schemeName: palette.name

    readonly property color background: palette.background
    readonly property color card: palette.card
    readonly property color cardPressed: palette.cardPressed
    readonly property color separator: palette.separator
    readonly property color text: palette.text
    readonly property color secondaryText: palette.secondaryText
    readonly property color tertiaryText: palette.tertiaryText
    readonly property color tint: palette.tint
    readonly property color on: palette.on              // a switch that is on
    readonly property color destructive: palette.destructive
    readonly property color fill: palette.fill          // fields, segmented tracks
    readonly property color segmentThumb: palette.segmentThumb
    readonly property color sliderTrack: palette.sliderTrack

    readonly property real largeTitleSize: 26 * unit
    readonly property real bodySize: 14 * unit
    readonly property real detailSize: 11.5 * unit
    readonly property real sectionSize: 11.5 * unit
    readonly property real controlTextSize: 12.5 * unit

    readonly property real pageMargin: 16 * unit
    readonly property real cardPadding: 14 * unit
    readonly property real cardRadius: 10 * unit
    readonly property real rowHeight: 40 * unit
    readonly property real rowPadding: 7 * unit
    readonly property real controlHeight: 26 * unit
    readonly property real gap: 10 * unit
    // a picker's open choices start this far past its label, so they read as its choices, not as the card's next rows
    readonly property real choiceIndent: 16 * unit

    // the build tools' label scrub (editor/ui/PropertyEditors.cpp): a few pixels before it starts; Shift coarse, Alt fine
    readonly property real scrubSlop: 3
    readonly property real scrubCoarse: 10
    readonly property real scrubFine: 0.1
}
