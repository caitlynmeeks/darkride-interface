//
//  EditorUndo.cpp
//  interface/src/editor
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "EditorUndo.h"

#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QUndoCommand>   // QtWidgets in Qt 5, QtGui in Qt 6

#include <EntitiesLogging.h>
#include <EntityItem.h>
#include <EntityScriptingInterface.h>
#include <EntityTree.h>
#include <EntityTreeRenderer.h>
#include <RenderableTerrainEntityItem.h>
#include <SharedUtil.h>

#include "Application.h"

// how long a new vehicle stays static and of type "none" before it becomes one (createEntities)
static const int BECOME_VEHICLE_AFTER_MS = 1500;

namespace {
    const int MAX_REMAP_DEPTH = 64;

    EntityTreePointer entityTree() {
        auto renderer = qApp->getEntities();
        return renderer ? renderer->getTree() : nullptr;
    }

    void applyProperties(EditorUndo* undo, const QList<QUuid>& ids, const QVector<EntityItemProperties>& properties) {
        auto entities = DependencyManager::get<EntityScriptingInterface>();
        for (int i = 0; i < ids.size() && i < properties.size(); ++i) {
            entities->editEntity(undo->resolve(ids[i]), properties[i]);
        }
    }

    void deleteEntities(EditorUndo* undo, const QList<QUuid>& ids) {
        auto entities = DependencyManager::get<EntityScriptingInterface>();
        for (const auto& id : ids) {
            entities->deleteEntity(undo->resolve(id));
        }
    }

    // A change to some properties of some entities, with the way back.
    class PropertyEditCommand : public QUndoCommand {
    public:
        PropertyEditCommand(EditorUndo* undo, const QString& text, const QList<QUuid>& ids,
            const QVector<EntityItemProperties>& before, const QVector<EntityItemProperties>& after, bool alreadyApplied) :
            QUndoCommand(text), _undo(undo), _ids(ids), _before(before), _after(after), _skipFirstRedo(alreadyApplied) {
        }
        void redo() override {
            if (_skipFirstRedo) {
                _skipFirstRedo = false;
                return;
            }
            qCDebug(entities) << "editor redo" << text() << "on" << _ids.size() << "entities";
            applyProperties(_undo, _ids, _after);
        }
        void undo() override {
            qCDebug(entities) << "editor undo" << text() << "on" << _ids.size() << "entities";
            applyProperties(_undo, _ids, _before);
        }
    private:
        EditorUndo* _undo;
        QList<QUuid> _ids;
        QVector<EntityItemProperties> _before;
        QVector<EntityItemProperties> _after;
        bool _skipFirstRedo;
    };

    // A sculpt stroke on a terrain's voxels, with the chunks as they were.
    class TerrainEditCommand : public QUndoCommand {
    public:
        TerrainEditCommand(EditorUndo* undo, const QString& text, const QUuid& id, const TerrainChunks& before,
            const TerrainChunks& after) :
            QUndoCommand(text), _undo(undo), _id(id), _before(before), _after(after) {
        }
        void redo() override {
            if (_skipFirstRedo) {
                _skipFirstRedo = false;
                return;
            }
            qCDebug(entities) << "editor redo" << text() << "on" << _after.size() << "chunks";
            _undo->applyTerrainChunks(_id, _after);
        }
        void undo() override {
            qCDebug(entities) << "editor undo" << text() << "on" << _before.size() << "chunks";
            _undo->applyTerrainChunks(_id, _before);
        }
    private:
        EditorUndo* _undo;
        QUuid _id;
        TerrainChunks _before;
        TerrainChunks _after;
        bool _skipFirstRedo { true };
    };

    // A paint stroke on a terrain's paint map, with the blocks as they were.
    class TerrainPaintCommand : public QUndoCommand {
    public:
        TerrainPaintCommand(EditorUndo* undo, const QString& text, const QUuid& id, const TerrainPaintBlocks& before,
            const TerrainPaintBlocks& after) :
            QUndoCommand(text), _undo(undo), _id(id), _before(before), _after(after) {
        }
        void redo() override {
            if (_skipFirstRedo) {
                _skipFirstRedo = false;
                return;
            }
            qCDebug(entities) << "editor redo" << text() << "on" << _after.size() << "paint blocks";
            _undo->applyTerrainPaint(_id, _after);
        }
        void undo() override {
            qCDebug(entities) << "editor undo" << text() << "on" << _before.size() << "paint blocks";
            _undo->applyTerrainPaint(_id, _before);
        }
    private:
        EditorUndo* _undo;
        QUuid _id;
        TerrainPaintBlocks _before;
        TerrainPaintBlocks _after;
        bool _skipFirstRedo { true };
    };

    // A terrain's layer stack changed, with the stack as it was.
    class TerrainLayersCommand : public QUndoCommand {
    public:
        TerrainLayersCommand(EditorUndo* undo, const QString& text, const QUuid& id, const terrain::LayerStack& before,
            const terrain::LayerStack& after) :
            QUndoCommand(text), _undo(undo), _id(id), _before(before), _after(after) {
        }
        void redo() override {
            if (_skipFirstRedo) {
                _skipFirstRedo = false;
                return;
            }
            qCDebug(entities) << "editor redo" << text() << "on" << _after.layers.size() << "layers";
            _undo->applyTerrainLayers(_id, _after);
        }
        void undo() override {
            qCDebug(entities) << "editor undo" << text() << "on" << _before.layers.size() << "layers";
            _undo->applyTerrainLayers(_id, _before);
        }
    private:
        EditorUndo* _undo;
        QUuid _id;
        terrain::LayerStack _before;
        terrain::LayerStack _after;
        bool _skipFirstRedo { true };
    };

    // Which faces of a terrain's box are closed: applied by the push itself (its first redo), unlike the steps above,
    // which record a change already made.
    class TerrainCapsCommand : public QUndoCommand {
    public:
        TerrainCapsCommand(EditorUndo* undo, const QString& text, const QUuid& id, uint8_t before, uint8_t after) :
            QUndoCommand(text), _undo(undo), _id(id), _before(before), _after(after) {
        }
        void redo() override {
            qCDebug(entities) << "editor redo" << text() << "closed faces" << _after;
            _undo->applyTerrainCaps(_id, _after);
        }
        void undo() override {
            qCDebug(entities) << "editor undo" << text() << "closed faces" << _before;
            _undo->applyTerrainCaps(_id, _before);
        }
    private:
        EditorUndo* _undo;
        QUuid _id;
        uint8_t _before;
        uint8_t _after;
    };

    // A terrain resampled to another voxel count, with the whole grid and paint map as they were.
    class TerrainResampleCommand : public QUndoCommand {
    public:
        TerrainResampleCommand(EditorUndo* undo, const QString& text, const QUuid& id, const terrain::VoxelGrid& gridBefore,
            const terrain::PaintMap& paintBefore, const terrain::VoxelGrid& gridAfter, const terrain::PaintMap& paintAfter) :
            QUndoCommand(text), _undo(undo), _id(id), _gridBefore(gridBefore), _paintBefore(paintBefore), _gridAfter(gridAfter),
            _paintAfter(paintAfter) {
        }
        void redo() override {
            if (_skipFirstRedo) {
                _skipFirstRedo = false;
                return;
            }
            qCDebug(entities) << "editor redo" << text() << "to" << _gridAfter.getVoxelCount().x << "x" << _gridAfter.getVoxelCount().y
                              << "x" << _gridAfter.getVoxelCount().z << "voxels";
            _undo->applyTerrainGrid(_id, _gridAfter, _paintAfter);
        }
        void undo() override {
            qCDebug(entities) << "editor undo" << text() << "to" << _gridBefore.getVoxelCount().x << "x" << _gridBefore.getVoxelCount().y
                              << "x" << _gridBefore.getVoxelCount().z << "voxels";
            _undo->applyTerrainGrid(_id, _gridBefore, _paintBefore);
        }
    private:
        EditorUndo* _undo;
        QUuid _id;
        terrain::VoxelGrid _gridBefore;
        terrain::PaintMap _paintBefore;
        terrain::VoxelGrid _gridAfter;
        terrain::PaintMap _paintAfter;
        bool _skipFirstRedo { true };
    };

    // Entities that were deleted; undo brings them back (with new ids, which the stack remaps).
    class DeleteEntitiesCommand : public QUndoCommand {
    public:
        DeleteEntitiesCommand(EditorUndo* undo, const QString& text, const QList<QUuid>& ids,
            const QVector<EntityItemProperties>& properties) :
            QUndoCommand(text), _undo(undo), _ids(ids), _properties(properties) {
        }
        void redo() override {
            deleteEntities(_undo, _ids);
        }
        void undo() override {
            _undo->createEntities(_properties, _ids, true);
        }
    private:
        EditorUndo* _undo;
        QList<QUuid> _ids;
        QVector<EntityItemProperties> _properties;
    };

    // Entities that were created (a paste, a duplicate); undo deletes them.
    class AddEntitiesCommand : public QUndoCommand {
    public:
        AddEntitiesCommand(EditorUndo* undo, const QString& text, const QList<QUuid>& ids,
            const QVector<EntityItemProperties>& properties) :
            QUndoCommand(text), _undo(undo), _ids(ids), _properties(properties) {
        }
        void redo() override {
            if (_skipFirstRedo) {
                _skipFirstRedo = false;
                return;
            }
            _undo->createEntities(_properties, _ids, true);
        }
        void undo() override {
            deleteEntities(_undo, _ids);
        }
        bool amend(const QUuid& id, const EntityItemProperties& changes) {
            for (int i = 0; i < _ids.size() && i < _properties.size(); ++i) {
                if (_undo->resolve(_ids[i]) == id) {
                    if (changes.dimensionsChanged()) {
                        _properties[i].setDimensions(changes.getDimensions());
                    }
                    if (changes.positionChanged()) {
                        _properties[i].setPosition(changes.getPosition());
                    }
                    return true;
                }
            }
            return false;
        }
    private:
        EditorUndo* _undo;
        QList<QUuid> _ids;
        QVector<EntityItemProperties> _properties;
        bool _skipFirstRedo { true };
    };

    // The selection as it was and as it became; the change has happened by the time this is pushed.
    class SelectionCommand : public QUndoCommand {
    public:
        SelectionCommand(EditorUndo* undo, const QList<QUuid>& before, const QList<QUuid>& after) :
            QUndoCommand(after.isEmpty() ? "Deselect" : "Select"), _undo(undo), _before(before), _after(after) {
        }
        void redo() override {
            if (_skipFirstRedo) {
                _skipFirstRedo = false;
                return;
            }
            _undo->applySelection(_after);
        }
        void undo() override {
            _undo->applySelection(_before);
        }
    private:
        EditorUndo* _undo;
        QList<QUuid> _before;
        QList<QUuid> _after;
        bool _skipFirstRedo { true };
    };
}

EditorUndo::EditorUndo(QObject* parent) : QObject(parent) {
}

void EditorUndo::pushSelection(const QList<QUuid>& before, const QList<QUuid>& after) {
    if (before != after) {
        _stack.push(new SelectionCommand(this, before, after));
    }
}

void EditorUndo::applySelection(const QList<QUuid>& ids) {
    if (!_applySelection) {
        return;
    }
    QList<QUuid> resolved;   // an entity made again by undo has a new id
    for (const auto& id : ids) {
        resolved << resolve(id);
    }
    _applySelection(resolved);
}

QUuid EditorUndo::resolve(const QUuid& id) const {
    QUuid current = id;
    for (int depth = 0; depth < MAX_REMAP_DEPTH; ++depth) {
        auto it = _remap.constFind(current);
        if (it == _remap.constEnd()) {
            break;
        }
        current = it.value();
    }
    return current;
}

void EditorUndo::remap(const QUuid& from, const QUuid& to) {
    if (!from.isNull() && !to.isNull() && from != to) {
        _remap[from] = to;
    }
}

bool EditorUndo::snapshot(const QUuid& id, EntityItemProperties& properties) {
    auto tree = entityTree();
    if (!tree) {
        return false;
    }
    bool found = false;
    tree->withReadLock([&] {
        auto entity = tree->findEntityByEntityItemID(id);
        if (entity) {
            properties = snapshotOf(entity);
            found = true;
        }
    });
    return found;
}

EntityItemProperties EditorUndo::snapshotOf(const EntityItemPointer& entity) {
    EntityItemProperties properties = entity->getProperties();
    // getProperties() gives position and rotation in the parent's frame, but edits take them in the world
    // (as scripts do): for an attached entity the Inspector showed, and undo restored, the wrong values
    properties.setPosition(entity->getWorldPosition());
    properties.setRotation(entity->getWorldOrientation());
    properties.setPositionChanged(false);
    properties.setRotationChanged(false);
    return properties;
}

bool EditorUndo::capture(const QUuid& id, EntityItemProperties& properties) {
    if (!snapshot(id, properties)) {
        return false;
    }
    // getProperties() fills every property in but marks none as set, and an add applies (and sends) only the
    // marked ones: unmarked, a copy came out as a default entity of its type (a model with no model, drawn
    // as a magenta box), keeping only what createEntities sets itself
    properties.markAllChanged();
    // placed by its position and rotation in the parent's frame, as captured (markAllChanged leaves these out,
    // and the add would read the world values as parent-relative ones)
    properties.setLocalPositionChanged(true);
    properties.setLocalRotationChanged(true);
    // the new entity's own, worked out when it is added
    properties.setQueryAACubeChanged(false);
    return true;
}

QList<QUuid> EditorUndo::withDescendants(const QList<QUuid>& ids) {
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    QList<QUuid> result;
    QSet<QUuid> seen;
    QList<QUuid> queue = ids;
    while (!queue.isEmpty()) {
        QUuid id = queue.takeFirst();
        if (id.isNull() || seen.contains(id)) {
            continue;
        }
        seen.insert(id);
        result << id;
        for (const auto& child : entities->getChildrenIDs(id)) {
            queue << child;
        }
    }
    return result;
}

bool EditorUndo::isLocked(const QUuid& id) {
    auto tree = entityTree();
    if (!tree) {
        return false;
    }
    bool locked = false;
    tree->withReadLock([&] {
        auto entity = tree->findEntityByEntityItemID(id);
        locked = entity && entity->getLocked();
    });
    return locked;
}

QList<QUuid> EditorUndo::createEntities(const QVector<EntityItemProperties>& properties, const QList<QUuid>& sourceIds,
    bool remapSources) {
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    QHash<QUuid, QUuid> batch;   // source id to new id, within this batch
    QList<QUuid> newIds;
    for (int i = 0; i < properties.size(); ++i) {
        EntityItemProperties copy = properties[i];
        QUuid parentID = copy.getParentID();
        if (!parentID.isNull()) {
            copy.setParentID(batch.contains(parentID) ? batch.value(parentID) : resolve(parentID));
        }
        // a car made with its Track (Create's Rail, a paste of both) rides the new Track, not the one it was copied from
        QUuid trackID = copy.getTrackID();
        if (!trackID.isNull() && batch.contains(trackID)) {
            copy.setTrackID(batch.value(trackID));
        }
        // a vehicle is made in two steps, as physics' rigs make theirs (tests-manual/physics-baseline): first static
        // and of type "none", then its type and dynamic a moment later. The physics server builds a vehicle's model
        // when it first sees the type, and a big entity can reach it in two packets: a model built from the first
        // takes what it lacked as zero (an airship with no ballast floats away; 10-24, about one in nine). A wheel is
        // only a picture of its parent's wheel, made at once
        const QString vehicleType = copy.getVehicle().getType();
        const bool twoStep = !vehicleType.isEmpty() && vehicleType != VehicleType::NONE && vehicleType != VehicleType::WHEEL;
        const bool dynamic = copy.getDynamic();
        if (twoStep) {
            copy.getVehicle().setType(VehicleType::NONE);
            copy.setDynamic(false);
        }
        // a new entity: a fresh creation time (with the old one, a finite lifetime would already be
        // over and the server would delete it on arrival), no stale edit stamp, no inherited physics
        copy.setCreated(UNKNOWN_CREATED_TIME);
        copy.setLastEdited(usecTimestampNow());
        copy.setActionData(QByteArray());
        copy.setSimulationOwner(QUuid(), 0);
        QUuid newId = entities->addEntity(copy);
        newIds << newId;
        if (twoStep && !newId.isNull()) {
            // undone in the meantime, the edit finds no entity and does nothing
            QTimer::singleShot(BECOME_VEHICLE_AFTER_MS, this, [entities, newId, vehicleType, dynamic] {
                EntityItemProperties become;
                become.setDynamic(dynamic);
                become.getVehicle().setType(vehicleType);
                entities->editEntity(newId, become);
            });
        }
        // a paste used to skip this (no ids to remap), so its children went back onto the originals
        QUuid sourceId = i < sourceIds.size() ? sourceIds[i] : QUuid();
        if (!sourceId.isNull() && !newId.isNull()) {
            batch[sourceId] = newId;
            if (remapSources) {
                remap(sourceId, newId);
            }
        }
    }
    return newIds;
}

void EditorUndo::amendCreated(const QUuid& id, const EntityItemProperties& changes) {
    QUuid current = resolve(id);
    for (int i = _stack.count() - 1; i >= 0; --i) {
        // the stack hands its commands out read-only, but they are ours
        auto command = dynamic_cast<AddEntitiesCommand*>(const_cast<QUndoCommand*>(_stack.command(i)));
        if (command && command->amend(current, changes)) {
            return;
        }
    }
}

void EditorUndo::pushPropertyEdit(const QString& text, const QList<QUuid>& ids, const QVector<EntityItemProperties>& before,
    const QVector<EntityItemProperties>& after, bool alreadyApplied) {
    if (ids.isEmpty()) {
        return;
    }
    _stack.push(new PropertyEditCommand(this, text, ids, before, after, alreadyApplied));
}

QList<QUuid> EditorUndo::pushDelete(const QString& text, const QList<QUuid>& ids) {
    QList<QUuid> all = withDescendants(ids);
    QList<QUuid> captured;
    QVector<EntityItemProperties> properties;
    for (const auto& id : all) {
        if (isLocked(id)) {
            continue;
        }
        EntityItemProperties capturedProperties;
        if (capture(id, capturedProperties)) {
            captured << id;
            properties << capturedProperties;
        }
    }
    if (captured.isEmpty()) {
        return captured;
    }
    _stack.push(new DeleteEntitiesCommand(this, text, captured, properties));
    return captured;
}

QList<QUuid> EditorUndo::pushAdd(const QString& text, const QVector<EntityItemProperties>& properties, const QList<QUuid>& sourceIds) {
    if (properties.isEmpty()) {
        return QList<QUuid>();
    }
    QList<QUuid> newIds = createEntities(properties, sourceIds, false);
    // the command keeps the new ids, and properties whose parents inside the batch name those ids instead of
    // the sources, so a redo makes the same family again and remaps the ids
    QHash<QUuid, QUuid> made;
    for (int i = 0; i < newIds.size() && i < sourceIds.size(); ++i) {
        if (!sourceIds[i].isNull() && !newIds[i].isNull()) {
            made[sourceIds[i]] = newIds[i];
        }
    }
    QVector<EntityItemProperties> recorded = properties;
    for (auto& entry : recorded) {
        auto parent = made.constFind(entry.getParentID());
        if (parent != made.constEnd()) {
            entry.setParentID(parent.value());
        }
    }
    _stack.push(new AddEntitiesCommand(this, text, newIds, recorded));
    return newIds;
}

void EditorUndo::pushTerrainEdit(const QString& text, const QUuid& id, const TerrainChunks& before, const TerrainChunks& after) {
    if (before.empty() && after.empty()) {
        return;
    }
    _stack.push(new TerrainEditCommand(this, text, id, before, after));
}

bool EditorUndo::applyTerrainChunks(const QUuid& id, const TerrainChunks& chunks) {
    auto tree = entityTree();
    if (!tree) {
        return false;
    }
    auto terrain = std::dynamic_pointer_cast<RenderableTerrainEntityItem>(tree->findEntityByID(resolve(id)));
    if (!terrain || !terrain->hasGrid()) {
        return false;
    }
    terrain::VoxelGrid& grid = terrain->getGrid();
    for (const auto& pair : chunks) {
        if (grid.containsChunk(pair.first)) {
            // a step holds every chunk of the places it changed, so a coarser chunk split to make room is filled over
            grid.placeChunk(pair.first, pair.second);
        }
    }
    terrain->markCollisionShapeStale();
    terrain->gridEdited();
    emit terrainEdited(id);
    return true;
}

void EditorUndo::pushTerrainPaint(const QString& text, const QUuid& id, const TerrainPaintBlocks& before, const TerrainPaintBlocks& after) {
    if (before.empty() && after.empty()) {
        return;
    }
    _stack.push(new TerrainPaintCommand(this, text, id, before, after));
}

void EditorUndo::pushTerrainLayers(const QString& text, const QUuid& id, const terrain::LayerStack& before, const terrain::LayerStack& after) {
    if (before == after) {
        return;
    }
    _stack.push(new TerrainLayersCommand(this, text, id, before, after));
}

bool EditorUndo::applyTerrainLayers(const QUuid& id, const terrain::LayerStack& layers) {
    auto tree = entityTree();
    if (!tree) {
        return false;
    }
    auto terrain = std::dynamic_pointer_cast<RenderableTerrainEntityItem>(tree->findEntityByID(resolve(id)));
    if (!terrain) {
        return false;
    }
    terrain->setLayers(layers);
    emit terrainEdited(id);
    return true;
}

void EditorUndo::pushTerrainCaps(const QString& text, const QUuid& id, uint8_t before, uint8_t after) {
    if (before == after) {
        return;
    }
    _stack.push(new TerrainCapsCommand(this, text, id, before, after));
}

bool EditorUndo::applyTerrainCaps(const QUuid& id, uint8_t caps) {
    auto tree = entityTree();
    if (!tree) {
        return false;
    }
    auto terrain = std::dynamic_pointer_cast<RenderableTerrainEntityItem>(tree->findEntityByID(resolve(id)));
    if (!terrain) {
        return false;
    }
    terrain->setCaps(caps);
    emit terrainEdited(id);
    return true;
}

void EditorUndo::pushTerrainResample(const QString& text, const QUuid& id, const terrain::VoxelGrid& gridBefore,
        const terrain::PaintMap& paintBefore, const terrain::VoxelGrid& gridAfter, const terrain::PaintMap& paintAfter) {
    _stack.push(new TerrainResampleCommand(this, text, id, gridBefore, paintBefore, gridAfter, paintAfter));
}

bool EditorUndo::applyTerrainGrid(const QUuid& id, const terrain::VoxelGrid& grid, const terrain::PaintMap& paint) {
    auto tree = entityTree();
    if (!tree) {
        return false;
    }
    auto terrain = std::dynamic_pointer_cast<RenderableTerrainEntityItem>(tree->findEntityByID(resolve(id)));
    if (!terrain) {
        return false;
    }
    terrain->setGridAndPaint(grid, paint);
    emit terrainEdited(id);
    return true;
}

bool EditorUndo::applyTerrainPaint(const QUuid& id, const TerrainPaintBlocks& blocks) {
    auto tree = entityTree();
    if (!tree) {
        return false;
    }
    auto terrain = std::dynamic_pointer_cast<RenderableTerrainEntityItem>(tree->findEntityByID(resolve(id)));
    if (!terrain || !terrain->hasGrid()) {
        return false;
    }
    terrain::PaintMap& paint = terrain->getPaint();
    for (const auto& pair : blocks) {
        paint.paste(pair.second);
    }
    terrain->paintEdited();
    emit terrainEdited(id);
    return true;
}
