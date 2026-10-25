//
//  voxelBuilder.js
//  scripts/developer/utilities/tools
//
//  Created by Kevin Thomas, 2026.
//  Copyright 2026 Kevin Thomas.
//
//  A snapping voxel builder with a clickable screen-space HUD and a single
//  black five-mile ground cube. The ground is created only from the GROUND
//  button or Shift+G. Undo covers voxels placed this session; it cannot
//  restore an erased one. Placement is capped at 4000 voxels per session.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

/* global Camera, Controller, Entities, Overlays, Quat, Script, Window */

(function () {
    "use strict";

    // Default voxel edge length, in metres.
    var DEFAULT_VOXEL_SIZE = 0.25;

    // Half-size voxel edge length for furniture, in metres.
    var HALF_VOXEL_SIZE = 0.125;

    // Maximum number of voxels this session will place.
    var VOXEL_LIMIT = 4000;

    // Placement count at which the builder warns about the cap.
    var WARN_LIMIT = 3000;

    // Side length of the single black ground cube, in metres. Five miles.
    var GROUND_SIZE = 8046.72;

    // Name of the single ground entity.
    var GROUND_NAME = "Voxel Ground";

    // JSON user data that identifies a voxel by this tool.
    var VOXEL_TAG = JSON.stringify({ voxelBuilder: "voxel" });

    // JSON user data that identifies the ground by this tool.
    var GROUND_TAG = JSON.stringify({ voxelBuilder: "ground" });

    // Dimensions of the single black ground cube.
    var GROUND_DIMENSIONS = { x: GROUND_SIZE, y: 1.0, z: GROUND_SIZE };

    // Colour of the single black ground cube.
    var GROUND_COLOR = { red: 8, green: 8, blue: 10 };

    // Maximum number of placed voxels that undo can step back through.
    var HISTORY_LIMIT = 200;

    // Height of the HUD bar in pixels.
    var HUD_HEIGHT = 92;

    // Background colour of the HUD bar.
    var HUD_BAR = { red: 12, green: 14, blue: 22 };

    // First accent stripe colour, cyan.
    var HUD_ACCENT_A = { red: 0, green: 220, blue: 255 };

    // Second accent stripe colour, magenta.
    var HUD_ACCENT_B = { red: 255, green: 60, blue: 170 };

    // Colour of the title and button text.
    var HUD_TITLE_COLOR = { red: 255, green: 255, blue: 255 };

    // Colour of the live colour status text.
    var HUD_STATUS_COLOR = { red: 255, green: 220, blue: 120 };

    // Colour of the paused status text.
    var HUD_PAUSED_COLOR = { red: 150, green: 154, blue: 164 };

    // Left edge of the HUD content in pixels.
    var HUD_LEFT = 20;

    // Top of the hint row relative to the bar, in pixels.
    var HUD_HINT_Y = 40;

    // Top of the swatch and button row relative to the bar, in pixels.
    var HUD_ROW_Y = 66;

    // Height of one swatch or button in pixels.
    var HUD_ROW_H = 18;

    // Side length of one colour swatch in pixels.
    var HUD_SWATCH = 18;

    // Horizontal step between swatches in pixels.
    var HUD_SWATCH_STEP = 24;

    // Outline colour of an unselected swatch.
    var HUD_SWATCH_BORDER = { red: 40, green: 44, blue: 54 };

    // Palette entries used by the swatches and the placed voxels.
    var PALETTE = [
        { name: "red", red: 205, green: 55, blue: 45 },
        { name: "orange", red: 230, green: 130, blue: 45 },
        { name: "yellow", red: 238, green: 205, blue: 70 },
        { name: "green", red: 65, green: 170, blue: 90 },
        { name: "cyan", red: 55, green: 200, blue: 210 },
        { name: "blue", red: 55, green: 110, blue: 225 },
        { name: "purple", red: 140, green: 85, blue: 220 },
        { name: "pink", red: 230, green: 90, blue: 170 },
        { name: "white", red: 230, green: 230, blue: 230 },
        { name: "black", red: 18, green: 18, blue: 20 }
    ];

    // Control hints shown along the bottom of the HUD bar.
    var HINTS = [
        { text: "LMB place",
          color: { red: 90, green: 230, blue: 130 } },
        { text: "SHIFT LMB erase",
          color: { red: 255, green: 105, blue: 95 } },
        { text: "click a colour",
          color: { red: 255, green: 210, blue: 80 } },
        { text: "SHIFT G ground",
          color: { red: 190, green: 130, blue: 255 } },
        { text: "SHIFT U undo",
          color: { red: 255, green: 160, blue: 70 } }
    ];

    // Current colour, size, build, ground, HUD and undo state.
    var state = {
        color: 0,
        half: false,
        building: true,
        groundID: null,
        hudIDs: [],
        statusID: null,
        swatchIDs: [],
        buttons: [],
        sizeButton: null,
        buildButton: null,
        interval: null,
        hudTop: 0,
        hudHeight: 0,
        placed: 0,
        warned: false,
        history: []
    };

    /**
     * Show a short message at the top of the screen.
     *
     * @param {string} message - The message to show.
     * @returns {void}
     */
    function announce(message) {
        Window.displayAnnouncement(message);
    }

    /**
     * Return the current voxel edge length in metres.
     *
     * @returns {number} 0.25 normally, 0.125 when half size is on.
     */
    function currentSize() {
        return state.half ? HALF_VOXEL_SIZE : DEFAULT_VOXEL_SIZE;
    }

    /**
     * Return the centre of the voxel cell that contains a coordinate.
     *
     * @param {number} value - World coordinate on one axis.
     * @returns {number} The cell centre on that axis.
     */
    function snap(value) {
        var size = currentSize();
        return (Math.floor(value / size) + 0.5) * size;
    }

    /**
     * Cast a precise, visible-only pick ray through the mouse position.
     *
     * @param {object} event - The mouse event.
     * @returns {object} The findRayIntersection result.
     */
    function pick(event) {
        var ray = Camera.computePickRay(event.x, event.y);
        return Entities.findRayIntersection(ray, true, [], [], true);
    }

    /**
     * Return the surface normal of a hit, defaulting to straight up.
     *
     * @param {object} hit - A findRayIntersection result.
     * @returns {object} A unit normal vector.
     */
    function faceNormal(hit) {
        var normal = hit.surfaceNormal;
        if (!normal ||
            (normal.x === 0 && normal.y === 0 && normal.z === 0)) {
            return { x: 0, y: 1, z: 0 };
        }
        return normal;
    }

    /**
     * Move a point along a vector by a distance.
     *
     * @param {object} point - The starting point.
     * @param {object} direction - The direction vector.
     * @param {number} distance - The distance to move.
     * @returns {object} The moved point.
     */
    function offsetPoint(point, direction, distance) {
        return {
            x: point.x + direction.x * distance,
            y: point.y + direction.y * distance,
            z: point.z + direction.z * distance
        };
    }

    /**
     * Snap a point to the centre of a voxel cell.
     *
     * @param {object} point - The point to snap.
     * @returns {object} The snapped point.
     */
    function snapPoint(point) {
        return { x: snap(point.x), y: snap(point.y), z: snap(point.z) };
    }

    /**
     * Return the centre of the voxel that goes against a picked face.
     *
     * @param {object} hit - A findRayIntersection result.
     * @returns {object} The snapped centre for the new voxel.
     */
    function placeCentre(hit) {
        var normal = faceNormal(hit);
        var point = offsetPoint(hit.intersection, normal, currentSize() / 2.0);
        return snapPoint(point);
    }

    /**
     * Return the tool role stored in a userData JSON string.
     *
     * @param {string} userData - The entity's userData string.
     * @returns {string|null} "voxel", "ground", or null if not tagged.
     */
    function taggedRole(userData) {
        if (!userData) {
            return null;
        }
        try {
            var data = JSON.parse(userData);
            return data.voxelBuilder || null;
        } catch (error) {
            return null;
        }
    }

    /**
     * Find the single black ground cube by its userData tag.
     *
     * @returns {string|null} The ground entity ID, or null if it is missing.
     */
    function findGround() {
        var ids = Entities.findEntities({ x: 0, y: 0, z: 0 }, GROUND_SIZE);
        var data = Entities.getMultipleEntityProperties(ids, "userData");
        for (var i = 0; i < data.length; i++) {
            if (taggedRole(data[i].userData) === "ground") {
                return ids[i];
            }
        }
        return null;
    }

    /**
     * Return the properties of the black five-mile ground cube.
     *
     * @returns {object} The entity properties.
     */
    function groundProperties() {
        var props = { type: "Box", shape: "Cube", name: GROUND_NAME };
        props.position = { x: 0, y: -0.5, z: 0 };
        props.dimensions = GROUND_DIMENSIONS;
        props.color = GROUND_COLOR;
        props.userData = GROUND_TAG;
        props.collisionless = false;
        return props;
    }

    /**
     * Create the single black five-mile ground cube.
     *
     * @returns {string|null} The new ground entity ID, or null if cannot rez.
     */
    function createGround() {
        if (!Entities.canRez()) {
            announce("You cannot build in this world");
            return null;
        }
        state.groundID = Entities.addEntity(groundProperties());
        announce("Black five-mile ground created");
        return state.groundID;
    }

    /**
     * Delete the ground cube and forget its ID.
     *
     * @returns {void}
     */
    function removeGround() {
        Entities.deleteEntity(state.groundID);
        state.groundID = null;
        announce("Ground removed");
    }

    /**
     * Remove the ground if it exists, otherwise create it.
     *
     * @returns {void}
     */
    function toggleGround() {
        if (!state.groundID) {
            state.groundID = findGround();
        }
        if (state.groundID) {
            removeGround();
            return;
        }
        createGround();
    }

    /**
     * Return the properties of one fixed-size voxel cube.
     *
     * @param {object} position - The snapped voxel centre.
     * @param {object} colour - The palette entry to use.
     * @returns {object} The entity properties with collision on.
     */
    function voxelProperties(position, colour) {
        var props = { type: "Box", shape: "Cube", name: "Voxel" };
        props.position = position;
        props.dimensions = {
            x: currentSize(), y: currentSize(), z: currentSize()
        };
        props.color = {
            red: colour.red, green: colour.green, blue: colour.blue
        };
        props.userData = VOXEL_TAG;
        props.collisionless = false;
        return props;
    }

    /**
     * Add a placed voxel to the undo history.
     *
     * @param {string} entityID - The new voxel entity ID.
     * @returns {void}
     */
    function rememberVoxel(entityID) {
        state.history.push(entityID);
        if (state.history.length > HISTORY_LIMIT) {
            state.history.shift();
        }
    }

    /**
     * Return whether the builder may place or erase right now.
     *
     * @returns {boolean} True when building is on and rezzing is allowed.
     */
    function canEdit() {
        if (!state.building) {
            return false;
        }
        if (!Entities.canRez()) {
            announce("You cannot build in this world");
            return false;
        }
        return true;
    }

    /**
     * Return whether the builder may add another voxel.
     *
     * @returns {boolean} True when editing is allowed and under the cap.
     */
    function canPlace() {
        if (!canEdit()) {
            return false;
        }
        if (state.placed >= VOXEL_LIMIT) {
            announce("Voxel limit reached");
            return false;
        }
        return true;
    }

    /**
     * Warn once when the placement count reaches the warning threshold.
     *
     * @returns {void}
     */
    function warnLimit() {
        if (!state.warned && state.placed >= WARN_LIMIT) {
            state.warned = true;
            announce("3000 voxels placed; 1000 to the cap");
        }
    }

    /**
     * Add one voxel at a picked face and update the count.
     *
     * @param {object} hit - A findRayIntersection result.
     * @returns {void}
     */
    function placeAt(hit) {
        var properties = voxelProperties(placeCentre(hit),
                                         PALETTE[state.color]);
        var entityID = Entities.addEntity(properties);
        if (!entityID) {
            announce("Could not place voxel");
            return;
        }
        rememberVoxel(entityID);
        state.placed += 1;
        warnLimit();
    }

    /**
     * Place one voxel against the surface under the mouse.
     *
     * @param {object} event - The left mouse press event.
     * @returns {void}
     */
    function placeVoxel(event) {
        if (!canPlace()) {
            return;
        }
        var hit = pick(event);
        if (!hit.intersects) {
            announce("Aim at the ground or a voxel");
            return;
        }
        placeAt(hit);
    }

    /**
     * Return whether an entity was placed by this tool.
     *
     * @param {string} entityID - The entity ID.
     * @returns {boolean} True for a tagged voxel, false for anything else.
     */
    function isVoxel(entityID) {
        var properties = Entities.getEntityProperties(entityID, ["userData"]);
        return taggedRole(properties.userData) === "voxel";
    }

    /**
     * Delete a picked voxel if it belongs to this tool.
     *
     * @param {object} hit - A findRayIntersection result.
     * @returns {void}
     */
    function eraseHit(hit) {
        if (!isVoxel(hit.entityID)) {
            return;
        }
        Entities.deleteEntity(hit.entityID);
        if (state.placed > 0) {
            state.placed -= 1;
        }
    }

    /**
     * Delete the voxel under the mouse, leaving everything else alone.
     *
     * @param {object} event - The Shift + left mouse press event.
     * @returns {void}
     */
    function eraseVoxel(event) {
        if (!canEdit()) {
            return;
        }
        var hit = pick(event);
        if (hit.intersects) {
            eraseHit(hit);
        }
    }

    /**
     * Delete the most recently placed voxel.
     *
     * @returns {void}
     */
    function undo() {
        var entityID = state.history.pop();
        if (!entityID) {
            announce("Nothing to undo");
            return;
        }
        Entities.deleteEntity(entityID);
        if (state.placed > 0) {
            state.placed -= 1;
        }
        announce("Last voxel removed");
    }

    /**
     * Select a palette colour by index and refresh the HUD.
     *
     * @param {number} index - Palette index from 0 to 9.
     * @returns {void}
     */
    function setColor(index) {
        state.color = index;
        updateStatus();
        updateSwatches();
    }

    /**
     * Step to the next palette colour.
     *
     * @returns {void}
     */
    function nextColor() {
        setColor((state.color + 1) % PALETTE.length);
    }

    /**
     * Step to the previous palette colour.
     *
     * @returns {void}
     */
    function previousColor() {
        setColor((state.color + PALETTE.length - 1) % PALETTE.length);
    }

    /**
     * Toggle between the default and half voxel size.
     *
     * @returns {void}
     */
    function toggleSize() {
        state.half = !state.half;
        updateStatus();
        updateSizeButton();
    }

    /**
     * Toggle building on and off so clicks can go to the world.
     *
     * @returns {void}
     */
    function toggleBuilding() {
        state.building = !state.building;
        updateStatus();
        updateBuildButton();
        updateBarLook();
    }

    /**
     * Return the live status text for the HUD.
     *
     * @returns {string} The size and colour, or PAUSED.
     */
    function statusText() {
        if (!state.building) {
            return "PAUSED";
        }
        return currentSize() + " m   " +
               PALETTE[state.color].name.toUpperCase();
    }

    /**
     * Return whether two overlay or entity IDs are the same.
     *
     * @param {object} first - The first ID.
     * @param {object} second - The second ID.
     * @returns {boolean} True if both IDs have the same string form.
     */
    function sameId(first, second) {
        return String(first) === String(second);
    }

    /**
     * Return the properties of one 2D rectangle overlay.
     *
     * @param {object} bounds - Pixel bounds of the rectangle.
     * @param {object} color - Rectangle colour.
     * @param {number} alpha - Rectangle opacity.
     * @param {number} radius - Corner radius in pixels.
     * @returns {object} The rectangle overlay properties.
     */
    function panelProps(bounds, color, alpha, radius) {
        return {
            bounds: bounds,
            color: color,
            alpha: alpha,
            radius: radius
        };
    }

    /**
     * Add one 2D rectangle overlay and remember it for cleanup.
     *
     * @param {object} bounds - Pixel bounds of the rectangle.
     * @param {object} color - Rectangle colour.
     * @param {number} alpha - Rectangle opacity.
     * @param {number} radius - Corner radius in pixels.
     * @returns {string} The overlay ID.
     */
    function addPanel(bounds, color, alpha, radius) {
        var id = Overlays.addOverlay(
            "rectangle", panelProps(bounds, color, alpha, radius));
        state.hudIDs.push(id);
        return id;
    }

    /**
     * Return the properties of one 2D text overlay.
     *
     * @param {object} bounds - Pixel bounds of the text.
     * @param {string} text - The text to show.
     * @param {object} color - Text colour.
     * @param {number} size - Font size in pixels.
     * @returns {object} The text overlay properties.
     */
    function textProps(bounds, text, color, size) {
        var props = { bounds: bounds, text: text, color: color };
        props.backgroundAlpha = 0.0;
        props.font = { size: size };
        props.lineHeight = size + 2;
        props.leftMargin = 0;
        props.topMargin = 0;
        return props;
    }

    /**
     * Add one 2D text overlay to the HUD.
     *
     * @param {object} bounds - Pixel bounds of the text.
     * @param {string} text - The text to show.
     * @param {object} color - Text colour.
     * @param {number} size - Font size in pixels.
     * @returns {string} The overlay ID.
     */
    function addLabel(bounds, text, color, size) {
        var id = Overlays.addOverlay(
            "text", textProps(bounds, text, color, size));
        state.hudIDs.push(id);
        return id;
    }

    /**
     * Return a HUD y coordinate relative to the bottom bar.
     *
     * @param {number} offset - Offset from the top of the bar in pixels.
     * @returns {number} The screen y coordinate.
     */
    function hudY(offset) {
        return state.hudTop + offset;
    }

    /**
     * Create the title and live status labels.
     *
     * @param {number} width - Window width in pixels.
     * @returns {void}
     */
    function createHeader(width) {
        addLabel({ x: HUD_LEFT, y: hudY(8), width: 320, height: 30 },
                 "VOXEL BUILDER", HUD_TITLE_COLOR, 22);
        state.statusID = addLabel(
            { x: width - 320, y: hudY(10), width: 300, height: 26 },
            statusText(), HUD_STATUS_COLOR, 16);
    }

    /**
     * Create the five coloured control hints.
     *
     * @returns {void}
     */
    function createHints() {
        for (var i = 0; i < HINTS.length; i++) {
            addLabel({ x: HUD_LEFT + i * 150, y: hudY(HUD_HINT_Y),
                       width: 146, height: 22 },
                     HINTS[i].text, HINTS[i].color, 14);
        }
    }

    /**
     * Create the ten clickable palette swatches.
     *
     * @returns {void}
     */
    function createSwatches() {
        state.swatchIDs = [];
        for (var i = 0; i < PALETTE.length; i++) {
            var id = addPanel(
                { x: HUD_LEFT + i * HUD_SWATCH_STEP, y: hudY(HUD_ROW_Y),
                  width: HUD_SWATCH, height: HUD_SWATCH },
                PALETTE[i], 1.0, 3);
            state.swatchIDs.push(id);
        }
    }

    /**
     * Add one clickable HUD button with a label.
     *
     * @param {number} x - Left edge in pixels.
     * @param {number} width - Button width in pixels.
     * @param {string} text - Button text.
     * @param {object} color - Button colour.
     * @param {function} action - Function to run when clicked.
     * @returns {object} The button record.
     */
    function addButton(x, width, text, color, action) {
        var panel = addPanel({ x: x, y: hudY(HUD_ROW_Y), width: width,
                               height: HUD_ROW_H }, color, 1.0, 4);
        var label = addLabel({ x: x + 5, y: hudY(HUD_ROW_Y + 2),
                               width: width - 10, height: HUD_ROW_H - 4 },
                             text, HUD_TITLE_COLOR, 11);
        var button = { panel: panel, label: label, action: action };
        state.buttons.push(button);
        return button;
    }

    /**
     * Create the ground, undo, half-size and build buttons.
     *
     * @returns {void}
     */
    function createButtons() {
        addButton(256, 88, "GROUND", HUD_ACCENT_A, toggleGround);
        addButton(354, 70, "UNDO", HUD_ACCENT_B, undo);
        state.sizeButton = addButton(434, 70, "HALF",
                                     { red: 72, green: 154, blue: 92 },
                                     toggleSize);
        state.buildButton = addButton(514, 90, "BUILD ON",
                                      { red: 110, green: 78, blue: 176 },
                                      toggleBuilding);
        updateSizeButton();
        updateBuildButton();
    }

    /**
     * Create the dark bar and its two accent stripes.
     *
     * @param {number} width - Window width in pixels.
     * @returns {void}
     */
    function createBars(width) {
        addPanel({ x: 0, y: hudY(0), width: width, height: HUD_HEIGHT },
                 HUD_BAR, 0.92, 0);
        addPanel({ x: 0, y: hudY(0), width: width, height: 4 },
                 HUD_ACCENT_A, 1.0, 0);
        addPanel({ x: 0, y: hudY(4), width: width, height: 2 },
                 HUD_ACCENT_B, 1.0, 0);
    }

    /**
     * Create the full HUD bar, header, hints, swatches and buttons.
     *
     * @returns {void}
     */
    function createHud() {
        var width = Window.innerWidth;
        state.hudHeight = Window.innerHeight;
        state.hudTop = state.hudHeight - HUD_HEIGHT;
        createBars(width);
        createHeader(width);
        createHints();
        createSwatches();
        createButtons();
        updateStatus();
        updateSwatches();
        updateBarLook();
    }

    /**
     * Update the live size, colour or paused label.
     *
     * @returns {void}
     */
    function updateStatus() {
        if (!state.statusID) {
            return;
        }
        Overlays.editOverlay(state.statusID, {
            text: statusText(),
            color: state.building ? PALETTE[state.color] : HUD_PAUSED_COLOR
        });
    }

    /**
     * Highlight the selected swatch with a white outline.
     *
     * @returns {void}
     */
    function updateSwatches() {
        for (var i = 0; i < state.swatchIDs.length; i++) {
            var selected = i === state.color;
            Overlays.editOverlay(state.swatchIDs[i], {
                borderColor: selected ? HUD_TITLE_COLOR : HUD_SWATCH_BORDER,
                borderWidth: selected ? 2 : 1
            });
        }
    }

    /**
     * Outline the half-size button when it is active.
     *
     * @returns {void}
     */
    function updateSizeButton() {
        if (!state.sizeButton) {
            return;
        }
        Overlays.editOverlay(state.sizeButton.panel, {
            borderColor: state.half ? HUD_TITLE_COLOR : HUD_SWATCH_BORDER,
            borderWidth: state.half ? 2 : 1
        });
    }

    /**
     * Show the build state on its button.
     *
     * @returns {void}
     */
    function updateBuildButton() {
        if (!state.buildButton) {
            return;
        }
        Overlays.editOverlay(state.buildButton.label, {
            text: state.building ? "BUILD ON" : "BUILD OFF"
        });
        Overlays.editOverlay(state.buildButton.panel, {
            borderColor: state.building ? HUD_TITLE_COLOR : HUD_SWATCH_BORDER,
            borderWidth: state.building ? 2 : 1
        });
    }

    /**
     * Grey the bar and swatches while building is paused.
     *
     * @returns {void}
     */
    function updateBarLook() {
        if (state.hudIDs.length < 1) {
            return;
        }
        Overlays.editOverlay(state.hudIDs[0], {
            alpha: state.building ? 0.92 : 0.45
        });
        for (var i = 0; i < state.swatchIDs.length; i++) {
            Overlays.editOverlay(state.swatchIDs[i], {
                alpha: state.building ? 1.0 : 0.35
            });
        }
    }

    /**
     * Return the palette index of a swatch overlay, or -1.
     *
     * @param {object} overlayID - The clicked overlay ID.
     * @returns {number} The palette index, or -1 if not a swatch.
     */
    function swatchIndex(overlayID) {
        for (var i = 0; i < state.swatchIDs.length; i++) {
            if (sameId(state.swatchIDs[i], overlayID)) {
                return i;
            }
        }
        return -1;
    }

    /**
     * Return the action of a clicked button overlay, or null.
     *
     * @param {object} overlayID - The clicked overlay ID.
     * @returns {function|null} The button action, or null.
     */
    function buttonAction(overlayID) {
        for (var i = 0; i < state.buttons.length; i++) {
            var button = state.buttons[i];
            if (sameId(button.panel, overlayID) ||
                sameId(button.label, overlayID)) {
                return button.action;
            }
        }
        return null;
    }

    /**
     * Run the action of a clicked swatch or button.
     *
     * @param {object} overlayID - The clicked overlay ID.
     * @returns {void}
     */
    function handleHudClick(overlayID) {
        var index = swatchIndex(overlayID);
        if (index >= 0) {
            setColor(index);
            return;
        }
        var action = buttonAction(overlayID);
        if (action) {
            action();
        }
    }

    /**
     * Handle a left click on the HUD area.
     *
     * @param {object} event - The mouse press event.
     * @returns {void}
     */
    function hudClick(event) {
        var overlayID = Overlays.getOverlayAtPoint(
            { x: event.x, y: event.y });
        handleHudClick(overlayID);
    }

    /**
     * Fit one HUD bar overlay to the window width.
     *
     * @param {number} index - Overlay index in the HUD list.
     * @param {number} y - Offset from the bottom bar top in pixels.
     * @param {number} height - Height of the overlay in pixels.
     * @param {number} width - Window width in pixels.
     * @returns {void}
     */
    function layoutBar(index, y, height, width) {
        Overlays.editOverlay(state.hudIDs[index], {
            bounds: { x: 0, y: hudY(y), width: width, height: height }
        });
    }

    /**
     * Fit the HUD bars to the window width.
     *
     * @param {number} width - Window width in pixels.
     * @returns {void}
     */
    function layoutBars(width) {
        layoutBar(0, 0, HUD_HEIGHT, width);
        layoutBar(1, 0, 4, width);
        layoutBar(2, 4, 2, width);
    }

    /**
     * Move the status label to the right edge of the window.
     *
     * @param {number} width - Window width in pixels.
     * @returns {void}
     */
    function layoutStatus(width) {
        if (!state.statusID) {
            return;
        }
        Overlays.editOverlay(state.statusID, {
            bounds: { x: width - 320, y: hudY(10), width: 300, height: 26 }
        });
    }

    /**
     * Keep the HUD fitted to the window, rebuilding it when the height
     * changes.
     *
     * @returns {void}
     */
    function layoutHud() {
        if (Window.innerHeight !== state.hudHeight) {
            removeHud();
            createHud();
            return;
        }
        var width = Window.innerWidth;
        layoutBars(width);
        layoutStatus(width);
    }

    /**
     * Delete every HUD overlay and clear its bookkeeping.
     *
     * @returns {void}
     */
    function removeHud() {
        for (var i = 0; i < state.hudIDs.length; i++) {
            Overlays.deleteOverlay(state.hudIDs[i]);
        }
        state.hudIDs = [];
        state.statusID = null;
        state.swatchIDs = [];
        state.buttons = [];
        state.sizeButton = null;
        state.buildButton = null;
    }

    /**
     * Disconnect input, stop the timer and remove the HUD when the script
     * stops.
     *
     * @returns {void}
     */
    function cleanup() {
        if (state.interval) {
            Script.clearInterval(state.interval);
            state.interval = null;
        }
        removeHud();
        Controller.mousePressEvent.disconnect(onMouse);
        Controller.keyPressEvent.disconnect(onKey);
    }

    /**
     * Handle a left click on the HUD bar.
     *
     * @param {object} event - The mouse press event.
     * @returns {void}
     */
    function onHudMouse(event) {
        if (event.isLeftButton) {
            hudClick(event);
        }
    }

    /**
     * Handle a click in the world by placing or erasing a voxel.
     *
     * @param {object} event - The mouse press event.
     * @returns {void}
     */
    function onWorldMouse(event) {
        if (!event.isLeftButton) {
            return;
        }
        if (event.isShifted) {
            eraseVoxel(event);
            return;
        }
        placeVoxel(event);
    }

    /**
     * Route a mouse press to the HUD or to the world.
     *
     * @param {object} event - The mouse press event.
     * @returns {void}
     */
    function onMouse(event) {
        if (event.y >= state.hudTop) {
            onHudMouse(event);
            return;
        }
        onWorldMouse(event);
    }

    // Shift-key actions for the ground, undo and HUD.
    var SHIFT_ACTIONS = { g: toggleGround, u: undo };

    // Comma and period cycle the palette.
    var SYMBOL_ACTIONS = { ",": previousColor, ".": nextColor };

    // Key-code fallback for keyboards that send no text for punctuation.
    var KEY_ACTIONS = { 44: previousColor, 46: nextColor };

    /**
     * Return the action bound to a key and shift state.
     *
     * @param {string} text - The lower-case key text.
     * @param {boolean} shifted - Whether shift is held.
     * @returns {function|null} The action, or null.
     */
    function keyAction(text, shifted) {
        var table = shifted ? SHIFT_ACTIONS : SYMBOL_ACTIONS;
        return table[text] || null;
    }

    /**
     * Return the action bound to a raw key code.
     *
     * @param {number} code - The Qt key code.
     * @returns {function|null} The action, or null.
     */
    function keyCodeAction(code) {
        return KEY_ACTIONS[code] || null;
    }

    /**
     * Handle a key press without using any movement keys.
     *
     * @param {object} event - The key press event.
     * @returns {void}
     */
    function onKey(event) {
        var text = (event.text || "").toLowerCase();
        var action = keyAction(text, event.isShifted);
        if (!action) {
            action = keyCodeAction(event.key);
        }
        if (action) {
            action();
        }
    }

    /**
     * Find an existing ground, wire input and build the HUD.
     *
     * @returns {void}
     */
    function main() {
        state.groundID = findGround();
        createHud();
        Controller.mousePressEvent.connect(onMouse);
        Controller.keyPressEvent.connect(onKey);
        state.interval = Script.setInterval(layoutHud, 1000);
        Script.scriptEnding.connect(cleanup);
        announce("Voxel builder ready");
    }

    main();
}());
