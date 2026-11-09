//
//  EditorTerrain.cpp
//  interface/src/editor
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "EditorTerrain.h"

#include <algorithm>
#include <cmath>

#include <QtCore/QByteArray>
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtGui/QImage>

#include <EntityItemID.h>
#include <EntityItemProperties.h>
#include <EntityScriptingInterface.h>
#include <EntityTree.h>
#include <EntityTreeRenderer.h>
#include <GLMHelpers.h>
#include <NumericalConstants.h>
#include <PickFilter.h>
#include <RenderableTerrainEntityItem.h>
#include <SharedUtil.h>
#include <terrain/Resample.h>
#include <terrain/TerrainFile.h>

#include "Application.h"
#include "EditorCreate.h"
#include "InterfaceLogging.h"

namespace {
    const float RAISE_RATE = 6.0f;          // voxels a second at full strength: the shift for Raise and Lower, the sphere's pace for Dig and Fill
    const float BLEND_RATE = 4.0f;          // blending a second at full strength, for Smooth and Flatten
    const float STEP_MIN = 0.2f;            // the smallest step applied: below it the byte distances lose the falloff
    const float REAPPLY_FRACTION = 0.25f;   // Dig, Fill and the paint modes go again once the pointer has moved this much of the radius
    const float CUT_MARGIN = 1.0f;          // voxels: a mesh's vertices sit within a voxel of the field's surface, so a hit this near one of the stroke's spheres is on it
    const float MIN_VOXEL_RADIUS = 0.75f;   // a brush smaller than a voxel would change nothing
    const float MIN_PAINT_RADIUS_TEXELS = 0.75f;
    // Paint lays the paint map where the ground's normal rises at least this far, the middle of the shader's fade
    // (terrain.slf: PAINT_FACING_LOW 0.3 to PAINT_FACING_HIGH 0.6); on steeper ground, where the map hardly shows,
    // and below the top surface, it paints the voxels' materials instead
    const float PAINT_MAP_FACING = 0.45f;
    const int TERRAIN_SEARCH_FRAMES = 30;   // how often the world's terrains are looked for
    const float TERRAIN_SEARCH_RADIUS = 4000.0f;
    const int OUTLINE_SEGMENTS = 48;
    const int OUTLINE_MASK_SAMPLES = 32;    // a shape picture's outline is traced on this many samples a side
    const float OUTLINE_MASK_LEVEL = 0.5f;  // along the grey where the picture crosses this
    const float OUTLINE_WIDTH_PIXELS = 2.0f;
    const glm::vec4 OUTLINE_COLOR(0.45f, 0.75f, 1.0f, 1.0f);
    const int SAVE_DELAY_MS = 2500;         // after the last change, before the file goes up
    const int SAVED_NOTE_MS = 5000;
    const int FAILED_NOTE_MS = 12000;
    const QString TERRAIN_FOLDER = QStringLiteral("/terrain");
    const QString TERRAIN_MIME_TYPE = QStringLiteral("application/octet-stream");
    // the most a resample makes, at level 0 throughout (finer detail comes from chunks split under small brushes):
    // a grid of this many voxels is already tens of megabytes of chunks and a long first meshing
    const int64_t MAX_RESAMPLE_VOXELS = 96LL * 1000 * 1000;
    const int MAX_RESAMPLE_AXIS = 2048;

    QString countText(const glm::ivec3& count) {
        return QStringLiteral("%1 × %2 × %3").arg(count.x).arg(count.y).arg(count.z);
    }

    QString metresText(float metres) {
        return QString::number(metres, 'f', metres < 0.1f ? 3 : 2) + QStringLiteral(" m");
    }

    // the world's visible terrains, picked against their ground
    PickFilter terrainPickFilter() {
        PickFilter::Flags flags;
        flags.set(PickFilter::DOMAIN_ENTITIES);
        flags.set(PickFilter::VISIBLE);
        flags.set(PickFilter::COLLIDABLE);
        flags.set(PickFilter::NONCOLLIDABLE);
        flags.set(PickFilter::PRECISE);
        return PickFilter(flags);
    }

    EntityTreePointer entityTree() {
        auto renderer = qApp->getEntities();
        return renderer ? renderer->getTree() : nullptr;
    }

    // Dig and Fill cut the brush's sphere out of the ground, or add it, a whole sphere a step
    bool isSphere(terrain::SculptMode mode) {
        return mode == terrain::SculptMode::Dig || mode == terrain::SculptMode::Fill;
    }

    // Whether the ray (in voxel units) enters one of the spheres before, or within the margin of, the point it hit at
    // hitDistance along it: the pointer is then over what the stroke has cut, not over fresh ground. A sphere behind
    // the ray's origin does not count.
    bool rayThroughSpheres(const glm::vec3& origin, const glm::vec3& direction, float hitDistance,
                           const std::vector<glm::vec4>& spheres, float margin) {
        for (const glm::vec4& sphere : spheres) {
            const glm::vec3 toOrigin = origin - glm::vec3(sphere);
            const float b = glm::dot(toOrigin, direction);
            const float discriminant = b * b - (glm::dot(toOrigin, toOrigin) - sphere.w * sphere.w);
            if (discriminant < 0.0f) {
                continue;   // the ray misses it
            }
            const float root = std::sqrt(discriminant);
            if (-b + root >= 0.0f && -b - root <= hitDistance + margin) {
                return true;
            }
        }
        return false;
    }

    bool isBlending(terrain::SculptMode mode) {
        return mode == terrain::SculptMode::Smooth || mode == terrain::SculptMode::Flatten;
    }

    // A stroke's step leaves its record of chunks with only those it changed: a chunk the same before and after goes
    // from both (chunks split or merged keep their keys on one side only, so they stay)
    void keepChangedChunks(TerrainChunks& before, TerrainChunks& after) {
        for (auto it = before.begin(); it != before.end();) {
            auto same = after.find(it->first);
            if (same != after.end() && same->second == it->second) {
                after.erase(same);
                it = before.erase(it);
            } else {
                ++it;
            }
        }
    }

    terrain::SculptMode invert(terrain::SculptMode mode) {
        switch (mode) {
            case terrain::SculptMode::Raise: return terrain::SculptMode::Lower;
            case terrain::SculptMode::Lower: return terrain::SculptMode::Raise;
            case terrain::SculptMode::Dig: return terrain::SculptMode::Fill;
            case terrain::SculptMode::Fill: return terrain::SculptMode::Dig;
            default: return mode;
        }
    }

    // The brush's shape pictures, by path, kept once read: a picture's grey (its alpha counted in) as the mask's
    // values; null when the path is not a readable picture.
    std::shared_ptr<const terrain::BrushMask> loadBrushMask(const QString& path) {
        static QHash<QString, std::shared_ptr<const terrain::BrushMask>> masks;
        auto found = masks.find(path);
        if (found != masks.end()) {
            return found.value();
        }
        std::shared_ptr<const terrain::BrushMask> mask;
        QImage image(path);
        if (!image.isNull() && image.width() > 0 && image.height() > 0) {
            image = image.convertToFormat(QImage::Format_ARGB32);
            std::vector<float> values((size_t)image.width() * image.height());
            for (int y = 0; y < image.height(); ++y) {
                const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
                for (int x = 0; x < image.width(); ++x) {
                    const QRgb pixel = row[x];
                    const float grey = (0.299f * qRed(pixel) + 0.587f * qGreen(pixel) + 0.114f * qBlue(pixel)) / 255.0f;
                    values[(size_t)y * image.width() + x] = grey * (float)qAlpha(pixel) / 255.0f;
                }
            }
            mask = std::make_shared<terrain::BrushMask>(image.width(), image.height(), std::move(values));
            if (mask->isEmpty()) {
                mask.reset();
            }
        }
        masks.insert(path, mask);
        return mask;
    }

    QString modeName(terrain::SculptMode mode) {
        switch (mode) {
            case terrain::SculptMode::Raise: return QStringLiteral("Raise");
            case terrain::SculptMode::Lower: return QStringLiteral("Lower");
            case terrain::SculptMode::Smooth: return QStringLiteral("Smooth");
            case terrain::SculptMode::Flatten: return QStringLiteral("Flatten");
            case terrain::SculptMode::Dig: return QStringLiteral("Dig");
            case terrain::SculptMode::Fill: return QStringLiteral("Fill");
            case terrain::SculptMode::Paint: return QStringLiteral("Paint");
            case terrain::SculptMode::PaintVoxels: return QStringLiteral("Paint Voxels");
        }
        return QStringLiteral("Sculpt");
    }
}

EditorTerrain::EditorTerrain(EditorUndo* undo, QObject* parent) : QObject(parent), _undo(undo) {
    _saveTimer.setSingleShot(true);
    _saveTimer.setInterval(SAVE_DELAY_MS);
    connect(&_saveTimer, &QTimer::timeout, this, &EditorTerrain::saveDirty);
}

void EditorTerrain::setBrush(const EditorTerrainBrush& brush) {
    if (brush.randomAngle && !_brush.randomAngle) {
        _randomAngle = randFloatInRange(0.0f, 360.0f);
    }
    _brush = brush;
    if (brush.shape != _maskShape) {
        _maskShape = brush.shape;
        _mask = brush.shape.isEmpty() ? nullptr : loadBrushMask(brush.shape);
        if (!brush.shape.isEmpty() && !_mask) {
            emit status(QStringLiteral("Brush shape not readable: ") + brush.shape, FAILED_NOTE_MS);
        }
    }
    if (_hasLastHit && !_stroking) {
        if (auto entity = terrainEntity(_lastHit.id)) {
            showOutline(*entity, _lastHit);
        }
    }
}

void EditorTerrain::setView(const glm::vec3& cameraPosition, float pixelSize) {
    _cameraPosition = cameraPosition;
    _pixelSize = pixelSize;
}

// --- finding the ground -------------------------------------------------------------------------

void EditorTerrain::findTerrains() {
    _searchFrame = _frameCounter;
    auto tree = entityTree();
    if (!tree) {
        _terrains.clear();
        return;
    }
    QVector<QUuid> found;
    tree->withReadLock([&] {
        tree->evalEntitiesInSphereWithType(_cameraPosition, TERRAIN_SEARCH_RADIUS, EntityTypes::Terrain, terrainPickFilter(), found);
    });
    _terrains = found;
}

std::shared_ptr<RenderableTerrainEntityItem> EditorTerrain::terrainEntity(const QUuid& id) const {
    auto tree = entityTree();
    if (!tree) {
        return nullptr;
    }
    return std::dynamic_pointer_cast<RenderableTerrainEntityItem>(tree->findEntityByID(id));
}

bool EditorTerrain::pick(const PickRay& ray, Hit& hit) {
    if (!ray) {
        return false;
    }
    if (_terrains.isEmpty()) {
        findTerrains();
        if (_terrains.isEmpty()) {
            return false;
        }
    }
    QVector<EntityItemID> include;
    for (const auto& id : _terrains) {
        include << EntityItemID(id);
    }
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    auto result = entities->evalRayIntersectionVector(ray, terrainPickFilter(), include, QVector<EntityItemID>());
    if (!result.intersects) {
        return false;
    }
    auto entity = terrainEntity(result.entityID);
    if (!entity || !entity->hasGrid()) {
        return false;
    }
    hit.id = result.entityID;
    hit.worldPoint = result.intersection;
    hit.voxelPoint = glm::vec3(entity->worldToVoxelMatrix() * glm::vec4(hit.worldPoint, 1.0f));
    // a normal goes to voxel space by the transpose of the way voxel points go to the world
    const glm::vec3 normal = glm::transpose(glm::mat3(entity->voxelToWorldMatrix())) * result.surfaceNormal;
    const float length = glm::length(normal);
    hit.normal = length > 1.0e-12f ? normal / length : glm::vec3(0.0f, 1.0f, 0.0f);
    return true;
}

bool EditorTerrain::pointsAtTerrain(const PickRay& ray) {
    Hit hit;
    return pick(ray, hit);
}

bool EditorTerrain::hover(const PickRay& ray) {
    if (_stroking) {
        return true;
    }
    Hit hit;
    auto entity = pick(ray, hit) ? terrainEntity(hit.id) : nullptr;
    if (!entity) {
        const bool left = _hasLastHit;
        _hasLastHit = false;
        _outline.clear();
        if (left) {
            emit terrainUnderPointerChanged();
        }
        return false;
    }
    const bool another = !_hasLastHit || hit.id != _lastHit.id;
    showOutline(*entity, hit);
    if (another) {
        emit terrainUnderPointerChanged();
    }
    return true;
}

QStringList EditorTerrain::layerNamesForTool(const QList<QUuid>& selected) const {
    const terrain::LayerStack* stack = nullptr;
    for (const QUuid& id : selected) {
        if ((stack = layersOf(id))) {
            break;
        }
    }
    if (!stack && _hasLastHit) {
        stack = layersOf(_lastHit.id);
    }
    const terrain::LayerStack defaults = stack ? terrain::LayerStack() : terrain::LayerStack::defaults();
    if (!stack) {
        stack = &defaults;
    }
    QStringList names;
    for (int slot = 0; slot < terrain::MAX_LAYERS; ++slot) {
        const terrain::Layer* layer = stack->find((uint8_t)slot);
        names << (layer ? QString::fromStdString(layer->name) : QString());
    }
    return names;
}

// --- the brush ----------------------------------------------------------------------------------

terrain::Brush EditorTerrain::voxelBrush(const RenderableTerrainEntityItem& entity, const Hit& hit) const {
    const glm::vec3 voxelScale = entity.getVoxelScale();
    const float metresPerVoxel = std::max((voxelScale.x + voxelScale.y + voxelScale.z) / 3.0f, 1.0e-4f);
    terrain::Brush brush;
    brush.center = hit.voxelPoint;
    brush.radius = std::max(MIN_VOXEL_RADIUS, _brush.radius / metresPerVoxel);
    brush.hardness = glm::clamp(_brush.hardness, 0.0f, 1.0f);
    brush.material = _stroking ? _strokeMaterial : (uint8_t)glm::clamp(_brush.material, 0, 255);
    brush.flattenHeight = _flattenHeight;
    brush.mask = _mask.get();
    brush.angle = glm::radians(angle());
    return brush;
}

terrain::PaintBrush EditorTerrain::paintBrush(const RenderableTerrainEntityItem& entity, const Hit& hit) const {
    const glm::vec3 voxelScale = entity.getVoxelScale();
    const float metresPerVoxel = std::max((voxelScale.x + voxelScale.z) / 2.0f, 1.0e-4f);
    const float texel = 1.0f / (float)std::max(entity.getPaint().getTexelsPerVoxel(), 1);
    terrain::PaintBrush brush;
    brush.center = glm::vec2(hit.voxelPoint.x, hit.voxelPoint.z);
    brush.radius = std::max(MIN_PAINT_RADIUS_TEXELS * texel, _brush.radius / metresPerVoxel);
    brush.strength = glm::clamp(_brush.strength, 0.0f, 1.0f);
    brush.hardness = glm::clamp(_brush.hardness, 0.0f, 1.0f);
    brush.layer = (uint8_t)glm::clamp(_brush.material, 0, terrain::PAINT_LAYERS - 1);
    brush.mask = _mask.get();
    brush.angle = glm::radians(angle());
    return brush;
}

void EditorTerrain::captureBeforePaint(const RenderableTerrainEntityItem& entity, const terrain::PaintBrush& brush) {
    const terrain::PaintMap& paint = entity.getPaint();
    glm::ivec2 low, high;
    if (!terrain::brushTexelBounds(paint, brush, low, high)) {
        return;
    }
    const int texelsPerVoxel = std::max(paint.getTexelsPerVoxel(), 1);
    const glm::ivec2 columnLow = terrain::PaintMap::columnOf(low / texelsPerVoxel);
    const glm::ivec2 columnHigh = terrain::PaintMap::columnOf(high / texelsPerVoxel);
    for (int z = columnLow.y; z <= columnHigh.y; ++z) {
        for (int x = columnLow.x; x <= columnHigh.x; ++x) {
            const glm::ivec2 column(x, z);
            if (_beforePaint.find(column) == _beforePaint.end()) {
                glm::ivec2 blockLow, blockHigh;
                paint.columnTexels(column, blockLow, blockHigh);
                terrain::PaintBlock block = paint.copy(blockLow, blockHigh);
                if (!block.isEmpty()) {
                    _beforePaint.emplace(column, std::move(block));
                }
            }
        }
    }
}

void EditorTerrain::captureBefore(const RenderableTerrainEntityItem& entity, const terrain::Brush& brush) {
    const terrain::VoxelGrid& grid = entity.getGrid();
    glm::ivec3 low, high;
    if (!terrain::brushBounds(grid, brush, low, high)) {
        return;
    }
    // A small brush splits the chunks under it, and the 2:1 rule (VoxelGrid.h) can split the level-0 places around
    // those, one ring for each level beyond the first; the stroke records whole places, so a step can put back
    // whatever the split made of them.
    const int reach = std::max(grid.getMaxLevel() - 1, 0) * terrain::CHUNK_SIZE;
    const glm::ivec3 slotLow = terrain::VoxelGrid::chunkOf(low - reach);
    const glm::ivec3 slotHigh = terrain::VoxelGrid::chunkOf(high + reach);
    for (int z = slotLow.z; z <= slotHigh.z; ++z) {
        for (int y = slotLow.y; y <= slotHigh.y; ++y) {
            for (int x = slotLow.x; x <= slotHigh.x; ++x) {
                const glm::ivec3 slot(x, y, z);
                if (!grid.containsChunk(slot) || !_strokeSlots.insert(slot).second) {
                    continue;
                }
                const glm::ivec3 origin = slot * terrain::CHUNK_SIZE;
                for (const terrain::ChunkKey& key : grid.chunksCovering(origin, origin + terrain::CHUNK_SIZE - 1)) {
                    _before.emplace(key, grid.getChunk(key));
                }
            }
        }
    }
}

TerrainChunks EditorTerrain::captureAfter(const RenderableTerrainEntityItem& entity) const {
    const terrain::VoxelGrid& grid = entity.getGrid();
    TerrainChunks after;
    for (const glm::ivec3& slot : _strokeSlots) {
        const glm::ivec3 origin = slot * terrain::CHUNK_SIZE;
        for (const terrain::ChunkKey& key : grid.chunksCovering(origin, origin + terrain::CHUNK_SIZE - 1)) {
            after.emplace(key, grid.getChunk(key));
        }
    }
    return after;
}

// --- the stroke ---------------------------------------------------------------------------------

bool EditorTerrain::beginStroke(const PickRay& ray, bool inverted, QString& refusal) {
    refusal.clear();
    if (_stroking) {
        endStroke(false);
    }
    Hit hit;
    if (!pick(ray, hit)) {
        return false;
    }
    if (EditorUndo::isLocked(hit.id)) {
        refusal = QStringLiteral("The terrain is locked");
        return false;
    }
    _stroking = true;
    _strokeId = hit.id;
    _ray = ray;
    _hasRay = true;
    _strokeMode = inverted ? invert(_brush.mode) : _brush.mode;
    // Shift swaps Paint and Erase; with Paint Voxels (as its Erase box does) it writes the bottom layer's material,
    // as Erase does where it falls through to the voxels
    _erasing = (_brush.mode == terrain::SculptMode::Paint || _brush.mode == terrain::SculptMode::PaintVoxels) &&
               _brush.erase != inverted;
    _strokeMaterial = (uint8_t)glm::clamp(_brush.material, 0, 255);
    if ((_strokeMode == terrain::SculptMode::PaintVoxels || _strokeMode == terrain::SculptMode::Paint) && _erasing) {
        if (auto entity = terrainEntity(_strokeId)) {
            const std::vector<uint8_t> order = entity->getLayers().order();
            if (!order.empty()) {
                _strokeMaterial = order.front();
            }
        }
    }
    _flattenHeight = hit.voxelPoint.y;
    _accumulated = STEP_MIN;   // the first step lands at once: a click alone does something
    _steps = 0;
    _changed = 0;
    _before.clear();
    _beforePaint.clear();
    _spheres.clear();
    _sphereAdvance = 0.0f;
    update(0.0f);
    return true;
}

void EditorTerrain::moveStroke(const PickRay& ray) {
    _ray = ray;
    _hasRay = true;
}

void EditorTerrain::update(float deltaTime) {
    ++_frameCounter;
    if (_searchFrame < 0 || _frameCounter - _searchFrame >= TERRAIN_SEARCH_FRAMES) {
        findTerrains();
    }
    if (!_stroking) {
        return;
    }
    auto entity = terrainEntity(_strokeId);
    if (!entity || !entity->hasGrid()) {
        dropStroke();   // the terrain went while it was being shaped
        return;
    }
    Hit hit;
    if (!_hasRay || !pick(_ray, hit) || hit.id != _strokeId) {
        return;         // the pointer is off the terrain: the stroke waits for it to come back
    }
    if (_strokeMode == terrain::SculptMode::Paint) {
        // The paint map is a picture seen from the sky: it shows on the ground the sky sees, where that faces up, and
        // down the walls near it (a raised terrace's sides, within terrain::PAINT_WALL_REACH of the top). Below that
        // (a cave's floor or walls, the ground under an overhang: terrain::TopSurface), and on ground facing down,
        // the stroke paints the voxels' materials instead, the layer's (Erase: the bottom layer's), as Paint Voxels
        // does, so it shows wherever it lands. The shader draws the same line.
        entity->updateTopSurface();
        const glm::vec3 terrainNormal = glm::normalize(hit.normal / entity->getVoxelScale());
        const bool steep = terrainNormal.y < PAINT_MAP_FACING;
        const bool onMap = terrainNormal.y >= 0.0f &&
            entity->getTopSurface().isOnTop(hit.voxelPoint, steep ? terrain::PAINT_WALL_REACH : terrain::TOP_SURFACE_TOLERANCE);
        if (onMap) {
            // the paint map: one pass of the brush, again once the pointer has moved on a little
            const terrain::PaintBrush brush = paintBrush(*entity, hit);
            if (_steps > 0 && glm::distance(hit.voxelPoint, _lastStepPoint) < brush.radius * REAPPLY_FRACTION) {
                showOutline(*entity, hit);
                return;
            }
            captureBeforePaint(*entity, brush);
            const int changed = terrain::paint(entity->getPaint(), brush, _erasing);
            if (changed > 0) {
                _changed += changed;
                entity->paintEdited();
            }
        } else {
            terrain::Brush brush = voxelBrush(*entity, hit);
            if (_steps > 0 && glm::distance(hit.voxelPoint, _lastStepPoint) < brush.radius * REAPPLY_FRACTION) {
                showOutline(*entity, hit);
                return;
            }
            brush.strength = glm::clamp(_brush.strength, 0.0f, 1.0f);
            brush.seed = _voxelPaintSeed++;
            captureBefore(*entity, brush);
            if (terrain::sculpt(entity->getGrid(), brush, terrain::SculptMode::PaintVoxels) > 0) {
                entity->gridEdited();
            }
        }
        _lastStepPoint = hit.voxelPoint;
        ++_steps;
        showOutline(*entity, hit);
        return;
    }
    terrain::Brush brush = voxelBrush(*entity, hit);
    if (_strokeMode == terrain::SculptMode::PaintVoxels) {
        // one pass of the brush, again once the pointer has moved on a little
        if (_steps > 0 && glm::distance(hit.voxelPoint, _lastStepPoint) < brush.radius * REAPPLY_FRACTION) {
            showOutline(*entity, hit);
            return;
        }
        brush.strength = glm::clamp(_brush.strength, 0.0f, 1.0f);
        brush.seed = _voxelPaintSeed++;
    } else if (isSphere(_strokeMode)) {
        // Dig and Fill: the sphere starts a radius short of the ground under the pointer, above it along the ground's
        // normal (Fill: below), and moves toward the point under the pointer at the pace the strength sets, no
        // further than that point. While the pointer stays over the stroke's own cut that point is the cut's far
        // wall, so a hold sinks the sphere in and then bores a tunnel (raises a column) at the pace, rather than a
        // sphere a frame, which the pick chasing the ground into each new sphere used to give. Over fresh ground the
        // sphere moves with the pointer at once, as far in as it has got, at most to its middle. Moving on brings a
        // step even before the pace has one due, so a drag at a low strength is not a row of dots.
        const glm::mat4 worldToVoxel = entity->worldToVoxelMatrix();
        const glm::vec3 origin = glm::vec3(worldToVoxel * glm::vec4(_ray.origin, 1.0f));
        const glm::vec3 direction = glm::normalize(glm::mat3(worldToVoxel) * _ray.direction);
        const float sign = _strokeMode == terrain::SculptMode::Dig ? 1.0f : -1.0f;   // into the ground, or out of it
        _accumulated += deltaTime * RAISE_RATE * glm::clamp(_brush.strength, 0.0f, 1.0f);
        const bool moved = _steps == 0 || glm::distance(hit.voxelPoint, _lastStepPoint) >= brush.radius * REAPPLY_FRACTION;
        if (_accumulated < STEP_MIN && !moved) {
            showOutline(*entity, hit);
            return;
        }
        const float step = _accumulated;
        _accumulated = 0.0f;
        const float hitDistance = glm::dot(hit.voxelPoint - origin, direction);
        if (_steps > 0 && rayThroughSpheres(origin, direction, hitDistance, _spheres, CUT_MARGIN)) {
            // over the cut: toward the point under the pointer at the pace; what goes into the ground there (out of
            // it, for Fill) counts as how far in the sphere has got
            const glm::vec3 toHit = hit.voxelPoint - _sphereCenter;
            const float distance = glm::length(toHit);
            if (distance > 1.0e-6f) {
                const glm::vec3 move = toHit * (std::min(distance, step) / distance);
                _sphereCenter += move;
                _sphereAdvance = std::max(0.0f, _sphereAdvance - sign * glm::dot(move, hit.normal));
            }
        } else {
            // fresh ground: under the pointer, as far in as the sphere has got
            _sphereAdvance += step;
            _sphereCenter = hit.voxelPoint + hit.normal * (sign * (brush.radius - std::min(_sphereAdvance, brush.radius)));
        }
        brush.center = _sphereCenter;
        brush.strength = 1.0f;
        brush.mask = nullptr;   // the sphere, whatever the shape
        _spheres.emplace_back(_sphereCenter, brush.radius);
    } else {
        _accumulated += deltaTime * (isBlending(_strokeMode) ? BLEND_RATE : RAISE_RATE) * glm::clamp(_brush.strength, 0.0f, 1.0f);
        if (_accumulated < STEP_MIN) {
            showOutline(*entity, hit);
            return;
        }
        brush.strength = isBlending(_strokeMode) ? std::min(_accumulated, 1.0f) : _accumulated;
        _accumulated = 0.0f;
    }
    captureBefore(*entity, brush);
    const int changed = terrain::sculpt(entity->getGrid(), brush, _strokeMode);
    if (changed > 0) {
        _changed += changed;
        entity->gridEdited();
    }
    _lastStepPoint = hit.voxelPoint;
    ++_steps;
    showOutline(*entity, hit);
}

void EditorTerrain::endStroke(bool cancel) {
    if (!_stroking) {
        return;
    }
    _stroking = false;
    auto entity = terrainEntity(_strokeId);
    if (entity && entity->hasGrid()) {
        if (_strokeMode == terrain::SculptMode::Paint) {
            if (cancel) {
                restorePaint(*entity, _beforePaint);
                if (!_before.empty()) {
                    restore(*entity, _before);   // the voxels it painted where the map does not show
                }
            } else {
                // one step for the stroke: the paint map's blocks it changed, and the voxels it painted where the map
                // does not show
                const QString name = (_erasing ? QStringLiteral("Erase") : QStringLiteral("Paint")) + QStringLiteral(" Terrain");
                const bool texels = _changed > 0 && !_beforePaint.empty();
                TerrainChunks voxelsAfter;
                if (!_before.empty()) {
                    voxelsAfter = captureAfter(*entity);
                    keepChangedChunks(_before, voxelsAfter);
                }
                const bool voxels = !_before.empty() || !voxelsAfter.empty();
                if (texels && voxels) {
                    _undo->stack()->beginMacro(name);
                }
                if (texels) {
                    TerrainPaintBlocks after;
                    const terrain::PaintMap& paint = entity->getPaint();
                    for (const auto& pair : _beforePaint) {
                        after.emplace(pair.first, paint.copy(pair.second.low, pair.second.low + pair.second.size - 1));
                    }
                    _undo->pushTerrainPaint(name, _strokeId, _beforePaint, after);
                    entity->paintEdited();
                }
                if (voxels) {
                    _undo->pushTerrainEdit(name, _strokeId, _before, voxelsAfter);
                    entity->markCollisionShapeStale();
                    entity->gridEdited();
                }
                if (texels && voxels) {
                    _undo->stack()->endMacro();
                }
                if (texels || voxels) {
                    markChanged(_strokeId);
                }
            }
        } else if (cancel) {
            restore(*entity, _before);
        } else if (!_before.empty()) {
            // the step keeps what the stroke changed: the chunks that differ, and those split or merged (whose keys
            // are on one side only); a chunk the same before and after is left out of both
            TerrainChunks after = captureAfter(*entity);
            keepChangedChunks(_before, after);
            if (!_before.empty() || !after.empty()) {
                const QString name = _strokeMode == terrain::SculptMode::PaintVoxels && _erasing ? QStringLiteral("Erase Voxels")
                                                                                              : modeName(_strokeMode);
                _undo->pushTerrainEdit(name + QStringLiteral(" Terrain"), _strokeId, _before, after);
                entity->markCollisionShapeStale();
                entity->gridEdited();
                markChanged(_strokeId);
            }
        }
    }
    _before.clear();
    _strokeSlots.clear();
    _beforePaint.clear();
    _steps = 0;
    _changed = 0;
    _accumulated = 0.0f;
    _spheres.clear();
    if (_brush.randomAngle) {
        // the next stroke's angle, which the outline shows from now on
        _randomAngle = randFloatInRange(0.0f, 360.0f);
        if (_hasLastHit) {
            if (auto shown = terrainEntity(_lastHit.id)) {
                showOutline(*shown, _lastHit);
            }
        }
    }
}

void EditorTerrain::dropStroke() {
    _stroking = false;
    _before.clear();
    _strokeSlots.clear();
    _beforePaint.clear();
    _steps = 0;
    _changed = 0;
    _accumulated = 0.0f;
    _spheres.clear();
    _hasLastHit = false;
    _outline.clear();
}

void EditorTerrain::restore(RenderableTerrainEntityItem& entity, const TerrainChunks& chunks) {
    terrain::VoxelGrid& grid = entity.getGrid();
    for (const auto& pair : chunks) {
        if (grid.containsChunk(pair.first)) {
            grid.placeChunk(pair.first, pair.second);   // whole places were recorded: fillers under a split are replaced
        }
    }
    entity.markCollisionShapeStale();
    entity.gridEdited();
}

void EditorTerrain::restorePaint(RenderableTerrainEntityItem& entity, const TerrainPaintBlocks& blocks) {
    terrain::PaintMap& paint = entity.getPaint();
    for (const auto& pair : blocks) {
        paint.paste(pair.second);
    }
    entity.paintEdited();
}

void EditorTerrain::clear() {
    if (_stroking) {
        endStroke(false);
    }
    _hasRay = false;
    _hasLastHit = false;
    _outline.clear();
}

// --- the file -----------------------------------------------------------------------------------

void EditorTerrain::resample(const QList<QUuid>& ids) {
    QList<QUuid> targets;
    for (const QUuid& id : ids) {
        if (terrainEntity(id)) {
            targets << id;
        }
    }
    if (targets.isEmpty() && _hasLastHit && terrainEntity(_lastHit.id)) {
        targets << _lastHit.id;
    }
    if (targets.isEmpty()) {
        emit status(QStringLiteral("Select a terrain, or point at one, to resample it"), FAILED_NOTE_MS);
        return;
    }
    for (const QUuid& id : targets) {
        resampleOne(id);
    }
}

void EditorTerrain::resampleOne(const QUuid& id) {
    auto entity = terrainEntity(id);
    if (!entity) {
        return;
    }
    if (!entity->hasGrid()) {
        emit status(QStringLiteral("The terrain's voxels have not arrived yet"), FAILED_NOTE_MS);
        return;
    }
    if (EditorUndo::isLocked(id)) {
        emit status(QStringLiteral("The terrain is locked"), FAILED_NOTE_MS);
        return;
    }
    if (_stroking && _strokeId == id) {
        endStroke(false);
    }
    const glm::ivec3 current = entity->getGrid().getVoxelCount();
    const glm::ivec3 target = entity->getVoxelCount();
    const glm::vec3 dimensions = entity->getScaledDimensions();
    const glm::vec3 currentScale = entity->getVoxelScale();
    const float currentMetres = (currentScale.x + currentScale.y + currentScale.z) / 3.0f;
    const glm::vec3 targetScale = dimensions / glm::vec3(target);
    const float targetMetres = (targetScale.x + targetScale.y + targetScale.z) / 3.0f;
    if (target == current) {
        emit status(QStringLiteral("The terrain is already %1 voxels of %2").arg(countText(current), metresText(currentMetres)), SAVED_NOTE_MS);
        return;
    }
    const int64_t total = (int64_t)target.x * target.y * target.z;
    if (total > MAX_RESAMPLE_VOXELS || glm::any(glm::greaterThan(target, glm::ivec3(MAX_RESAMPLE_AXIS)))) {
        emit status(QStringLiteral("Resampling at %1 would take %2 voxels (%3 million); the most is %4 million along at most %5 a side. "
                                   "Raise the Voxel Size, or make the terrain smaller.")
                        .arg(metresText(entity->getVoxelSize()), countText(target)).arg(total / 1000000).arg(MAX_RESAMPLE_VOXELS / 1000000)
                        .arg(MAX_RESAMPLE_AXIS), FAILED_NOTE_MS);
        return;
    }

    QElapsedTimer timer;
    timer.start();
    terrain::VoxelGrid grid = terrain::resampleGrid(entity->getGrid(), target);
    const glm::ivec2 footprint(target.x, target.z);
    const int texelsPerVoxel = RenderableTerrainEntityItem::paintTexelsPerVoxel((targetScale.x + targetScale.z) * 0.5f, footprint);
    terrain::PaintMap paint = terrain::resamplePaint(entity->getPaint(), footprint, texelsPerVoxel);
    _undo->pushTerrainResample(QStringLiteral("Resample Terrain"), id, entity->getGrid(), entity->getPaint(), grid, paint);
    entity->setGridAndPaint(std::move(grid), std::move(paint));
    _hasLastHit = false;   // the outline was placed on the old voxels
    markChanged(id);
    qCDebug(interfaceapp) << "terrain" << id << "resampled from" << current.x << "x" << current.y << "x" << current.z << "to" << target.x
                          << "x" << target.y << "x" << target.z << "voxels in" << timer.elapsed() << "ms;" << entity->getGrid().countDenseChunks()
                          << "dense chunks";
    emit status(QStringLiteral("Terrain resampled: %1 voxels of %2 → %3 of %4 (%5 s)")
                    .arg(countText(current), metresText(currentMetres), countText(target), metresText(targetMetres))
                    .arg(timer.elapsed() / 1000.0, 0, 'f', 1), SAVED_NOTE_MS * 2);
}

void EditorTerrain::markChanged(const QUuid& id) {
    _dirty.insert(id);
    _saveTimer.start();   // from the start again: the file goes up once the changes pause
}

void EditorTerrain::flushSaves() {
    _saveTimer.stop();
    saveDirty();
}

void EditorTerrain::saveDirty() {
    bool waiting = false;
    for (const QUuid& id : QSet<QUuid>(_dirty)) {
        if ((_stroking && id == _strokeId) || _saving.contains(id)) {
            waiting = true;   // in the middle of a stroke, or of the last save: it goes up after
            continue;
        }
        _dirty.remove(id);
        save(id);
    }
    if (waiting && !_saveTimer.isActive()) {
        _saveTimer.start();
    }
}

// --- the layers --------------------------------------------------------------------------------

const terrain::LayerStack* EditorTerrain::layersOf(const QUuid& id) const {
    auto entity = terrainEntity(id);
    return entity ? &entity->getLayers() : nullptr;
}

uint64_t EditorTerrain::layersVersionOf(const QUuid& id) const {
    auto entity = terrainEntity(id);
    return entity ? entity->getLayersVersion() : 0;
}

bool EditorTerrain::setLayers(const QUuid& id, const terrain::LayerStack& layers, const QString& undoText,
                              const terrain::LayerStack* before) {
    auto entity = terrainEntity(id);
    if (!entity || !layers.isValid() || EditorUndo::isLocked(id)) {
        return false;
    }
    if (undoText.isEmpty()) {
        entity->setLayers(layers);   // a preview: the step comes with the commit
        return true;
    }
    const terrain::LayerStack was = before ? *before : entity->getLayers();
    entity->setLayers(layers);
    if (was != layers) {
        _undo->pushTerrainLayers(undoText, id, was, layers);
        markChanged(id);
    }
    return true;
}

int EditorTerrain::capsOf(const QUuid& id) const {
    auto entity = terrainEntity(id);
    return entity ? (int)entity->getCaps() : -1;
}

bool EditorTerrain::setCaps(const QUuid& id, uint8_t caps, const QString& undoText) {
    auto entity = terrainEntity(id);
    if (!entity || !entity->hasGrid() || EditorUndo::isLocked(id)) {
        return false;
    }
    const uint8_t was = entity->getCaps();
    if (was == caps) {
        return true;
    }
    _undo->pushTerrainCaps(undoText, id, was, caps);   // the step's first redo applies it and has the terrain saved
    return true;
}

// --- saving ------------------------------------------------------------------------------------

void EditorTerrain::save(const QUuid& id) {
    auto entity = terrainEntity(id);
    if (!entity || !entity->hasGrid()) {
        return;   // gone, or nothing to save
    }
    const terrain::Bytes bytes = terrain::saveTerrain(entity->getGrid(), &entity->getPaint(), &entity->getLayers());
    const QByteArray data((const char*)bytes.data(), (int)bytes.size());
    const QString fileName = QStringLiteral("terrain-") + id.toString(QUuid::WithoutBraces) + QStringLiteral(".drkt");
    const uint32_t version = std::max(entity->getTerrainVersion(), entity->getGridVersion()) + 1;

    auto upload = new EditorUpload(this);
    QString error;
    if (!upload->startData(data, fileName, TERRAIN_FOLDER, TERRAIN_MIME_TYPE, error)) {
        upload->deleteLater();
        _dirty.insert(id);   // kept for the next change to try again
        _saveFailures++;
        _lastSaveError = error;
        qCWarning(interfaceapp) << "terrain" << id << "could not be saved:" << error;
        emit status(QStringLiteral("Terrain not saved: ") + error, FAILED_NOTE_MS);
        return;
    }
    _saving.insert(id);
    emit status(QStringLiteral("Saving terrain…"), 0);
    connect(upload, &EditorUpload::finished, this, [this, upload, id, version](const QString& url) {
        upload->deleteLater();
        _saving.remove(id);
        auto entity = terrainEntity(id);
        if (entity) {
            // this client already holds what the file holds: told so, the renderer does not fetch its own save
            entity->setGridSource(url, version);
            EntityItemProperties properties;
            properties.setTerrainURL(url);
            properties.setTerrainVersion(version);
            DependencyManager::get<EntityScriptingInterface>()->editEntity(id, properties);
        }
        qCDebug(interfaceapp) << "terrain" << id << "saved as" << url << "version" << version;
        emit status(QStringLiteral("Terrain saved"), SAVED_NOTE_MS);
        if (_dirty.contains(id) && !_saveTimer.isActive()) {
            _saveTimer.start();   // changed again while this went up
        }
    });
    connect(upload, &EditorUpload::failed, this, [this, upload, id](const QString& error) {
        upload->deleteLater();
        _saving.remove(id);
        _dirty.insert(id);
        _saveFailures++;
        _lastSaveError = error;
        qCWarning(interfaceapp) << "terrain" << id << "could not be saved:" << error;
        emit status(QStringLiteral("Terrain not saved: ") + error, FAILED_NOTE_MS);
    });
}

// --- the outline --------------------------------------------------------------------------------

void EditorTerrain::showOutline(const RenderableTerrainEntityItem& entity, const Hit& hit) {
    _lastHit = hit;
    _hasLastHit = true;
    const glm::quat orientation = entity.getWorldOrientation();
    const glm::vec3 up = orientation * Vectors::UNIT_Y;
    const glm::vec3 right = orientation * Vectors::UNIT_X;
    const glm::vec3 forward = orientation * Vectors::UNIT_Z;
    const glm::mat4 worldToVoxel = entity.worldToVoxelMatrix();
    const glm::mat4 voxelToWorld = entity.voxelToWorldMatrix();
    // each point of the outline is dropped onto the ground from above it, so it lies on hills and in hollows
    const float lift = _brush.radius + 1.0f;
    const glm::vec3 voxelDown(worldToVoxel * glm::vec4(-up, 0.0f));
    auto onGround = [&](const glm::vec2& offset) {   // an offset from the pointer along x and z, in metres
        glm::vec3 point = hit.worldPoint + offset.x * right + offset.y * forward;
        const glm::vec3 voxelOrigin(worldToVoxel * glm::vec4(point + up * lift, 1.0f));
        float t = 0.0f;
        glm::vec3 normal;
        if (entity.rayHitsGround(voxelOrigin, voxelDown, t, normal)) {
            point = glm::vec3(voxelToWorld * glm::vec4(voxelOrigin + voxelDown * t, 1.0f));
        }
        return point;
    };
    QVector<QVector<glm::vec3>> rings;
    if (_mask && !isSphere(_brush.mode)) {
        // the shape picture's edges, turned as the brush is (the turn weightAt undoes, BrushMask.cpp)
        const float turn = glm::radians(angle());
        const float c = std::cos(turn);
        const float s = std::sin(turn);
        for (const std::vector<glm::vec2>& curve : _mask->outline(OUTLINE_MASK_LEVEL, OUTLINE_MASK_SAMPLES)) {
            QVector<glm::vec3> ring;
            ring.reserve((int)curve.size());
            for (const glm::vec2& p : curve) {
                const float w = -p.y;   // the picture's top is toward -z
                ring << onGround(_brush.radius * glm::vec2(p.x * c + w * s, -p.x * s + w * c));
            }
            rings << ring;
        }
    } else {
        QVector<glm::vec3> ring;
        ring.reserve(OUTLINE_SEGMENTS);
        for (int i = 0; i < OUTLINE_SEGMENTS; ++i) {
            const float angle = TWO_PI * (float)i / (float)OUTLINE_SEGMENTS;
            ring << onGround(_brush.radius * glm::vec2(std::cos(angle), std::sin(angle)));
        }
        rings << ring;
    }
    const glm::vec3 lineOfSight = _cameraPosition - hit.worldPoint;
    const float depth = std::max(0.01f, glm::length(lineOfSight));
    const glm::vec3 toCamera = depth > 1.0e-4f ? lineOfSight / depth : Vectors::UNIT_Z;
    _outline.set(rings, OUTLINE_COLOR, _pixelSize * depth * OUTLINE_WIDTH_PIXELS, toCamera);
}
