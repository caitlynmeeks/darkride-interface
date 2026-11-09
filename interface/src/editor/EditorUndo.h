//
//  EditorUndo.h
//  interface/src/editor
//
//  The undo stack for build mode. Nothing edits the world without going through it. A deleted
//  entity comes back with a new id, so the stack keeps a map from old ids to new ones and every
//  command resolves its ids through it.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_EditorUndo_h
#define hifi_EditorUndo_h

#include <functional>
#include <map>

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QUuid>
#include <QtCore/QVector>
#include <QUndoStack>   // QtWidgets in Qt 5, QtGui in Qt 6

#include <EntityItemProperties.h>
#include <terrain/Layers.h>
#include <terrain/PaintMap.h>
#include <terrain/VoxelGrid.h>

// the chunks of a terrain's grid as they were, or will be, by level and coordinates: what a sculpt stroke records.
// A stroke that split a chunk records the one chunk before and the eight after; a step puts back whole places
using TerrainChunks = std::map<terrain::ChunkKey, terrain::Chunk>;
// the blocks of a terrain's paint map (one per chunk column) as they were, or will be: what a paint stroke records
using TerrainPaintBlocks = std::map<glm::ivec2, terrain::PaintBlock, terrain::IVec2Less>;

class EditorUndo : public QObject {
    Q_OBJECT
public:
    explicit EditorUndo(QObject* parent = nullptr);

    QUndoStack* stack() { return &_stack; }

    // the id an entity has now, following re-creations
    QUuid resolve(const QUuid& id) const;
    void remap(const QUuid& from, const QUuid& to);

    // everything the world knows about an entity, the way scripts see it: position and rotation in
    // the world (edits take them so), the parent-relative ones in localPosition and localRotation.
    // Nothing is marked as set; read values from it, or use capture() to make the entity again.
    static bool snapshot(const QUuid& id, EntityItemProperties& properties);
    static EntityItemProperties snapshotOf(const EntityItemPointer& entity);
    // a snapshot with every property marked as set, to make the entity again (a copy, or one that
    // was deleted): an add applies and sends only the marked properties
    static bool capture(const QUuid& id, EntityItemProperties& properties);
    // the ids plus every descendant, parents before children, without duplicates
    static QList<QUuid> withDescendants(const QList<QUuid>& ids);
    static bool isLocked(const QUuid& id);

    // creates entities from properties (parents before children) and returns the new ids in the same
    // order. sourceIds name what each entry was captured from: a parent among them is replaced by the
    // entity made for it, so a copied family stays together. With remapSources, the stack also resolves
    // each source id to its new entity from now on (deleted entities coming back; not a paste, whose
    // originals remain).
    QList<QUuid> createEntities(const QVector<EntityItemProperties>& properties, const QList<QUuid>& sourceIds,
        bool remapSources);

    // records a property edit; applies it first unless it already happened (a finished drag)
    void pushPropertyEdit(const QString& text, const QList<QUuid>& ids, const QVector<EntityItemProperties>& before,
        const QVector<EntityItemProperties>& after, bool alreadyApplied);
    // deletes the entities and their descendants now, skipping locked ones; returns what went
    QList<QUuid> pushDelete(const QString& text, const QList<QUuid>& ids);
    // creates entities now and records it; returns the new ids in order. sourceIds: as for
    // createEntities (a paste gives the originals; a brand new entity gives none)
    QList<QUuid> pushAdd(const QString& text, const QVector<EntityItemProperties>& properties, const QList<QUuid>& sourceIds);
    // a sculpt stroke on a terrain, already applied: the chunks it touched before and after. Undo and redo put
    // them back into the entity's grid on this client (the voxels live there until the terrain is saved)
    void pushTerrainEdit(const QString& text, const QUuid& id, const TerrainChunks& before, const TerrainChunks& after);
    // puts chunks into a terrain entity's grid, and has the collision shape follow; false when the entity is
    // not a terrain on this client
    bool applyTerrainChunks(const QUuid& id, const TerrainChunks& chunks);
    // a paint stroke on a terrain, already applied: the blocks of its paint map it touched before and after
    void pushTerrainPaint(const QString& text, const QUuid& id, const TerrainPaintBlocks& before, const TerrainPaintBlocks& after);
    // puts blocks into a terrain entity's paint map; false when the entity is not a terrain on this client
    bool applyTerrainPaint(const QUuid& id, const TerrainPaintBlocks& blocks);
    // a change to a terrain's layer stack (the Inspector's Layers section), already applied: the stack before and after
    void pushTerrainLayers(const QString& text, const QUuid& id, const terrain::LayerStack& before, const terrain::LayerStack& after);
    // replaces a terrain entity's layer stack; false when the entity is not a terrain on this client
    bool applyTerrainLayers(const QUuid& id, const terrain::LayerStack& layers);
    // a change to which faces of a terrain's box are closed (the Inspector's Closed Box row): the step applies it
    // when pushed, and on undo and redo
    void pushTerrainCaps(const QString& text, const QUuid& id, uint8_t before, uint8_t after);
    // sets a terrain entity's closed faces; false when the entity is not a terrain on this client
    bool applyTerrainCaps(const QUuid& id, uint8_t caps);
    // a terrain resampled to another voxel count, already applied: the whole grid and paint map before and after
    // (a resample changes every chunk, so the step keeps both wholesale)
    void pushTerrainResample(const QString& text, const QUuid& id, const terrain::VoxelGrid& gridBefore,
        const terrain::PaintMap& paintBefore, const terrain::VoxelGrid& gridAfter, const terrain::PaintMap& paintAfter);
    // replaces a terrain entity's grid and paint map; false when the entity is not a terrain on this client
    bool applyTerrainGrid(const QUuid& id, const terrain::VoxelGrid& grid, const terrain::PaintMap& paint);

    // a change made to a created entity since, which a redo of its creation should make too (a new model's own
    // size, once it has loaded): the dimensions and position in `changes` replace the recorded ones
    void amendCreated(const QUuid& id, const EntityItemProperties& changes);

    // the selection is part of the history, as in Unity: a change of it is a step (pushed once it has happened),
    // undone and redone through the editor, the ids following entities made again
    void setSelectionApplier(std::function<void(const QList<QUuid>&)> applier) { _applySelection = applier; }
    void pushSelection(const QList<QUuid>& before, const QList<QUuid>& after);
    void applySelection(const QList<QUuid>& ids);

signals:
    // a terrain's voxels were put back by undo or redo: what keeps the terrain's file wants to know
    void terrainEdited(const QUuid& id);

private:
    QUndoStack _stack;
    QHash<QUuid, QUuid> _remap;
    std::function<void(const QList<QUuid>&)> _applySelection;
};

#endif // hifi_EditorUndo_h
