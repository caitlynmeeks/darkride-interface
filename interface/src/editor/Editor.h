//
//  Editor.h
//  interface/src/editor
//
//  Build mode: the native scene editor. Owns the scene-view camera, the grid, the selection,
//  the undo stack, the gizmo and the panels docked around the viewport; Application hands it
//  the viewport's input while it is active.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_Editor_h
#define hifi_Editor_h

#include <QtCore/QByteArray>
#include <QtGui/QColor>
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QPointer>
#include <QtCore/QObject>
#include <QtCore/QPoint>
#include <QtCore/QRectF>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/QVariant>
#include <QtCore/QVector>
#include <QtGui/QVector3D>

#include <glm/gtc/quaternion.hpp>

#include <AABox.h>
#include <DependencyManager.h>
#include <EntityItemProperties.h>
#include <RegisteredMetaTypes.h>
#include <SettingHandle.h>

#include "EditorBounds.h"
#include "EditorCamera.h"
#include "EditorClipboard.h"
#include <QtGui/QPixmap>

#include "EditorGizmo.h"
#include "EditorLightGizmos.h"
#include "EditorTrackGizmos.h"
#include "EditorVehicleGizmos.h"
#include "EditorGrid.h"
#include "EditorSelection.h"
#include "EditorTerrain.h"
#include "EditorUndo.h"
#include "ui/EditorToolBar.h"

class EditorEntityList;
class QAction;
class QDockWidget;
class QDropEvent;
class QKeyEvent;
class QLabel;
class QMenu;
class QMouseEvent;
class QStatusBar;
class QUrl;
class QWheelEvent;
class AssetsPanel;
class EditorUpload;
class EditorWorld;
class EntityTreeModel;
class HierarchyPanel;
class InspectorPanel;
class TerrainToolBar;
class UserFiles;

class Editor : public QObject, public Dependency {
    Q_OBJECT
    SINGLETON_DEPENDENCY

    Q_PROPERTY(bool active READ isActive WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool gridVisible READ isGridVisible WRITE setGridVisible NOTIFY gridVisibleChanged)
    Q_PROPERTY(float flySpeed READ flySpeed NOTIFY cameraChanged)
    // the world axes in camera space (x right, y up, z towards the viewer), for the scene gizmo
    Q_PROPERTY(QVector3D axisX READ axisX NOTIFY cameraChanged)
    Q_PROPERTY(QVector3D axisY READ axisY NOTIFY cameraChanged)
    Q_PROPERTY(QVector3D axisZ READ axisZ NOTIFY cameraChanged)
    // the selection rectangle being dragged in the viewport, in viewport pixels
    Q_PROPERTY(bool marqueeActive READ isMarqueeActive NOTIFY marqueeChanged)
    Q_PROPERTY(QRectF marquee READ marquee NOTIFY marqueeChanged)

public:
    Editor();

    bool isActive() const { return _active; }
    bool isGridVisible() const { return _gridVisible; }
    float flySpeed() const { return _camera.flySpeed(); }
    QVector3D axisX() const { return _axes[0]; }
    QVector3D axisY() const { return _axes[1]; }
    QVector3D axisZ() const { return _axes[2]; }
    bool isMarqueeActive() const { return _marqueeActive; }
    QRectF marquee() const { return _marqueeRect; }

    EditorCamera& camera() { return _camera; }
    EditorGrid& grid() { return _grid; }
    EditorSelection& selection() { return _selection; }
    EditorUndo& undoStack() { return _undo; }

    // Application calls this once the window, the scene and the desktop UI exist. Until then build
    // mode cannot be entered: the menu restores its saved state at startup by triggering the item.
    void setReady();

    // called by Application every frame while active, before the camera is read
    void update(float deltaTime);

    // viewport input; each returns true when the editor consumed the event
    bool handleMousePress(QMouseEvent* event);
    bool handleMouseMove(QMouseEvent* event);
    bool handleMouseRelease(QMouseEvent* event);
    bool handleMouseDoubleClick(QMouseEvent* event);
    bool handleWheel(QWheelEvent* event);
    bool handleKeyPress(QKeyEvent* event);
    bool handleKeyRelease(QKeyEvent* event);
    // the editor's keys when the focus is in one of its panels rather than the viewport
    bool eventFilter(QObject* watched, QEvent* event) override;

    // a Create menu with every entity kind, for the toolbar and context menus; with a viewport
    // position, new entities go where that position points instead of the middle of the view
    QMenu* buildCreateMenu(QWidget* parent, const QPoint* viewportPos = nullptr);
    // a file or URL dropped on the viewport: models and images are placed where it landed
    bool handleDrop(QDropEvent* event);
    // an entities file or URL (.json, .json.gz, .svo) into the world: placed under the cursor as a paste is (a whole
    // world's file, 10 km or more across, where its positions say), selected, one undo step; build mode comes on for
    // it. The menu's imports, and a file dropped on the window or opened by its address, come here
    bool importEntities(const QString& url);
    // the selection and its descendants to an entities file at this path (Build > Export Selected Entities… once a
    // path is chosen); how many entities were written, -1 when nothing was
    int exportSelectionTo(const QString& path);
    // what the entity could not load, as its Hierarchy row says (refreshLoadProblems); empty when nothing has failed
    QString loadProblem(const QUuid& id) const;
    // the colour of the entity's Hierarchy row (EntityTreeModel::rowColor), invalid for the panel's own
    QColor hierarchyRowColor(const QUuid& id) const;
    // a Build menu item that only works while build mode is on
    void addMenuAction(QAction* action);

    Q_INVOKABLE void viewFromAxis(int axis, int sign);

    // the whole Inspector as drawn, scrolled or not (a scripted test's picture of it, Test.saveInspectorPicture)
    QPixmap inspectorPicture() const;
    // a gizmo drag of the selection driven by a scripted test (Test.editorDrag): begin captures the selection as a
    // press on the gizmo would (a move handle, or a rotate ring when rotate), every call applies the delta or the
    // rotation about the selection's centre as the pointer's motion would, final takes the undo step as the release would;
    // with scaling, a scale handle's drag, by `scale` along the gizmo's axes (Test.editorScaleDrag)
    void dragSelectionForTest(const glm::vec3& delta, const glm::quat& rotationDelta, bool rotate, bool begin, bool final,
                              const glm::vec3& scale = glm::vec3(1.0f), bool scaling = false);
    // files from this computer uploaded into the Assets panel's folder, as its Upload… does, and the status bar's text
    // (Test.editorUploadFiles, Test.editorStatus)
    void uploadFilesForTest(const QStringList& localPaths);
    QString statusForTest() const;

public slots:
    void setActive(bool active);
    void toggleActive() { setActive(!_active); }
    // selects these entities, turning build mode on first (a scripted test, Test.selectInEditor)
    void select(const QList<QUuid>& ids);
    // a Track's control points, for scripted tests (Test.chooseTrackPoint and the others): what a click on a point's
    // diamond, Shift+click on the curve and Delete do
    void chooseTrackPoint(int index, bool fromInspector);
    int chosenTrackPoint() const { return _trackPointChosen; }
    // an Inspector edit of the selection, begun and committed as a list's + button does (Test.commitInspectorEdit)
    void testInspectorEdit(const QString& key, const QVariant& value);
    void insertTrackPoint(float arcLength);
    void deleteTrackPoint();
    void setGridVisible(bool visible);
    void setTool(EditorToolBar::Tool tool);
    void setLocalSpace(bool local);
    void setPivotMode(bool pivot);

    void undo();
    void redo();
    // docks the panels back where they started, for a panel that was torn off and lost
    void resetLayout();
    // My Files (the Assets panel), made the first time it's asked for: Build mode's, and normal play's too
    // (ui/Panels.h). Where the window's saved layout has it, open or closed as it was
    QDockWidget* myFilesPanel();
    // Build mode's toolbar rows, placed by the editor rather than left to a saved layout
    void placeToolBars();
    // after a named layout is put back (View > Layouts, one list for both modes): the Hierarchy, the Inspector and
    // the rows follow the mode, not the layout, which may have been saved in the other one
    void layoutApplied(bool savedInBuildMode);
    // Build mode's own panels, once it has been on (null before)
    QDockWidget* hierarchyPanel() const;
    QDockWidget* inspectorPanel() const;
    // the panels' style sheet (styles/editor.qss) on the window, its colours the skin's while one is on
    // (PanelTheme::getSkin): called once the panels exist and again whenever the skin changes
    void applyStyle();
    void frameSelection();
    // flies the build camera to a Hierarchy row that is not loaded, so the stream brings it; it is selected when it
    // arrives (NATIVE-EDITOR.md 8). A loaded entity is selected and framed
    void goToListed(const QUuid& id);
    // asks the entity server again for its list of every entity (the Hierarchy's Refresh)
    void refreshEntityList();
    // the Hierarchy's rows in all and loaded, and the list's state, for scripted tests (Test.getHierarchyCounts)
    QVariantMap hierarchyCounts() const;
    // what a click on the entity's row in the Hierarchy does (Test.clickHierarchyRow)
    void clickHierarchyRow(const QUuid& id);
    // what a click on a Hierarchy row's eye ("hide"), hand ("pick"), lock ("lock") or script mark ("scripts") does,
    // Alt held for onlyThis (Test.hierarchyToggle); false when the row isn't there or nothing could change
    bool clickHierarchyToggle(const QString& which, const QUuid& id, bool onlyThis);
    // a stroke along a row toggle's column, from the first row to the last (Test.hierarchyStroke)
    bool strokeHierarchyToggle(const QString& which, const QList<QUuid>& ids, bool onlyThis);
    // the row's toggles and what they amount to (Test.hierarchyToggles): hidden here (and whether the entity is
    // drawn), unpickable, locked, scripts off, the Hierarchy's note and the last scripts-off reminder
    QVariantMap hierarchyToggles(const QUuid& id) const;
    // what a click in the viewport at the entity's middle selects, and what a marquee over the whole viewport would
    // (Test.editorPickAt, Test.editorMarqueeIds)
    QUuid pickAtEntityForTest(const QUuid& id) const;
    QList<QUuid> marqueeAllForTest() const;
    // the Hierarchy note's Show All, as a click on it
    void showAllHere();
    void goToSelection();           // Shift+F: frame it and take the avatar there
    void bringSelectionHere();      // Option+Shift+F: move it to the avatar
    void create(const QString& key);
    // the Terrain tool's Random tick set as a click on it does (Test.terrainRandomAngle): { found, angleEnabled }
    QVariantMap setTerrainRandomAngleForTest(bool on);
    // a stroke of the Terrain tool straight down onto the entity's middle, pressed and let go (Test.terrainStroke):
    // the angle it turned the brush's picture by, or -1 when no terrain was there
    float terrainStrokeForTest(const QUuid& id);
    // the angle the next stroke will use, which the outline shows (Test.terrainNextAngle)
    float terrainNextAngleForTest() const { return _terrain.angle(); }
    // the Terrain tool's saving, for the quit (ui/QuitCheck.cpp): sculpting still waiting goes up before Darkride quits
    EditorTerrain& terrain() { return _terrain; }
    void createModel();             // asks for a URL
    void createFromMenu();          // the sender's data() is the catalogue key
    void deleteSelection();
    void duplicateSelection();
    void copySelection();
    void cutSelection();
    void paste();
    void selectAll();
    // Build > Import Entities… / Import Entities from URL…: an entities file (.json, .json.gz, .svo, as Export writes
    // and world backups are) placed under the cursor like a paste, selected, one undo step; Build > Export Selected
    // Entities…: the selection with its descendants, to a .json file (the old Create app's Import and Export)
    void importEntitiesFromFile();
    void importEntitiesFromUrl();
    void exportSelection();
    // Build > Use Stable Links: every link in the world to one of the user's own files by its path becomes the file's
    // stable link, read from a fresh listing of My Files; one undo step, and a report of what could not be matched
    // (NATIVE-EDITOR.md section 7.4)
    void useStableLinks();
    // Build > Bake Impostor: every selected entity without a selected ancestor, one after the other (ImpostorBaker)
    void bakeImpostor();
    // Build > Bake All Impostors: every prop of the world without a card yet (ImpostorBaker::bakeAll), the progress
    // on the status bar; Build > Remove Impostor: the selected entities' entries
    void bakeAllImpostors();
    void removeImpostor();

signals:
    void activeChanged(bool active);
    void gridVisibleChanged(bool visible);
    void cameraChanged();
    void marqueeChanged();

private:
    enum class DragKind { None, Move, Rotate, Scale, TrackPoint };   // TrackPoint: a Track's chosen control point, by the Move gizmo

    void createPanels();
    void loadStyle();   // styles/editor.qss, once, for My Files or Build mode's panels, whichever comes first
    void showPanels(bool show);
    // the docked panels on the window that aren't Build mode's (chat's), shown
    QList<QDockWidget*> otherDockedPanels() const;
    // one of Build mode's panels, new to a side where another panel is docked, goes above it with most of the height
    void placeAbove(QDockWidget* ours, Qt::DockWidgetArea area);
    void focusViewport();
    void takeFocusUnlessTyping();
    void setMenuActionsEnabled(bool enabled);
    void updateAxes();
    void updateGizmo();
    // the tool has changed: the gizmo, the Terrain tool's row and its outline follow
    void applyTool();
    void updateTerrainBar();
    void updateRenderMethod();
    void onSelectionChanged();
    // the selection has changed by the user's hand: a step in the history, as in Unity
    void recordSelection();
    void rename(const QUuid& id, const QString& name);
    // makes the entities children of the parent (none: top-level), where they are in the world staying so
    void reparent(const QList<QUuid>& ids, const QUuid& parentId);

    // the Inspector: snapshots of the selection, and edits coming back from its fields
    void refreshInspector(bool selectionChanged);
    // the Inspector header's toggle icons, for the selection (the Hierarchy rows' states)
    void updateInspectorToggles();
    // the Hierarchy's rows and the Inspector say what failed to load (EntityRenderer::loadFailures), once a second
    void refreshLoadProblems();
    // the selected entities' load failures, one line each (named when several are selected)
    QString selectionLoadProblem() const;
    bool selectionSnapshots(QVector<EntityItemProperties>& snapshots, EntityTypes::EntityType& type, bool& multiple) const;
    void onInspectorEditBegan(const QString& key);
    void onInspectorLiveEdit(const QString& key, const QVariant& value);
    void applyLiveEdit(const QString& key, QVariant value);   // a copy: the paced send passes _liveEditValue, which this clears
    void onInspectorCommitted(const QString& key, const QVariant& value);
    // an Inspector value for the given entities, as one undo step
    void commitInspectorEdit(const QString& key, const QVariant& value, const QList<QUuid>& targets);
    void onInspectorEditCancelled(const QString& key);
    // a picture chosen for an Inspector field (a light's Cookie): uploaded, then set on the entities selected now
    void onInspectorUpload(const QString& key, const QString& localPath);
    // the edit an Inspector value amounts to for one entity: Scale is turned into dimensions, an entry of
    // userData is put into the userData the entity had (`before`, which holds it)
    EntityItemProperties inspectorEdit(const QString& key, const QVariant& value, const glm::vec3& naturalSize,
                                       const EntityItemProperties& before) const;

    // Scaling an entity scales what is under it, as in Unity (Caity's bug batch, 10-29): each child's offset and its
    // own size grow with it, all the way down. A child as it was when the scaling began; `parent` is the entry it hangs
    // from, or -1 - i for the i-th entity scaled
    struct ScaledChild {
        QUuid id;
        int parent;
        glm::vec3 localPosition;
        glm::vec3 dimensions;      // its own
        glm::quat localRotation;
        glm::quat frame;           // the axes its offset is in, in its parent's: a joint's, else the parent's own
        float lineHeight { 0.0f }; // a Text's, which grows with it
        float falloff { 0.0f };    // a Light's, which keeps its glow in proportion to its range, as Range does
    };
    // the children of the entities, parents first: not those among the entities (scaled in their own right), the
    // locked ones (they take no edits: they and theirs stay as they were) or an avatar sitting on one
    QVector<ScaledChild> childrenToScale(const QList<QUuid>& ids) const;
    // the children's edits for each entity's new size over its old, along its own axes
    static QVector<EntityItemProperties> childScaleEdits(const QVector<ScaledChild>& children,
                                                         const QVector<glm::vec3>& ratios);
    static EntityItemProperties childScaleBefore(const ScaledChild& child);
    void sendChildScale(const QVector<ScaledChild>& children, const QVector<EntityItemProperties>& edits, bool final);

    PickRay pickRay(const QPoint& viewportPos) const;
    QUuid pickEntity(const QPoint& viewportPos) const;
    QUuid pickEntityOnRay(const PickRay& ray) const;
    // where the cursor points in the world: a surface, or the grid plane; false when off the viewport
    bool cursorWorldPoint(glm::vec3& point) const;
    glm::vec3 selectionAnchor() const;
    // the last selected entity's world transform; false when it is not in the tree
    bool primaryTransform(glm::vec3& position, glm::quat& rotation) const;
    glm::vec3 gizmoPosition() const;
    glm::quat gizmoRotation() const;
    glm::vec3 viewDirection() const;

    void clickSelect(const QPoint& viewportPos, Qt::KeyboardModifiers modifiers);
    void updateMarquee(const QPoint& viewportPos);
    void finishMarquee(Qt::KeyboardModifiers modifiers);
    QList<QUuid> entitiesInRect(const QRectF& rect) const;

    bool snapActive() const;
    void beginGizmoDrag(GizmoHandle handle, const PickRay& ray);
    void moveGizmoDrag(const QPoint& viewportPos);
    // V held while moving: the vertex of another object's mesh under the cursor, or its surface
    bool vertexSnapTarget(const QPoint& viewportPos, glm::vec3& target) const;
    void applyDrag(bool final);

    // creating
    glm::vec3 placementPoint(const QPoint* viewportPos = nullptr) const;
    float cameraYaw() const;
    void createAt(const QString& key, const glm::vec3& point);
    void createModelAt(const glm::vec3& point);
    void pasteAt(const glm::vec3& target);
    QList<QUuid> place(EntityItemProperties properties, const QString& text, const glm::vec3& point);
    // reads an entity file as an add takes it (importEntities, createPreset)
    bool readEntityFile(const QString& url, QVector<EntityItemProperties>& properties, QList<QUuid>& ids, AABox& bounds,
                        int& count);
    // one of the app's ready-made families of entities (Create > Special items > Vehicles): the file's ground (y 0)
    // on the point, at least ahead metres from the camera, facing the way the camera looks; one undo step
    bool createPreset(const QString& presetUrl, const QString& label, const glm::vec3& point, float ahead);
    void placeModel(const QUrl& url, const glm::vec3& point);
    void placeImage(const QUrl& url, const glm::vec3& point);
    void beginUpload(const QString& localPath, const glm::vec3& point, bool image);
    // the toolbar follows an upload: its progress, and why it failed
    void showUploadProgress(EditorUpload* upload);
    void pollModelDimensions();
    void showHierarchyMenu(const QPoint& globalPos);
    // Use Stable Links, once the listing is in
    void rewriteStableLinks(const UserFiles& files);
    void showViewportMenu(const QPoint& viewportPos, const QPoint& globalPos);
    void addScriptActions(QMenu& menu);
    void addSelectionActions(QMenu& menu, const glm::vec3* pasteTarget);
    // a note in the middle of the toolbar; one with a timeout clears itself
    void setStatus(const QString& text, int clearAfterMs = 0);
    // The QML overlay (chat, a dialog, a web view with a field) has the keyboard: the editor leaves its keys alone.
    bool qmlHasTextFocus() const;

    // read-only: the domain gives no building rights (an instance before World Edit is on)
    void updateReadOnly();
    QString readOnlyReason() const;
    // true when the world can be changed; otherwise says why on the toolbar and returns false
    bool editable();

    // wireframes for selected zones, lights and particle systems
    void updateBounds();

    // the lights' icons, and the handles of a light selected on its own (see EditorLightGizmos.h)
    void updateLightGizmos();
    // a selected vehicle's wheels, seats and force points, the part chosen in the Inspector bright (EditorVehicleGizmos.h)
    void updateVehicleGizmos();
    // the tracks' curves, and the handles of a Track selected on its own (EditorTrackGizmos.h)
    void updateTrackGizmos();
    // the Track selected on its own, whose points can be chosen and moved; false when the selection is anything else
    bool trackPointTrack(QUuid& id) const;
    // (chooseTrackPoint, insertTrackPoint and deleteTrackPoint are public, above: a click chooses a point, bright in
    // the viewport with the Move gizmo on it and the Inspector's Points list on its row; Shift+click on the curve
    // inserts one at an arc length; Delete removes the chosen one)
    // the Track's box grown or shrunk to hold its curve and guideway, as an edit of its dimensions
    void fitTrackBox(const EntityItemPointer& track, const QVector<glm::vec3>& points, EntityItemProperties& properties) const;
    void setGizmosVisible(bool visible);
    // what a click there selects: a light's icon, or else the surface under it
    QUuid pickAt(const QPoint& viewportPos) const;
    QUuid pickOnRay(const PickRay& ray) const;
    bool pointerOverHierarchy() const;
    void beginLightHandleDrag(const EditorLightHandle& handle, const PickRay& ray);
    void moveLightHandleDrag(const QPoint& viewportPos);
    void endLightHandleDrag(bool cancel);
    // the value a light handle's drag asks for, as the Inspector shows it: a range in metres, an angle in degrees
    float lightHandleValue(const PickRay& ray) const;

    bool _ready { false };
    bool _active { false };
    // a build mode toggle's timing, for its log line (Caity, 10-24: toggles took 0.5-2 s at times): when the Build
    // Mode shortcut's key was made (the system's clock, ms since start-up) and reached the client (the steady clock)
    int64_t _toggleKeyMadeMs { -1 };
    int64_t _toggleKeyArrivedMs { -1 };
    void reportToggleTiming(bool active, int64_t startMs, int64_t endMs);
    bool _gridVisible { true };
    EditorCamera _camera;
    EditorGrid _grid;
    EditorSelection _selection;
    EditorUndo _undo;
    QList<QUuid> _selectionWas;     // the selection before its latest change, for the history
    int _historyDepth { 0 };        // undo or redo is running: what the selection does then is not a new step
    int _selectionBatch { 0 };      // a gesture changing the selection in several steps (a marquee): one step at its end
    EditorClipboard _clipboard;
    EditorGizmo _gizmo;
    EditorToolBar::Tool _tool { EditorToolBar::Tool::Move };
    bool _localSpace { false };
    bool _pivotMode { false };
    bool _snapAlways { false };
    bool _vertexSnap { false };         // V is held
    float _moveSnap { 0.25f };          // metres
    float _rotateSnap { 15.0f };        // degrees
    float _scaleSnap { 0.1f };          // of the scale factor

    QVector3D _axes[3];
    glm::quat _axesOrientation;
    bool _axesValid { false };
    int _frameCounter { 0 };
    float _loadCheckSeconds { 0.0f };   // since the last refreshLoadProblems

    // a click or a marquee in the viewport
    bool _clickPending { false };
    QPoint _pressPos;
    Qt::KeyboardModifiers _pressModifiers;
    bool _marqueeActive { false };
    QRectF _marqueeRect;

    // a gizmo drag: the top-level selected entities and where they started
    DragKind _dragKind { DragKind::None };
    QList<QUuid> _dragIds;
    QVector<glm::vec3> _dragPositions;
    QVector<glm::quat> _dragRotations;
    QVector<glm::vec3> _dragDimensions;
    QVector<EntityItemProperties> _dragBefore;
    glm::vec3 _dragPivot;
    glm::quat _dragGizmoRotation;
    glm::vec3 _dragDelta;
    glm::quat _dragRotationDelta;
    glm::vec3 _dragScale { 1.0f };
    QVector<ScaledChild> _dragChildren;   // under the entities a scale drag scales
    QPoint _lastDragPos;
    QElapsedTimer _editTimer;

    // an edit in progress in the Inspector: the ids and the values before, for undo
    QTimer _inspectorRefresh;
    // a scrub in the Inspector sends its edits no faster than a gizmo drag does: the latest value waits here
    QTimer _liveEditTimer;
    QElapsedTimer _liveEditClock;
    QVariant _liveEditValue;
    QString _inspectorEditKey;
    QList<QUuid> _inspectorEditIds;
    QVector<EntityItemProperties> _inspectorBefore;

    // a teleport puts the camera back on the avatar: what Shift+F framed is framed again
    bool _reframePending { false };
    AABox _reframeBox;
    int _reframeFrames { 0 };
    // a new model is a 1 m box until it loads and reports its size
    QTimer _modelDimensionsTimer;
    // a new model, until it has taken its own size and kept it
    struct AwaitedModel {
        int polls { 0 };
        quint64 createdAt { 0 };   // usecTimestampNow()
        quint64 sizedAt { 0 };     // when its own size was last applied; 0 until then
        int sizings { 0 };
    };
    QHash<QUuid, AwaitedModel> _modelsAwaitingDimensions;
    QList<QPointer<QAction>> _menuActions;
    int _uploadsActive { 0 };
    EditorWorld* _world { nullptr };
    void updateWorldControls();
    bool _readOnly { false };
    QTimer _statusTimer;
    QPoint _rightPressPos;
    EditorBounds _bounds;
    std::vector<EditorBoundsShape> _boundsShapes;
    EditorLightGizmos _lightGizmos;
    EditorTrackGizmos _trackGizmos;
    QVector<QUuid> _nearbyTracks;       // the tracks round the camera, looked for every so often
    int _trackSearchFrame { -1 };
    int _trackPointChosen { -1 };       // the control point chosen in the Track selected on its own, -1 for none
    QUuid _trackPointTrack;             // that track
    // a drag of the chosen point: the track's points and transform when it began
    QVector<glm::vec3> _dragTrackPoints;
    glm::vec3 _dragTrackPosition;
    glm::quat _dragTrackRotation;
    EditorVehicleGizmos _vehicleGizmos;
    std::vector<EditorVehicleGizmoVehicle> _vehicleGizmoVehicles;
    QHash<QString, int> _vehiclePartChosen;   // the row selected in each of the Inspector's vehicle lists, by key ("vehicle.wheels")
    EditorTerrain _terrain;             // the Terrain tool: its brushes, strokes and outline
    bool _stableLinksPending { false };  // Use Stable Links waits for the listing of My Files
    bool _gizmosVisible { true };       // every light's icon, not only the selected ones'
    QVector<QUuid> _nearbyLights;       // the lights round the camera, looked for every so often
    int _lightSearchFrame { -1 };       // when they were last looked for; -1 to look at once
    // a light handle's drag: an Inspector edit of the handle's property, from the viewport
    EditorLightHandle _lightHandleDrag;
    QString _lightHandleKey;
    float _lightHandleOffset { 0.0f };  // where on the handle it was taken, so it doesn't jump to the pointer
    float _lightHandleValue { 0.0f };
    bool _lightHandleMoved { false };
    ulong _lastEscapeTime { 0 };        // the key event time of a first Escape, waiting for a second
    // per entity in an Inspector edit of Scale: its own size, to turn scale into dimensions
    QVector<glm::vec3> _inspectorNatural;
    QVector<ScaledChild> _inspectorChildren;   // under the entities an Inspector edit of Scale or Size scales

    bool _panelsCreated { false };
    // a panel of ours put above another (placeAbove), sized once both show: Qt doesn't size a hidden one
    QList<QPair<QPointer<QDockWidget>, QPointer<QDockWidget>>> _sharesToSize;
    QString _styleSource;     // styles/editor.qss as shipped (applyStyle)
    bool _syncingSelection { false };
    HierarchyPanel* _hierarchy { nullptr };
    InspectorPanel* _inspector { nullptr };
    AssetsPanel* _assets { nullptr };     // My Files, along the bottom
    EditorToolBar* _toolBar { nullptr };
    TerrainToolBar* _terrainBar { nullptr };
    int _impostorBakesQueued { 0 };       // Bake All's count, for the status bar
    int _impostorBakesDone { 0 };
    bool _impostorBakerConnected { false };
    QStatusBar* _statusBar { nullptr };   // the bottom of the window, in build mode: notes go there
    QLabel* _statusLabel { nullptr };
    EntityTreeModel* _treeModel { nullptr };
    // the entity server's list for the Hierarchy, and the rows chosen there that are on their way: they join the
    // selection as they arrive
    EditorEntityList* _entityList { nullptr };
    QList<QUuid> _arrivalIds;
    bool _selectingArrival { false };
    QUuid _goingTo;                       // the row the camera flew to, for the status bar
    QString _goingToName;
    QTimer _arrivalTimer;
    void onHierarchySelection(const QList<QUuid>& ids);
    void onEntityLoaded(const QUuid& id);
    // The Hierarchy's eye and hand (Caity, 2026-10-26: "do it like unity"): what this builder has hidden in their own
    // view and made unpickable by their own clicks, as Unity's scene visibility and pickability are. Kept on this
    // computer only, by entity id (unique across worlds), never sent to the world; the shared Visible and Ignore
    // Picking are the Inspector's
    QSet<QUuid> _hiddenHere;
    QSet<QUuid> _noPick;
    bool _localStatesLoaded { false };
    Setting::Handle<QStringList> _hiddenHereSetting { "editor/hiddenHere", QStringList() };
    Setting::Handle<QStringList> _noPickSetting { "editor/noPick", QStringList() };
    QTimer _hiddenHereTimer;   // entities that arrive while build mode is on are hidden again in one pass
    void loadLocalStates();
    void saveLocalStates();
    void onHierarchyToggle(int toggle, const QList<QUuid>& ids, bool on);
    // what this builder hid, hidden in this view (EntityItem::setLocallyVisible) while build mode is on, and shown
    // again as it goes off
    void applyHiddenHere(bool hide);
    void setLocallyHidden(const QList<QUuid>& ids, bool hidden);
    bool excludedFromPicks(const QUuid& id) const { return _hiddenHere.contains(id) || _noPick.contains(id); }
    // as build mode closes: a banner when entities here have their scripts off (shared, unlike the eye and hand);
    // its text kept for Test.hierarchyToggles
    void remindOfScriptsOff();
    QString _scriptsOffReminder;
    void updateListNote();
    // build mode narrows the field of view; the user's own is kept here so it survives a crash
    Setting::Handle<float> _fovBeforeBuild { "editor/fovBeforeBuild", 0.0f };
    float _savedFov { 0.0f };
};

#endif // hifi_Editor_h
