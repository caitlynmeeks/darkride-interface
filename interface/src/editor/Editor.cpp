//
//  Editor.cpp
//  interface/src/editor
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "Editor.h"
#include "ImpostorBaker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QMimeData>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QColor>
#include <QtGui/QCursor>
#include <QtGui/QImage>
#include <QtGui/QDropEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtCore/QEvent>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QAbstractSpinBox>
#include <QAction>   // QtWidgets in Qt 5, QtGui in Qt 6
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QTreeView>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QtQuickWidgets/QQuickWidget>
#endif

#include <OffscreenUi.h>

#include <EntityItem.h>
#include <EntityScriptingInterface.h>
#include <EntityTree.h>
#include <EntityTreeElement.h>
#include <EntityTreeRenderer.h>
#include <RenderableEntityItem.h>
#include <LightEntityItem.h>
#include <TextEntityItem.h>
#include <ParticleEffectEntityItem.h>
#include <VehiclePropertyGroup.h>
#include <WheeledVehicleSpec.h>
#include <gl/GLWidget.h>
#include <GLMHelpers.h>
#include <MainWindow.h>
#include <NodeList.h>
#include <NumericalConstants.h>
#include <PathUtils.h>
#include <SharedUtil.h>
#include <PickFilter.h>
#include <SimulationFlags.h>
#include <ViewFrustum.h>

#include <TrackCurve.h>
#include <TrackEntityItem.h>

#include <plugins/DisplayPlugin.h>
#include "Application.h"
#include "EntityClipboard.h"
#include "avatar/AvatarManager.h"
#include "avatar/MyAvatar.h"
#include "EditorCreate.h"
#include "EditorWorld.h"
#include "EditorEntityList.h"
#include "EntityTreeModel.h"
#include "InterfaceLogging.h"
#include "Menu.h"
#include "scripting/RenderScriptingInterface.h"
#include "PropertySpec.h"
#include "ui/AssetsPanel.h"
#include "ui/CodeEditor.h"   // interface/src/ui: the Code panel
#include "ui/PanelTheme.h"
#include "ui/Popup.h"
#include "ui/HierarchyPanel.h"
#include "ui/InspectorPanel.h"
#include "ui/PropertyEditors.h"
#include "ui/TerrainToolBar.h"
#include "ui/ViewportContainer.h"
#include "ui/WindowLayout.h"
#include "chat/ChatWindow.h"
#include <gl/GLHelpers.h>

namespace {
    const int INSPECTOR_REFRESH_MS = 100;   // how often the Inspector re-reads a small selection
    const int INSPECTOR_REFRESH_SLOW_MS = 500;   // and a large one
    const int INSPECTOR_FAST_LIMIT = 16;
    const int CLICK_SLOP_PIXELS = 5;        // a press that moves less than this is a click
    const int EDIT_INTERVAL_MS = 33;        // while dragging, edits go out at most this often
    const int SCALE_EDIT_INTERVAL_MS = 100; // scale edits are heavier for the server
    const int PRUNE_EVERY_FRAMES = 30;      // how often the selection drops entities that are gone
    const float LOAD_CHECK_SECONDS = 1.0f;  // how often the Hierarchy and Inspector look for what failed to load
    const float PASTE_FALLBACK_DISTANCE = 3.0f;   // in front of the camera, when the cursor points at nothing
    const float MAX_PLACEMENT_DISTANCE = 200.0f;  // a grid-plane hit farther than this is not a useful spot
    const float GO_TO_STANDOFF = 1.5f;            // the gap between the avatar and what it went to
    const int MODEL_DIMENSIONS_POLL_MS = 500;     // how often a new model is asked for its size
    // and for how long: a big upload is downloaded and parsed again before it reports its size (a 23 MB model
    // took longer than the 30 s this used to be, and stayed a 1 m box)
    const int MODEL_DIMENSIONS_POLL_LIMIT = 1200;   // 10 minutes
    // a model already in the cache loads at once, and its size went out before the server had the new entity:
    // the server never took it and sent back its own 1 m box. So the size waits a moment after the creation,
    // and is watched for a while after, to be applied again if the box comes back.
    const quint64 MODEL_SIZE_DELAY_USECS = 1500 * USECS_PER_MSEC;
    const quint64 MODEL_SIZE_WATCH_USECS = 5 * USECS_PER_SECOND;
    const int MODEL_SIZE_ATTEMPTS = 3;
    const int REFRAME_FRAMES = 180;               // how long after Shift+F a camera reset re-frames
    const int REJECTION_CHECK_MS = 3000;          // a new entity gone by then was refused by the server
    const int STATUS_CLEAR_MS = 6000;             // how long a note on the status bar stays
    const QString SCALE_KEY = QStringLiteral("scale");
    const QString LOCKED_KEY = QStringLiteral("locked");
    // a lock without the right is refused by the world with nothing on the screen: Locked (the Inspector's and the
    // Hierarchy's) greys out and says why
    const QString NO_LOCK_RIGHT = "You can't lock or unlock things in this world. That takes the world's "
        "Lock / Unlock right, which its owner can give.";
    const QString SCRIPTS_ENABLED_KEY = QStringLiteral("scriptsEnabled");
    const int SCRIPTS_OFF_REMINDER_MS = 7000;   // the reminder, as build mode closes, that scripts are still off
    const int HIDDEN_HERE_PASS_MS = 150;   // entities that arrived are hidden again this long after the last
    const QString RANGE_KEY = QStringLiteral("range");
    const QString VOXEL_SIZE_KEY = QStringLiteral("voxelSize");
    const QString CUTOFF_KEY = QStringLiteral("cutoff");
    const float MIN_DIMENSION = 0.001f;
    // the lights round the camera, for their icons: looked for this often, this far out
    const int LIGHT_SEARCH_FRAMES = 15;
    const float LIGHT_SEARCH_RADIUS = 150.0f;
    const int TRACK_SEARCH_FRAMES = 15;
    const QString TRACK_POINTS_KEY = QStringLiteral("trackPoints");
    const float TRACK_SEARCH_RADIUS = 600.0f;   // a track runs far; its gizmo is wanted from afar
    const float TRACK_BOX_MARGIN = 0.5f;        // m round the curve and guideway in a fitted Track box
    // what the light handles can drag to: the Inspector's own limits, and no narrower a cone than this
    const float MIN_LIGHT_RANGE = 0.01f;
    const float MIN_SPOT_CUTOFF = 1.0f;
    const float MAX_SPOT_CUTOFF = 90.0f;
    const ulong DOUBLE_ESCAPE_MS = 400;     // two taps of Escape this close leave build mode
    const float BUILD_MODE_FOV = 90.0f;     // the client's units (its default is 120); less edge stretch on the gizmos

    // what a click or a paste can land on: the world's visible entities, picked against their meshes
    PickFilter worldPickFilter() {
        PickFilter::Flags flags;
        flags.set(PickFilter::DOMAIN_ENTITIES);
        flags.set(PickFilter::VISIBLE);
        flags.set(PickFilter::COLLIDABLE);
        flags.set(PickFilter::NONCOLLIDABLE);
        flags.set(PickFilter::PRECISE);
        return PickFilter(flags);
    }

    // the world's lights, switched on or off, for their icons
    PickFilter lightSearchFilter() {
        PickFilter::Flags flags;
        flags.set(PickFilter::DOMAIN_ENTITIES);
        flags.set(PickFilter::VISIBLE);
        flags.set(PickFilter::INVISIBLE);
        flags.set(PickFilter::COLLIDABLE);
        flags.set(PickFilter::NONCOLLIDABLE);
        return PickFilter(flags);
    }

    float tanHalfFov() {
        return 1.0f / qApp->getCamera().getProjection()[1][1];
    }

    float viewportHeight() {
        auto viewport = qApp->getPrimaryWidget();
        return viewport ? (float)std::max(1, viewport->height()) : 1.0f;
    }

    glm::vec3 snapVector(const glm::vec3& v, float increment) {
        return glm::round(v / increment) * increment;
    }

    float snapValue(float v, float increment) {
        return std::round(v / increment) * increment;
    }

    EntityTreePointer entityTree() {
        auto renderer = qApp->getEntities();
        return renderer ? renderer->getTree() : nullptr;
    }

    // the shortcut modifier: Command, and also Option and Control, for hands trained on other keyboards
    // (a PC keyboard's Alt key is Option on macOS; Option never pairs with a letter in the editor otherwise)
    bool commandModifier(Qt::KeyboardModifiers modifiers) {
        return modifiers.testFlag(Qt::ControlModifier) || modifiers.testFlag(Qt::AltModifier)
            || modifiers.testFlag(Qt::MetaModifier);
    }

    GizmoMode gizmoModeFor(EditorToolBar::Tool tool) {
        switch (tool) {
            case EditorToolBar::Tool::Rotate: return GizmoMode::Rotate;
            case EditorToolBar::Tool::Scale: return GizmoMode::Scale;
            default: return GizmoMode::Move;
        }
    }
}

Editor::Editor() : _selection(this), _undo(this), _terrain(&_undo, this) {
    connect(&_selection, &EditorSelection::changed, this, &Editor::onSelectionChanged);
    connect(&_undo, &EditorUndo::terrainEdited, &_terrain, &EditorTerrain::markChanged);
    connect(&_terrain, &EditorTerrain::status, this, &Editor::setStatus);
    _undo.setSelectionApplier([this](const QList<QUuid>& ids) {
        ++_historyDepth;
        _selection.set(ids);
        --_historyDepth;
    });
    _inspectorRefresh.setInterval(INSPECTOR_REFRESH_MS);
    connect(&_inspectorRefresh, &QTimer::timeout, this, [this] { refreshInspector(false); });
    _liveEditTimer.setSingleShot(true);
    connect(&_liveEditTimer, &QTimer::timeout, this, [this] {
        if (!_inspectorEditKey.isEmpty() && _liveEditValue.isValid()) {
            applyLiveEdit(_inspectorEditKey, _liveEditValue);
        }
    });
    _modelDimensionsTimer.setInterval(MODEL_DIMENSIONS_POLL_MS);
    connect(&_modelDimensionsTimer, &QTimer::timeout, this, &Editor::pollModelDimensions);
    _world = new EditorWorld(this);
    connect(_world, &EditorWorld::changed, this, [this] {
        updateWorldControls();
        updateReadOnly();   // the reason given for read-only depends on what the world is
    });
    connect(_world, &EditorWorld::message, this, [this](const QString& text) {
        // "Saving…" stays until the result replaces it; results clear themselves
        setStatus(text, text.endsWith(QStringLiteral("…")) ? 0 : STATUS_CLEAR_MS);
    });
    _statusTimer.setSingleShot(true);
    connect(&_statusTimer, &QTimer::timeout, this, [this] { setStatus(QString()); });
    const int ARRIVAL_WAIT_MS = 10000;
    _arrivalTimer.setSingleShot(true);
    _arrivalTimer.setInterval(ARRIVAL_WAIT_MS);
    connect(&_arrivalTimer, &QTimer::timeout, this, [this] {
        if (!_goingTo.isNull() && _treeModel && !_treeModel->isLoaded(_goingTo)) {
            setStatus(QString("%1 hasn't arrived. It may have been deleted since the list was made: "
                              "Refresh the Entities list.").arg(_goingToName), STATUS_CLEAR_MS);
        }
        _goingTo = QUuid();
    });
}

// --- build mode on and off ----------------------------------------------------------------------

void Editor::setReady() {
    _ready = true;
    // the world controls follow the domain: ask again whenever a connection completes; the
    // panels follow the building rights the domain grants (World Edit grants them live)
    auto nodeList = DependencyManager::get<NodeList>();
    connect(&nodeList->getDomainHandler(), &DomainHandler::connectedToDomain, this, [this] {
        if (_active) {
            _world->refresh();
            updateReadOnly();
        }
    });
    connect(nodeList.data(), &NodeList::canRezChanged, this, [this] {
        if (_active) {
            updateReadOnly();
        }
    });
    connect(nodeList.data(), &NodeList::canRezTmpChanged, this, [this] {
        if (_active) {
            updateReadOnly();
        }
    });
    // the Lock / Unlock right: the Inspector's Locked follows it
    connect(nodeList.data(), &NodeList::isAllowedEditorChanged, this, [this] {
        if (_active) {
            updateReadOnly();
        }
    });
    // the graphics settings can switch to forward rendering while build mode is on (the Low and Potato presets do)
    connect(RenderScriptingInterface::getInstance(), &RenderScriptingInterface::settingsChanged, this, [this] {
        if (_active) {
            updateRenderMethod();
        }
    });
    // the client died in build mode last time: give the field of view back
    float stale = _fovBeforeBuild.get();
    if (stale > 0.0f) {
        qApp->setFieldOfView(stale);
        _fovBeforeBuild.set(0.0f);
    }
}

static int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Editor::setActive(bool active) {
    if (active == _active) {
        return;
    }
    if (active && !_ready) {
        // the menu's saved state, restored at startup: build mode always starts off
        Menu::getInstance()->setIsOptionChecked(MenuOption::BuildMode, false);
        return;
    }
    const int64_t toggleStartMs = steadyNowMs();
    // the GL view keeps its size while the panels come or go, and takes the final one once, below: coming on, the
    // panels arrived one by one, and the view and the overlay were resized five times over (10-24: a toggle's jank)
    if (auto viewport = qApp->getPrimaryWidget()) {
        if (auto container = qobject_cast<ViewportContainer*>(viewport->parentWidget())) {
            container->setDeferResizes(true);
        }
    }
    _active = active;
    if (active) {
        createPanels();
        showPanels(true);
        _savedFov = qApp->getFieldOfView();
        _fovBeforeBuild.set(_savedFov);
        qApp->setFieldOfView(BUILD_MODE_FOV);
        _camera.enter();
        _grid.setVisible(_gridVisible);
        _selection.setHighlightEnabled(true);
        updateRenderMethod();
        // a builder sees everything: a zone set to cull what is outside it hid new entities (a new light gave no
        // light until the zone's list was made again) and made lights blink as they were edited
        if (auto renderer = qApp->getEntities()) {
            renderer->setZoneCullingSuspended(true);
        }
        if (_treeModel) {
            _treeModel->attach();
        }
        refreshEntityList();
        // what this builder hid is hidden in their view again
        applyHiddenHere(true);
        _axesValid = false;
        updateAxes();
        refreshInspector(true);
        _inspectorRefresh.start();
        focusViewport();
        setStatus(QString());
        _world->refresh();
    } else {
        // everything shows again as everyone sees it
        _hiddenHereTimer.stop();
        applyHiddenHere(false);
        remindOfScriptsOff();
        _boundsShapes.clear();
        _bounds.setShapes(_boundsShapes);
        endLightHandleDrag(false);
        _terrain.clear();
        _terrain.flushSaves();   // sculpting that was waiting to be saved goes up now
        _lightGizmos.clear();
        _vehicleGizmoVehicles.clear();
        _vehicleGizmos.setVehicles(_vehicleGizmoVehicles);
        _nearbyLights.clear();
        _lightSearchFrame = -1;
        // the tracks' curves too: update() stops once build mode is off, so the overlay would stay, into the next world
        _trackGizmos.clear();
        _nearbyTracks.clear();
        _trackSearchFrame = -1;
        _trackPointChosen = -1;
        _trackPointTrack = QUuid();
        _arrivalIds.clear();
        _goingTo = QUuid();
        _arrivalTimer.stop();
        _inspectorRefresh.stop();
        if (_gizmo.isDragging()) {
            applyDrag(true);
            _gizmo.endDrag();
            _dragKind = DragKind::None;
        }
        showPanels(false);   // the window's layout saves itself as they go (ui/WindowLayout.h)
        // the keyboard goes back to the world: a field that had it would keep it, hidden, and the avatar's keys
        // with it
        focusViewport();
        _camera.leave();
        if (_savedFov > 0.0f) {
            qApp->setFieldOfView(_savedFov);
        }
        _fovBeforeBuild.set(0.0f);
        _grid.setVisible(false);
        _gizmo.setVisible(false);
        _selection.setHighlightEnabled(false);
        updateRenderMethod();
        if (auto renderer = qApp->getEntities()) {
            renderer->setZoneCullingSuspended(false);
        }
        _selection.clear();
        _marqueeActive = false;
        emit marqueeChanged();
    }
    // The panels take their places now, and the GL view its new size with them: left to the next layout pass, with
    // resizes held for the splitters, the view kept its old size for a moment (a stretched picture as build mode came
    // and went). Then, while the panels can be dragged, the view takes a new size only once the dragging stops.
    // The layout settles with the view's resizes still held (from the top of setActive), and the view then takes
    // its final size once, inside the guard.
    if (auto viewport = qApp->getPrimaryWidget()) {
        if (auto container = qobject_cast<ViewportContainer*>(viewport->parentWidget())) {
            auto window = qApp->getWindow();
            {
                gl::ViewChangeGuard guard;
                if (window && window->layout()) {
                    window->layout()->activate();
                }
                container->setDeferResizes(false);
            }
            container->setDeferResizes(active);
        }
    }
    setMenuActionsEnabled(active);
    if (active) {
        updateReadOnly();
    }
    Menu::getInstance()->setIsOptionChecked(MenuOption::BuildMode, active);
    emit activeChanged(active);
    reportToggleTiming(active, toggleStartMs, steadyNowMs());
}

// One line a toggle: how long the shortcut's key waited before the client got it (its queue time), from then to
// the toggle, the toggle's own work, and when the next frame begun after it was shown. The frame is watched for from
// a short timer, so the line comes a moment after the toggle.
void Editor::reportToggleTiming(bool active, int64_t startMs, int64_t endMs) {
    auto plugin = qApp->getActiveDisplayPlugin();
    if (!plugin) {
        return;
    }
    QString key = "no key";
    if (_toggleKeyArrivedMs >= 0 && startMs - _toggleKeyArrivedMs < 5000) {
        // the key's own time is the system's uptime, the steady clock's base on macOS; a figure that makes no sense
        // (another base) is left out
        int64_t queued = _toggleKeyArrivedMs - _toggleKeyMadeMs;
        key = (queued >= 0 && queued < 60000 ? QString("the key waited %1 ms to reach the client, ").arg(queued) : QString())
            + QString("%1 ms from there to the toggle").arg(startMs - _toggleKeyArrivedMs);
    }
    _toggleKeyArrivedMs = -1;
    const uint32_t presentsAtEnd = plugin->presentCount();
    auto timer = new QTimer(this);
    timer->setInterval(5);
    connect(timer, &QTimer::timeout, this, [timer, plugin, presentsAtEnd, active, key, startMs, endMs] {
        const int64_t now = steadyNowMs();
        // the second present after the toggle: the first may be a frame begun before it
        if (plugin->presentCount() >= presentsAtEnd + 2 || now - endMs > 5000) {
            qCDebug(interfaceapp).noquote() << QString("Build mode %1: %2; the toggle took %3 ms; the next new frame was "
                                                      "shown %4 ms after it")
                .arg(active ? "on" : "off").arg(key).arg(endMs - startMs)
                .arg(plugin->presentCount() >= presentsAtEnd + 2 ? QString::number(now - endMs) : QString("more than 5000"));
            timer->stop();
            timer->deleteLater();
        }
    });
    timer->start();
}

void Editor::updateRenderMethod() {
    // Build mode outlines the selection, and both render methods draw the outline now (the forward path since
    // 2026-10-16, over its resolved frame), so the main view renders by the graphics settings' method in build mode
    // too; this only re-applies it, as it used to force deferred here while deferred alone had the outline.
    auto engine = qApp->getRenderEngine();
    auto config = engine ? dynamic_cast<render::SwitchConfig*>(
        engine->getConfiguration()->getConfig("RenderMainView.DeferredForwardSwitch")) : nullptr;
    if (!config) {
        return;
    }
    config->setBranch((uint8_t)RenderScriptingInterface::getInstance()->getRenderMethod());
}

void Editor::setGridVisible(bool visible) {
    if (visible == _gridVisible) {
        return;
    }
    _gridVisible = visible;
    if (_active) {
        _grid.setVisible(visible);
    }
    Menu::getInstance()->setIsOptionChecked(MenuOption::BuildGrid, visible);
    if (_toolBar) {
        _toolBar->setGridChecked(visible);
    }
    emit gridVisibleChanged(visible);
}

void Editor::setTool(EditorToolBar::Tool tool) {
    if (_gizmo.isDragging() || _terrain.isStroking()) {
        return;
    }
    _tool = tool;
    if (_toolBar) {
        _toolBar->setTool(tool);
    }
    applyTool();
}

void Editor::applyTool() {
    if (_tool != EditorToolBar::Tool::Terrain) {
        _terrain.clear();
    }
    updateTerrainBar();
    updateGizmo();
}

void Editor::updateTerrainBar() {
    if (!_terrainBar) {
        return;
    }
    bool show = _active && _tool == EditorToolBar::Tool::Terrain;
    if (_terrainBar->isHidden() == show) {
        gl::ViewChangeGuard guard;   // the Terrain row coming or going resizes the GL view
        _terrainBar->setVisible(show);
    }
}

void Editor::setLocalSpace(bool local) {
    if (_localSpace == local) {
        return;
    }
    _localSpace = local;
    if (_toolBar) {
        _toolBar->setLocalSpace(local);
    }
    updateGizmo();
}

void Editor::setPivotMode(bool pivot) {
    if (_pivotMode == pivot) {
        return;
    }
    _pivotMode = pivot;
    if (_toolBar) {
        _toolBar->setPivotMode(pivot);
    }
    updateGizmo();
}

void Editor::setMenuActionsEnabled(bool enabled) {
    for (const auto& action : _menuActions) {
        if (action) {
            // the Create items (their data is the catalogue key) also need building rights
            bool creates = !action->data().toString().isEmpty();
            action->setEnabled(enabled && !(creates && _readOnly));
        }
    }
}

// --- read-only ----------------------------------------------------------------------------------

QString Editor::readOnlyReason() const {
    if (_world->isInstance() && _world->canEdit()) {
        return "Read-only: turn on World Edit (toolbar) to change this world";
    }
    return "Read-only: you have no building rights in this world";
}

void Editor::updateReadOnly() {
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    _readOnly = !entities->canRez() && !entities->canRezTmp();
    QString reason = readOnlyReason();
    if (_inspector) {
        _inspector->setReadOnly(_readOnly, reason);
        _inspector->setCanLock(entities->canAdjustLocks(), NO_LOCK_RIGHT);
    }
    if (_hierarchy) {
        _hierarchy->setReadOnly(_readOnly, reason);
        _hierarchy->setCanLock(!_readOnly && entities->canAdjustLocks(), _readOnly ? reason : NO_LOCK_RIGHT);
    }
    updateInspectorToggles();
    if (_toolBar) {
        _toolBar->setCreateEnabled(!_readOnly);
    }
    setMenuActionsEnabled(_active);
    if (_readOnly && _gizmo.isDragging()) {
        _gizmo.endDrag();
        _dragKind = DragKind::None;
    }
    if (_readOnly) {
        endLightHandleDrag(true);
    }
    updateGizmo();
}

bool Editor::editable() {
    if (!_readOnly) {
        return true;
    }
    setStatus(readOnlyReason(), STATUS_CLEAR_MS);
    return false;
}

void Editor::addMenuAction(QAction* action) {
    if (action) {
        action->setEnabled(_active);
        _menuActions << action;
    }
}

void Editor::createPanels() {
    if (_panelsCreated) {
        return;
    }
    auto window = qApp->getWindow();
    if (!window) {
        return;
    }
    _panelsCreated = true;

    _treeModel = new EntityTreeModel(this);
    // the style (and the Hierarchy's red and grey rows) from the skin, now and whenever it changes
    loadStyle();
    applyStyle();
    connect(_treeModel, &EntityTreeModel::renameRequested, this, &Editor::rename);
    connect(_treeModel, &EntityTreeModel::reparentRequested, this, &Editor::reparent);

    connect(_treeModel, &EntityTreeModel::entityLoaded, this, &Editor::onEntityLoaded);
    // a domain change empties the tree and the list with it: ask the new server
    connect(_treeModel, &EntityTreeModel::listCleared, this, &Editor::refreshEntityList);

    _entityList = new EditorEntityList(this);
    connect(_entityList, &EditorEntityList::received, this, [this](const QVector<EntityListMessage::Record>& records) {
        _treeModel->setListed(records);
    });
    connect(_entityList, &EditorEntityList::stateChanged, this, &Editor::updateListNote);

    _hierarchy = new HierarchyPanel(window);
    _hierarchy->setModel(_treeModel);
    connect(_hierarchy, &HierarchyPanel::selectionChangedByUser, this, &Editor::onHierarchySelection);
    connect(_hierarchy, &HierarchyPanel::frameRequested, this, [this](const QUuid& id) {
        if (_treeModel && !_treeModel->isLoaded(id)) {
            goToListed(id);
            return;
        }
        _selection.set({ id });
        frameSelection();
    });
    connect(_hierarchy, &HierarchyPanel::goToRequested, this, &Editor::goToListed);
    connect(_hierarchy, &HierarchyPanel::refreshRequested, this, &Editor::refreshEntityList);
    connect(_hierarchy, &HierarchyPanel::contextMenuRequested, this, &Editor::showHierarchyMenu);
    // the rows' eye, hand and lock
    loadLocalStates();
    _hierarchy->setLocalStates(_hiddenHere, _noPick);
    connect(_hierarchy, &HierarchyPanel::toggleRequested, this,
            [this](HierarchyPanel::Toggle toggle, const QList<QUuid>& ids, bool on) { onHierarchyToggle(toggle, ids, on); });
    connect(_hierarchy, &HierarchyPanel::showAllRequested, this, &Editor::showAllHere);
    _hiddenHereTimer.setSingleShot(true);
    _hiddenHereTimer.setInterval(HIDDEN_HERE_PASS_MS);
    connect(&_hiddenHereTimer, &QTimer::timeout, this, [this] {
        if (_active) {
            applyHiddenHere(true);
        }
    });
    // an entity this builder hid comes back from the server as a new item, shown: hidden again as it arrives
    auto hideArrivals = [this] {
        if (_active && !_hiddenHere.isEmpty() && !_hiddenHereTimer.isActive()) {
            _hiddenHereTimer.start();
        }
    };
    connect(_treeModel, &QAbstractItemModel::rowsInserted, this, hideArrivals);
    connect(_treeModel, &EntityTreeModel::entityLoaded, this, hideArrivals);

    _inspector = new InspectorPanel(window);
    connect(_inspector, &InspectorPanel::editBegan, this, &Editor::onInspectorEditBegan);
    connect(_inspector, &InspectorPanel::liveEdit, this, &Editor::onInspectorLiveEdit);
    connect(_inspector, &InspectorPanel::committed, this, &Editor::onInspectorCommitted);
    // the header's toggle icons: the Hierarchy rows' rules, for the selection alone
    connect(_inspector, &InspectorPanel::toggleClicked, this, [this](int toggle) {
        if (_hierarchy) {
            _hierarchy->toggleIds((HierarchyPanel::Toggle)toggle, _selection.ids());
        }
    });
    connect(_inspector, &InspectorPanel::editCancelled, this, &Editor::onInspectorEditCancelled);
    connect(_inspector, &InspectorPanel::uploadRequested, this, &Editor::onInspectorUpload);

    // My Files along the bottom, made already if normal play opened it
    myFilesPanel();

    _toolBar = new EditorToolBar(window);
    _toolBar->setGridChecked(_gridVisible);
    _toolBar->setGizmosChecked(_gizmosVisible);
    _toolBar->setTool(_tool);
    _toolBar->setLocalSpace(_localSpace);
    _toolBar->setPivotMode(_pivotMode);
    connect(_toolBar, &EditorToolBar::exitRequested, this, [this] { setActive(false); });
    connect(_toolBar, &EditorToolBar::gridToggled, this, &Editor::setGridVisible);
    connect(_toolBar, &EditorToolBar::gizmosToggled, this, &Editor::setGizmosVisible);
    connect(_toolBar, &EditorToolBar::toolChanged, this, [this](EditorToolBar::Tool tool) {
        _tool = tool;
        applyTool();
    });
    connect(_toolBar, &EditorToolBar::localSpaceToggled, this, &Editor::setLocalSpace);
    connect(_toolBar, &EditorToolBar::pivotToggled, this, &Editor::setPivotMode);
    connect(_toolBar, &EditorToolBar::snapToggled, this, [this](bool on) { _snapAlways = on; });
    _toolBar->setCreateMenu(buildCreateMenu(_toolBar));
    connect(_toolBar, &EditorToolBar::worldEditToggled, this, [this](bool on) { _world->setEditing(on); });
    connect(_toolBar, &EditorToolBar::saveWorldRequested, this, [this] { _world->save(); });
    updateWorldControls();

    // the Terrain tool's row, under the toolbar, shown while that tool is active
    _terrainBar = new TerrainToolBar(window);
    connect(_terrainBar, &TerrainToolBar::brushChanged, this, [this] { _terrain.setBrush(_terrainBar->brush()); });
    connect(_terrainBar, &TerrainToolBar::resampleRequested, this, [this] {
        if (editable()) {
            _terrain.resample(_selection.ids());
        }
    });
    _terrain.setBrush(_terrainBar->brush());
    // a Terrain's Layers section in the Inspector reads and sets the layers through the tool, and its selected
    // layer is the one the Paint tools paint; the toolbar's Layer field names the slots after the selected
    // terrain's layers (else the one under the pointer's)
    _inspector->setTerrainEditor(&_terrain);
    connect(_inspector, &InspectorPanel::terrainLayerChosen, this, [this](int slot) { _terrainBar->setLayer(slot); });
    _terrainBar->setLayerNameSource([this] { return _terrain.layerNamesForTool(_selection.ids()); });
    connect(_inspector, &InspectorPanel::terrainLayersShown, _terrainBar, &TerrainToolBar::refreshLayerNames);
    // a vehicle's Wheels or Seats list: the selected row is drawn bright in the viewport
    connect(_inspector, &InspectorPanel::vehiclePartChosen, this, [this](const QString& key, int index) {
        _vehiclePartChosen[key] = index;
    });
    // a Track's Points list: the selected row is the point the viewport draws bright and the Move gizmo stands on
    connect(_inspector, &InspectorPanel::trackPointChosen, this, [this](int index) { chooseTrackPoint(index, true); });
    connect(&_terrain, &EditorTerrain::terrainUnderPointerChanged, _terrainBar, &TerrainToolBar::refreshLayerNames);

    // notes ("Terrain saved", an upload's progress, a refusal) in a status bar along the bottom, where the
    // toolbars' buttons cannot crowd them out
    _statusBar = new QStatusBar(window);
    _statusBar->setObjectName("EditorStatusBar");
    _statusBar->setSizeGripEnabled(false);
    _statusLabel = new QLabel(_statusBar);
    _statusLabel->setObjectName("EditorStatus");
    _statusLabel->setTextFormat(Qt::PlainText);
    _statusBar->addWidget(_statusLabel, 1);

    // each panel where the window's saved layout has it (ui/WindowLayout.h, restored after sign-in), else where it
    // starts
    auto layout = WindowLayout::instance();
    QList<QDockWidget*> across;
    QList<int> widths;
    if (!layout->restoreDock(_hierarchy)) {
        window->addDockWidget(Qt::LeftDockWidgetArea, _hierarchy);
        placeAbove(_hierarchy, Qt::LeftDockWidgetArea);
        across << _hierarchy;
        widths << 260;
    }
    if (!layout->restoreDock(_inspector)) {
        window->addDockWidget(Qt::RightDockWidgetArea, _inspector);
        placeAbove(_inspector, Qt::RightDockWidgetArea);
        across << _inspector;
        widths << 430;
    }
    layout->watch(_hierarchy);
    layout->watch(_inspector);
    placeToolBars();
    _terrainBar->setVisible(false);
    window->setStatusBar(_statusBar);
    _statusBar->setVisible(false);
    if (!across.isEmpty()) {
        window->resizeDocks(across, widths, Qt::Horizontal);
    }
    // hidden until showPanels: normal play's layout from before Build mode had its own could show them (My Files is
    // normal play's too, as that layout has it)
    _hierarchy->hide();
    _inspector->hide();
    qApp->installEventFilter(this);

    // a layout saved with both toolbars on one row would keep them there
    placeToolBars();
    updateTerrainBar();   // the saved layout knows nothing of the tool
}

void Editor::loadStyle() {
    if (!_styleSource.isEmpty()) {
        return;
    }
    // one dark stylesheet for the panels; scoped by object name so the rest of the window is untouched. Under a skin
    // (ui/Skins.h) its colours become the skin's, and they follow the skin as it changes
    QFile styleSheet(PathUtils::resourcesPath() + "styles/editor.qss");
    if (styleSheet.open(QIODevice::ReadOnly)) {
        _styleSource = QString::fromUtf8(styleSheet.readAll());
    }
    applyStyle();
    connect(DependencyManager::get<PanelTheme>().data(), &PanelTheme::changed, this, &Editor::applyStyle,
            Qt::UniqueConnection);
}

QDockWidget* Editor::myFilesPanel() {
    if (_assets) {
        return _assets;
    }
    auto window = qApp->getWindow();
    if (!window) {
        return nullptr;
    }
    loadStyle();
    gl::ViewChangeGuard guard;   // a panel coming back docked changes the GL view
    _assets = new AssetsPanel(window);
    // its notes (a link copied, an upload's progress) go to Build mode's status bar, or to its own foot in normal play
    connect(_assets, &AssetsPanel::status, this, [this](const QString& text, int clearAfterMs) {
        setStatus(text, clearAfterMs);
    });
    // the tools, the handle space, Frame and V work from the panel too, in Build mode
    _assets->setKeyFallback([this](QKeyEvent* event) {
        return event->type() == QEvent::KeyPress ? handleKeyPress(event) : handleKeyRelease(event);
    });
    // its listing is fetched as it shows (not tabbed behind another panel)
    connect(_assets, &QDockWidget::visibilityChanged, _assets, &AssetsPanel::setActive);
    // it runs under the Hierarchy and the viewport, as Unity's Project window does; the Inspector keeps the right side
    // whole
    window->setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    // where the window's saved layout has it, open or closed as it was; asked before it's added anywhere, as adding it
    // drops the layout's place for it
    _assets->hide();
    if (!WindowLayout::instance()->restoreDock(_assets)) {
        window->addDockWidget(Qt::BottomDockWidgetArea, _assets);
        window->resizeDocks({ _assets }, { 240 }, Qt::Vertical);
        _assets->hide();
    }
    WindowLayout::instance()->watch(_assets);
    return _assets;
}

QDockWidget* Editor::hierarchyPanel() const {
    return _hierarchy;
}

QDockWidget* Editor::inspectorPanel() const {
    return _inspector;
}

void Editor::placeToolBars() {
    // The build toolbar is a row of its own, so its spacer keeps the world and view controls at the right end;
    // the Terrain tool's controls are the row under it. Toolbars sharing a row each get their natural width and
    // only the last is stretched, so a spacer can push nothing past a neighbour: the rows are placed here rather
    // than left to the saved layout.
    auto window = qApp->getWindow();
    gl::ViewChangeGuard guard;   // the rows leaving and returning resize the GL view twice over
    window->removeToolBar(_terrainBar);
    window->removeToolBar(_toolBar);
    window->addToolBar(Qt::TopToolBarArea, _toolBar);
    window->addToolBarBreak(Qt::TopToolBarArea);
    window->addToolBar(Qt::TopToolBarArea, _terrainBar);
}

void Editor::focusViewport() {
    // and from a web entity clicked before (the Cinema's remote): it keeps the keyboard otherwise, every key going to
    // its page first (KeyboardFocus::deliverKeyEvent), since build mode's clicks never reach the app's own focus change;
    // F, Delete and the rest did nothing (Caity's desk test, 10-29)
    qApp->getKeyboardFocus().setFocusedEntity(UNKNOWN_ENTITY_ID);
    if (auto viewport = qApp->getPrimaryWidget()) {
        if (!viewport->hasFocus()) {
            viewport->setFocus(Qt::MouseFocusReason);
        }
    }
}

namespace {
    // a field on the desktop surface has the keyboard (chat, a dialog, a URL box)
    bool surfaceHasTextFocus() {
        if (auto offscreenUi = DependencyManager::get<OffscreenUi>()) {
            if (auto window = offscreenUi->getWindow()) {
                if (auto item = window->activeFocusItem()) {
                    return item->inherits("QQuickTextInput") || item->inherits("QQuickTextEdit")
                        || item->inherits("QQuickWebEngineView");
                }
            }
        }
        return false;
    }

    // a field in a panel has the keyboard: a panel's view isn't on the desktop surface. Asked as the input method asks, so
    // a web page's field counts as a QML one does, and a button or a page with no field in use doesn't
    bool panelHasTextFocus() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QObject* focus = QGuiApplication::focusObject();   // a panel's view is a window of its own
#else
        auto view = qobject_cast<QQuickWidget*>(QApplication::focusWidget());
        QObject* focus = view ? view->quickWindow()->activeFocusItem() : nullptr;
#endif
        auto item = qobject_cast<QQuickItem*>(focus);
        return item && item->inputMethodQuery(Qt::ImEnabled).toBool();
    }
}

void Editor::takeFocusUnlessTyping() {
    QWidget* focus = QApplication::focusWidget();
    if (focus && (qobject_cast<QLineEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus) ||
                  qobject_cast<QTextEdit*>(focus) || qobject_cast<QPlainTextEdit*>(focus))) {
        return;
    }
    if (panelHasTextFocus()) {
        return;
    }
    focusViewport();
}

void Editor::showPanels(bool show) {
    if (!_panelsCreated) {
        return;
    }
    // An open popup (a dropdown's list in the Inspector) closes first, outside the guard: a combo box closing its
    // list runs an event loop of its own on macOS (the chosen item's flash), and a QML surface rendered in that loop
    // waits for its render thread, which waits for the GL lock the guard holds. Hiding the panels with the list open
    // did that and froze the client (10-24: the Render Layer list open, Cmd+B).
    for (int i = 0; i < 4 && QApplication::activePopupWidget(); i++) {
        QApplication::activePopupWidget()->close();
    }
    gl::ViewChangeGuard guard;   // panels coming and going change the window round the GL view
    auto window = qApp->getWindow();
    auto layout = WindowLayout::instance();
    if (show) {
        // Build mode's own arrangement, as it was left (normal play keeps its own: ui/WindowLayout.h); the first
        // time, its panels where they start and the others as they are
        if (!layout->enterBuildMode()) {
            _hierarchy->show();
            _inspector->show();
            _assets->show();
            for (const auto& share : _sharesToSize) {
                if (share.first && share.second && !share.second->isHidden() && !share.second->isFloating()) {
                    window->resizeDocks({ share.first, share.second }, { 3000, 2000 }, Qt::Vertical);
                }
            }
        }
        _sharesToSize.clear();
        placeToolBars();   // the rows are placed here rather than left to a saved layout
    } else {
        layout->leaveBuildMode();
        // the Hierarchy and the Inspector are Build mode's only, whatever normal play's arrangement says
        _hierarchy->hide();
        _inspector->hide();
    }
    _toolBar->setVisible(show);
    _statusBar->setVisible(show);
    updateTerrainBar();
}

QList<QDockWidget*> Editor::otherDockedPanels() const {
    QList<QDockWidget*> found;
    auto window = qApp->getWindow();
    if (!window) {
        return found;
    }
    for (QDockWidget* dock : window->findChildren<QDockWidget*>()) {
        if (dock != _hierarchy && dock != _inspector && dock != _assets && !dock->isHidden() && !dock->isFloating() &&
            window->dockWidgetArea(dock) != Qt::NoDockWidgetArea) {
            found << dock;
        }
    }
    return found;
}

void Editor::placeAbove(QDockWidget* ours, Qt::DockWidgetArea area) {
    auto window = qApp->getWindow();
    for (QDockWidget* other : otherDockedPanels()) {
        if (window->dockWidgetArea(other) == area) {
            // added after it, ours would sit under it with only what it asks for (the Inspector came out 86 px tall
            // under chat, 10-25); the sizes given later are shares, which Qt fits to the side
            window->splitDockWidget(ours, other, Qt::Vertical);
            // 3 to 2, once ours shows (showPanels)
            _sharesToSize.append(qMakePair(QPointer<QDockWidget>(ours), QPointer<QDockWidget>(other)));
            return;
        }
    }
}

namespace {
    // Darkride's own panel colours (styles/editor.qss) and the skin colour each one becomes under a skin (SKINS.md
    // "colors"): backgrounds, fields, headers, hovers, lines, text and the selection blue. The orange World Edit button,
    // the read-only banner, white text on the selection and the terrain drop target's light blues keep their own.
    const QHash<QString, QString>& skinRoleForColor() {
        static const QHash<QString, QString> ROLES {
            { "#383838", "background" },
            { "#2a2a2a", "fill" }, { "#2f2f2f", "fill" }, { "#323232", "fill" },
            { "#3c3c3c", "card" }, { "#454545", "card" }, { "#3a3a3a", "card" },
            { "#404040", "cardPressed" }, { "#474747", "cardPressed" }, { "#505050", "cardPressed" },
            { "#1f1f1f", "separator" }, { "#232323", "separator" }, { "#262626", "separator" }, { "#2b2b2b", "separator" },
            { "#5a5a5a", "segmentThumb" }, { "#555555", "segmentThumb" },
            { "#d2d2d2", "text" }, { "#c4c4c4", "text" }, { "#f0f0f0", "text" },
            { "#b8b8b8", "secondaryText" }, { "#9a9a9a", "secondaryText" }, { "#8f8f8f", "secondaryText" },
            { "#8a8a8a", "secondaryText" },
            { "#7a7a7a", "tertiaryText" }, { "#6a6a6a", "tertiaryText" },
            { "#2c5d87", "tint" }, { "#3a6f9c", "tint" },
            { "#ff6b6b", "destructive" }
        };
        return ROLES;
    }
    const QString STYLE_BEGIN = "/* build mode panels: begin */";
    const QString STYLE_END = "/* build mode panels: end */";
}

void Editor::applyStyle() {
    auto window = qApp->getWindow();
    if (!window || _styleSource.isEmpty()) {
        return;
    }
    // the skin's colours over Darkride's own, where the skin gives that role a colour (a skin may set only some)
    QString themed = _styleSource;
    QVariantMap colors = DependencyManager::get<PanelTheme>()->getSkin().value("colors").toMap();
    if (!colors.isEmpty()) {
        static const QRegularExpression HEX("#[0-9a-fA-F]{6}\\b");
        QString out;
        int last = 0;
        auto matches = HEX.globalMatch(_styleSource);
        while (matches.hasNext()) {
            auto match = matches.next();
            QString role = skinRoleForColor().value(match.captured(0).toLower());
            QColor skinColor(colors.value(role).toString());
            out += _styleSource.mid(last, match.capturedStart() - last);
            out += (!role.isEmpty() && skinColor.isValid()) ? skinColor.name(QColor::HexArgb) : match.captured(0);
            last = match.capturedEnd();
        }
        out += _styleSource.mid(last);
        themed = out;
    }
    // the Hierarchy's rows that take their own colour: red, couldn't load (destructive); grey, not loaded yet
    // (tertiaryText)
    if (_treeModel) {
        QColor failed(colors.value("destructive").toString());
        QColor unloaded(colors.value("tertiaryText").toString());
        _treeModel->setRowColors(failed.isValid() ? failed : QColor(255, 107, 107),
                                 unloaded.isValid() ? unloaded : QColor(0x7a, 0x7a, 0x7a));
    }
    // the window's sheet holds other parts too: ours goes between its markers, in place
    QString sheet = window->styleSheet();
    int begin = sheet.indexOf(STYLE_BEGIN);
    int end = sheet.indexOf(STYLE_END);
    QString block = STYLE_BEGIN + "\n" + themed + "\n" + STYLE_END;
    if (begin >= 0 && end > begin) {
        sheet.replace(begin, end + STYLE_END.size() - begin, block);
    } else {
        sheet += "\n" + block;
    }
    window->setStyleSheet(sheet);
}

void Editor::resetLayout() {
    // every panel back where it started (as Caity was told, 10-25), chat's too: floating at its first size and place,
    // open or closed as it was. My Files docks along the bottom, closed in normal play (ui/WindowLayout::resetToDefault
    // closes the bar's panels)
    ChatWindow::resetDock();
    WindowLayout::instance()->saveSoon();
    auto window = qApp->getWindow();
    if (!window) {
        return;
    }
    {
        gl::ViewChangeGuard guard;   // the panels moving resize the GL view
        if (_assets) {
            _assets->setFloating(false);
            window->addDockWidget(Qt::BottomDockWidgetArea, _assets);
            window->resizeDocks({ _assets }, { 240 }, Qt::Vertical);
            _assets->setVisible(_active);
        }
        if (_panelsCreated) {
            _hierarchy->setFloating(false);
            _inspector->setFloating(false);
            window->addDockWidget(Qt::LeftDockWidgetArea, _hierarchy);
            window->addDockWidget(Qt::RightDockWidgetArea, _inspector);
            window->resizeDocks({ _hierarchy, _inspector }, { 260, 430 }, Qt::Horizontal);
        }
    }
    if (_panelsCreated) {
        placeToolBars();
        showPanels(_active);
    }
}

void Editor::layoutApplied(bool savedInBuildMode) {
    if (!_panelsCreated) {
        return;   // Build mode's panels take their places from the layout when they're made
    }
    gl::ViewChangeGuard guard;   // panels coming and going change the window round the GL view
    if (_active) {
        // a layout from normal play has Build mode's panels closed
        if (!savedInBuildMode) {
            _hierarchy->show();
            _inspector->show();
        }
        placeToolBars();
    } else {
        _hierarchy->hide();
        _inspector->hide();
    }
    _toolBar->setVisible(_active);
    _statusBar->setVisible(_active);
    updateTerrainBar();
}

// --- every frame --------------------------------------------------------------------------------

void Editor::update(float deltaTime) {
    if (!_active) {
        return;
    }
    // a teleport or a domain change puts the camera back on the avatar; take it again from there
    if (qApp->getCamera().getMode() != CAMERA_MODE_INDEPENDENT) {
        _camera.enter();
        _axesValid = false;
        if (_reframePending) {
            _camera.frame(_reframeBox);
            _reframePending = false;
        }
    }
    if (_reframePending && --_reframeFrames <= 0) {
        _reframePending = false;
    }
    _camera.update(deltaTime);
    updateAxes();
    if (++_frameCounter % PRUNE_EVERY_FRAMES == 0 && !_gizmo.isDragging()) {
        _selection.prune();
    }
    _loadCheckSeconds += deltaTime;
    if (_loadCheckSeconds >= LOAD_CHECK_SECONDS) {
        _loadCheckSeconds = 0.0f;
        refreshLoadProblems();
    }
    // the grid's lines are a set width in points, not pixels, so they don't thin to a hair on a Retina display
    _grid.setPixelsPerPoint((float)qApp->getDevicePixelRatio() * RenderScriptingInterface::getInstance()->getViewportResolutionScale());
    updateGizmo();
    updateBounds();
    updateLightGizmos();
    updateVehicleGizmos();
    updateTrackGizmos();
    _terrain.setView(_camera.position(), 2.0f * tanHalfFov() / viewportHeight());
    if (_tool == EditorToolBar::Tool::Terrain) {
        _terrain.update(deltaTime);
    }
}

void Editor::updateBounds() {
    // Unity's gizmo colours: trigger volumes green, lights pale yellow; particles a soft blue
    static const glm::vec4 ZONE_COLOR(0.55f, 0.95f, 0.55f, 1.0f);
    static const glm::vec4 LIGHT_COLOR(1.0f, 0.96f, 0.55f, 1.0f);
    static const glm::vec4 PARTICLE_COLOR(0.55f, 0.8f, 1.0f, 1.0f);
    static const float MIN_EMITTER_SIZE = 0.1f;
    std::vector<EditorBoundsShape> shapes;
    auto tree = entityTree();
    if (tree && !_selection.isEmpty()) {
        tree->withReadLock([&] {
            for (const auto& id : _selection.ids()) {
                auto entity = tree->findEntityByEntityItemID(id);
                if (!entity) {
                    continue;
                }
                EditorBoundsShape shape;
                switch (entity->getType()) {
                    case EntityTypes::Zone:
                        shape.kind = EditorBoundsShape::Box;
                        shape.color = ZONE_COLOR;
                        break;
                    case EntityTypes::Light: {
                        // as Unity draws them: a point light's reach as a sphere round it, a spot light's beam as a cone
                        // down its -Z axis; the reach is half the light's largest dimension (what the engine uses)
                        auto light = std::static_pointer_cast<LightEntityItem>(entity);
                        float range = glm::compMax(entity->getScaledDimensions()) * 0.5f;
                        shape.kind = light->getIsSpotlight() ? EditorBoundsShape::Cone : EditorBoundsShape::Sphere;
                        shape.color = LIGHT_COLOR;
                        shape.position = entity->getWorldPosition();
                        shape.rotation = entity->getWorldOrientation();
                        shape.dimensions = glm::vec3(range * 2.0f);
                        shape.range = range;
                        shape.angle = glm::radians(light->getCutoff());
                        shapes.push_back(shape);
                        continue;
                    }
                    case EntityTypes::ParticleEffect: {
                        // the emitter, where the particles start, as Unity draws it: the entity's own dimensions
                        // are the whole space its particles can reach (the engine works them out from speed,
                        // spread and lifespan), far larger than the spray you see
                        auto particles = std::static_pointer_cast<ParticleEffectEntityItem>(entity);
                        ShapeType emitter = particles->getShapeType();
                        shape.kind = emitter == SHAPE_TYPE_BOX || emitter == SHAPE_TYPE_PLANE
                            ? EditorBoundsShape::Box : EditorBoundsShape::Sphere;
                        shape.color = PARTICLE_COLOR;
                        shape.position = entity->getWorldPosition();
                        shape.rotation = entity->getWorldOrientation() * particles->getEmitOrientation();
                        shape.dimensions = particles->getEmitDimensions();
                        if (glm::length(shape.dimensions) < MIN_EMITTER_SIZE) {
                            shape.dimensions = glm::vec3(MIN_EMITTER_SIZE);   // a point emitter still shows
                        }
                        shapes.push_back(shape);
                        continue;
                    }
                    default:
                        continue;   // the selection outline shows everything else
                }
                // the middle of the entity, wherever its registration point puts its position
                glm::vec3 dimensions = entity->getScaledDimensions();
                glm::quat rotation = entity->getWorldOrientation();
                glm::vec3 offset = (glm::vec3(0.5f) - entity->getRegistrationPoint()) * dimensions;
                shape.position = entity->getWorldPosition() + rotation * offset;
                shape.rotation = rotation;
                shape.dimensions = dimensions;
                shapes.push_back(shape);
            }
        });
    }
    if (shapes != _boundsShapes) {
        _boundsShapes = shapes;
        _bounds.setShapes(shapes);
    }
}

void Editor::setGizmosVisible(bool visible) {
    if (visible == _gizmosVisible) {
        return;
    }
    _gizmosVisible = visible;
    _lightSearchFrame = -1;   // look for the lights at once when the icons come back
    if (_toolBar) {
        _toolBar->setGizmosChecked(visible);
    }
}

void Editor::updateLightGizmos() {
    auto tree = entityTree();
    if (!tree) {
        _lightGizmos.clear();
        return;
    }
    glm::vec3 cameraPosition = _camera.position();
    // the lights round the camera, looked for now and then: searching the tree every frame would cost too much
    if (_gizmosVisible && (_lightSearchFrame < 0 || _frameCounter - _lightSearchFrame >= LIGHT_SEARCH_FRAMES)) {
        _lightSearchFrame = _frameCounter;
        QVector<QUuid> found;
        tree->withReadLock([&] {
            tree->evalEntitiesInSphereWithType(cameraPosition, LIGHT_SEARCH_RADIUS, EntityTypes::Light, lightSearchFilter(), found);
        });
        _nearbyLights = found;
    }

    std::vector<EditorLightGizmoLight> lights;
    QSet<QUuid> shown;
    const auto& selected = _selection.ids();
    tree->withReadLock([&] {
        auto show = [&](const QUuid& id) {
            if (shown.contains(id)) {
                return;
            }
            auto entity = tree->findEntityByEntityItemID(id);
            if (!entity || entity->getType() != EntityTypes::Light) {
                return;
            }
            shown.insert(id);
            auto light = std::static_pointer_cast<LightEntityItem>(entity);
            EditorLightGizmoLight gizmo;
            gizmo.id = id;
            gizmo.spot = light->getIsSpotlight();
            gizmo.position = entity->getWorldPosition();
            gizmo.rotation = entity->getWorldOrientation();
            gizmo.range = glm::compMax(entity->getScaledDimensions()) * 0.5f;   // the reach the engine uses
            gizmo.angle = glm::radians(light->getCutoff());
            gizmo.selected = selected.contains(id);
            lights.push_back(gizmo);
        };
        for (const auto& id : selected) {
            show(id);
        }
        if (_gizmosVisible) {
            for (const auto& id : _nearbyLights) {
                show(id);
            }
        }
    });

    // a light selected on its own gets handles, unless the world can't be changed
    QUuid handlesFor;
    if (!_readOnly && selected.size() == 1 && shown.contains(selected.first())) {
        handlesFor = selected.first();
    }
    float pixelSize = 2.0f * tanHalfFov() / viewportHeight();
    _lightGizmos.update(lights, handlesFor, cameraPosition, viewDirection(), pixelSize);
}

void Editor::updateVehicleGizmos() {
    // the points the physics acts at: the centre of mass in orange, an airship's or aircraft's others in white
    static const glm::vec4 MASS_COLOR(1.0f, 0.6f, 0.2f, 1.0f);
    static const glm::vec4 POINT_COLOR(0.92f, 0.92f, 0.92f, 1.0f);
    static const float MASS_CROSS = 0.3f;
    static const float POINT_CROSS = 0.2f;
    std::vector<EditorVehicleGizmoVehicle> vehicles;
    auto tree = entityTree();
    if (tree && !_selection.isEmpty()) {
        tree->withReadLock([&] {
            QSet<QUuid> shown;
            for (const auto& id : _selection.ids()) {
                auto entity = tree->findEntityByEntityItemID(id);
                if (!entity) {
                    continue;
                }
                // a wheel entity selected shows its parent's gizmo, with that wheel bright
                QString chosenWheel;
                if (entity->isWheel()) {
                    chosenWheel = entity->getVehicleProperties().getWheel();
                    entity = tree->findEntityByEntityItemID(entity->getParentID());
                    if (!entity) {
                        continue;
                    }
                }
                if (!entity->isVehicle() || shown.contains(entity->getID())) {
                    continue;
                }
                shown.insert(entity->getID());
                const VehiclePropertyGroup& group = entity->getVehicleProperties();
                EditorVehicleGizmoVehicle vehicle;
                vehicle.position = entity->getWorldPosition();
                vehicle.rotation = entity->getWorldOrientation();
                // the Inspector's chosen rows are on the first vehicle selected (the one the Inspector shows)
                bool first = vehicles.empty() && chosenWheel.isEmpty();
                int chosenWheelRow = first ? _vehiclePartChosen.value(QStringLiteral("vehicle.wheels"), -1) : -1;
                int chosenSeatRow = first ? _vehiclePartChosen.value(QStringLiteral("vehicle.seats"), -1) : -1;
                std::vector<WheelSpec> wheels;
                WheeledVehicleSpec::parseWheels(group.getWheels(), wheels);
                for (size_t i = 0; i < wheels.size(); ++i) {
                    const WheelSpec& wheel = wheels[i];
                    EditorVehicleGizmoWheel gizmo;
                    gizmo.center = wheel.center;
                    gizmo.radius = wheel.radius;
                    gizmo.width = wheel.width;
                    gizmo.travel = wheel.suspensionDistance;
                    gizmo.rest = 1.0f - wheel.targetPosition;
                    gizmo.chosen = (int)i == chosenWheelRow || (!chosenWheel.isEmpty() && wheel.name == chosenWheel);
                    vehicle.wheels.push_back(gizmo);
                }
                std::vector<SeatSpec> seats;
                WheeledVehicleSpec::parseSeats(group.getSeats(), seats);
                for (size_t i = 0; i < seats.size(); ++i) {
                    const SeatSpec& seat = seats[i];
                    EditorVehicleGizmoSeat gizmo;
                    gizmo.position = seat.position;
                    gizmo.yaw = seat.yaw;
                    gizmo.driver = seat.driver;
                    gizmo.chosen = (int)i == chosenSeatRow;
                    vehicle.seats.push_back(gizmo);
                }
                auto point = [&](const glm::vec3& position, float size, const glm::vec4& color) {
                    EditorVehicleGizmoPoint gizmo;
                    gizmo.position = position;
                    gizmo.size = size;
                    gizmo.color = color;
                    vehicle.points.push_back(gizmo);
                };
                point(group.getCenterOfMass(), MASS_CROSS, MASS_COLOR);
                if (entity->isAirship()) {
                    point(group.getCenterOfBuoyancy(), POINT_CROSS, POINT_COLOR);
                    point(group.getThrustPoint(), POINT_CROSS, POINT_COLOR);
                    point(group.getFinPoint(), POINT_CROSS, POINT_COLOR);
                } else if (entity->isAircraft()) {
                    point(group.getWingPoint(), POINT_CROSS, POINT_COLOR);
                    point(group.getThrustPoint(), POINT_CROSS, POINT_COLOR);
                    point(group.getFinPoint(), POINT_CROSS, POINT_COLOR);
                }
                vehicles.push_back(vehicle);
            }
        });
    }
    if (vehicles != _vehicleGizmoVehicles) {
        _vehicleGizmoVehicles = vehicles;
        _vehicleGizmos.setVehicles(vehicles);
    }
}

void Editor::updateTrackGizmos() {
    auto tree = entityTree();
    if (!tree) {
        _trackGizmos.clear();
        return;
    }
    glm::vec3 cameraPosition = _camera.position();
    // the tracks round the camera, looked for now and then, as the lights are
    if (_gizmosVisible && (_trackSearchFrame < 0 || _frameCounter - _trackSearchFrame >= TRACK_SEARCH_FRAMES)) {
        _trackSearchFrame = _frameCounter;
        QVector<QUuid> found;
        tree->withReadLock([&] {
            tree->evalEntitiesInSphereWithType(cameraPosition, TRACK_SEARCH_RADIUS, EntityTypes::Track, lightSearchFilter(), found);
        });
        _nearbyTracks = found;
    }

    std::vector<EditorTrackGizmoTrack> tracks;
    QSet<QUuid> shown;
    const auto& selected = _selection.ids();
    tree->withReadLock([&] {
        auto show = [&](const QUuid& id) {
            if (shown.contains(id)) {
                return;
            }
            auto entity = tree->findEntityByEntityItemID(id);
            auto track = std::dynamic_pointer_cast<TrackEntityItem>(entity);
            if (!track) {
                return;
            }
            shown.insert(id);
            EditorTrackGizmoTrack gizmo;
            gizmo.id = id;
            gizmo.position = track->getWorldPosition();
            gizmo.rotation = track->getWorldOrientation();
            QVector<glm::vec3> points = track->getTrackPoints();
            QVector<float> bank = track->getTrackBank();
            QVector<float> dwell = track->getTrackDwell();
            gizmo.points.assign(points.begin(), points.end());
            gizmo.bank.assign(bank.begin(), bank.end());
            gizmo.dwell.assign(dwell.begin(), dwell.end());
            gizmo.closed = track->getTrackClosed();
            gizmo.selected = selected.contains(id);
            tracks.push_back(gizmo);
        };
        for (const auto& id : selected) {
            show(id);
        }
        if (_gizmosVisible) {
            for (const auto& id : _nearbyTracks) {
                show(id);
            }
        }
    });

    // a track selected on its own gets handles on its points, unless the world can't be changed
    QUuid handlesFor;
    if (!_readOnly && trackPointTrack(handlesFor) && !shown.contains(handlesFor)) {
        handlesFor = QUuid();
    }
    int chosen = handlesFor.isNull() || handlesFor != _trackPointTrack ? -1 : _trackPointChosen;
    float pixelSize = 2.0f * tanHalfFov() / viewportHeight();
    _trackGizmos.update(tracks, handlesFor, chosen, cameraPosition, viewDirection(), pixelSize);
}

bool Editor::trackPointTrack(QUuid& id) const {
    if (_selection.count() != 1) {
        return false;
    }
    auto tree = entityTree();
    if (!tree) {
        return false;
    }
    QUuid only = _selection.ids().first();
    bool track = false;
    tree->withReadLock([&] {
        auto entity = tree->findEntityByEntityItemID(only);
        track = entity && entity->getType() == EntityTypes::Track;
    });
    if (track) {
        id = only;
    }
    return track;
}

void Editor::chooseTrackPoint(int index, bool fromInspector) {
    QUuid trackId;
    if (index < 0 || !trackPointTrack(trackId)) {
        index = -1;
        trackId = QUuid();
    }
    bool changed = index != _trackPointChosen || trackId != _trackPointTrack;
    _trackPointChosen = index;
    _trackPointTrack = trackId;
    if (changed && !fromInspector && _inspector) {
        _inspector->chooseTrackPoint(index);
    }
    if (index >= 0 && _tool != EditorToolBar::Tool::Move && !fromInspector) {
        setTool(EditorToolBar::Tool::Move);   // the point is moved with the Move gizmo
    }
    updateTrackGizmos();
    updateGizmo();
}

void Editor::fitTrackBox(const EntityItemPointer& track, const QVector<glm::vec3>& points, EntityItemProperties& properties) const {
    auto item = std::dynamic_pointer_cast<TrackEntityItem>(track);
    if (!item || points.isEmpty()) {
        return;
    }
    // the curve's points, with the guideway hanging below them (its beam, and the pylons down to their floor) and
    // a margin round it all; the curve can bow a little past its points, which the margin covers
    glm::vec3 low = points.first();
    glm::vec3 high = points.first();
    for (const glm::vec3& point : points) {
        low = glm::min(low, point);
        high = glm::max(high, point);
    }
    bool beam = item->getTrackProfile() != QStringLiteral("none");
    float side = beam ? 0.5f * item->getTrackWidth() : 0.0f;
    float drop = beam ? item->getTrackClearance() + item->getTrackHeight() : 0.0f;
    low -= glm::vec3(side, drop, side);
    high += glm::vec3(side, 0.0f, side);
    if (beam && item->getTrackPylonSpacing() > 0.0f) {
        low.y = std::min(low.y, item->getTrackPylonFloor());
    }
    low -= glm::vec3(TRACK_BOX_MARGIN);
    high += glm::vec3(TRACK_BOX_MARGIN);
    // the box sits at the entity's registration point: each axis reaches as far as the farther side needs
    glm::vec3 registration = glm::clamp(item->getRegistrationPoint(), glm::vec3(0.01f), glm::vec3(0.99f));
    glm::vec3 dimensions;
    for (int axis = 0; axis < 3; ++axis) {
        dimensions[axis] = std::max(std::max(-low[axis], 0.0f) / registration[axis], std::max(high[axis], 0.0f) / (1.0f - registration[axis]));
        dimensions[axis] = std::max(dimensions[axis], 2.0f * TRACK_BOX_MARGIN);
    }
    properties.setDimensions(dimensions);
}

void Editor::insertTrackPoint(float arcLength) {
    QUuid trackId;
    if (!editable() || !trackPointTrack(trackId)) {
        return;
    }
    auto tree = entityTree();
    if (!tree) {
        return;
    }
    EntityItemProperties before;
    EntityItemProperties after;
    int inserted = -1;
    tree->withReadLock([&] {
        auto entity = tree->findEntityByEntityItemID(trackId);
        auto track = std::dynamic_pointer_cast<TrackEntityItem>(entity);
        if (!track || track->getLocked()) {
            return;
        }
        QVector<glm::vec3> points = track->getTrackPoints();
        QVector<float> bank = track->getTrackBank();
        QVector<float> dwell = track->getTrackDwell();
        if (points.size() >= TrackEntityItem::MAX_POINTS) {
            return;
        }
        bank.resize(points.size());
        dwell.resize(points.size());
        TrackCurve curve;
        curve.build(std::vector<glm::vec3>(points.begin(), points.end()), std::vector<float>(bank.begin(), bank.end()), track->getTrackClosed());
        if (!curve.isValid()) {
            return;
        }
        arcLength = curve.normalize(arcLength);
        // the segment the spot is in: after the last point whose arc length it has passed (on a loop, the segment
        // back to the first point comes after the last)
        int segment = 0;
        for (int i = 0; i < points.size(); ++i) {
            if (curve.arcLengthAtPoint(i) <= arcLength) {
                segment = i;
            }
        }
        int next = (segment + 1) % points.size();
        float from = curve.arcLengthAtPoint(segment);
        float to = next == 0 ? curve.length() : curve.arcLengthAtPoint(next);
        float fraction = to > from ? glm::clamp((arcLength - from) / (to - from), 0.0f, 1.0f) : 0.0f;
        before.setTrackPoints(points);
        before.setTrackBank(bank);
        before.setTrackDwell(dwell);
        before.setDimensions(track->getScaledDimensions());
        inserted = segment + 1;
        points.insert(inserted, curve.positionAt(arcLength));
        bank.insert(inserted, glm::mix(bank[segment], bank[next], fraction));
        dwell.insert(inserted, 0.0f);
        after.setTrackPoints(points);
        after.setTrackBank(bank);
        after.setTrackDwell(dwell);
        fitTrackBox(entity, points, after);
    });
    if (inserted < 0) {
        return;
    }
    _undo.pushPropertyEdit("Insert Point", { trackId }, { before }, { after }, false);
    chooseTrackPoint(inserted, false);
    refreshInspector(false);
}

void Editor::deleteTrackPoint() {
    QUuid trackId;
    if (!editable() || _trackPointChosen < 0 || !trackPointTrack(trackId) || _gizmo.isDragging()) {
        return;
    }
    auto tree = entityTree();
    if (!tree) {
        return;
    }
    EntityItemProperties before;
    EntityItemProperties after;
    int remaining = -1;
    int index = _trackPointChosen;
    tree->withReadLock([&] {
        auto entity = tree->findEntityByEntityItemID(trackId);
        auto track = std::dynamic_pointer_cast<TrackEntityItem>(entity);
        if (!track || track->getLocked()) {
            return;
        }
        QVector<glm::vec3> points = track->getTrackPoints();
        QVector<float> bank = track->getTrackBank();
        QVector<float> dwell = track->getTrackDwell();
        if (index >= points.size() || points.size() <= (int)TrackCurve::MIN_POINTS) {
            return;   // a track keeps two points at least
        }
        bank.resize(points.size());
        dwell.resize(points.size());
        before.setTrackPoints(points);
        before.setTrackBank(bank);
        before.setTrackDwell(dwell);
        before.setDimensions(track->getScaledDimensions());
        points.removeAt(index);
        bank.removeAt(index);
        dwell.removeAt(index);
        after.setTrackPoints(points);
        after.setTrackBank(bank);
        after.setTrackDwell(dwell);
        fitTrackBox(entity, points, after);
        remaining = points.size();
    });
    if (remaining < 0) {
        setStatus("A track keeps two points at least", STATUS_CLEAR_MS);
        return;
    }
    _undo.pushPropertyEdit("Delete Point", { trackId }, { before }, { after }, false);
    chooseTrackPoint(std::min(index, remaining - 1), false);
    refreshInspector(false);
}

bool Editor::pointerOverHierarchy() const {
    return _hierarchy && _hierarchy->isVisible() && _hierarchy->rect().contains(_hierarchy->mapFromGlobal(QCursor::pos()));
}

QUuid Editor::pickAt(const QPoint& viewportPos) const {
    return pickOnRay(pickRay(viewportPos));
}

QUuid Editor::pickOnRay(const PickRay& ray) const {
    // icons are drawn over everything, so they are picked first, as in Unity
    QUuid icon = _lightGizmos.iconAt(ray);
    return icon.isNull() || excludedFromPicks(icon) ? pickEntityOnRay(ray) : icon;
}

float Editor::lightHandleValue(const PickRay& ray) const {
    if (_lightHandleDrag.kind == EditorLightHandle::Angle) {
        return glm::degrees(_lightGizmos.angleAt(ray, _lightHandleDrag));
    }
    return _lightGizmos.rangeAt(ray, _lightHandleDrag);
}

void Editor::beginLightHandleDrag(const EditorLightHandle& handle, const PickRay& ray) {
    EditorLightGizmoLight light;
    if (!_lightGizmos.handleLight(light) || !editable()) {
        return;
    }
    // the drag is an Inspector edit of Range or Spot Cutoff, made from the viewport: paced, and one undo step
    QString key = handle.kind == EditorLightHandle::Angle ? CUTOFF_KEY : RANGE_KEY;
    onInspectorEditBegan(key);
    if (_inspectorEditIds.isEmpty()) {
        _inspectorEditKey.clear();   // locked
        return;
    }
    _lightHandleDrag = handle;
    _lightHandleKey = key;
    float current = handle.kind == EditorLightHandle::Angle ? glm::degrees(light.angle) : light.range;
    _lightHandleOffset = current - lightHandleValue(ray);
    _lightHandleValue = current;
    _lightHandleMoved = false;
    _lightGizmos.setActive(handle);
}

void Editor::moveLightHandleDrag(const QPoint& viewportPos) {
    float value = lightHandleValue(pickRay(viewportPos)) + _lightHandleOffset;
    value = _lightHandleKey == RANGE_KEY ? std::max(value, MIN_LIGHT_RANGE) : glm::clamp(value, MIN_SPOT_CUTOFF, MAX_SPOT_CUTOFF);
    if (value == _lightHandleValue) {
        return;
    }
    _lightHandleValue = value;
    _lightHandleMoved = true;
    onInspectorLiveEdit(_lightHandleKey, QVariant((double)value));
}

void Editor::endLightHandleDrag(bool cancel) {
    if (_lightHandleDrag.isNone()) {
        return;
    }
    QString key = _lightHandleKey;
    _lightHandleDrag = EditorLightHandle();
    _lightHandleKey.clear();
    _lightGizmos.setActive(EditorLightHandle());
    if (_lightHandleMoved && !cancel) {
        onInspectorCommitted(key, QVariant((double)_lightHandleValue));
    } else if (_lightHandleMoved) {
        onInspectorEditCancelled(key);   // puts back what the light had
    } else {
        // a click on a handle: nothing changed, so nothing to send or undo
        _liveEditTimer.stop();
        _inspectorEditKey.clear();
        _inspectorEditIds.clear();
        _inspectorBefore.clear();
        _inspectorNatural.clear();
    }
}

void Editor::updateAxes() {
    glm::quat orientation = _camera.orientation();
    if (_axesValid && orientation == _axesOrientation) {
        return;
    }
    _axesOrientation = orientation;
    _axesValid = true;
    glm::quat inverse = glm::inverse(orientation);
    for (int i = 0; i < 3; ++i) {
        glm::vec3 axis(0.0f);
        axis[i] = 1.0f;
        glm::vec3 v = inverse * axis;
        _axes[i] = QVector3D(v.x, v.y, v.z);
    }
    emit cameraChanged();
}

glm::vec3 Editor::viewDirection() const {
    return _camera.orientation() * Vectors::FRONT;
}

bool Editor::primaryTransform(glm::vec3& position, glm::quat& rotation) const {
    auto tree = entityTree();
    QUuid id = _selection.primary();
    if (!tree || id.isNull()) {
        return false;
    }
    bool found = false;
    tree->withReadLock([&] {
        auto entity = tree->findEntityByEntityItemID(id);
        if (entity) {
            position = entity->getWorldPosition();
            rotation = entity->getWorldOrientation();
            found = true;
        }
    });
    return found;
}

glm::vec3 Editor::gizmoPosition() const {
    // a Track's chosen control point: the Move gizmo stands on it
    QUuid trackId;
    if (_trackPointChosen >= 0 && _tool == EditorToolBar::Tool::Move && trackPointTrack(trackId)) {
        EditorTrackGizmoTrack track;
        if (_trackGizmos.handleTrack(track) && track.id == trackId && _trackPointChosen < (int)track.points.size()) {
            return _trackGizmos.pointInWorld(_trackPointChosen);
        }
    }
    glm::vec3 position;
    glm::quat rotation;
    if (_pivotMode && primaryTransform(position, rotation)) {
        return position;
    }
    return _selection.center();
}

glm::quat Editor::gizmoRotation() const {
    // scale is always in the object's own axes; move and rotate follow the space toggle
    if (_tool == EditorToolBar::Tool::Scale || _localSpace) {
        glm::vec3 position;
        glm::quat rotation;
        if (primaryTransform(position, rotation)) {
            return rotation;
        }
    }
    return glm::quat();
}

void Editor::updateGizmo() {
    // a read-only world gets no handles: nothing could be moved anyway
    bool show = _active && !_readOnly && _tool != EditorToolBar::Tool::View && _tool != EditorToolBar::Tool::Terrain
        && !_selection.isEmpty();
    if (show) {
        _gizmo.setMode(gizmoModeFor(_tool));
        glm::vec3 position;
        glm::quat rotation;
        if (_gizmo.isDragging()) {
            position = _dragPivot + ((_dragKind == DragKind::Move || _dragKind == DragKind::TrackPoint) ? _dragDelta : glm::vec3(0.0f));
            rotation = _dragGizmoRotation;
        } else {
            position = gizmoPosition();
            rotation = gizmoRotation();
        }
        // sized by depth along the view axis, not straight-line distance, so the handles keep the
        // same projected size at the edge of the frame as in the middle (Unity does the same)
        float depth = std::max(0.01f, glm::dot(position - _camera.position(), viewDirection()));
        float scale = depth * 2.0f * tanHalfFov() * (EditorGizmo::SIZE_PIXELS / viewportHeight());
        // it faces the camera along the line of sight, so that off centre it still shows its front half
        glm::vec3 lineOfSight = position - _camera.position();
        _gizmo.setTransform(position, rotation, scale,
            glm::length(lineOfSight) > 1.0e-4f ? glm::normalize(lineOfSight) : viewDirection());
    }
    _gizmo.setVisible(show);
}

void Editor::viewFromAxis(int axis, int sign) {
    if (!_active) {
        return;
    }
    _camera.viewFromAxis(axis, sign);
    updateAxes();
}

// --- selection ----------------------------------------------------------------------------------

void Editor::recordSelection() {
    // a change made by undo or redo is not a new step, nor is one while build mode is off (leaving it clears the
    // selection)
    QList<QUuid> ids = _selection.ids();
    if (_active && _historyDepth == 0 && ids != _selectionWas) {
        _undo.pushSelection(_selectionWas, ids);
    }
    _selectionWas = ids;
}

void Editor::onSelectionChanged() {
    if (_selectionBatch == 0) {
        recordSelection();
    }
    QUuid trackId;
    if (_trackPointChosen >= 0 && (!trackPointTrack(trackId) || trackId != _trackPointTrack)) {
        _trackPointChosen = -1;
        _trackPointTrack = QUuid();
    }
    // rows chosen in the Hierarchy that have not arrived are given up when the selection is made elsewhere
    if (!_syncingSelection && !_selectingArrival) {
        _arrivalIds.clear();
    }
    if (_hierarchy && !_syncingSelection) {
        _syncingSelection = true;
        _hierarchy->selectIds(_selection.ids() + _arrivalIds);
        _syncingSelection = false;
    }
    refreshInspector(true);
    updateGizmo();
}

// --- the Inspector ------------------------------------------------------------------------------

bool Editor::selectionSnapshots(QVector<EntityItemProperties>& snapshots, EntityTypes::EntityType& type, bool& multiple) const {
    auto tree = entityTree();
    if (!tree) {
        return false;
    }
    snapshots.clear();
    type = EntityTypes::Unknown;
    multiple = false;
    tree->withReadLock([&] {
        for (const auto& id : _selection.ids()) {
            auto entity = tree->findEntityByEntityItemID(id);
            if (!entity) {
                continue;
            }
            EntityTypes::EntityType entityType = entity->getType();
            if (entityType == EntityTypes::Box || entityType == EntityTypes::Sphere) {
                entityType = EntityTypes::Shape;
            }
            if (snapshots.isEmpty()) {
                type = entityType;
            } else if (entityType != type) {
                multiple = true;
            }
            snapshots << EditorUndo::snapshotOf(entity);   // Position and Rotation in the world
        }
    });
    return !snapshots.isEmpty();
}

// --- what failed to load (Caity, 10-24: an Image whose picture never came, and nothing said so) --------------------

void Editor::refreshLoadProblems() {
    // every entity the Hierarchy lists: its row goes red while its model, image or a texture has failed, with the
    // reasons in its tooltip; asked of the renderers (EntityRenderer::loadFailures), so an entity too far away to have
    // been loaded says nothing yet
    auto treeRenderer = DependencyManager::get<EntityTreeRenderer>();
    if (!_treeModel || !treeRenderer) {
        return;
    }
    for (const auto& id : _treeModel->allIds()) {
        auto renderer = treeRenderer->renderableForEntityId(id);
        _treeModel->setLoadProblem(id, renderer ? renderer->loadFailures().join("\n") : QString());
    }
    if (_inspector && !_selection.isEmpty()) {
        _inspector->setLoadProblem(selectionLoadProblem());
    }
}

QColor Editor::hierarchyRowColor(const QUuid& id) const {
    return _treeModel ? _treeModel->rowColor(id) : QColor();
}

QString Editor::loadProblem(const QUuid& id) const {
    return _treeModel ? _treeModel->loadProblem(id) : QString();
}

QString Editor::selectionLoadProblem() const {
    if (!_treeModel) {
        return QString();
    }
    const int MAX_LINES = 6;
    QStringList lines;
    bool several = _selection.ids().size() > 1;
    for (const auto& id : _selection.ids()) {
        QString problem = _treeModel->loadProblem(id);
        if (problem.isEmpty()) {
            continue;
        }
        if (several) {
            // which of the selected it is: its row's name in the Hierarchy
            QString name = _treeModel->data(_treeModel->indexForId(id), Qt::DisplayRole).toString();
            problem = name + ": " + problem;
        }
        lines << problem;
        if (lines.size() >= MAX_LINES) {
            lines << "…";
            break;
        }
    }
    return lines.join("\n");
}

void Editor::refreshInspector(bool selectionChanged) {
    if (!_inspector || !_active) {
        return;
    }
    QVector<EntityItemProperties> snapshots;
    EntityTypes::EntityType type;
    bool multiple;
    if (!selectionSnapshots(snapshots, type, multiple)) {
        _inspector->setSummary("Nothing selected");
        return;
    }
    _inspectorRefresh.setInterval(snapshots.size() > INSPECTOR_FAST_LIMIT ? INSPECTOR_REFRESH_SLOW_MS : INSPECTOR_REFRESH_MS);
    if (selectionChanged) {
        QString title;
        if (snapshots.size() == 1) {
            QString name = snapshots.first().getName();
            QString typeName = EntityTypes::getEntityTypeName(type);
            title = name.isEmpty() ? typeName : name + "  ·  " + typeName;
        } else {
            title = QString("%1 entities selected").arg(snapshots.size());
        }
        _inspector->showEntities(snapshots, type, multiple, title);
        _inspector->setLoadProblem(selectionLoadProblem());
    } else if (_inspectorEditKey.isEmpty() || !_lightHandleDrag.isNone()) {
        // a light handle's drag is an edit no field is making: the fields follow it
        _inspector->refreshValues(snapshots);
    }
    updateInspectorToggles();
}

void Editor::updateInspectorToggles() {
    if (_inspector && _hierarchy) {
        _inspector->setToggleStates(_hierarchy->statesFor(_selection.ids()));
    }
}

EntityItemProperties Editor::inspectorEdit(const QString& key, const QVariant& value, const glm::vec3& naturalSize,
                                           const EntityItemProperties& before) const {
    // Scale is the one Inspector property that is not stored: it becomes dimensions, the scale
    // times the entity's own size
    EntityItemProperties properties;
    if (PropertySpecs::isEntry(key)) {
        // the entry goes into the JSON the entity holds, among its others (and a light's Shadows also sets whether
        // it casts any)
        for (const QString& stored : PropertySpecs::storedKeys(key)) {
            PropertySpecs::set(stored, properties, PropertySpecs::get(stored, before));
        }
        PropertySpecs::set(key, properties, value);
    } else if (key == SCALE_KEY) {
        QVector3D scale = value.value<QVector3D>();
        glm::vec3 dimensions = glm::vec3(scale.x(), scale.y(), scale.z()) * naturalSize;
        properties.setDimensions(glm::max(dimensions, glm::vec3(MIN_DIMENSION)));
    } else if (key == RANGE_KEY) {
        // a light's range, with its falloff kept in proportion so that its glow grows and shrinks with its
        // sphere (for Range, `naturalSize` holds the range and falloff it had: x and y)
        float range = std::max((float)value.toDouble(), MIN_DIMENSION);
        properties.setDimensions(glm::vec3(range * 2.0f));
        if (naturalSize.x > 0.0f) {
            properties.setFalloffRadius(naturalSize.y * range / naturalSize.x);
        }
    } else {
        PropertySpecs::set(key, properties, value);
    }
    return properties;
}

// an Inspector edit that scales what it changes, and so its children
static bool scalesChildren(const QString& key) {
    return key == SCALE_KEY || key == QStringLiteral("dimensions");
}

// an entity's new size over its old, along each of its axes (1 along one it had no size on)
static glm::vec3 sizeRatio(const glm::vec3& before, const glm::vec3& after) {
    glm::vec3 ratio(1.0f);
    for (int i = 0; i < 3; ++i) {
        if (before[i] > 0.0f) {
            ratio[i] = after[i] / before[i];
        }
    }
    return ratio;
}

// a scale along a parent's x, y and z, seen along each axis of something turned by `rotation` in it: exact for a scale
// the same along all three and for a turn by right angles; otherwise the nearest without a skew, which entities can't
// have (Unity's children skew)
static glm::vec3 scaleAlong(const glm::vec3& ratio, const glm::quat& rotation) {
    return glm::vec3(glm::length(ratio * (rotation * Vectors::UNIT_X)), glm::length(ratio * (rotation * Vectors::UNIT_Y)),
                     glm::length(ratio * (rotation * Vectors::UNIT_Z)));
}

QVector<Editor::ScaledChild> Editor::childrenToScale(const QList<QUuid>& ids) const {
    QVector<ScaledChild> children;
    auto tree = entityTree();
    if (!tree) {
        return children;
    }
    tree->withReadLock([&] {
        auto add = [&](const EntityItemPointer& parent, int parentIndex) {
            parent->forEachChild([&](const SpatiallyNestablePointer& nestable) {
                if (nestable->getNestableType() != NestableType::Entity || ids.contains(nestable->getID())) {
                    return;
                }
                auto child = std::static_pointer_cast<EntityItem>(nestable);
                if (child->getLocked()) {
                    return;
                }
                ScaledChild entry;
                entry.id = child->getID();
                entry.parent = parentIndex;
                entry.localPosition = child->getLocalPosition();
                entry.dimensions = child->getUnscaledDimensions();
                entry.localRotation = child->getLocalOrientation();
                quint16 joint = child->getParentJointIndex();
                if (joint != INVALID_JOINT_INDEX) {
                    entry.frame = parent->getAbsoluteJointRotationInObjectFrame(joint);
                }
                if (child->getType() == EntityTypes::Text) {
                    entry.lineHeight = std::static_pointer_cast<TextEntityItem>(child)->getLineHeight();
                } else if (child->getType() == EntityTypes::Light) {
                    entry.falloff = std::static_pointer_cast<LightEntityItem>(child)->getFalloffRadius();
                }
                children << entry;
            });
        };
        for (int i = 0; i < ids.size(); ++i) {
            int first = children.size();
            if (auto entity = tree->findEntityByEntityItemID(ids[i])) {
                add(entity, -1 - i);
            }
            for (int j = first; j < children.size(); ++j) {
                if (auto child = tree->findEntityByEntityItemID(children[j].id)) {
                    add(child, j);
                }
            }
        }
    });
    return children;
}

QVector<EntityItemProperties> Editor::childScaleEdits(const QVector<ScaledChild>& children,
                                                      const QVector<glm::vec3>& ratios) {
    QVector<EntityItemProperties> edits;
    QVector<glm::vec3> own;   // each child's scale along its own axes, which its own children grow by
    for (const auto& child : children) {
        glm::vec3 ratio = child.parent < 0 ? ratios.value(-1 - child.parent, glm::vec3(1.0f)) : own[child.parent];
        // its offset grows along its parent's axes (a joint's offset along the joint's)
        glm::vec3 offset = glm::inverse(child.frame) * (ratio * (child.frame * child.localPosition));
        own << scaleAlong(ratio, child.frame * child.localRotation);
        glm::vec3 scale = own.last();
        EntityItemProperties properties;
        properties.setLocalPosition(offset);
        if (child.falloff > 0.0f) {
            // a light's range is a sphere: it grows by its mean, its glow with it
            scale = glm::vec3((scale.x + scale.y + scale.z) / 3.0f);
            properties.setFalloffRadius(child.falloff * scale.x);
        }
        properties.setLocalDimensions(glm::max(child.dimensions * scale, glm::vec3(MIN_DIMENSION)));
        if (child.lineHeight > 0.0f) {
            properties.setLineHeight(child.lineHeight * scale.y);
        }
        edits << properties;
    }
    return edits;
}

EntityItemProperties Editor::childScaleBefore(const ScaledChild& child) {
    EntityItemProperties properties;
    properties.setLocalPosition(child.localPosition);
    properties.setLocalDimensions(child.dimensions);
    if (child.falloff > 0.0f) {
        properties.setFalloffRadius(child.falloff);
    }
    if (child.lineHeight > 0.0f) {
        properties.setLineHeight(child.lineHeight);
    }
    return properties;
}

void Editor::sendChildScale(const QVector<ScaledChild>& children, const QVector<EntityItemProperties>& edits, bool final) {
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    auto tree = entityTree();
    for (int i = 0; i < children.size() && i < edits.size(); ++i) {
        entities->editEntity(children[i].id, edits[i]);
        if (!final && tree) {
            // as for the entity scaled: while the drag lasts, the old collision shape is good enough
            tree->withWriteLock([&] {
                if (auto entity = tree->findEntityByEntityItemID(children[i].id)) {
                    entity->clearDirtyFlags(Simulation::DIRTY_SHAPE | Simulation::DIRTY_MASS);
                }
            });
        }
    }
}

void Editor::onInspectorEditBegan(const QString& key) {
    // a scrub starts (or a colour picker opens): what the entities hold now is the undo point
    _inspectorEditKey = key;
    _inspectorEditIds.clear();
    _inspectorBefore.clear();
    _inspectorNatural.clear();
    QStringList storedKeys = PropertySpecs::storedKeys(key);
    for (const auto& id : _selection.ids()) {
        EntityItemProperties snapshot;
        if (!EditorUndo::snapshot(id, snapshot) || (EditorUndo::isLocked(id) && key != LOCKED_KEY)) {
            continue;
        }
        glm::vec3 naturalSize = PropertySpecs::naturalSize(snapshot);
        if (key == SCALE_KEY && naturalSize == glm::vec3(0.0f)) {
            continue;   // a model that has not loaded: its scale is not known yet
        }
        EntityItemProperties before;
        for (const QString& stored : storedKeys) {
            PropertySpecs::set(stored, before, PropertySpecs::get(stored, snapshot));
        }
        if (key == RANGE_KEY) {
            naturalSize = glm::vec3(PropertySpecs::get(RANGE_KEY, snapshot).toFloat(), snapshot.getFalloffRadius(), 0.0f);
            before.setFalloffRadius(snapshot.getFalloffRadius());
        }
        _inspectorEditIds << id;
        _inspectorBefore << before;
        _inspectorNatural << naturalSize;
    }
    _inspectorChildren = scalesChildren(key) ? childrenToScale(_inspectorEditIds) : QVector<ScaledChild>();
}

void Editor::onInspectorLiveEdit(const QString& key, const QVariant& value) {
    if (key != _inspectorEditKey) {
        return;
    }
    // a scrub moves with the mouse, a hundred times a second or more: send no faster than a gizmo drag (sizes
    // more slowly, being heavier for the server), the latest value going out when the wait is over
    bool sizes = key == SCALE_KEY || key == QStringLiteral("dimensions");   // a light's Range has no shape to rebuild
    int interval = sizes ? SCALE_EDIT_INTERVAL_MS : EDIT_INTERVAL_MS;
    _liveEditValue = value;
    if (_liveEditTimer.isActive()) {
        return;
    }
    qint64 elapsed = _liveEditClock.isValid() ? _liveEditClock.elapsed() : interval;
    if (elapsed >= interval) {
        applyLiveEdit(key, value);
    } else {
        _liveEditTimer.start(interval - (int)elapsed);
    }
}

void Editor::applyLiveEdit(const QString& key, QVariant value) {
    _liveEditClock.restart();
    _liveEditValue = QVariant();
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    QVector<glm::vec3> ratios;
    for (int i = 0; i < _inspectorEditIds.size(); ++i) {
        EntityItemProperties edit = inspectorEdit(key, value, _inspectorNatural[i], _inspectorBefore[i]);
        entities->editEntity(_inspectorEditIds[i], edit);
        ratios << sizeRatio(_inspectorBefore[i].getDimensions(), edit.getDimensions());
    }
    if (!_inspectorChildren.isEmpty()) {
        sendChildScale(_inspectorChildren, childScaleEdits(_inspectorChildren, ratios), true);
    }
}

void Editor::onInspectorEditCancelled(const QString& key) {
    if (key != _inspectorEditKey) {
        return;
    }
    // the live preview goes back to what the entities held when the edit began; no undo step
    _liveEditTimer.stop();
    _liveEditValue = QVariant();
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    for (int i = 0; i < _inspectorEditIds.size(); ++i) {
        entities->editEntity(_inspectorEditIds[i], _inspectorBefore[i]);
    }
    QVector<EntityItemProperties> childrenBefore;
    for (const auto& child : _inspectorChildren) {
        childrenBefore << childScaleBefore(child);
    }
    sendChildScale(_inspectorChildren, childrenBefore, true);
    _inspectorEditKey.clear();
    _inspectorEditIds.clear();
    _inspectorBefore.clear();
    _inspectorNatural.clear();
    _inspectorChildren.clear();
    refreshInspector(false);
}

void Editor::testInspectorEdit(const QString& key, const QVariant& value) {
    onInspectorEditBegan(key);
    onInspectorLiveEdit(key, value);
    onInspectorCommitted(key, value);
}

void Editor::onInspectorCommitted(const QString& key, const QVariant& value) {
    commitInspectorEdit(key, value, _selection.ids());
}

void Editor::commitInspectorEdit(const QString& key, const QVariant& value, const QList<QUuid>& targets) {
    if (key == QStringLiteral("parentID")) {
        // Parent typed in: the same move as a drag in the Hierarchy, the entity staying where it is in the world
        QString text = value.toString().trimmed();
        QUuid parentId(text);
        if (!text.isEmpty() && parentId.isNull()) {
            setStatus("Parent must be an entity's id, or empty for none", STATUS_CLEAR_MS * 2);
        } else {
            reparent(targets, parentId);
        }
        refreshInspector(false);
        return;
    }
    QList<QUuid> ids;
    QVector<EntityItemProperties> before;
    QVector<glm::vec3> naturalSizes;
    QVector<ScaledChild> children;
    bool alreadyApplied = key == _inspectorEditKey && !_inspectorEditIds.isEmpty();
    bool entryRefused = false;   // an entry of a JSON property, on an entity whose property can't hold it
    if (alreadyApplied) {
        // the end of a scrub: the values went out live, the undo point was taken at its start
        ids = _inspectorEditIds;
        before = _inspectorBefore;
        naturalSizes = _inspectorNatural;
        children = _inspectorChildren;
        _liveEditTimer.stop();
        applyLiveEdit(key, value);
    } else {
        QStringList storedKeys = PropertySpecs::storedKeys(key);
        for (const auto& id : targets) {
            EntityItemProperties snapshot;
            // a locked entity takes no edits, but Locked itself: that is how it gets unlocked
            if (!EditorUndo::snapshot(id, snapshot) || (EditorUndo::isLocked(id) && key != LOCKED_KEY)) {
                continue;
            }
            if (PropertySpecs::isEntry(key) && !PropertySpecs::takesEntry(key, snapshot)) {
                entryRefused = true;
                continue;
            }
            glm::vec3 naturalSize = PropertySpecs::naturalSize(snapshot);
            if (key == SCALE_KEY && naturalSize == glm::vec3(0.0f)) {
                continue;
            }
            EntityItemProperties properties;
            for (const QString& stored : storedKeys) {
                PropertySpecs::set(stored, properties, PropertySpecs::get(stored, snapshot));
            }
            if (key == RANGE_KEY) {
                naturalSize = glm::vec3(PropertySpecs::get(RANGE_KEY, snapshot).toFloat(), snapshot.getFalloffRadius(), 0.0f);
                properties.setFalloffRadius(snapshot.getFalloffRadius());
            }
            ids << id;
            before << properties;
            naturalSizes << naturalSize;
        }
        if (scalesChildren(key)) {
            children = childrenToScale(ids);
        }
    }
    _inspectorEditKey.clear();
    _inspectorEditIds.clear();
    _inspectorBefore.clear();
    _inspectorNatural.clear();
    _inspectorChildren.clear();
    if (ids.isEmpty() || (!alreadyApplied && !editable())) {
        if (entryRefused) {
            auto spec = PropertySpecs::find(key);
            QString why = PropertySpecs::isUserDataEntry(key) ? "Its User Data is plain text, not entries"
                                                              : "Its Material Data isn't a set of materials";
            setStatus(QString("%1, so it can't hold a %2").arg(why, spec ? spec->label.toLower() : key),
                      STATUS_CLEAR_MS * 2);
        }
        refreshInspector(false);
        return;
    }
    QVector<EntityItemProperties> after;
    QVector<glm::vec3> ratios;
    for (int i = 0; i < ids.size(); ++i) {
        after << inspectorEdit(key, value, naturalSizes[i], before[i]);
        ratios << sizeRatio(before[i].getDimensions(), after[i].getDimensions());
    }
    // the children in the same undo step (sent already when the value went out live)
    QVector<EntityItemProperties> childEdits = childScaleEdits(children, ratios);
    for (int i = 0; i < children.size(); ++i) {
        ids << children[i].id;
        before << childScaleBefore(children[i]);
        after << childEdits[i];
    }
    if (key == TRACK_POINTS_KEY) {
        // the box follows the curve, as after a drag, an insert or a delete in the viewport; a scrub's points went out
        // live without it, so the box goes now (the undo point holds the dimensions: storedKeys)
        auto tree = entityTree();
        auto entities = DependencyManager::get<EntityScriptingInterface>();
        for (int i = 0; i < ids.size() && tree; ++i) {
            tree->withReadLock([&] {
                if (auto entity = tree->findEntityByEntityItemID(ids[i])) {
                    fitTrackBox(entity, after[i].getTrackPoints(), after[i]);
                }
            });
            if (alreadyApplied) {
                entities->editEntity(ids[i], after[i]);
            }
        }
    }
    auto spec = PropertySpecs::find(key);
    _undo.pushPropertyEdit(spec ? spec->label : key, ids, before, after, alreadyApplied);
    if (key == VOXEL_SIZE_KEY) {
        _terrain.resample(ids);   // a terrain's voxels follow its new size (its own undo step, after the property's)
    }
    refreshInspector(false);
}

PickRay Editor::pickRay(const QPoint& viewportPos) const {
    return qApp->computePickRay((float)viewportPos.x(), (float)viewportPos.y());
}

QUuid Editor::pickEntity(const QPoint& viewportPos) const {
    return pickEntityOnRay(pickRay(viewportPos));
}

QUuid Editor::pickEntityOnRay(const PickRay& ray) const {
    // what this builder hid or made unpickable (the Hierarchy's eye and hand) lets the click through, to what's behind
    QVector<EntityItemID> passedBy;
    for (const auto& id : _hiddenHere) {
        passedBy << id;
    }
    for (const auto& id : _noPick) {
        passedBy << id;
    }
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    auto result = entities->evalRayIntersectionVector(ray, worldPickFilter(), QVector<EntityItemID>(), passedBy);
    return result.intersects ? result.entityID : QUuid();
}

bool Editor::cursorWorldPoint(glm::vec3& point) const {
    auto viewport = qApp->getPrimaryWidget();
    if (!viewport) {
        return false;
    }
    QPoint local = viewport->mapFromGlobal(QCursor::pos());
    if (!viewport->rect().contains(local)) {
        return false;
    }
    PickRay ray = pickRay(local);
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    auto result = entities->evalRayIntersectionVector(ray, worldPickFilter(), QVector<EntityItemID>(), QVector<EntityItemID>());
    if (result.intersects) {
        point = result.intersection;
        return true;
    }
    // nothing under the cursor: the grid plane
    if (std::abs(ray.direction.y) > 1.0e-4f) {
        float t = (_grid.height() - ray.origin.y) / ray.direction.y;
        if (t > 0.0f) {
            point = ray.origin + ray.direction * t;
            return true;
        }
    }
    return false;
}

glm::vec3 Editor::selectionAnchor() const {
    // the middle of the selection's footprint: what a paste puts on the point under the cursor
    AABox box;
    if (!_selection.bounds(box)) {
        return glm::vec3(0.0f);
    }
    glm::vec3 center = box.calcCenter();
    return glm::vec3(center.x, box.getMinimumPoint().y, center.z);
}

void Editor::clickSelect(const QPoint& viewportPos, Qt::KeyboardModifiers modifiers) {
    bool shift = modifiers.testFlag(Qt::ShiftModifier);
    bool command = modifiers.testFlag(Qt::ControlModifier);
    QUuid hit = pickAt(viewportPos);
    if (hit.isNull()) {
        if (!shift && !command) {
            _selection.clear();
        }
        return;
    }
    if (command) {
        _selection.toggle(hit);
    } else if (shift) {
        _selection.add(hit);
    } else {
        _selection.set({ hit });
    }
}

void Editor::updateMarquee(const QPoint& viewportPos) {
    _marqueeRect = QRectF(QPointF(_pressPos), QPointF(viewportPos)).normalized();
    emit marqueeChanged();
}

QList<QUuid> Editor::entitiesInRect(const QRectF& rect) const {
    QList<QUuid> ids;
    auto tree = entityTree();
    auto viewport = qApp->getPrimaryWidget();
    if (!tree || !viewport || !_treeModel) {
        return ids;
    }
    ViewFrustum frustum;
    qApp->getViewState().copyViewFrustum(frustum);
    glm::mat4 viewProjection = frustum.getProjection() * frustum.getView();
    float width = (float)std::max(1, viewport->width());
    float height = (float)std::max(1, viewport->height());
    QList<QUuid> all = _treeModel->allIds();
    tree->withReadLock([&] {
        for (const auto& id : all) {
            auto entity = tree->findEntityByEntityItemID(id);
            if (!entity || !entity->getVisible() || excludedFromPicks(id)) {
                continue;
            }
            glm::vec4 clip = viewProjection * glm::vec4(entity->getWorldPosition(), 1.0f);
            if (clip.w <= 0.0f) {
                continue;   // behind the camera
            }
            float sx = (clip.x / clip.w + 1.0f) * 0.5f * width;
            float sy = (1.0f - clip.y / clip.w) * 0.5f * height;
            if (rect.contains(QPointF(sx, sy))) {
                ids << id;
            }
        }
    });
    return ids;
}

void Editor::finishMarquee(Qt::KeyboardModifiers modifiers) {
    _marqueeActive = false;
    emit marqueeChanged();
    QList<QUuid> inside = entitiesInRect(_marqueeRect);
    bool shift = modifiers.testFlag(Qt::ShiftModifier);
    bool command = modifiers.testFlag(Qt::ControlModifier);
    ++_selectionBatch;   // one step in the history, however many toggles
    if (command) {
        for (const auto& id : inside) {
            _selection.toggle(id);
        }
    } else if (shift) {
        QList<QUuid> ids = _selection.ids();
        ids << inside;
        _selection.set(ids);
    } else {
        _selection.set(inside);
    }
    --_selectionBatch;
    recordSelection();
}

void Editor::select(const QList<QUuid>& ids) {
    if (!_active) {
        setActive(true);
    }
    _selection.set(ids);
}

QPixmap Editor::inspectorPicture() const {
    return _inspector ? _inspector->contentPicture() : QPixmap();
}

void Editor::uploadFilesForTest(const QStringList& localPaths) {
    if (_assets) {
        _assets->uploadFilesForTest(localPaths);
    }
}

QString Editor::statusForTest() const {
    return _statusLabel ? _statusLabel->text() : QString();
}

void Editor::dragSelectionForTest(const glm::vec3& delta, const glm::quat& rotationDelta, bool rotate, bool begin, bool final,
                                  const glm::vec3& scale, bool scaling) {
    if (!_active) {
        setActive(true);
    }
    if (begin || _dragKind == DragKind::None) {
        _dragIds.clear();
        _dragPositions.clear();
        _dragRotations.clear();
        _dragDimensions.clear();
        _dragBefore.clear();
        auto tree = entityTree();
        if (!tree) {
            return;
        }
        DragKind kind = rotate ? DragKind::Rotate : scaling ? DragKind::Scale : DragKind::Move;
        tree->withReadLock([&] {
            for (const auto& id : _selection.topLevel()) {
                auto entity = tree->findEntityByEntityItemID(id);
                if (!entity || entity->getLocked()) {
                    continue;
                }
                glm::vec3 position = entity->getWorldPosition();
                glm::quat rotation = entity->getWorldOrientation();
                EntityItemProperties before;
                before.setPosition(position);
                if (kind == DragKind::Rotate) {
                    before.setRotation(rotation);
                } else if (kind == DragKind::Scale) {
                    before.setDimensions(entity->getScaledDimensions());
                }
                _dragIds << id;
                _dragPositions << position;
                _dragRotations << rotation;
                _dragDimensions << entity->getScaledDimensions();
                _dragBefore << before;
            }
        });
        if (_dragIds.isEmpty()) {
            return;
        }
        _dragChildren = kind == DragKind::Scale ? childrenToScale(_dragIds) : QVector<ScaledChild>();
        _dragKind = kind;
        _dragPivot = _dragPositions.first();
        _dragGizmoRotation = _gizmo.rotation();
        _dragScale = glm::vec3(1.0f);
        _editTimer.start();
    }
    _dragDelta = delta;
    _dragRotationDelta = rotationDelta;
    _dragScale = scale;
    applyDrag(final);
    if (final) {
        _dragKind = DragKind::None;
        _dragIds.clear();
    }
}

void Editor::selectAll() {
    if (_active && _treeModel) {
        _selection.set(_treeModel->allIds());
    }
}

// --- stable links -------------------------------------------------------------------------------

namespace {
    // the entity properties that can hold a link to a file, read and written through the Inspector's accessors. A
    // terrain's own file (terrainURL) is left out: its next save writes the stable link, and a changed link would
    // have the renderer fetch the file again, over strokes not yet saved
    const QStringList LINK_KEYS {
        "modelURL", "compoundShapeURL", "animation.url", "textures", "imageURL", "sourceUrl", "scriptURL", "materialURL",
        "materialData", "script", "serverScripts", "collisionSoundURL", "userData", "skybox.url", "ambientLight.ambientURL",
        "filterURL", "font"
    };
    const QStringList SCRIPT_KEYS { "script", "serverScripts" };
    const int MISSING_SHOWN = 30;   // the links the report lists by name
    const int STABLE_LINKS_TIMEOUT_MS = 30000;
}

void Editor::useStableLinks() {
    if (!_active || _stableLinksPending || !editable()) {
        return;
    }
    UserFiles* files = _assets ? _assets->files() : nullptr;
    if (!files || !UserFiles::isLoggedIn()) {
        setStatus("Sign in to use stable links: they come from My Files", STATUS_CLEAR_MS);
        return;
    }
    // from a fresh listing, so that a file moved a moment ago is found where it is now
    _stableLinksPending = true;
    setStatus("Reading My Files…");
    auto onListed = std::make_shared<QMetaObject::Connection>();
    auto onFailed = std::make_shared<QMetaObject::Connection>();
    auto done = std::make_shared<bool>(false);
    auto finish = [this, onListed, onFailed, done] {
        QObject::disconnect(*onListed);
        QObject::disconnect(*onFailed);
        *done = true;
        _stableLinksPending = false;
    };
    // a listing that never comes does not hold the command for ever
    QTimer::singleShot(STABLE_LINKS_TIMEOUT_MS, this, [this, done, finish] {
        if (!*done) {
            finish();
            setStatus("My Files did not answer: no links were changed", STATUS_CLEAR_MS);
        }
    });
    *onListed = connect(files, &UserFiles::listed, this, [this, files, finish] {
        finish();
        if (!files->hasListing()) {
            setStatus("My Files could not be read: no links were changed", STATUS_CLEAR_MS);
            return;
        }
        rewriteStableLinks(*files);
    });
    *onFailed = connect(files, &UserFiles::listingFailed, this, [this, finish](const QString& why) {
        finish();
        setStatus("My Files could not be read (" + why + "): no links were changed", STATUS_CLEAR_MS);
    });
    files->refresh();
}

void Editor::rewriteStableLinks(const UserFiles& files) {
    if (!_active || !_treeModel || !editable()) {
        setStatus(QString(), 0);
        return;   // build mode was left, or the rights went, while the listing came
    }
    const QString undoText = "Use Stable Links";
    int rewritten = 0;
    int lockedLinks = 0;
    QStringList missing;
    QList<QUuid> ids;
    QVector<EntityItemProperties> before;
    QVector<EntityItemProperties> after;
    QVector<QPair<QUuid, terrain::LayerStack>> layerChanges;
    QSet<QUuid> changed;
    int scriptsRestarting = 0;
    for (const QUuid& id : _treeModel->allIds()) {
        EntityItemProperties properties;
        if (!EditorUndo::snapshot(id, properties)) {
            continue;
        }
        EntityItemProperties was;
        EntityItemProperties now;
        int count = 0;
        bool scriptChanged = false;
        for (const QString& key : LINK_KEYS) {
            const QString value = PropertySpecs::get(key, properties).toString();
            if (value.isEmpty()) {
                continue;
            }
            int found = 0;
            const QString stable = files.withStableLinks(value, found, missing);
            if (found > 0) {
                PropertySpecs::set(key, was, value);
                PropertySpecs::set(key, now, stable);
                count += found;
                scriptChanged = scriptChanged || SCRIPT_KEYS.contains(key);
            }
        }
        // a terrain's layers live in its file on this client: their textures' links
        terrain::LayerStack layers;
        int layerCount = 0;
        if (const terrain::LayerStack* stack = _terrain.layersOf(id)) {
            layers = *stack;
            for (auto& layer : layers.layers) {
                for (std::string* url : { &layer.materialURL, &layer.normalURL, &layer.roughnessURL, &layer.occlusionURL, &layer.heightURL }) {
                    if (url->empty()) {
                        continue;
                    }
                    int found = 0;
                    const QString stable = files.withStableLinks(QString::fromStdString(*url), found, missing);
                    if (found > 0) {
                        *url = stable.toStdString();
                        layerCount += found;
                    }
                }
            }
        }
        if (count + layerCount == 0) {
            continue;
        }
        if (EditorUndo::isLocked(id)) {
            lockedLinks += count + layerCount;
            continue;
        }
        if (count > 0) {
            ids << id;
            before << was;
            after << now;
            if (scriptChanged) {
                ++scriptsRestarting;
            }
        }
        if (layerCount > 0) {
            layerChanges.push_back({ id, layers });
        }
        rewritten += count + layerCount;
        changed.insert(id);
    }

    // one step for all of it
    if (!changed.isEmpty()) {
        _undo.stack()->beginMacro(undoText);
        if (!ids.isEmpty()) {
            _undo.pushPropertyEdit(undoText, ids, before, after, false);
        }
        for (const auto& change : layerChanges) {
            _terrain.setLayers(change.first, change.second, undoText);
        }
        _undo.stack()->endMacro();
    }
    qCDebug(interfaceapp) << "Use Stable Links:" << rewritten << "links in" << changed.size() << "entities;" << missing.size()
                          << "not in My Files;" << lockedLinks << "on locked entities";

    // the report, in Darkride's popups (ui/Popup.h): a banner when every link it could change was changed, an alert
    // with the reasons behind Details when some were left as they were
    QStringList notes;
    if (scriptsRestarting > 0) {
        notes << QString("%1 %2 with the new links.").arg(scriptsRestarting)
                     .arg(scriptsRestarting == 1 ? "entity's scripts restart" : "entities' scripts restart");
    }
    if (!missing.isEmpty()) {
        notes << QString("%1 %2 to a path where My Files has no file now: moved, renamed or deleted since. %3 left as "
                         "%4 are; a moved file is still found through its former path.")
                     .arg(missing.size()).arg(missing.size() == 1 ? "link points" : "links point")
                     .arg(missing.size() == 1 ? "It was" : "They were").arg(missing.size() == 1 ? "it" : "they");
    }
    if (lockedLinks > 0) {
        notes << QString("%1 %2 on locked entities %3 left as %4 are: unlock them first.").arg(lockedLinks)
                     .arg(lockedLinks == 1 ? "link" : "links").arg(lockedLinks == 1 ? "was" : "were")
                     .arg(lockedLinks == 1 ? "it" : "they");
    }
    setStatus(rewritten > 0 ? QString("Use Stable Links: %1 %2 changed").arg(rewritten).arg(rewritten == 1 ? "link" : "links")
                            : QString("Use Stable Links: nothing changed"), STATUS_CLEAR_MS);
    const int leftAlone = missing.size() + lockedLinks;
    const QString title = QString("%1 %2 now stable").arg(rewritten).arg(rewritten == 1 ? "link" : "links");
    const QString changedLine = QString("In %1 %2: %3 working when a file is moved or renamed. Undo puts %4 back.")
                                    .arg(changed.size()).arg(changed.size() == 1 ? "entity" : "entities")
                                    .arg(rewritten == 1 ? "it keeps" : "they keep").arg(rewritten == 1 ? "it" : "them");
    if (leftAlone == 0) {
        if (rewritten > 0) {
            new popup::Banner(popup::appIcon(), title,
                              changedLine + (scriptsRestarting > 0 ? " " + notes.first() : QString()), qApp->getWindow());
        } else {
            new popup::Banner(popup::appIcon(), "Nothing to change",
                              "No link in this world names one of your files by its path.", qApp->getWindow());
        }
        return;
    }
    QString details = (rewritten > 0 ? QStringList(changedLine) + notes : notes).join("\n\n");
    if (!missing.isEmpty()) {
        QStringList shown = missing.mid(0, MISSING_SHOWN);
        if (missing.size() > MISSING_SHOWN) {
            shown << QString("… and %1 more").arg(missing.size() - MISSING_SHOWN);
        }
        details += "\n\nNot in My Files:\n" + shown.join('\n');
    }
    popup::Alert alert(popup::appIcon(), rewritten > 0 ? title : QString("No links were changed"),
                       QString("%1 %2 left as %3 %4.").arg(leftAlone).arg(leftAlone == 1 ? "link was" : "links were")
                           .arg(leftAlone == 1 ? "it" : "they").arg(leftAlone == 1 ? "was" : "were"),
                       details, { { "OK", popup::Role::Default } }, qApp->getWindow());
    alert.exec();
}

void Editor::frameSelection() {
    AABox box;
    if (_active && _selection.bounds(box)) {
        _camera.frame(box);
        updateAxes();
    }
}

// --- the Hierarchy's rows that are not loaded (NATIVE-EDITOR.md 8) -----------------------------

void Editor::onHierarchySelection(const QList<QUuid>& ids) {
    if (_syncingSelection) {
        return;
    }
    // the gizmo, the outline and the Inspector need the entity: rows that are not loaded wait for it
    QList<QUuid> loaded;
    QList<QUuid> waiting;
    for (const auto& id : ids) {
        if (!_treeModel || _treeModel->isLoaded(id)) {
            loaded << id;
        } else {
            waiting << id;
        }
    }
    _arrivalIds = waiting;
    _syncingSelection = true;
    _selection.set(loaded);
    _syncingSelection = false;
}

void Editor::goToListed(const QUuid& id) {
    if (!_active || !_treeModel) {
        return;
    }
    AACube cube;
    QString name;
    if (!_treeModel->listedPlace(id, cube, name)) {
        if (_treeModel->isLoaded(id)) {
            _selection.set({ id });
            frameSelection();
        }
        return;
    }
    if (!_arrivalIds.contains(id)) {
        _arrivalIds << id;
    }
    // the stream follows the build camera: once the entity looks big enough from here, the server sends it
    _goingTo = id;
    _goingToName = name;
    _camera.frame(AABox(cube));
    updateAxes();
    setStatus(QString("Going to %1. It shows when the server sends it.").arg(name));
    _arrivalTimer.start();
}

void Editor::onEntityLoaded(const QUuid& id) {
    if (id == _goingTo) {
        _arrivalTimer.stop();
        setStatus(QString("%1 is here.").arg(_goingToName), STATUS_CLEAR_MS);
        _goingTo = QUuid();
    }
    if (_arrivalIds.removeOne(id)) {
        _selectingArrival = true;
        _selection.add(id);
        _selectingArrival = false;
    }
}

void Editor::refreshEntityList() {
    if (_active && _entityList) {
        _entityList->request();
    }
}

void Editor::updateListNote() {
    if (!_hierarchy || !_entityList) {
        return;
    }
    QString note;
    switch (_entityList->state()) {
        case EditorEntityList::State::Waiting:
            note = "Asking the server for everything in the world…";
            break;
        case EditorEntityList::State::Unanswered:
            note = "The server did not send its list: only what has loaded is shown.";
            break;
        default:
            break;
    }
    _hierarchy->setListNote(note);
}

QVariantMap Editor::setTerrainRandomAngleForTest(bool on) {
    QVariantMap result;
    auto random = _terrainBar ? _terrainBar->findChild<QCheckBox*>("EditorTerrainRandomAngle") : nullptr;
    auto angle = _terrainBar ? _terrainBar->findChild<QWidget*>("EditorTerrainAngle") : nullptr;
    result["found"] = random && angle;
    if (random && angle) {
        random->setChecked(on);
        result["angleEnabled"] = angle->isEnabled();
    }
    return result;
}

float Editor::terrainStrokeForTest(const QUuid& id) {
    auto tree = entityTree();
    glm::vec3 position;
    bool found = false;
    if (tree) {
        tree->withReadLock([&] {
            if (auto entity = tree->findEntityByEntityItemID(id)) {
                position = entity->getWorldPosition();
                found = true;
            }
        });
    }
    if (!found) {
        return -1.0f;
    }
    const float used = _terrain.angle();
    QString refusal;
    if (!_terrain.beginStroke(PickRay(position + glm::vec3(0.0f, 30.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)), false, refusal)) {
        return -1.0f;
    }
    _terrain.endStroke(false);
    return used;
}

void Editor::loadLocalStates() {
    if (_localStatesLoaded) {
        return;
    }
    _localStatesLoaded = true;
    for (const auto& text : _hiddenHereSetting.get()) {
        _hiddenHere.insert(QUuid(text));
    }
    for (const auto& text : _noPickSetting.get()) {
        _noPick.insert(QUuid(text));
    }
    _hiddenHere.remove(QUuid());
    _noPick.remove(QUuid());
}

void Editor::saveLocalStates() {
    auto texts = [](const QSet<QUuid>& ids) {
        QStringList list;
        for (const auto& id : ids) {
            list << id.toString(QUuid::WithoutBraces);
        }
        list.sort();
        return list;
    };
    _hiddenHereSetting.set(texts(_hiddenHere));
    _noPickSetting.set(texts(_noPick));
}

void Editor::onHierarchyToggle(int toggle, const QList<QUuid>& ids, bool on) {
    if (toggle == HierarchyPanel::Lock || toggle == HierarchyPanel::Scripts) {
        // the entity's own Locked, or whether its scripts run, for everyone: one undo step for the row and what's
        // under it (on is locked, or scripts off)
        if (toggle == HierarchyPanel::Lock) {
            commitInspectorEdit(LOCKED_KEY, on, ids);
        } else {
            commitInspectorEdit(SCRIPTS_ENABLED_KEY, !on, ids);
        }
        refreshInspector(false);
        return;
    }
    QSet<QUuid>& states = toggle == HierarchyPanel::Hide ? _hiddenHere : _noPick;
    for (const auto& id : ids) {
        if (on) {
            states.insert(id);
        } else {
            states.remove(id);
        }
    }
    if (toggle == HierarchyPanel::Hide && _active) {
        setLocallyHidden(ids, on);
    }
    saveLocalStates();
    if (_hierarchy) {
        _hierarchy->setLocalStates(_hiddenHere, _noPick);
    }
    updateInspectorToggles();   // the header's eye and pointer: no property changed to refresh it
}

void Editor::remindOfScriptsOff() {
    // scripts turned off here stay off for everyone after build mode: said once as it closes, so a door left not
    // opening isn't a surprise later (only to someone who can turn them on again)
    int off = (_treeModel && !_readOnly) ? _treeModel->scriptsOffCount() : 0;
    if (off == 0) {
        _scriptsOffReminder.clear();
        return;
    }
    QString title = off == 1 ? QString("1 entity has its scripts off")
                             : QString("%1 entities have their scripts off").arg(off);
    QString text = "They stay off for everyone until they're turned on again in Build mode, in the Entities list or the "
                   "Inspector.";
    _scriptsOffReminder = title + "\n" + text;
    new popup::Banner(popup::appIcon(), title, text, qApp->getWindow(), SCRIPTS_OFF_REMINDER_MS);
}

void Editor::showAllHere() {
    // this world's: what's hidden in other worlds stays as it was left there
    QList<QUuid> here;
    for (const auto& id : _hiddenHere) {
        if (_treeModel && _treeModel->indexForId(id).isValid()) {
            here << id;
        }
    }
    onHierarchyToggle(HierarchyPanel::Hide, here, false);
}

void Editor::applyHiddenHere(bool hide) {
    setLocallyHidden(_hiddenHere.values(), hide);
}

void Editor::setLocallyHidden(const QList<QUuid>& ids, bool hidden) {
    auto tree = entityTree();
    if (!tree) {
        return;
    }
    QList<QUuid> changed;
    tree->withReadLock([&] {
        for (const auto& id : ids) {
            auto entity = tree->findEntityByEntityItemID(id);
            if (entity && entity->getLocallyVisible() == hidden) {
                entity->setLocallyVisible(!hidden);
                changed << id;
            }
        }
    });
    // the renderer takes the change on its next update, as it does an edit's
    if (auto renderer = qApp->getEntities()) {
        for (const auto& id : changed) {
            renderer->onEntityChanged(id);
        }
    }
}

bool Editor::strokeHierarchyToggle(const QString& which, const QList<QUuid>& ids, bool onlyThis) {
    if (!_hierarchy) {
        return false;
    }
    auto toggle = which == "hide" ? HierarchyPanel::Hide : which == "pick" ? HierarchyPanel::NoPick
                : which == "scripts" ? HierarchyPanel::Scripts : HierarchyPanel::Lock;
    return _hierarchy->testStroke(toggle, ids, onlyThis);
}

bool Editor::clickHierarchyToggle(const QString& which, const QUuid& id, bool onlyThis) {
    if (!_hierarchy) {
        return false;
    }
    auto toggle = which == "hide" ? HierarchyPanel::Hide : which == "pick" ? HierarchyPanel::NoPick
                : which == "scripts" ? HierarchyPanel::Scripts : HierarchyPanel::Lock;
    return _hierarchy->clickToggle(toggle, id, onlyThis);
}

QVariantMap Editor::hierarchyToggles(const QUuid& id) const {
    QVariantMap result;
    if (!_hierarchy) {
        return result;
    }
    result["hidden"] = _hierarchy->isOn(HierarchyPanel::Hide, id);
    result["noPick"] = _hierarchy->isOn(HierarchyPanel::NoPick, id);
    result["locked"] = _hierarchy->isOn(HierarchyPanel::Lock, id);
    result["scriptsOff"] = _hierarchy->isOn(HierarchyPanel::Scripts, id);
    result["scriptsOffReminder"] = _scriptsOffReminder;
    result["hiddenCount"] = _hierarchy->hiddenCount();
    if (auto counts = _hierarchy->findChild<QLabel*>("EditorHierarchyCounts")) {
        result["note"] = counts->text();
    }
    if (auto tree = qApp->getEntities() ? qApp->getEntities()->getTree() : nullptr) {
        tree->withReadLock([&] {
            if (auto entity = tree->findEntityByEntityItemID(id)) {
                result["drawn"] = entity->getVisible() && entity->getLocallyVisible();
            }
        });
    }
    return result;
}

QUuid Editor::pickAtEntityForTest(const QUuid& id) const {
    auto tree = qApp->getEntities() ? qApp->getEntities()->getTree() : nullptr;
    glm::vec3 position;
    bool found = false;
    if (tree) {
        tree->withReadLock([&] {
            if (auto entity = tree->findEntityByEntityItemID(id)) {
                position = entity->getWorldPosition();
                found = true;
            }
        });
    }
    if (!found) {
        return QUuid();
    }
    // the click's ray, from the camera through the entity's middle
    glm::vec3 eye = _camera.position();
    return pickOnRay(PickRay(eye, glm::normalize(position - eye)));
}

QList<QUuid> Editor::marqueeAllForTest() const {
    auto viewport = qApp->getPrimaryWidget();
    return viewport ? entitiesInRect(QRectF(viewport->rect())) : QList<QUuid>();
}

QVariantMap Editor::hierarchyCounts() const {
    QVariantMap counts;
    counts["entities"] = _treeModel ? _treeModel->count() : 0;
    counts["loaded"] = _treeModel ? _treeModel->loadedCount() : 0;
    counts["listed"] = _treeModel && _treeModel->hasList();
    QString state = "none";
    if (_entityList) {
        switch (_entityList->state()) {
            case EditorEntityList::State::Waiting: state = "waiting"; break;
            case EditorEntityList::State::Answered: state = "answered"; break;
            case EditorEntityList::State::Unanswered: state = "unanswered"; break;
            default: break;
        }
    }
    counts["state"] = state;
    QVariantList selected;
    for (const auto& id : _selection.ids()) {
        selected << id.toString(QUuid::WithoutBraces);
    }
    counts["selected"] = selected;
    return counts;
}

void Editor::clickHierarchyRow(const QUuid& id) {
    if (!_active || !_hierarchy) {
        return;
    }
    _syncingSelection = true;
    _hierarchy->selectIds({ id });
    _syncingSelection = false;
    onHierarchySelection({ id });
    if (_treeModel && !_treeModel->isLoaded(id)) {
        goToListed(id);
    }
}

// --- the gizmo drag -----------------------------------------------------------------------------

bool Editor::snapActive() const {
    return _snapAlways || commandModifier(QGuiApplication::keyboardModifiers());
}

void Editor::beginGizmoDrag(GizmoHandle handle, const PickRay& ray) {
    _dragIds.clear();
    _dragPositions.clear();
    _dragRotations.clear();
    _dragDimensions.clear();
    _dragBefore.clear();
    auto tree = entityTree();
    if (!tree) {
        return;
    }
    DragKind kind = EditorGizmo::isRing(handle) ? DragKind::Rotate
                  : EditorGizmo::isScale(handle) ? DragKind::Scale : DragKind::Move;
    // the Move gizmo on a Track's chosen control point moves that point, in the track's own space
    QUuid trackId;
    if (kind == DragKind::Move && _trackPointChosen >= 0 && trackPointTrack(trackId)) {
        bool ready = false;
        tree->withReadLock([&] {
            auto entity = tree->findEntityByEntityItemID(trackId);
            auto track = std::dynamic_pointer_cast<TrackEntityItem>(entity);
            if (!track || track->getLocked()) {
                return;
            }
            _dragTrackPoints = track->getTrackPoints();
            if (_trackPointChosen >= _dragTrackPoints.size()) {
                return;
            }
            _dragTrackPosition = track->getWorldPosition();
            _dragTrackRotation = track->getWorldOrientation();
            EntityItemProperties before;
            before.setTrackPoints(_dragTrackPoints);
            before.setDimensions(track->getScaledDimensions());
            _dragIds << trackId;
            _dragPositions << _dragTrackPosition;
            _dragRotations << _dragTrackRotation;
            _dragDimensions << track->getScaledDimensions();
            _dragBefore << before;
            ready = true;
        });
        if (!ready) {
            return;
        }
        _dragKind = DragKind::TrackPoint;
        _dragPivot = _gizmo.position();
        _dragGizmoRotation = _gizmo.rotation();
        _dragDelta = glm::vec3(0.0f);
        _dragRotationDelta = glm::quat();
        _dragScale = glm::vec3(1.0f);
        _lastDragPos = _pressPos;
        _editTimer.start();
        _gizmo.beginDrag(handle, ray);
        if (!_gizmo.isDragging()) {
            _dragKind = DragKind::None;
        } else {
            _trackGizmos.setActive(_trackPointChosen);
        }
        return;
    }
    tree->withReadLock([&] {
        for (const auto& id : _selection.topLevel()) {
            auto entity = tree->findEntityByEntityItemID(id);
            if (!entity || entity->getLocked()) {
                continue;
            }
            glm::vec3 position = entity->getWorldPosition();
            glm::quat rotation = entity->getWorldOrientation();
            glm::vec3 dimensions = entity->getScaledDimensions();
            EntityItemProperties before;
            before.setPosition(position);
            if (kind == DragKind::Rotate) {
                before.setRotation(rotation);
            } else if (kind == DragKind::Scale) {
                before.setDimensions(dimensions);
            }
            _dragIds << id;
            _dragPositions << position;
            _dragRotations << rotation;
            _dragDimensions << dimensions;
            _dragBefore << before;
        }
    });
    if (_dragIds.isEmpty()) {
        return;
    }
    _dragChildren = kind == DragKind::Scale ? childrenToScale(_dragIds) : QVector<ScaledChild>();
    _dragKind = kind;
    _dragPivot = _gizmo.position();
    _dragGizmoRotation = _gizmo.rotation();
    _dragDelta = glm::vec3(0.0f);
    _dragRotationDelta = glm::quat();
    _dragScale = glm::vec3(1.0f);
    _lastDragPos = _pressPos;
    _editTimer.start();
    _gizmo.beginDrag(handle, ray);
    if (!_gizmo.isDragging()) {
        _dragKind = DragKind::None;
    }
}

bool Editor::vertexSnapTarget(const QPoint& viewportPos, glm::vec3& target) const {
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    QVector<EntityItemID> discard;
    for (const auto& id : _dragIds) {
        discard << EntityItemID(id);
    }
    auto result = entities->evalRayIntersectionVector(pickRay(viewportPos), worldPickFilter(), QVector<EntityItemID>(), discard);
    if (!result.intersects) {
        return false;
    }
    target = result.intersection;
    // a model pick reports the triangle it hit: take its nearest corner
    QVariantMap triangle = result.extraInfo.value("subMeshTriangleWorld").toMap();
    float best = FLT_MAX;
    for (const auto& key : { "v0", "v1", "v2" }) {
        bool valid = false;
        glm::vec3 vertex = vec3FromVariant(triangle.value(key), valid);
        if (valid) {
            float distance = glm::length(vertex - result.intersection);
            if (distance < best) {
                best = distance;
                target = vertex;
            }
        }
    }
    return true;
}

void Editor::moveGizmoDrag(const QPoint& viewportPos) {
    PickRay ray = pickRay(viewportPos);
    QPoint mouseDelta = viewportPos - _lastDragPos;
    _lastDragPos = viewportPos;
    switch (_dragKind) {
        case DragKind::Move:
        case DragKind::TrackPoint: {
            glm::vec3 target;
            if (_vertexSnap && vertexSnapTarget(viewportPos, target)) {
                _dragDelta = target - _dragPivot;   // the pivot lands on the vertex
            } else {
                glm::vec3 delta = _gizmo.dragDelta(ray);
                _dragDelta = snapActive() ? snapVector(delta, _moveSnap) : delta;
            }
            break;
        }
        case DragKind::Rotate: {
            if (_gizmo.dragHandle() == GizmoHandle::Sphere) {
                _dragRotationDelta = _gizmo.dragFree((float)mouseDelta.x(), (float)mouseDelta.y());
                break;
            }
            float angle = _gizmo.dragAngle(ray);
            if (snapActive()) {
                angle = glm::radians(snapValue(glm::degrees(angle), _rotateSnap));
            }
            _dragRotationDelta = glm::angleAxis(angle, _gizmo.dragAxis());
            break;
        }
        case DragKind::Scale: {
            glm::vec3 factor = _gizmo.dragScale(ray);
            if (snapActive()) {
                for (int i = 0; i < 3; ++i) {
                    factor[i] = std::max(_scaleSnap, snapValue(factor[i], _scaleSnap));
                }
            }
            _dragScale = factor;
            _gizmo.setStretch(factor);
            break;
        }
        default:
            break;
    }
    // scale edits also reach the entity server, which rebuilds its own shapes: send those less often
    int interval = _dragKind == DragKind::Scale ? SCALE_EDIT_INTERVAL_MS : EDIT_INTERVAL_MS;
    if (_editTimer.elapsed() >= interval) {
        applyDrag(false);
    }
}

void Editor::applyDrag(bool final) {
    _editTimer.restart();
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    auto tree = entityTree();
    QVector<EntityItemProperties> after;
    QString text;
    for (int i = 0; i < _dragIds.size(); ++i) {
        EntityItemProperties properties;
        switch (_dragKind) {
            case DragKind::Move:
                text = "Move";
                properties.setPosition(_dragPositions[i] + _dragDelta);
                break;
            case DragKind::TrackPoint: {
                text = "Move Point";
                QVector<glm::vec3> points = _dragTrackPoints;
                if (_trackPointChosen >= 0 && _trackPointChosen < points.size()) {
                    glm::vec3 world = _dragTrackPosition + _dragTrackRotation * points[_trackPointChosen] + _dragDelta;
                    points[_trackPointChosen] = glm::inverse(_dragTrackRotation) * (world - _dragTrackPosition);
                }
                properties.setTrackPoints(points);
                if (final && tree) {
                    tree->withReadLock([&] {
                        if (auto entity = tree->findEntityByEntityItemID(_dragIds[i])) {
                            fitTrackBox(entity, points, properties);
                        }
                    });
                }
                break;
            }
            case DragKind::Rotate:
                text = "Rotate";
                properties.setRotation(_dragRotationDelta * _dragRotations[i]);
                // around the common centre, or each about its own pivot
                properties.setPosition(_pivotMode ? _dragPositions[i]
                    : _dragPivot + _dragRotationDelta * (_dragPositions[i] - _dragPivot));
                break;
            case DragKind::Scale: {
                text = "Scale";
                properties.setDimensions(glm::max(_dragDimensions[i] * _dragScale, glm::vec3(MIN_DIMENSION)));
                if (_pivotMode) {
                    properties.setPosition(_dragPositions[i]);
                } else {
                    // the group grows about the pivot, along the gizmo's axes
                    glm::vec3 local = glm::inverse(_dragGizmoRotation) * (_dragPositions[i] - _dragPivot);
                    properties.setPosition(_dragPivot + _dragGizmoRotation * (local * _dragScale));
                }
                break;
            }
            default:
                return;
        }
        entities->editEntity(_dragIds[i], properties);
        if (_dragKind == DragKind::Scale && !final && tree) {
            // a dimension change makes physics rebuild the collision shape from the mesh, which is
            // slow and stalls the main thread; while the drag lasts the old shape is good enough
            tree->withWriteLock([&] {
                if (auto entity = tree->findEntityByEntityItemID(_dragIds[i])) {
                    entity->clearDirtyFlags(Simulation::DIRTY_SHAPE | Simulation::DIRTY_MASS);
                }
            });
        }
        after << properties;
    }
    QList<QUuid> ids = _dragIds;
    QVector<EntityItemProperties> before = _dragBefore;
    if (_dragKind == DragKind::Scale && !_dragChildren.isEmpty()) {
        QVector<glm::vec3> ratios;
        for (int i = 0; i < _dragIds.size(); ++i) {
            ratios << sizeRatio(_dragDimensions[i], glm::max(_dragDimensions[i] * _dragScale, glm::vec3(MIN_DIMENSION)));
        }
        QVector<EntityItemProperties> childEdits = childScaleEdits(_dragChildren, ratios);
        sendChildScale(_dragChildren, childEdits, final);
        for (int i = 0; i < _dragChildren.size(); ++i) {
            ids << _dragChildren[i].id;
            before << childScaleBefore(_dragChildren[i]);
            after << childEdits[i];
        }
    }
    bool changed = (_dragKind == DragKind::Move || _dragKind == DragKind::TrackPoint) ? _dragDelta != glm::vec3(0.0f)
                 : _dragKind == DragKind::Rotate ? _dragRotationDelta != glm::quat()
                 : _dragScale != glm::vec3(1.0f);
    if (final && changed) {
        _undo.pushPropertyEdit(text, ids, before, after, true);
    }
}

// --- edits --------------------------------------------------------------------------------------

void Editor::undo() {
    auto stack = _undo.stack();
    qCDebug(interfaceapp) << "editor undo:" << stack->undoText() << "index" << stack->index() << "of" << stack->count()
        << "active" << _active << "dragging" << _gizmo.isDragging();
    if (_active && !_gizmo.isDragging() && editable()) {
        ++_historyDepth;
        stack->undo();
        --_historyDepth;
    }
}

void Editor::redo() {
    auto stack = _undo.stack();
    qCDebug(interfaceapp) << "editor redo:" << stack->redoText() << "index" << stack->index() << "of" << stack->count();
    if (_active && !_gizmo.isDragging() && editable()) {
        ++_historyDepth;
        stack->redo();
        --_historyDepth;
    }
}

void Editor::rename(const QUuid& id, const QString& name) {
    if (!_treeModel) {
        return;
    }
    EntityItemProperties before;
    before.setName(_treeModel->indexForId(id).data(Qt::EditRole).toString());
    EntityItemProperties after;
    after.setName(name);
    _undo.pushPropertyEdit("Rename", { id }, { before }, { after }, false);
}

void Editor::reparent(const QList<QUuid>& ids, const QUuid& parentId) {
    if (!_active || !_treeModel || !editable()) {
        return;
    }
    QList<QUuid> moved;
    QVector<EntityItemProperties> before;
    QVector<EntityItemProperties> after;
    for (const auto& id : ids) {
        // of a family dragged together only its head moves: the rest come along with it, as in Unity
        bool withinDragged = false;
        for (const auto& other : ids) {
            if (other != id && _treeModel->isDescendantOf(id, other)) {
                withinDragged = true;
                break;
            }
        }
        EntityItemProperties snapshot;
        if (withinDragged || id == parentId || EditorUndo::isLocked(id) || !EditorUndo::snapshot(id, snapshot)
                || snapshot.getParentID() == parentId || (!parentId.isNull() && _treeModel->isDescendantOf(parentId, id))) {
            continue;
        }
        // it stays where it is in the world: the edit carries the world position and rotation, which the engine
        // makes relative to the new parent, and undoing, to the old one
        EntityItemProperties was;
        was.setParentID(snapshot.getParentID());
        was.setPosition(snapshot.getPosition());
        was.setRotation(snapshot.getRotation());
        EntityItemProperties now;
        now.setParentID(parentId);
        now.setPosition(snapshot.getPosition());
        now.setRotation(snapshot.getRotation());
        moved << id;
        before << was;
        after << now;
    }
    if (moved.isEmpty()) {
        return;
    }
    _undo.pushPropertyEdit(parentId.isNull() ? "Unparent" : "Parent", moved, before, after, false);
    if (!parentId.isNull() && _hierarchy) {
        _hierarchy->expand(parentId);   // what went under it shows
    }
    refreshInspector(false);
}

void Editor::deleteSelection() {
    if (!_active || _selection.isEmpty() || _gizmo.isDragging() || !editable()) {
        return;
    }
    // one step with its selection change, as Unity has it; the selection goes first, so that undoing brings
    // the entities back before selecting them again
    auto stack = _undo.stack();
    stack->beginMacro("Delete");
    QList<QUuid> ids = _selection.topLevel();
    _selection.clear();
    _undo.pushDelete("Delete", ids);
    stack->endMacro();
}

void Editor::duplicateSelection() {
    if (!_active || _selection.isEmpty() || _gizmo.isDragging() || !editable()) {
        return;
    }
    // in place, as Unity does; the copy is selected and the gizmo is on it, the two one step
    EditorClipboard copies;
    copies.copy(_selection.topLevel(), selectionAnchor());
    auto stack = _undo.stack();
    stack->beginMacro("Duplicate");
    QList<QUuid> created = copies.paste(_undo, "Duplicate");
    if (!created.isEmpty()) {
        _selection.set(created);
    }
    stack->endMacro();
}

void Editor::copySelection() {
    if (_active && !_selection.isEmpty()) {
        _clipboard.copy(_selection.topLevel(), selectionAnchor());
    }
}

void Editor::cutSelection() {
    if (!_active || _selection.isEmpty() || _gizmo.isDragging() || !editable()) {
        return;
    }
    _clipboard.copy(_selection.topLevel(), selectionAnchor());
    deleteSelection();
}

void Editor::paste() {
    // the copies land where the cursor points, or a few metres in front of the camera
    glm::vec3 target;
    if (!cursorWorldPoint(target)) {
        target = _camera.position() + viewDirection() * PASTE_FALLBACK_DISTANCE;
    }
    pasteAt(target);
}

void Editor::pasteAt(const glm::vec3& target) {
    if (!_active || _clipboard.isEmpty() || _gizmo.isDragging() || !editable()) {
        return;
    }
    auto stack = _undo.stack();
    stack->beginMacro("Paste");   // the copies and their selection are one step
    QList<QUuid> created = _clipboard.paste(_undo, "Paste", target - _clipboard.anchor());
    if (!created.isEmpty()) {
        _selection.set(created);
    }
    stack->endMacro();
}

// --- entity files: import and export -------------------------------------------------------------

namespace {
    // where the file dialogs open: the last folder used this session, Documents at first
    QString& entityFilesFolder() {
        static QString folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        return folder;
    }
}

void Editor::importEntitiesFromFile() {
    if (!_active || !editable()) {
        return;
    }
    QString path = QFileDialog::getOpenFileName(qApp->getWindow(), "Import Entities", entityFilesFolder(),
        "Entities (*.json *.json.gz *.svo);;All files (*)");
    if (path.isEmpty()) {
        return;
    }
    entityFilesFolder() = QFileInfo(path).absolutePath();
    importEntities(QUrl::fromLocalFile(path).toString());
}

void Editor::importEntitiesFromUrl() {
    if (!_active || !editable()) {
        return;
    }
    const popup::Field field { "https://…", QString() };
    popup::Alert alert(popup::appIcon(), "Import Entities from URL", "A link to a .json, .json.gz or .svo file.",
                       QString(), { { "Cancel", popup::Role::Cancel }, { "Import", popup::Role::Default } },
                       qApp->getWindow(), &field);
    alert.exec();
    if (alert.answer() == 1) {
        importEntities(alert.fieldText());
    }
}

// reads an entity file (an export, the old Create app's, one of the app's presets) as an add takes it: every property
// set, parents before children, a child in its parent's frame; bounds is the top level's footprint. false when it can't
// be read (count -1) or holds none it can place; count is how many it holds (more than ids when a parent loop left
// some out)
bool Editor::readEntityFile(const QString& url, QVector<EntityItemProperties>& properties, QList<QUuid>& ids,
                            AABox& bounds, int& count) {
    count = 0;
    // the entities' clipboard reads the file, of any of the formats the old Create app took, with model links
    // relative to the file resolved; the scripts' Clipboard.importEntities reads into the same tree
    auto& clipboard = qApp->getEntityClipboard();
    if (!clipboard.importEntities(url)) {
        count = -1;
        return false;
    }
    auto tree = clipboard.tree();
    QHash<QUuid, EntityItemProperties> byId;
    QList<QUuid> found;
    tree->withReadLock([&] {
        tree->recurseTreeWithOperation([&](const OctreeElementPointer& element, void*) {
            std::static_pointer_cast<EntityTreeElement>(element)->forEachEntity([&](const EntityItemPointer& entity) {
                byId.insert(entity->getID(), entity->getProperties());
                found << entity->getID();
            });
            return true;
        });
    });
    count = found.size();
    if (found.isEmpty()) {
        return false;
    }

    // as an add takes them (EditorUndo::capture's marking): every property set, a child placed in its parent's
    // frame; parents first, so the add can put each child on its new parent. An entity attached to a parent the
    // file lacks (an avatar's, or one outside an export) comes in at the top level
    properties.clear();
    ids.clear();
    QSet<QUuid> placed;
    bool hasBounds = false;
    while (ids.size() < found.size()) {
        int before = ids.size();
        for (const auto& id : found) {
            if (placed.contains(id)) {
                continue;
            }
            EntityItemProperties entry = byId.value(id);
            QUuid parentID = entry.getParentID();
            if (!parentID.isNull() && !byId.contains(parentID)) {
                entry.setParentID(QUuid());
                parentID = QUuid();
            }
            if (!parentID.isNull() && !placed.contains(parentID)) {
                continue;   // after its parent
            }
            entry.markAllChanged();
            entry.setLocalPositionChanged(true);
            entry.setLocalRotationChanged(true);
            entry.setQueryAACubeChanged(false);
            if (parentID.isNull()) {
                // the file's top level: positions in the world, relative to the export's lowest corner
                glm::vec3 dimensions = entry.getDimensions();
                AABox box(entry.getPosition() - 0.5f * dimensions, dimensions);
                if (hasBounds) {
                    bounds += box;
                } else {
                    bounds = box;
                    hasBounds = true;
                }
            }
            properties << entry;
            ids << id;
            placed.insert(id);
        }
        if (ids.size() == before) {
            break;   // a parent loop: what is left cannot be placed
        }
    }

    return !ids.isEmpty();
}

bool Editor::importEntities(const QString& url) {
    if (!_active) {
        setActive(true);
    }
    if (!_active || _gizmo.isDragging() || !editable()) {
        return false;
    }
    QString name = QUrl(url).fileName();
    if (name.isEmpty()) {
        name = url;
    }
    setStatus("Importing " + name + "…");
    QVector<EntityItemProperties> properties;
    QList<QUuid> ids;
    AABox bounds;
    int found = 0;
    if (!readEntityFile(url, properties, ids, bounds, found)) {
        setStatus(found < 0 ? "Could not read " + name + " as entities"
                  : found == 0 ? name + " holds no entities"
                  : "Could not place " + name + "'s entities: their parents make a loop", STATUS_CLEAR_MS);
        return false;
    }

    // the footprint's middle lands under the cursor, as a paste's does; a whole world (the old Create app's 10 km
    // rule) stays where its positions put it
    const float WHOLE_WORLD_SIZE = 10000.0f;
    glm::vec3 center = bounds.calcCenter();
    glm::vec3 anchor(center.x, bounds.getMinimumPoint().y, center.z);
    glm::vec3 extent = bounds.getDimensions();
    bool wholeWorld = glm::max(extent.x, glm::max(extent.y, extent.z)) >= WHOLE_WORLD_SIZE;
    glm::vec3 target = anchor;
    if (!wholeWorld && !cursorWorldPoint(target)) {
        target = _camera.position() + viewDirection() * PASTE_FALLBACK_DISTANCE;
    }

    EditorClipboard imported;
    imported.set(properties, ids, anchor);
    auto stack = _undo.stack();
    stack->beginMacro("Import " + name);   // the entities and their selection are one step
    QList<QUuid> created = imported.paste(_undo, "Import " + name, target - anchor);
    if (!created.isEmpty()) {
        _selection.set(created);
    }
    stack->endMacro();
    int skipped = found - ids.size();
    setStatus(QString("Imported %1 entit%2 from %3%4").arg(ids.size()).arg(ids.size() == 1 ? "y" : "ies").arg(name)
        .arg(skipped > 0 ? QString(" (%1 in a parent loop left out)").arg(skipped) : QString()), STATUS_CLEAR_MS);
    return !created.isEmpty();
}

void Editor::exportSelection() {
    if (!_active) {
        return;
    }
    if (_selection.isEmpty()) {
        setStatus("Select what to export first", STATUS_CLEAR_MS);
        return;
    }
    QString path = QFileDialog::getSaveFileName(qApp->getWindow(), "Export Selected Entities",
        QDir(entityFilesFolder()).filePath("entities.json"), "Entities (*.json)");
    if (path.isEmpty()) {
        return;
    }
    if (!path.endsWith(".json", Qt::CaseInsensitive)) {
        path += ".json";
    }
    entityFilesFolder() = QFileInfo(path).absolutePath();
    exportSelectionTo(path);
}

int Editor::exportSelectionTo(const QString& path) {
    if (_selection.isEmpty()) {
        return -1;
    }
    // the selection with everything under it; positions relative to the lowest corner of the top level, as the
    // old Create app's Export wrote them, so an import can place them anywhere
    QList<QUuid> all = EditorUndo::withDescendants(_selection.topLevel());
    QVector<QUuid> ids(all.begin(), all.end());
    bool ok = qApp->getEntityClipboard().exportEntities(path, ids);
    setStatus(ok ? QString("Exported %1 entit%2 to %3").arg(ids.size()).arg(ids.size() == 1 ? "y" : "ies")
                       .arg(QFileInfo(path).fileName())
                 : "Could not write " + QFileInfo(path).fileName(), STATUS_CLEAR_MS);
    return ok ? ids.size() : -1;
}

// --- go to, bring here --------------------------------------------------------------------------

void Editor::goToSelection() {
    AABox box;
    if (!_active || !_selection.bounds(box)) {
        return;
    }
    _camera.frame(box);
    updateAxes();
    // the avatar lands a little way off the selection on the camera's side, facing it; the
    // teleport also streams the surroundings in, which the camera alone does not
    glm::vec3 center = box.calcCenter();
    float radius = std::max(0.5f, glm::length(box.getScale()) * 0.5f);
    glm::vec3 toCamera = _camera.position() - center;
    toCamera.y = 0.0f;
    if (glm::length(toCamera) < 1.0e-3f) {
        toCamera = Vectors::UNIT_Z;
    }
    toCamera = glm::normalize(toCamera);
    auto avatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    glm::vec3 stand = center + toCamera * (radius + GO_TO_STANDOFF);
    stand.y = box.getCorner().y + avatar->getHeight() * 0.5f;   // feet level with the selection's bottom
    glm::vec3 forward = -toCamera;
    glm::quat orientation = glm::angleAxis(std::atan2(-forward.x, -forward.z), Vectors::UP);
    avatar->goToLocation(stand, true, orientation, false, true);
    // the teleport may put the camera back on the avatar; when it does, frame again
    _reframePending = true;
    _reframeBox = box;
    _reframeFrames = REFRAME_FRAMES;
}

void Editor::bringSelectionHere() {
    AABox box;
    if (!_active || !_selection.bounds(box) || _gizmo.isDragging() || !editable()) {
        return;
    }
    auto avatar = DependencyManager::get<AvatarManager>()->getMyAvatar();
    glm::vec3 feet = avatar->getWorldFeetPosition();
    glm::vec3 forward = avatar->getWorldOrientation() * Vectors::FRONT;
    forward.y = 0.0f;
    if (glm::length(forward) < 1.0e-3f) {
        forward = Vectors::FRONT;
    }
    forward = glm::normalize(forward);
    glm::vec3 center = box.calcCenter();
    float radius = std::max(0.5f, glm::length(box.getScale()) * 0.5f);
    // the selection lands in front of the avatar, its bottom at the avatar's feet
    glm::vec3 target = feet + forward * (radius + GO_TO_STANDOFF);
    target.y = feet.y + (center.y - box.getCorner().y);
    glm::vec3 delta = target - center;

    auto tree = entityTree();
    if (!tree) {
        return;
    }
    QList<QUuid> ids;
    QVector<EntityItemProperties> before, after;
    tree->withReadLock([&] {
        for (const auto& id : _selection.topLevel()) {
            auto entity = tree->findEntityByEntityItemID(id);
            if (!entity || entity->getLocked()) {
                continue;
            }
            EntityItemProperties was, now;
            was.setPosition(entity->getWorldPosition());
            now.setPosition(entity->getWorldPosition() + delta);
            ids << id;
            before << was;
            after << now;
        }
    });
    if (ids.isEmpty()) {
        return;
    }
    _undo.pushPropertyEdit("Bring Here", ids, before, after, false);
    frameSelection();
}

// --- create -------------------------------------------------------------------------------------

QMenu* Editor::buildCreateMenu(QWidget* parent, const QPoint* viewportPos) {
    auto menu = new QMenu("Create", parent);
    // from a context menu, new entities go where the right click was; the point is taken now,
    // while the cursor is still there and not over the menu
    bool atClick = viewportPos != nullptr;
    glm::vec3 clickPoint = atClick ? placementPoint(viewportPos) : glm::vec3(0.0f);
    QHash<QString, QMenu*> groups;
    // a group's submenu, made the first time it's named, inside its parent's ("Special items/Vehicles")
    std::function<QMenu*(const QString&, bool)> groupMenu = [&](const QString& path, bool separator) {
        if (!groups.contains(path)) {
            int slash = path.lastIndexOf('/');
            QMenu* parent = slash < 0 ? menu : groupMenu(path.left(slash), separator);
            if (slash < 0 && separator) {
                menu->addSeparator();
            }
            groups[path] = parent->addMenu(path.mid(slash + 1));
            groups[path]->setToolTipsVisible(true);
        }
        return groups[path];
    };
    for (const auto& item : EditorCreate::items()) {
        QMenu* target = menu;
        if (!item.group.isEmpty()) {
            target = groupMenu(item.group, item.separatorBefore);
        } else if (item.separatorBefore) {
            menu->addSeparator();
        }
        QString key = item.key;
        QAction* action = target->addAction(item.label, this, [this, key, atClick, clickPoint] {
            if (atClick) {
                createAt(key, clickPoint);
            } else {
                create(key);
            }
        });
        action->setToolTip(item.tooltip);
        action->setEnabled(!item.disabled);
    }
    return menu;
}

void Editor::createFromMenu() {
    auto action = qobject_cast<QAction*>(sender());
    if (action) {
        create(action->data().toString());
    }
}

void Editor::create(const QString& key) {
    createAt(key, placementPoint());
}

void Editor::createAt(const QString& key, const glm::vec3& point) {
    if (!_active || _gizmo.isDragging() || !editable()) {
        return;
    }
    if (key == "model") {
        createModelAt(point);
        return;
    }
    auto item = EditorCreate::item(key);
    if (!item || item->disabled) {
        return;
    }
    if (!item->preset.isEmpty()) {
        createPreset(PathUtils::resourcesUrl(item->preset).toString(), item->label, point, item->ahead);
        return;
    }
    EntityItemProperties properties = EditorCreate::properties(key, point, cameraYaw());
    if (item->type == EntityTypes::Material && !_selection.isEmpty()) {
        // a material applies to its parent: the first selected entity
        properties.setParentID(_selection.topLevel().first());
        properties.setPosition(glm::vec3(0.0f));
    }
    if (key == "trackcar") {
        // the car rides the Track selected when it is made: the physics takes it onto the curve from wherever it
        // was placed (the nearest point of the track)
        QUuid trackId;
        if (trackPointTrack(trackId)) {
            properties.setTrackID(trackId);
        } else {
            setStatus("Select a Track first and the car rides it; set Track in its Moving Frame section otherwise", STATUS_CLEAR_MS * 2);
        }
    }
    place(properties, "Create " + item->label, point);
}

void Editor::createModel() {
    createModelAt(placementPoint());
}

void Editor::createModelAt(const glm::vec3& point) {
    if (!_active || !editable()) {
        return;
    }
    const popup::Field field { "https://…", QString() };
    popup::Alert alert(popup::appIcon(), "Create Model", "A link to a .glb, .gltf, .fbx or .obj file.", QString(),
                       { { "Cancel", popup::Role::Cancel }, { "Create", popup::Role::Default } }, qApp->getWindow(), &field);
    alert.exec();
    const QString text = alert.fieldText();
    if (alert.answer() != 1 || text.isEmpty()) {
        return;
    }
    QUrl url(text);
    if (!url.isValid() || url.scheme().isEmpty()) {
        setStatus("Not a URL: " + text, STATUS_CLEAR_MS);
        return;
    }
    placeModel(url, point);
}

glm::vec3 Editor::placementPoint(const QPoint* viewportPos) const {
    // where a new entity goes: under the given viewport point (a drop), else the middle of the
    // view; on the surface there, or on the grid plane, or a few metres out
    auto viewport = qApp->getPrimaryWidget();
    QPoint local = viewportPos ? *viewportPos : (viewport ? viewport->rect().center() : QPoint());
    PickRay ray = pickRay(local);
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    auto result = entities->evalRayIntersectionVector(ray, worldPickFilter(), QVector<EntityItemID>(), QVector<EntityItemID>());
    if (result.intersects) {
        return result.intersection;
    }
    if (std::abs(ray.direction.y) > 1.0e-4f) {
        float t = (_grid.height() - ray.origin.y) / ray.direction.y;
        if (t > 0.0f && t < MAX_PLACEMENT_DISTANCE) {
            return ray.origin + ray.direction * t;
        }
    }
    return ray.origin + ray.direction * PASTE_FALLBACK_DISTANCE;
}

float Editor::cameraYaw() const {
    glm::vec3 forward = _camera.orientation() * Vectors::FRONT;
    return std::atan2(-forward.x, -forward.z);
}

bool Editor::createPreset(const QString& presetUrl, const QString& label, const glm::vec3& point, float ahead) {
    if (!editable()) {
        qCWarning(interfaceapp) << "editor: no rez rights here, not creating" << label;
        return false;
    }
    QVector<EntityItemProperties> properties;
    QList<QUuid> ids;
    AABox bounds;
    int count = 0;
    if (!readEntityFile(presetUrl, properties, ids, bounds, count)) {
        qCWarning(interfaceapp) << "editor: could not read the preset" << presetUrl << count;
        setStatus("Could not make the " + label, STATUS_CLEAR_MS);
        return false;
    }
    // the file's ground is its y 0, and its front -z (as the Track's rigs face): turned to face the way the camera
    // looks, about the footprint's middle, so you see it from behind, ready to get in
    glm::vec3 center = bounds.calcCenter();
    glm::vec3 anchor(center.x, 0.0f, center.z);
    glm::quat turn = glm::angleAxis(cameraYaw(), Vectors::UP);
    QSet<QUuid> inFile(ids.begin(), ids.end());
    for (auto& entry : properties) {
        if (entry.getParentID().isNull() || !inFile.contains(entry.getParentID())) {
            glm::vec3 position = anchor + turn * (entry.getPosition() - anchor);
            glm::quat rotation = turn * entry.getRotation();
            entry.setPosition(position);
            entry.setLocalPosition(position);
            entry.setRotation(rotation);
            entry.setLocalRotation(rotation);
            entry.setLocalRotationChanged(false);
        }
    }
    // a big one far enough out that it doesn't swallow the camera: along the view, on the level of the point
    glm::vec3 target = point;
    if (ahead > 0.0f) {
        glm::vec3 flat = target - _camera.position();
        flat.y = 0.0f;
        glm::vec3 forward = viewDirection();
        forward.y = 0.0f;
        if (glm::length(forward) > EPSILON && glm::length(flat) < ahead) {
            glm::vec3 out = _camera.position() + glm::normalize(forward) * ahead;
            target = glm::vec3(out.x, point.y, out.z);
        }
    }
    // on the surface under it: the view's middle can miss every surface (a level view hits nothing), and a vehicle
    // made in the air drops onto whatever is below
    const float GROUND_SEARCH_ABOVE = 0.5f;
    PickRay down(target + Vectors::UP * GROUND_SEARCH_ABOVE, -Vectors::UP);
    auto ground = DependencyManager::get<EntityScriptingInterface>()->evalRayIntersectionVector(down, worldPickFilter(),
        QVector<EntityItemID>(), QVector<EntityItemID>());
    if (ground.intersects && ground.distance < MAX_PLACEMENT_DISTANCE) {
        target = ground.intersection;
    }
    if (snapActive()) {
        // on the grid across, staying on the ground
        glm::vec3 snapped = snapVector(target, _moveSnap);
        target.x = snapped.x;
        target.z = snapped.z;
    }
    EditorClipboard family;
    family.set(properties, ids, anchor);
    const QString text = "Create " + label;
    auto stack = _undo.stack();
    stack->beginMacro(text);   // every entity of it and its selection are one step
    QList<QUuid> created = family.paste(_undo, text, target - anchor);
    if (!created.isEmpty()) {
        _selection.set(created);
    }
    stack->endMacro();
    if (created.isEmpty()) {
        setStatus("Could not make the " + label, STATUS_CLEAR_MS);
        return false;
    }
    setTool(EditorToolBar::Tool::Move);
    return true;
}

QList<QUuid> Editor::place(EntityItemProperties properties, const QString& text, const glm::vec3& point) {
    if (!editable()) {
        // the domain says so up front; an instance world is read-only until World Edit is on
        qCWarning(interfaceapp) << "editor: no rez rights here, not creating" << text;
        return QList<QUuid>();
    }
    if (properties.getParentID().isNull()) {
        glm::vec3 position = point + Vectors::UP * EditorCreate::lift(properties);
        if (snapActive()) {
            position = snapVector(position, _moveSnap);
        }
        properties.setPosition(position);
    }
    auto stack = _undo.stack();
    stack->beginMacro(text);   // the new entity and its selection are one step
    QList<QUuid> ids = _undo.pushAdd(text, QVector<EntityItemProperties> { properties }, QList<QUuid>());
    QList<QUuid> created;
    for (const auto& id : ids) {
        if (!id.isNull()) {
            created << id;
        }
    }
    if (!created.isEmpty()) {
        _selection.set(created);
    }
    stack->endMacro();
    if (!created.isEmpty()) {
        setTool(EditorToolBar::Tool::Move);
        // the entity server can still refuse the add (a filter, or rights the domain did not
        // report): the entity then vanishes a moment later, and that deserves a word. An undo in
        // the meantime is not a refusal.
        int pushedAt = _undo.stack()->index();
        QTimer::singleShot(REJECTION_CHECK_MS, this, [this, created, text, pushedAt] {
            EntityItemProperties check;
            if (_undo.stack()->index() >= pushedAt && !EditorUndo::snapshot(_undo.resolve(created.first()), check)) {
                setStatus("The world refused \"" + text + "\" (no building rights here?)", STATUS_CLEAR_MS);
                qCWarning(interfaceapp) << "editor: the server removed the new entity" << text << created.first();
            }
        });
    }
    return created;
}

void Editor::placeModel(const QUrl& url, const glm::vec3& point) {
    for (const auto& id : place(EditorCreate::modelProperties(url, point), "Create Model", point)) {
        AwaitedModel awaited;
        awaited.createdAt = usecTimestampNow();
        _modelsAwaitingDimensions.insert(id, awaited);
    }
    if (!_modelsAwaitingDimensions.isEmpty() && !_modelDimensionsTimer.isActive()) {
        _modelDimensionsTimer.start();
    }
}

void Editor::placeImage(const QUrl& url, const glm::vec3& point) {
    place(EditorCreate::imageProperties(url, point, cameraYaw()), "Create Image", point);
}

void Editor::pollModelDimensions() {
    // a new model is a 1 m box until it loads; then it takes its own size, its bottom staying put
    auto entities = DependencyManager::get<EntityScriptingInterface>();
    quint64 now = usecTimestampNow();
    auto isNear = [](const glm::vec3& a, const glm::vec3& b) {   // not "near": a macro on Windows
        return !glm::any(glm::greaterThan(glm::abs(a - b), glm::vec3(MIN_DIMENSION)));
    };
    for (auto it = _modelsAwaitingDimensions.begin(); it != _modelsAwaitingDimensions.end();) {
        AwaitedModel& awaited = it.value();
        QUuid id = _undo.resolve(it.key());
        EntityItemProperties current = entities->getEntityProperties(id);
        glm::vec3 size = current.getDimensions();
        glm::vec3 natural = glm::max(current.getNaturalDimensions(), glm::vec3(MIN_DIMENSION));
        bool loaded = glm::any(glm::greaterThan(current.getNaturalDimensions(), glm::vec3(MIN_DIMENSION)));
        bool isBox = isNear(size, EditorCreate::MODEL_PLACEHOLDER_DIMENSIONS);
        bool done = false;
        if (current.getType() != EntityTypes::Model) {
            done = true;   // gone: deleted, or its creation undone
        } else if (!isBox && !(awaited.sizedAt > 0 && isNear(size, natural))) {
            done = true;   // resized by hand meanwhile: leave it be
        } else if (awaited.sizedAt > 0 && !isBox) {
            done = now - awaited.sizedAt > MODEL_SIZE_WATCH_USECS;   // kept its size long enough
        } else if (loaded && now - awaited.createdAt > MODEL_SIZE_DELAY_USECS) {
            if (awaited.sizings >= MODEL_SIZE_ATTEMPTS) {
                qCWarning(interfaceapp) << "editor: a new model keeps losing its size" << id;
                done = true;
            } else {
                EntityItemProperties edit;
                edit.setDimensions(natural);
                glm::vec3 position = current.getPosition();
                position.y += (natural.y - size.y) * 0.5f;
                edit.setPosition(position);
                entities->editEntity(id, edit);
                _undo.amendCreated(it.key(), edit);   // so that a redo makes it this size, not the box
                awaited.sizedAt = now;
                ++awaited.sizings;
            }
        }
        if (done || ++awaited.polls > MODEL_DIMENSIONS_POLL_LIMIT) {
            it = _modelsAwaitingDimensions.erase(it);
        } else {
            ++it;
        }
    }
    if (_modelsAwaitingDimensions.isEmpty()) {
        _modelDimensionsTimer.stop();
    }
}

namespace {
    QString unescapeHtml(QString text) {
        return text.replace("&amp;", "&").replace("&quot;", "\"").replace("&#39;", "'").replace("&#x2F;", "/");
    }

    // the address in a srcset with the largest width or density ("a.jpg 320w, b.jpg 1200w", "a.jpg 1x, b.jpg 2x")
    QString largestInSrcset(const QString& srcset) {
        static const QRegularExpression SEPARATOR(",\\s+");
        static const QRegularExpression SPACES("\\s+");
        QString best;
        double bestSize = -1.0;
        for (const QString& candidate : srcset.split(SEPARATOR, Qt::SkipEmptyParts)) {
            QStringList parts = candidate.trimmed().split(SPACES, Qt::SkipEmptyParts);
            if (parts.isEmpty()) {
                continue;
            }
            double size = 1.0;
            if (parts.size() > 1) {
                bool ok = false;
                double value = parts[1].left(parts[1].size() - 1).toDouble(&ok);   // "1200w" -> 1200, "2x" -> 2
                if (ok) {
                    size = value;
                }
            }
            if (size > bestSize) {
                bestSize = size;
                best = parts[0];
            }
        }
        return best;
    }

    // the addresses of the pictures in dragged HTML, the first picture first. News sites load pictures lazily:
    // src is then a placeholder (a data: address or a tiny GIF) and the picture is in srcset or a data- attribute.
    QList<QUrl> pictureAddresses(const QString& html, const QUrl& base) {
        static const QRegularExpression IMG_TAG("<img\\b[^>]*>", QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression ATTRIBUTE("\\s([a-zA-Z_:-]+)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')");
        static const QStringList PREFERRED = { "srcset", "data-srcset", "data-src", "data-lazy-src", "data-original", "src" };
        QList<QUrl> addresses;
        auto tags = IMG_TAG.globalMatch(html);
        while (tags.hasNext()) {
            QString tag = tags.next().captured(0);
            QHash<QString, QString> attributes;
            auto found = ATTRIBUTE.globalMatch(tag);
            while (found.hasNext()) {
                auto match = found.next();
                attributes.insert(match.captured(1).toLower(), match.captured(2).isNull() ? match.captured(3) : match.captured(2));
            }
            for (const auto& name : PREFERRED) {
                QString value = unescapeHtml(attributes.value(name).trimmed());
                if (name.endsWith("srcset")) {
                    value = largestInSrcset(value);
                }
                if (value.isEmpty() || value.startsWith("data:", Qt::CaseInsensitive)) {
                    continue;
                }
                if (value.startsWith("//")) {
                    value.prepend("https:");   // the page's own scheme, which these days is https
                }
                QUrl url = base.isValid() ? base.resolved(QUrl(value)) : QUrl(value);
                if (url.scheme() == "http" || url.scheme() == "https") {
                    addresses << url;
                    break;
                }
            }
        }
        return addresses;
    }

    // an address fit for the log: no query (it can carry a key), no fragment, no user
    QString bareAddress(const QUrl& url) {
        return url.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo).toString();
    }
}

bool Editor::handleDrop(QDropEvent* event) {
    if (!_active) {
        return false;
    }
    auto mime = event->mimeData();
    QList<QUrl> remote;
    QList<QUrl> local;
    QList<bool> remoteIsImage;
    QList<bool> localIsImage;
    auto consider = [&](const QUrl& url, bool knownImage) {
        if (remote.contains(url) || local.contains(url)) {
            return;   // a browser offers the same picture as a link and in its HTML
        }
        QString name = url.fileName();
        bool image = knownImage || EditorCreate::isImageFile(name);
        bool model = !image && EditorCreate::isModelFile(name);
        if (!image && !model) {
            return;
        }
        if (url.isLocalFile()) {
            local << url;
            localIsImage << image;
        } else if (url.scheme() == "http" || url.scheme() == "https") {
            remote << url;
            remoteIsImage << image;
        }
    };
    for (const auto& url : mime->urls()) {
        consider(url, false);
    }
    // a browser drags a picture as the link around it (often an article) plus a copy of the file; the
    // picture's own address is in the dragged HTML, and needs no upload
    if (remote.isEmpty() && mime->hasHtml()) {
        QUrl base;   // what a relative address is relative to: the page, as near as the drag tells
        for (const auto& url : mime->urls()) {
            if (url.scheme() == "http" || url.scheme() == "https") {
                base = url;
                break;
            }
        }
        for (const auto& url : pictureAddresses(mime->html(), base)) {
            consider(url, true);
            if (!remote.isEmpty()) {
                break;   // the first picture is the one dragged
            }
        }
    }
    if (remote.isEmpty() && local.isEmpty() && mime->hasText()) {
        consider(QUrl(mime->text().trimmed()), false);
    }
    // no address at all, but the browser sent the picture itself: upload that copy
    if (remote.isEmpty() && local.isEmpty() && mime->hasImage()) {
        QImage image = qvariant_cast<QImage>(mime->imageData());
        QString path = QDir(QDir::tempPath()).filePath(
            QString("picture-%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")));
        if (!image.isNull() && image.save(path, "PNG")) {
            local << QUrl::fromLocalFile(path);
            localIsImage << true;
        }
    }
    QStringList offered;
    for (const auto& url : mime->urls()) {
        offered << bareAddress(url);
    }
    QStringList placing;
    for (const auto& url : remote + local) {
        placing << bareAddress(url);
    }
    qCDebug(interfaceapp) << "editor drop:" << mime->formats() << "links" << offered << "placing" << placing;
    if (remote.isEmpty() && local.isEmpty()) {
        if (mime->hasHtml() || mime->hasImage()) {
            setStatus("Nothing to place: no picture or model found in what was dropped", STATUS_CLEAR_MS);
        }
        return false;   // a script or a place, perhaps: the client's own handling
    }
    if (!editable()) {
        return true;   // ours to refuse: said so on the toolbar
    }
    QPoint pos = event->pos();
    glm::vec3 point = placementPoint(&pos);
    if (!remote.isEmpty()) {
        // the web address wins over a local copy of the same thing
        for (int i = 0; i < remote.size(); ++i) {
            if (remoteIsImage[i]) {
                placeImage(remote[i], point);
            } else {
                placeModel(remote[i], point);
            }
        }
        return true;
    }
    for (int i = 0; i < local.size(); ++i) {
        beginUpload(local[i].toLocalFile(), point, localIsImage[i]);
    }
    return true;
}

void Editor::beginUpload(const QString& localPath, const glm::vec3& point, bool image) {
    auto upload = new EditorUpload(this);
    QString error;
    if (!upload->start(localPath, image ? "/images" : "/models", error)) {
        setStatus("Upload failed: " + error, STATUS_CLEAR_MS * 2);
        qCWarning(interfaceapp) << "editor upload could not start" << localPath << error;
        upload->deleteLater();
        return;
    }
    showUploadProgress(upload);
    connect(upload, &EditorUpload::finished, this, [this, upload, point, image](const QString& url) {
        --_uploadsActive;
        if (!_active) {
            setStatus("Uploaded " + upload->fileName(), STATUS_CLEAR_MS);   // build mode ended meanwhile
        } else {
            setStatus(_uploadsActive > 0 ? QString("Uploading…") : QString());
            if (image) {
                placeImage(QUrl(url), point);
            } else {
                placeModel(QUrl(url), point);
            }
        }
        upload->deleteLater();
    });
}

void Editor::showUploadProgress(EditorUpload* upload) {
    ++_uploadsActive;
    setStatus(EditorUpload::uploadingStatus(upload->fileName(), 0, QString(), true));
    connect(upload, &EditorUpload::progress, this, [this, upload](qint64 sent, qint64 total) {
        if (total <= 0) {
            return;
        }
        if (sent >= total) {
            setStatus(EditorUpload::uploadingStatus(upload->fileName(), 100, QString(), true));
        } else {
            setStatus(EditorUpload::uploadingStatus(upload->fileName(), (int)(sent * 100 / total), QString(), true));
        }
    });
    connect(upload, &EditorUpload::failed, this, [this, upload](const QString& error) {
        --_uploadsActive;
        setStatus("Upload of " + upload->fileName() + " failed: " + error, STATUS_CLEAR_MS * 2);
        upload->deleteLater();
    });
}

void Editor::onInspectorUpload(const QString& key, const QString& localPath) {
    if (!editable()) {
        return;
    }
    QList<QUuid> targets = _selection.ids();   // the entities the picture was chosen for, whatever is selected later
    auto upload = new EditorUpload(this);
    QString error;
    if (!upload->start(localPath, "/images", error)) {
        setStatus("Upload failed: " + error, STATUS_CLEAR_MS * 2);
        qCWarning(interfaceapp) << "editor upload could not start" << localPath << error;
        upload->deleteLater();
        return;
    }
    showUploadProgress(upload);
    connect(upload, &EditorUpload::finished, this, [this, upload, key, targets](const QString& url) {
        --_uploadsActive;
        if (!_active) {
            setStatus("Uploaded " + upload->fileName(), STATUS_CLEAR_MS);   // build mode ended meanwhile
        } else {
            setStatus(_uploadsActive > 0 ? QString("Uploading…") : QString());
            commitInspectorEdit(key, QVariant(url), targets);
        }
        upload->deleteLater();
    });
}

void Editor::addSelectionActions(QMenu& menu, const glm::vec3* pasteTarget) {
    bool any = !_selection.isEmpty();
    auto add = [&](const QString& text, const QString& keys, std::function<void()> run, bool enabled) {
        QAction* action = menu.addAction(text, this, run);
        if (!keys.isEmpty()) {
            action->setShortcut(QKeySequence(keys));   // shown as a hint; the menu is gone after
            action->setShortcutVisibleInContextMenu(true);
        }
        action->setEnabled(enabled);
    };
    add("Frame Selected", "F", [this] { frameSelection(); }, any);
    add("Go to Selected", "Shift+F", [this] { goToSelection(); }, any);
    add("Bring Selected Here", "Alt+Shift+F", [this] { bringSelectionHere(); }, any && !_readOnly);
    menu.addSeparator();
    add("Cut", "Ctrl+X", [this] { cutSelection(); }, any && !_readOnly);
    add("Copy", "Ctrl+C", [this] { copySelection(); }, any);
    glm::vec3 target = pasteTarget ? *pasteTarget : glm::vec3(0.0f);
    bool atTarget = pasteTarget != nullptr;
    add("Paste", "Ctrl+V", [this, target, atTarget] {
        if (atTarget) {
            pasteAt(target);
        } else {
            paste();
        }
    }, !_clipboard.isEmpty() && !_readOnly);
    add("Duplicate", "Ctrl+D", [this] { duplicateSelection(); }, any && !_readOnly);
    add("Delete", "Backspace", [this] { deleteSelection(); }, any && !_readOnly);
}

void Editor::showHierarchyMenu(const QPoint& globalPos) {
    // a row that is not loaded: there is nothing to edit yet
    if (_hierarchy && _treeModel) {
        auto tree = _hierarchy->treeView();
        QUuid id = tree->indexAt(tree->viewport()->mapFromGlobal(globalPos)).data(EntityTreeModel::IdRole).toUuid();
        if (!id.isNull() && !_treeModel->isLoaded(id)) {
            QMenu menu;
            menu.addAction("Go There", this, [this, id] { goToListed(id); });
            menu.addAction("Copy ID", this, [id] {
                QGuiApplication::clipboard()->setText(id.toString(QUuid::WithoutBraces));
            });
            menu.exec(globalPos);
            return;
        }
    }
    QMenu menu;
    QMenu* create = buildCreateMenu(&menu);
    create->setEnabled(!_readOnly);
    menu.addMenu(create);
    menu.addSeparator();
    addSelectionActions(menu, nullptr);
    menu.addSeparator();
    menu.addAction("Rename", this, [this] {
        if (_hierarchy) {
            auto tree = _hierarchy->treeView();
            tree->edit(tree->currentIndex());
        }
    })->setEnabled(!_selection.isEmpty() && !_readOnly);
    addScriptActions(menu);
    menu.exec(globalPos);
}

// Edit Script and Edit Server Script: one selected entity's script in the Code panel (Make v1; Qt 6 only)
void Editor::addScriptActions(QMenu& menu) {
    if (!CodeEditor::isAvailable()) {
        return;
    }
    const QList<QUuid> ids = _selection.ids();
    const bool one = ids.size() == 1;
    menu.addSeparator();
    menu.addAction("Edit Script", this, [ids] {
        DependencyManager::get<CodeEditor>()->open(ids.first(), "script");
    })->setEnabled(one);
    menu.addAction("Edit Server Script", this, [ids] {
        DependencyManager::get<CodeEditor>()->open(ids.first(), "serverScripts");
    })->setEnabled(one && !_readOnly);
}

void Editor::showViewportMenu(const QPoint& viewportPos, const QPoint& globalPos) {
    if (!_active) {
        return;
    }
    // a right click on something outside the selection selects it first, as in the Hierarchy
    QUuid hit = pickAt(viewportPos);
    if (!hit.isNull() && !_selection.ids().contains(hit)) {
        _selection.set({ hit });
    }
    // new and pasted entities go where the click was
    glm::vec3 point = placementPoint(&viewportPos);
    QMenu menu;
    QMenu* create = buildCreateMenu(&menu, &viewportPos);
    create->setEnabled(!_readOnly);
    menu.addMenu(create);
    menu.addSeparator();
    addSelectionActions(menu, &point);
    addScriptActions(menu);
    menu.exec(globalPos);
}

void Editor::setStatus(const QString& text, int clearAfterMs) {
    if (_statusLabel) {
        _statusLabel->setText(text);
        _statusLabel->setToolTip(text);   // the whole message, should the bar have had to cut it
    }
    // in normal play there's no status bar: My Files shows the note at its foot (an upload's progress, a link copied)
    if (_assets) {
        _assets->setNote(_active ? QString() : text);
    }
    if (clearAfterMs > 0 && !text.isEmpty()) {
        _statusTimer.start(clearAfterMs);
    } else {
        _statusTimer.stop();
    }
}

void Editor::updateWorldControls() {
    if (_toolBar) {
        _toolBar->setWorld(_world->isInstance(), _world->canEdit(), _world->isEditing(), _world->isBusy());
    }
}

// --- input --------------------------------------------------------------------------------------

bool Editor::handleMousePress(QMouseEvent* event) {
    if (!_active) {
        return false;
    }
    // a press in the viewport takes the keyboard back from the panels (a right click would not, on macOS)
    focusViewport();
    if (event->button() == Qt::RightButton) {
        _rightPressPos = event->pos();   // where the context menu opens, if this is a click
    }
    if (_camera.mousePress(event)) {
        return true;
    }
    if (event->button() != Qt::LeftButton) {
        return true;
    }
    PickRay ray = pickRay(event->pos());
    _pressPos = event->pos();
    _pressModifiers = event->modifiers();
    if (_gizmo.isVisible()) {
        GizmoHandle handle = _gizmo.hitTest(ray);
        if (handle != GizmoHandle::None) {
            beginGizmoDrag(handle, ray);
            return true;
        }
    }
    EditorLightHandle lightHandle = _lightGizmos.handleAt(ray);
    if (!lightHandle.isNone()) {
        beginLightHandleDrag(lightHandle, ray);
        return true;
    }
    // a Track's control point: a click chooses it (the Move gizmo then stands on it); Shift+click on the curve
    // between the points puts a new one there
    int trackPoint = _trackGizmos.pointAt(ray);
    if (trackPoint >= 0) {
        chooseTrackPoint(trackPoint, false);
        return true;
    }
    if (event->modifiers().testFlag(Qt::ShiftModifier) && _tool != EditorToolBar::Tool::Terrain) {
        float arcLength;
        glm::vec3 onCurve;
        if (_trackGizmos.curveAt(ray, arcLength, onCurve)) {
            insertTrackPoint(arcLength);
            return true;
        }
    }
    // the Terrain tool: a press on the ground begins a stroke; elsewhere it is a click like any other
    if (_tool == EditorToolBar::Tool::Terrain && _terrain.pointsAtTerrain(ray)) {
        if (!editable()) {
            return true;
        }
        QString refusal;
        if (!_terrain.beginStroke(ray, event->modifiers().testFlag(Qt::ShiftModifier), refusal) && !refusal.isEmpty()) {
            setStatus(refusal, STATUS_CLEAR_MS);
        }
        return true;
    }
    _clickPending = true;
    return true;
}

bool Editor::handleMouseMove(QMouseEvent* event) {
    if (!_active) {
        return false;
    }
    if (_camera.mouseMove(event)) {
        return true;
    }
    if (_gizmo.isDragging()) {
        moveGizmoDrag(event->pos());
        return true;
    }
    if (!_lightHandleDrag.isNone()) {
        moveLightHandleDrag(event->pos());
        return true;
    }
    if (_terrain.isStroking()) {
        _terrain.moveStroke(pickRay(event->pos()));
        return true;
    }
    if (_marqueeActive) {
        updateMarquee(event->pos());
        return true;
    }
    if (_clickPending) {
        if ((event->pos() - _pressPos).manhattanLength() > CLICK_SLOP_PIXELS) {
            // a drag on empty space is a selection rectangle
            _clickPending = false;
            _marqueeActive = true;
            updateMarquee(event->pos());
        }
        return true;
    }
    if (event->buttons() == Qt::NoButton) {
        // the keys follow the mouse into the viewport, as in Unity, unless something is being typed
        takeFocusUnlessTyping();
        PickRay ray = pickRay(event->pos());
        GizmoHandle gizmoHover = _gizmo.isVisible() ? _gizmo.hitTest(ray) : GizmoHandle::None;
        if (_gizmo.isVisible()) {
            _gizmo.setHover(gizmoHover);
        }
        // the transform gizmo is picked first: only one of them lights up
        EditorLightHandle lightHover = gizmoHover == GizmoHandle::None ? _lightGizmos.handleAt(ray) : EditorLightHandle();
        _lightGizmos.setHover(lightHover);
        _trackGizmos.setHover(gizmoHover == GizmoHandle::None && lightHover.isNone() ? _trackGizmos.pointAt(ray) : -1);
        if (_tool == EditorToolBar::Tool::Terrain) {
            _terrain.hover(ray);
        }
    }
    return false;
}

bool Editor::handleMouseRelease(QMouseEvent* event) {
    if (!_active) {
        return false;
    }
    if (_camera.mouseRelease(event)) {
        // a right click without looking around opens the context menu, after this event is done
        if (event->button() == Qt::RightButton && _camera.wasRightClick()) {
            QPoint pos = _rightPressPos;
            auto viewport = qApp->getPrimaryWidget();
            QPoint global = viewport ? viewport->mapToGlobal(pos) : event->globalPos();
            QTimer::singleShot(0, this, [this, pos, global] { showViewportMenu(pos, global); });
        }
        return true;
    }
    if (event->button() != Qt::LeftButton) {
        return true;
    }
    if (_gizmo.isDragging()) {
        applyDrag(true);
        _gizmo.endDrag();
        _dragKind = DragKind::None;
        _trackGizmos.setActive(-1);
        updateGizmo();
        return true;
    }
    if (!_lightHandleDrag.isNone()) {
        endLightHandleDrag(false);
        return true;
    }
    if (_terrain.isStroking()) {
        _terrain.endStroke(false);
        return true;
    }
    if (_marqueeActive) {
        updateMarquee(event->pos());
        finishMarquee(_pressModifiers);
        return true;
    }
    if (_clickPending) {
        _clickPending = false;
        clickSelect(event->pos(), _pressModifiers);
    }
    return true;
}

bool Editor::handleMouseDoubleClick(QMouseEvent* event) {
    if (!_active) {
        return false;
    }
    // The second click of a double-click is a click like any other in the scene: it selects what it hits, or lays
    // another terrain stroke. It used to frame the entity hit, which flung the camera away from the ground being
    // edited; framing is on F, and on a double-click in the Hierarchy.
    return handleMousePress(event);
}

bool Editor::handleWheel(QWheelEvent* event) {
    if (!_active) {
        return false;
    }
    bool consumed = _camera.wheel(event);
    emit cameraChanged();
    return consumed;
}

bool Editor::qmlHasTextFocus() const {
    return surfaceHasTextFocus() || panelHasTextFocus();
}

bool Editor::handleKeyPress(QKeyEvent* event) {
    if (!_active || qmlHasTextFocus()) {
        return false;   // a QML or web field being typed in (chat, a dialog, a URL box) keeps every key, paste included
    }
    if (_camera.keyPress(event)) {
        return true;
    }
    bool command = commandModifier(event->modifiers());
    bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    switch (event->key()) {
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (_trackPointChosen >= 0) {
                deleteTrackPoint();   // the chosen control point, not the track
            } else {
                deleteSelection();
            }
            return true;
        case Qt::Key_Escape:
            if (_terrain.isStroking()) {
                _terrain.endStroke(true);   // the ground goes back as it was
                return true;
            }
            if (_trackPointChosen >= 0) {
                chooseTrackPoint(-1, false);   // the point first, the track on a second Escape
                return true;
            }
            _selection.clear();
            return true;
        case Qt::Key_F:
            if (shift && command) {
                bringSelectionHere();
            } else if (shift) {
                goToSelection();
            } else {
                frameSelection();
            }
            return true;
        case Qt::Key_Q:
            if (!command) { setTool(EditorToolBar::Tool::View); return true; }
            break;
        case Qt::Key_W:
            if (!command) { setTool(EditorToolBar::Tool::Move); return true; }
            break;
        case Qt::Key_E:
            if (!command) { setTool(EditorToolBar::Tool::Rotate); return true; }
            break;
        case Qt::Key_R:
            if (!command) { setTool(EditorToolBar::Tool::Scale); return true; }
            break;
        case Qt::Key_T:
            if (!command) { setTool(EditorToolBar::Tool::Terrain); return true; }
            break;
        case Qt::Key_BracketLeft:
        case Qt::Key_BracketRight:
            // the brush grows and shrinks by steps, as in Unity
            if (_tool == EditorToolBar::Tool::Terrain && _terrainBar) {
                _terrainBar->setRadius(_terrain.brush().radius * (event->key() == Qt::Key_BracketRight ? 1.25f : 0.8f));
                return true;
            }
            break;
        case Qt::Key_X:
            if (command) { cutSelection(); } else { setLocalSpace(!_localSpace); }
            return true;
        case Qt::Key_Z:
            if (command) {
                if (shift) { redo(); } else { undo(); }
            } else {
                setPivotMode(!_pivotMode);
            }
            return true;
        case Qt::Key_A:
            if (command) { selectAll(); return true; }
            break;
        case Qt::Key_D:
            if (command) { duplicateSelection(); return true; }
            break;
        case Qt::Key_C:
            if (command) { copySelection(); return true; }
            break;
        case Qt::Key_V:
            if (command) {
                paste();
            } else if (!event->isAutoRepeat()) {
                _vertexSnap = true;
            }
            return true;
        default:
            break;
    }
    return false;
}

bool Editor::handleKeyRelease(QKeyEvent* event) {
    if (!_active) {
        return false;
    }
    // not a panel's field: a key held for the camera has to let go, whichever field has the keyboard by then
    if (surfaceHasTextFocus() && !_vertexSnap) {
        return false;
    }
    if (event->key() == Qt::Key_V && !event->isAutoRepeat()) {
        _vertexSnap = false;
        return true;
    }
    return _camera.keyRelease(event);
}

bool Editor::eventFilter(QObject* watched, QEvent* event) {
    // In build mode the world takes a drop only over the viewport. Qt hands a drag that a widget turns down to its
    // parents, up to the main window, which gives every drop to the world: a model dragged onto a picture field (which
    // refused it) was placed in the world. Over the panels, a drag nothing in them takes is turned down here.
    const QEvent::Type type = event->type();
    if (_active && (type == QEvent::DragEnter || type == QEvent::DragMove || type == QEvent::Drop)
            && watched == qApp->getWindow()) {
        QWidget* window = qApp->getWindow();
        QWidget* viewport = qApp->getPrimaryWidget();
        auto drop = static_cast<QDropEvent*>(event);   // the enter and move events are drop events too
        const bool overViewport = viewport && viewport->isVisible() && viewport->window() == window
            && QRect(viewport->mapTo(window, QPoint(0, 0)), viewport->size()).contains(drop->pos());
        if (!overViewport) {
            drop->ignore();
            return true;
        }
        return false;
    }
    // the Build Mode shortcut (Cmd+B): when it was made and when it got here, for the toggle's log line. The first
    // sight of it only: Qt offers a shortcut to more than one receiver.
    if (event->type() == QEvent::ShortcutOverride) {
        auto keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_B && (keyEvent->modifiers() & Qt::ControlModifier) && !keyEvent->isAutoRepeat()) {
            int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (_toggleKeyArrivedMs < 0 || now - _toggleKeyArrivedMs > 100) {
                _toggleKeyMadeMs = (int64_t)keyEvent->timestamp();
                _toggleKeyArrivedMs = now;
            }
        }
    }
    // the camera keys (1 first person, 2 selfie, 3 look at) do nothing in build mode, which has its own camera:
    // a stray one (a number meant for a field) quietly changed the camera you came back to
    if (_active && event->type() == QEvent::ShortcutOverride) {
        auto keyEvent = static_cast<QKeyEvent*>(event);
        int key = keyEvent->key();
        bool plain = (keyEvent->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
        if (plain && (key == Qt::Key_1 || key == Qt::Key_2 || key == Qt::Key_3)) {
            event->accept();   // an ordinary key for whatever has the keyboard, not a menu shortcut
            return true;
        }
        return false;
    }
    if (!_active || event->type() != QEvent::KeyPress || !_panelsCreated) {
        return false;
    }
    if (QApplication::activeModalWidget()) {
        return false;   // a dialog (the colour picker) keeps its own keys
    }
    auto widget = qobject_cast<QWidget*>(watched);
    if (!widget) {
        return false;
    }
    auto keyEvent = static_cast<QKeyEvent*>(event);
    bool command = commandModifier(keyEvent->modifiers());
    bool shift = keyEvent->modifiers().testFlag(Qt::ShiftModifier);
    auto isTextEntry = [](QWidget* candidate) {
        return candidate && (qobject_cast<QLineEdit*>(candidate) || qobject_cast<QPlainTextEdit*>(candidate)
            || qobject_cast<QTextEdit*>(candidate) || qobject_cast<QAbstractSpinBox*>(candidate));
    };
    // the widget with the keyboard counts, not only the one this event has reached: a key a field ignores (Escape)
    // is passed up through the field's parents, which are not fields
    bool typing = isTextEntry(widget) || isTextEntry(QApplication::focusWidget());
    bool inTree = qobject_cast<QTreeView*>(widget) || (widget->parentWidget() && qobject_cast<QTreeView*>(widget->parentWidget()));
    bool inPanels = _hierarchy->isAncestorOf(widget) || _inspector->isAncestorOf(widget) || _toolBar->isAncestorOf(widget);

    // a QML text field (chat, a dialog) being typed in keeps its keys too
    auto qmlTyping = [this] { return qmlHasTextFocus(); };

    // undo and redo are taken here, ahead of everything, from any widget of the window: the menu
    // shortcut and the viewport's key handler were never reached on macOS. A text field keeps them only while it
    // holds typing not yet committed; one just clicked into, or committed with Enter, passes them on, so that an
    // undo straight after an edit undoes the edit rather than the typing
    auto fieldPassesUndo = [] {
        QWidget* focus = QApplication::focusWidget();
        if (auto line = qobject_cast<QLineEdit*>(focus)) {
            return !line->isModified();
        }
        if (auto spin = qobject_cast<ScrubSpinBox*>(focus)) {   // a number field is the keyboard's, not its text
            return !spin->isTyping();
        }
        return false;
    };
    if (command && keyEvent->key() == Qt::Key_Z && (!typing || fieldPassesUndo())) {
        if (!qmlTyping()) {
            if (shift) { redo(); } else { undo(); }
            return true;
        }
        return false;
    }

    // Enter in an Inspector field is the end of it: the field commits (its own handling, after this) and the
    // keyboard goes back to the world, so the field is not left highlighted, with the avatar's keys in it. Escape
    // drops typing not yet committed and does the same. The JSON box keeps both keys. In a list (User Data), a
    // name being typed in place is the list's own editor, which takes Enter and Escape itself, queued: the keyboard
    // follows once it has
    bool fieldKey = keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter || keyEvent->key() == Qt::Key_Escape;
    if (fieldKey && !keyEvent->isAutoRepeat() && widget == QApplication::focusWidget() && _inspector->isAncestorOf(widget)
            && (qobject_cast<QLineEdit*>(widget) || qobject_cast<QAbstractSpinBox*>(widget))) {
        bool inList = false;
        PropertyEditor* editor = nullptr;
        for (QWidget* parent = widget->parentWidget(); parent && !editor; parent = parent->parentWidget()) {
            inList = inList || qobject_cast<QAbstractItemView*>(parent);
            editor = qobject_cast<PropertyEditor*>(parent);
        }
        if (keyEvent->key() == Qt::Key_Escape && editor) {
            editor->revertTyping();
        }
        QTimer::singleShot(inList ? 1 : 0, this, [this] { focusViewport(); });
        return keyEvent->key() == Qt::Key_Escape && !inList;   // Enter goes on to the field, which commits on it
    }

    // Escape twice in quick succession leaves build mode, wherever the keyboard is but a text field, where
    // Escape belongs to the field; the first still does what it did. A press passed on from a widget to its
    // parent comes through here again, with the same time, so a press is known by its time.
    if (keyEvent->key() == Qt::Key_Escape && !keyEvent->isAutoRepeat() && !typing && !qmlTyping()) {
        ulong time = keyEvent->timestamp();
        if (time != _lastEscapeTime) {
            bool second = _lastEscapeTime != 0 && time - _lastEscapeTime < DOUBLE_ESCAPE_MS;
            _lastEscapeTime = second ? 0 : time;
            if (second) {
                QTimer::singleShot(0, this, [this] { setActive(false); });
                return true;
            }
        }
        return false;
    }

    // F with the pointer over the Hierarchy finds the selection there, rather than framing it in the viewport
    if (keyEvent->key() == Qt::Key_F && !command && !shift && !typing && pointerOverHierarchy()) {
        if (!_selection.isEmpty()) {
            _hierarchy->reveal(_selection.primary());
        }
        return true;
    }

    if (!inPanels) {
        return false;
    }
    // text fields keep their own keys while typing; the JSON box keeps everything
    if (qobject_cast<QPlainTextEdit*>(widget)) {
        return false;
    }
    switch (keyEvent->key()) {
        case Qt::Key_D:
            if (command && !typing) { duplicateSelection(); return true; }
            break;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (inTree) { deleteSelection(); return true; }
            break;
        case Qt::Key_F:
            if (inTree) {
                if (shift && command) { bringSelectionHere(); return true; }
                if (shift) { goToSelection(); return true; }
                if (!command) { frameSelection(); return true; }
            }
            break;
        default:
            break;
    }
    return false;
}

void Editor::bakeImpostor() {
    QList<QUuid> ids = _selection.topLevel();
    if (ids.isEmpty()) {
        qCDebug(interfaceapp) << "Bake Impostor: nothing selected";
        setStatus("Bake Impostor: nothing selected", STATUS_CLEAR_MS);
        return;
    }
    auto baker = DependencyManager::get<ImpostorBaker>();
    for (const QUuid& id : ids) {
        baker->bake(id, 256);
    }
    setStatus(ids.size() == 1 ? QStringLiteral("Baking the impostor…") : QString("Baking %1 impostors…").arg(ids.size()));
}

void Editor::bakeAllImpostors() {
    auto baker = DependencyManager::get<ImpostorBaker>();
    if (!_impostorBakerConnected) {
        _impostorBakerConnected = true;
        connect(baker.data(), &ImpostorBaker::baked, this, [this](const QUuid& id, bool ok, const QString& message) {
            if (_impostorBakesQueued == 0) {
                setStatus(ok ? QStringLiteral("Impostor baked") : QString("Impostor bake failed: %1").arg(message), STATUS_CLEAR_MS);
                return;
            }
            ++_impostorBakesDone;
            if (_impostorBakesDone >= _impostorBakesQueued) {
                setStatus(QString("Baked %1 impostors").arg(_impostorBakesDone), STATUS_CLEAR_MS);
                _impostorBakesQueued = 0;
                _impostorBakesDone = 0;
            } else {
                setStatus(QString("Baking impostors: %1 of %2…").arg(_impostorBakesDone).arg(_impostorBakesQueued));
            }
        });
    }
    const float MAX_RADIUS = 4.0f;   // a prop's size: a bigger group is opened and its children taken in turn
    const float MIN_RADIUS = 0.3f;   // a die needs no card
    int count = baker->bakeAll(256, MAX_RADIUS, MIN_RADIUS, false);
    if (count == 0) {
        setStatus("Bake All Impostors: nothing to bake (every prop between 0.3 and 4 m has a card, or no model has loaded)", STATUS_CLEAR_MS);
        return;
    }
    _impostorBakesQueued += count;
    setStatus(QString("Baking impostors: 0 of %1…").arg(_impostorBakesQueued));
}

void Editor::removeImpostor() {
    QList<QUuid> ids = _selection.topLevel();
    if (ids.isEmpty()) {
        setStatus("Remove Impostor: nothing selected", STATUS_CLEAR_MS);
        return;
    }
    auto baker = DependencyManager::get<ImpostorBaker>();
    for (const QUuid& id : ids) {
        baker->remove(id);
    }
    setStatus(ids.size() == 1 ? QStringLiteral("Impostor removed") : QString("Impostors removed from %1 entities").arg(ids.size()), STATUS_CLEAR_MS);
}
