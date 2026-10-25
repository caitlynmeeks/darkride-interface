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

    // Small separation used when testing whether two voxel boxes overlap.
    var OVERLAP_EPSILON = 0.0001;

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

    // Distance the ground is dropped below the voxel bottoms to avoid coplanar z-fighting.
    var GROUND_DROP = 0.01;

    // Colour of the single black ground cube.
    var GROUND_COLOR = { red: 8, green: 8, blue: 10 };

    // Maximum number of placed voxels that undo can step back through.
    var HISTORY_LIMIT = 200;

    // Height of the HUD bar in pixels.
    var HUD_HEIGHT = 114;

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

    // Colour of the erase-mode status text.
    var HUD_ERASE_COLOR = { red: 255, green: 145, blue: 135 };

    // Left edge of the HUD content in pixels.
    var HUD_LEFT = 20;

    // Number of palette swatches in each row.
    var HUD_SWATCH_COLUMNS = 12;

    // Top of the first palette row relative to the bar, in pixels.
    var HUD_SWATCH_Y = 40;

    // Vertical step between palette rows in pixels.
    var HUD_SWATCH_ROW_STEP = 24;

    // Top of the bottom control row, aligned with the last palette row.
    var HUD_ROW_Y = 88;

    // Top of the upper button row, aligned with the second palette row.
    var HUD_BUTTON_ROW_A = HUD_SWATCH_Y + HUD_SWATCH_ROW_STEP;

    // Height of one swatch or button in pixels.
    var HUD_ROW_H = 18;

    // Side length of one colour swatch in pixels.
    var HUD_SWATCH = 18;

    // Horizontal step between swatches in pixels.
    var HUD_SWATCH_STEP = 24;

    // Left edge of the button group, after the palette grid.
    var HUD_BUTTON_X = HUD_LEFT + HUD_SWATCH_COLUMNS * HUD_SWATCH_STEP;

    // Width of every control button in pixels.
    var HUD_BUTTON_W = 90;

    // Horizontal step between control buttons in pixels.
    var HUD_BUTTON_STEP = 100;

    // Dark control colours that keep the white labels readable.
    var BUTTON_COLORS = {
        ground: { red: 20, green: 90, blue: 110 },
        undo: { red: 120, green: 45, blue: 95 },
        half: { red: 45, green: 105, blue: 65 },
        build: { red: 80, green: 60, blue: 135 },
        erase: { red: 140, green: 45, blue: 40 }
    };

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
        { name: "black", red: 18, green: 18, blue: 20 },
        { name: "brown", red: 120, green: 72, blue: 40 },
        { name: "tan", red: 210, green: 180, blue: 140 },
        { name: "gold", red: 212, green: 175, blue: 55 },
        { name: "lime", red: 140, green: 200, blue: 60 },
        { name: "olive", red: 110, green: 120, blue: 50 },
        { name: "teal", red: 0, green: 128, blue: 128 },
        { name: "navy", red: 30, green: 50, blue: 110 },
        { name: "sky", red: 120, green: 190, blue: 235 },
        { name: "maroon", red: 110, green: 35, blue: 45 },
        { name: "gray", red: 128, green: 128, blue: 128 },
        { name: "light gray", red: 190, green: 190, blue: 190 },
        { name: "dark gray", red: 60, green: 60, blue: 65 },
        { name: "beige", red: 225, green: 210, blue: 180 },
        { name: "cream", red: 245, green: 240, blue: 220 },
        { name: "salmon", red: 235, green: 130, blue: 120 },
        { name: "coral", red: 240, green: 100, blue: 80 },
        { name: "magenta", red: 200, green: 40, blue: 160 },
        { name: "violet", red: 95, green: 70, blue: 180 },
        { name: "turquoise", red: 60, green: 200, blue: 180 },
        { name: "slate", red: 70, green: 85, blue: 105 },
        { name: "forest", red: 35, green: 100, blue: 55 },
        { name: "moss", red: 90, green: 130, blue: 70 },
        { name: "sand", red: 195, green: 170, blue: 120 },
        { name: "copper", red: 180, green: 100, blue: 60 },
        { name: "rose", red: 220, green: 150, blue: 170 },
        { name: "indigo", red: 75, green: 55, blue: 140 }
    ];

    // Current colour, size, build, ground, HUD and undo state.
    var state = {
        color: 0,
        half: false,
        building: true,
        erasing: false,
        groundID: null,
        hudIDs: [],
        statusID: null,
        swatchIDs: [],
        buttons: [],
        sizeButton: null,
        buildButton: null,
        eraseButton: null,
        interval: null,
        hudTop: 0,
        hudHeight: 0,
        placed: 0,
        warned: false,
        rightPress: null,
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
     * Drop an existing ground so its top sits just below the voxel bottoms.
     *
     * @returns {void}
     */
    function settleGround() {
        if (!state.groundID) { return; }
        var props = Entities.getEntityProperties(state.groundID, ["position", "dimensions"]);
        var top = props.position.y + props.dimensions.y / 2;
        if (Math.abs(top) > GROUND_DROP / 2) { return; }
        Entities.editEntity(state.groundID, {
            position: { x: props.position.x, y: props.position.y - GROUND_DROP, z: props.position.z }
        });
    }

    /**
     * Return the properties of the black five-mile ground cube.
     *
     * @returns {object} The entity properties.
     */
    function groundProperties() {
        var props = { type: "Box", shape: "Cube", name: GROUND_NAME };
        props.position = { x: 0, y: -0.5 - GROUND_DROP, z: 0 };
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
     * Return whether the builder may erase right now.
     *
     * @returns {boolean} True when rezzing is allowed, even when paused.
     */
    function canErase() {
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
     * Return whether two axis-aligned boxes overlap.
     *
     * @param {object} aPos - Centre of the first box.
     * @param {object} aSize - Dimensions of the first box.
     * @param {object} bPos - Centre of the second box.
     * @param {object} bSize - Dimensions of the second box.
     * @returns {boolean} True when the boxes overlap.
     */
    function boxesOverlap(aPos, aSize, bPos, bSize) {
        return Math.abs(aPos.x - bPos.x) < (aSize.x + bSize.x) / 2 - OVERLAP_EPSILON &&
               Math.abs(aPos.y - bPos.y) < (aSize.y + bSize.y) / 2 - OVERLAP_EPSILON &&
               Math.abs(aPos.z - bPos.z) < (aSize.z + bSize.z) / 2 - OVERLAP_EPSILON;
    }

    /**
     * Return whether a new voxel would overlap an existing tagged voxel.
     *
     * @param {object} position - The new voxel centre.
     * @param {number} size - The new voxel edge length.
     * @returns {boolean} True when the cell is already filled.
     */
    function isOccupied(position, size) {
        var ids = Entities.findEntities(position, DEFAULT_VOXEL_SIZE * 2);
        var props = Entities.getMultipleEntityProperties(ids, ["position", "dimensions", "userData"]);
        var boxSize = { x: size, y: size, z: size };
        for (var i = 0; i < props.length; i++) {
            if (taggedRole(props[i].userData) !== "voxel") { continue; }
            if (boxesOverlap(position, boxSize, props[i].position, props[i].dimensions)) { return true; }
        }
        return false;
    }

    /**
     * Add one voxel at a picked face if its cell is empty.
     *
     * @param {object} hit - A findRayIntersection result.
     * @returns {void}
     */
    function placeAt(hit) {
        var position = placeCentre(hit);
        if (isOccupied(position, currentSize())) {
            announce("That space is already filled");
            return;
        }
        addVoxel(position);
    }

    /**
     * Add one voxel and update the placement count.
     *
     * @param {object} position - The snapped voxel centre.
     * @returns {void}
     */
    function addVoxel(position) {
        var properties = voxelProperties(position, paletteColour());
        var entityID = Entities.addEntity(properties);
        if (!entityID) {
            announce("Could not place voxel");
            return;
        }
        keepVoxel(entityID, properties.color);
    }

    /**
     * Reinforce a new voxel's colour and add it to the history.
     *
     * @param {string} entityID - The new voxel entity ID.
     * @param {object} color - The voxel colour.
     * @returns {void}
     */
    function keepVoxel(entityID, color) {
        Entities.editEntity(entityID, { color: color });
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
        return properties && taggedRole(properties.userData) === "voxel";
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
        deleteVoxel(hit.entityID);
    }

    /**
     * Delete the tagged voxel under the mouse.
     *
     * @param {object} event - The mouse event to pick with.
     * @returns {void}
     */
    function eraseAt(event) {
        var hit = pick(event);
        if (hit.intersects) {
            eraseHit(hit);
        }
    }

    /**
     * Delete the voxel under the mouse, leaving everything else alone.
     *
     * @param {object} event - The Shift + left mouse press event.
     * @returns {void}
     */
    function eraseVoxel(event) {
        if (canErase()) {
            eraseAt(event);
        }
    }

    /**
     * Return whether two quaternions are effectively the same orientation.
     *
     * @param {object} first - The first quaternion.
     * @param {object} second - The second quaternion.
     * @returns {boolean} True when the dot product is near one.
     */
    function sameOrientation(first, second) {
        var dot = first.x * second.x + first.y * second.y +
                  first.z * second.z + first.w * second.w;
        return Math.abs(dot) > 0.9995;
    }

    /**
     * Remember the camera orientation when the right button goes down.
     *
     * @param {object} event - The right mouse press event.
     * @returns {void}
     */
    function rememberRightPress(event) {
        state.rightPress = {
            orientation: Camera.orientation,
            x: event.x,
            y: event.y
        };
    }

    /**
     * Return whether a right press and release was a click, not a look.
     *
     * @param {object} event - The right mouse release event.
     * @param {object} press - The remembered press details.
     * @returns {boolean} True for a click that did not turn or move.
     */
    function isRightClick(event, press) {
        if (!sameOrientation(press.orientation, Camera.orientation)) {
            return false;
        }
        return Math.abs(event.x - press.x) <= 4 &&
               Math.abs(event.y - press.y) <= 4;
    }

    /**
     * Erase on a right click that did not turn the camera.
     *
     * @param {object} event - The right mouse release event.
     * @returns {void}
     */
    function onMouseRelease(event) {
        if (!event.isRightButton || !state.rightPress) {
            return;
        }
        var press = state.rightPress;
        state.rightPress = null;
        if (canErase() && isRightClick(event, press)) {
            eraseAt(event);
        }
    }

    /**
     * Remove deleted entities from the undo history and return the next one.
     *
     * @returns {string|null} The last voxel that still exists, or null.
     */
    function takeLastVoxel() {
        while (state.history.length > 0) {
            var entityID = state.history.pop();
            if (isVoxel(entityID)) {
                return entityID;
            }
        }
        return null;
    }

    /**
     * Delete a voxel and its placement count.
     *
     * @param {string} entityID - The voxel entity ID.
     * @returns {void}
     */
    function deleteVoxel(entityID) {
        Entities.deleteEntity(entityID);
        if (state.placed > 0) {
            state.placed -= 1;
        }
    }

    /**
     * Delete the most recently placed voxel.
     *
     * @returns {void}
     */
    function undo() {
        var entityID = takeLastVoxel();
        if (!entityID) {
            announce("Nothing to undo");
            return;
        }
        deleteVoxel(entityID);
        announce("Last voxel removed");
    }

    /**
     * Select a palette colour by index and refresh the HUD.
     *
     * @param {number} index - Palette index.
     * @returns {void}
     */
    function setColor(index) {
        if (!isFinite(index) || index < 0 || index >= PALETTE.length) {
            index = 0;
        }
        state.color = index;
        updateStatus();
        updateSwatches();
    }

    /**
     * Return the selected palette colour, resetting an invalid index to red.
     *
     * @returns {object} A valid palette entry.
     */
    function paletteColour() {
        var colour = PALETTE[state.color];
        if (!colour) {
            state.color = 0;
            return PALETTE[0];
        }
        return colour;
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
        if (state.building) {
            state.erasing = false;
        }
        updateStatus();
        updateBuildButton();
        updateEraseButton();
        updateBarLook();
    }

    /**
     * Toggle erase mode so left clicks delete voxels.
     *
     * @returns {void}
     */
    function toggleErase() {
        state.erasing = !state.erasing;
        state.building = !state.erasing;
        updateStatus();
        updateBuildButton();
        updateEraseButton();
        updateBarLook();
        announce(state.erasing ? "Erase mode on" : "Erase mode off");
    }

    /**
     * Show the erase state on its button.
     *
     * @returns {void}
     */
    function updateEraseButton() {
        if (!state.eraseButton) {
            return;
        }
        Overlays.editOverlay(state.eraseButton.label, {
            text: state.erasing ? "ERASE ON" : "ERASE OFF"
        });
        Overlays.editOverlay(state.eraseButton.panel, {
            borderColor: state.erasing ? HUD_STATUS_COLOR : HUD_SWATCH_BORDER,
            borderWidth: state.erasing ? 2 : 1
        });
    }

    /**
     * Return the live status text for the HUD.
     *
     * @returns {string} The size and colour, or PAUSED.
     */
    function statusText() {
        if (state.erasing) {
            return "ERASE";
        }
        if (!state.building) {
            return "PAUSED";
        }
        return currentSize() + " m   " + paletteColour().name.toUpperCase();
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
     * Return the bounds of one palette swatch.
     *
     * @param {number} index - The palette index.
     * @returns {object} The overlay bounds.
     */
    function swatchBounds(index) {
        var column = index % HUD_SWATCH_COLUMNS;
        var row = Math.floor(index / HUD_SWATCH_COLUMNS);
        var x = HUD_LEFT + column * HUD_SWATCH_STEP;
        var y = HUD_SWATCH_Y + row * HUD_SWATCH_ROW_STEP;
        return { x: x, y: hudY(y), width: HUD_SWATCH, height: HUD_SWATCH };
    }

    /**
     * Create the clickable palette swatches in their rows.
     *
     * @returns {void}
     */
    function createSwatches() {
        state.swatchIDs = [];
        for (var i = 0; i < PALETTE.length; i++) {
            state.swatchIDs.push(addPanel(swatchBounds(i), PALETTE[i], 1.0, 3));
        }
    }

    /**
     * Add one clickable HUD button with a label.
     *
     * @param {number} x - Left edge in pixels.
     * @param {number} y - Top edge in pixels.
     * @param {number} width - Button width in pixels.
     * @param {string} text - Button text.
     * @param {object} color - Button colour.
     * @param {function} action - Function to run when clicked.
     * @returns {object} The button record.
     */
    function addButton(x, y, width, text, color, action) {
        var panel = addPanel({ x: x, y: hudY(y), width: width,
                               height: HUD_ROW_H }, color, 1.0, 4);
        var label = addLabel({ x: x + 5, y: hudY(y + 2),
                               width: width - 10, height: HUD_ROW_H - 4 },
                             text, HUD_TITLE_COLOR, 11);
        var button = { panel: panel, label: label, action: action };
        state.buttons.push(button);
        return button;
    }

    /**
     * Create the ground, undo and half-size buttons.
     *
     * @returns {void}
     */
    function createToolButtons() {
        addButton(HUD_BUTTON_X, HUD_BUTTON_ROW_A, HUD_BUTTON_W,
                  "GROUND", BUTTON_COLORS.ground, toggleGround);
        addButton(HUD_BUTTON_X + HUD_BUTTON_STEP, HUD_BUTTON_ROW_A, HUD_BUTTON_W,
                  "UNDO", BUTTON_COLORS.undo, undo);
        state.sizeButton = addButton(HUD_BUTTON_X + 2 * HUD_BUTTON_STEP,
                                     HUD_BUTTON_ROW_A, HUD_BUTTON_W,
                                     "HALF", BUTTON_COLORS.half, toggleSize);
    }

    /**
     * Create the build and erase mode buttons.
     *
     * @returns {void}
     */
    function createModeButtons() {
        state.buildButton = addButton(HUD_BUTTON_X + HUD_BUTTON_STEP / 2, HUD_ROW_Y,
                                      HUD_BUTTON_W, "BUILD ON", BUTTON_COLORS.build, toggleBuilding);
        state.eraseButton = addButton(HUD_BUTTON_X + HUD_BUTTON_STEP * 1.5, HUD_ROW_Y,
                                      HUD_BUTTON_W, "ERASE OFF", BUTTON_COLORS.erase, toggleErase);
    }

    /**
     * Create the HUD buttons and apply their initial state.
     *
     * @returns {void}
     */
    function createButtons() {
        createToolButtons();
        createModeButtons();
        updateSizeButton();
        updateBuildButton();
        updateEraseButton();
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
     * Record the window height and the top of the HUD bar.
     *
     * @returns {void}
     */
    function measureHud() {
        state.hudHeight = Window.innerHeight;
        state.hudTop = state.hudHeight - HUD_HEIGHT;
    }

    /**
     * Refresh the live HUD text, swatch outlines and bar tint.
     *
     * @returns {void}
     */
    function refreshHud() {
        updateStatus();
        updateSwatches();
        updateBarLook();
    }

    /**
     * Create the full HUD bar, header, swatches and buttons.
     *
     * @returns {void}
     */
    function createHud() {
        measureHud();
        var width = Window.innerWidth;
        createBars(width);
        createHeader(width);
        createSwatches();
        createButtons();
        refreshHud();
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
            color: state.erasing ? HUD_ERASE_COLOR :
                   state.building ? HUD_STATUS_COLOR : HUD_PAUSED_COLOR
        });
    }

    /**
     * Highlight the selected swatch with a yellow outline.
     *
     * @returns {void}
     */
    function updateSwatches() {
        for (var i = 0; i < state.swatchIDs.length; i++) {
            var selected = i === state.color;
            Overlays.editOverlay(state.swatchIDs[i], {
                borderColor: selected ? HUD_STATUS_COLOR : HUD_SWATCH_BORDER,
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
            borderColor: state.half ? HUD_STATUS_COLOR : HUD_SWATCH_BORDER,
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
            borderColor: state.building ? HUD_STATUS_COLOR : HUD_SWATCH_BORDER,
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
        var active = state.building || state.erasing;
        Overlays.editOverlay(state.hudIDs[0], {
            alpha: active ? 0.92 : 0.45
        });
        for (var i = 0; i < state.swatchIDs.length; i++) {
            Overlays.editOverlay(state.swatchIDs[i], {
                alpha: active ? 1.0 : 0.35
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
     * Disconnect the mouse and key handlers from the builder.
     *
     * @returns {void}
     */
    function disconnectInput() {
        Controller.mousePressEvent.disconnect(onMouse);
        Controller.mouseReleaseEvent.disconnect(onMouseRelease);
        Controller.keyPressEvent.disconnect(onKey);
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
        disconnectInput();
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
        if (event.isShifted || state.erasing) {
            eraseVoxel(event);
            return;
        }
        placeVoxel(event);
    }

    /**
     * Handle a world press by remembering a right press or placing with left.
     *
     * @param {object} event - The mouse press event.
     * @returns {void}
     */
    function onWorldPress(event) {
        if (event.isRightButton) {
            rememberRightPress(event);
            return;
        }
        onWorldMouse(event);
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
        onWorldPress(event);
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
     * Connect the mouse and key handlers to the builder.
     *
     * @returns {void}
     */
    function wireInput() {
        Controller.mousePressEvent.connect(onMouse);
        Controller.mouseReleaseEvent.connect(onMouseRelease);
        Controller.keyPressEvent.connect(onKey);
    }

    /**
     * Re-apply the saved colours to every voxel and ground after a reload.
     *
     * @returns {void}
     */
    function refreshVoxelColors() {
        var ids = Entities.findEntities({ x: 0, y: 0, z: 0 }, GROUND_SIZE);
        var props = Entities.getMultipleEntityProperties(ids, ["name", "color"]);
        for (var i = 0; i < ids.length; i++) {
            if (props[i].name === "Voxel" || props[i].name === GROUND_NAME) {
                Entities.editEntity(ids[i], { color: props[i].color });
            }
        }
    }

    /**
     * Schedule colour refreshes after the world has loaded.
     *
     * @returns {void}
     */
    function scheduleColorRefresh() {
        Script.setTimeout(refreshVoxelColors, 1500);
        Script.setTimeout(refreshVoxelColors, 5000);
    }

    /**
     * Find an existing ground, settle it, wire input and build the HUD.
     *
     * @returns {void}
     */
    function main() {
        state.groundID = findGround();
        settleGround();
        scheduleColorRefresh();
        createHud();
        wireInput();
        state.interval = Script.setInterval(layoutHud, 1000);
        Script.scriptEnding.connect(cleanup);
        announce("Voxel builder ready");
    }

    main();
}());
