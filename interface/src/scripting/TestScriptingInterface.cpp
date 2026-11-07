//
//  Created by Bradley Austin Davis on 2016/12/12
//  Copyright 2013-2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: these functions: frameTrace, dumpFrameTrace, takeFrameTrace (the scripts' calls into the frame
// trace, which the vehicle check's kits read the ride through). The rest of this file is not frozen.
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.
//
#include "TestScriptingInterface.h"

#include <memory>

#include <MeshPartPayload.h>

#include <SpringBones.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QLoggingCategory>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QVector3D>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QToolButton>

#include <shared/FileUtils.h>
#include <shared/QtHelpers.h>
#include <DependencyManager.h>
#include <MainWindow.h>
#include <OffscreenUi.h>
#include <QtQuick/QQuickItem>
#include <StatTracker.h>
#include <Trace.h>
#include <FrameTrace.h>
#include <ui/TabletScriptingInterface.h>

#include "Application.h"
#include "InterfaceLogging.h"
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtQuick/QQuickWindow>
#include "ui/QmlDock.h"
#include <plugins/DisplayPlugin.h>
#include <display-plugins/DisplayLink.h>
#include <display-plugins/OpenGLDisplayPlugin.h>
#include <gl/GLHelpers.h>

#include "avatar/AvatarImporter.h"
#include "chat/WorldChat.h"
#include "editor/Editor.h"
#include "ui/DockTitleBar.h"
#include "ui/QmlDock.h"
#include "ui/StandAloneJSConsole.h"
#include "ui/WindowLayout.h"
#include "ui/Popup.h"
#include "ui/Skins.h"
#include "graphics/GraphicsEngine.h"
#include "editor/ui/PropertyEditors.h"
#include "editor/ui/ScriptEditor.h"
#include "ui/CodeEditor.h"
#include <EntityTreeRenderer.h>
#include <RenderableEntityItem.h>

Q_LOGGING_CATEGORY(trace_test, "trace.test")

TestScriptingInterface* TestScriptingInterface::getInstance() {
    static TestScriptingInterface sharedInstance;
    return &sharedInstance;
}

void TestScriptingInterface::quit() {
    qApp->quit();
}

void TestScriptingInterface::waitForTextureIdle() {
    waitForCondition(0, []()->bool {
        return (0 == gpu::Context::getTexturePendingGPUTransferCount());
    });
}

void TestScriptingInterface::waitForDownloadIdle() {
    waitForCondition(0, []()->bool {
        return (0 == ResourceCache::getLoadingRequestCount()) && (0 == ResourceCache::getPendingRequestCount());
    });
}

void TestScriptingInterface::waitForProcessingIdle() {
    auto statTracker = DependencyManager::get<StatTracker>();
    waitForCondition(0, [statTracker]()->bool {
        return (0 == statTracker->getStat("Processing").toInt() && 0 == statTracker->getStat("PendingProcessing").toInt());
    });
}

void TestScriptingInterface::waitIdle() {
    // Initial wait for some incoming work
    QThread::sleep(1);
    waitForDownloadIdle();
    waitForProcessingIdle();
    waitForTextureIdle();
}

bool TestScriptingInterface::loadTestScene(QString scene) {
    if (QThread::currentThread() != thread()) {
        bool result;
        BLOCKING_INVOKE_METHOD(this, "loadTestScene", Q_RETURN_ARG(bool, result), Q_ARG(QString, scene));
        return result;
    }

    static const QString TEST_ROOT = "https://raw.githubusercontent.com/highfidelity/hifi_tests/master/";
    static const QString TEST_BINARY_ROOT = "https://hifi-public.s3.amazonaws.com/test_scene_data/";
    static const QString TEST_SCRIPTS_ROOT = TEST_ROOT + "scripts/";
    static const QString TEST_SCENES_ROOT = TEST_ROOT + "scenes/";
    
    DependencyManager::get<ResourceManager>()->setUrlPrefixOverride("atp:/", TEST_BINARY_ROOT + scene + ".atp/");
    auto tree = qApp->getEntities()->getTree();
    auto treeIsClient = tree->getIsClient();
    // Force the tree to accept the load regardless of permissions
    tree->setIsClient(false);
    auto result = tree->readFromURL(TEST_SCENES_ROOT + scene + ".json");
    tree->setIsClient(treeIsClient);
    return result;
}

bool TestScriptingInterface::startTracing(QString logrules) {
    if (!logrules.isEmpty()) {
        QLoggingCategory::setFilterRules(logrules);
    }

    if (!DependencyManager::isSet<tracing::Tracer>()) {
        return false;
    }

    DependencyManager::get<tracing::Tracer>()->startTracing();
    return true;
}

bool TestScriptingInterface::stopTracing(QString filename) {
    if (!DependencyManager::isSet<tracing::Tracer>()) {
        return false;
    }

    auto tracer = DependencyManager::get<tracing::Tracer>();
    tracer->stopTracing();
    tracer->serialize(filename);
    return true;
}

void TestScriptingInterface::clear() {
    qApp->postLambdaEvent([] {
        qApp->getEntities()->clear();
    });
}

bool TestScriptingInterface::waitForConnection(qint64 maxWaitMs) {
    // Wait for any previous connection to die
    QThread::sleep(1);
    return waitForCondition(maxWaitMs, []()->bool {
        return DependencyManager::get<NodeList>()->getDomainHandler().isConnected();
    });
}

void TestScriptingInterface::wait(int milliseconds) {
    QThread::msleep(milliseconds);
}

bool TestScriptingInterface::waitForCondition(qint64 maxWaitMs, std::function<bool()> condition) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition()) {
        if (maxWaitMs > 0 && elapsed.elapsed() > maxWaitMs) {
            return false;
        }
        QThread::msleep(1);
    }
    return condition();
}

void TestScriptingInterface::startTraceEvent(QString name) {
    tracing::traceEvent(trace_test(), name, tracing::DurationBegin, "");
}

void TestScriptingInterface::endTraceEvent(QString name) {
    tracing::traceEvent(trace_test(), name, tracing::DurationEnd);
}

void TestScriptingInterface::savePhysicsSimulationStats(QString originalPath) {
    QString path = FileUtils::replaceDateTimeTokens(originalPath);
    path = FileUtils::computeDocumentPath(path);
    if (!FileUtils::canCreateFile(path)) {
        return;
    }
    qApp->saveNextPhysicsStats(path);
}

void TestScriptingInterface::clearCaches() {
	qApp->reloadResourceCaches();
}

// Writes a JSON object from javascript to a file
void TestScriptingInterface::saveObject(QVariant variant, const QString& filename) {
    if (_testResultsLocation.isNull()) {
        return;
    }

    QJsonDocument jsonDocument;
    jsonDocument = QJsonDocument::fromVariant(variant);
    if (jsonDocument.isNull()) {
        return;
    }

    QByteArray jsonData = jsonDocument.toJson();

    // Append trailing slash if needed
    if (_testResultsLocation.right(1) != "/") {
        _testResultsLocation += "/";
    }

    QString filepath = QDir::cleanPath(_testResultsLocation + filename);
    QFile file(filepath);

    file.open(QFile::WriteOnly);
    file.write(jsonData);
    file.close();
}

void TestScriptingInterface::showMaximized() {
    qApp->getWindow()->showMaximized();
}

void TestScriptingInterface::setOtherAvatarsReplicaCount(int count) {
    qApp->setOtherAvatarsReplicaCount(count);
}

int TestScriptingInterface::getOtherAvatarsReplicaCount() {
    return qApp->getOtherAvatarsReplicaCount();
}

void TestScriptingInterface::setMinimumGPUTextureMemStabilityCount(int count) {
    QMetaObject::invokeMethod(qApp, "setMinimumGPUTextureMemStabilityCount", Qt::DirectConnection, Q_ARG(int, count));
}

void TestScriptingInterface::setSpringBoneLockstep(bool on) {
    springbones::Solver::setLockstep(on);
}

void TestScriptingInterface::advanceSpringBones(float seconds) {
    springbones::Solver::advance(seconds);
}

void TestScriptingInterface::advanceSpringBones(float seconds, const glm::vec3& avatarMove) {
    springbones::Solver::advance(seconds, avatarMove);
}

int TestScriptingInterface::getSpringBoneSteps() {
    return (int)springbones::Solver::getStepCount();
}

float TestScriptingInterface::getSpringBonePendingTime() {
    return springbones::Solver::getPendingTime();
}

int TestScriptingInterface::getSpringBoneConsumed() {
    return (int)springbones::Solver::getConsumedCount();
}

float TestScriptingInterface::getSpringBoneAlpha() {
    return springbones::Solver::getLastAlpha();
}

int TestScriptingInterface::getSpringBoneUpdates() {
    return (int)springbones::Solver::getUpdateCount();
}

float TestScriptingInterface::getGameLoopRate() {
    return qApp->getGameLoopRate();
}

float TestScriptingInterface::getRenderLoopRate() {
    return qApp->getRenderLoopRate();
}

bool TestScriptingInterface::isTextureLoadingComplete() {
    bool result;
    QMetaObject::invokeMethod(qApp, "gpuTextureMemSizeStable", Qt::DirectConnection, Q_RETURN_ARG(bool, result));
    return result;
}

void TestScriptingInterface::selectInEditor(const QStringList& ids) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "selectInEditor", Q_ARG(QStringList, ids));
        return;
    }
    QList<QUuid> uuids;
    for (const QString& id : ids) {
        uuids << QUuid(id);
    }
    DependencyManager::get<Editor>()->select(uuids);
}

void TestScriptingInterface::frameEditorSelection() {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "frameEditorSelection");
        return;
    }
    DependencyManager::get<Editor>()->frameSelection();
}

// the Inspector's row of that property, if it is a ScriptEditor
static ScriptEditor* inspectorScriptRow(const QString& key) {
    auto window = qApp->getWindow();
    if (!window) {
        return nullptr;
    }
    for (auto row : window->findChildren<ScriptEditor*>()) {
        if (row->spec().key == key) {
            return row;
        }
    }
    return nullptr;
}

QString TestScriptingInterface::editorScriptStatus(const QString& key) {
    if (QThread::currentThread() != thread()) {
        QString result;
        BLOCKING_INVOKE_METHOD(this, "editorScriptStatus", Q_RETURN_ARG(QString, result), Q_ARG(QString, key));
        return result;
    }
    auto row = inspectorScriptRow(key);
    return row ? row->statusText() : QString();
}

bool TestScriptingInterface::reloadEditorScript(const QString& key) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "reloadEditorScript", Q_RETURN_ARG(bool, result), Q_ARG(QString, key));
        return result;
    }
    auto row = inspectorScriptRow(key);
    return row && row->pressReload();
}

QVariantMap TestScriptingInterface::inspectorField(const QString& key) {
    if (QThread::currentThread() != thread()) {
        QVariantMap result;
        BLOCKING_INVOKE_METHOD(this, "inspectorField", Q_RETURN_ARG(QVariantMap, result), Q_ARG(QString, key));
        return result;
    }
    QVariantMap result;
    auto window = qApp->getWindow();
    QWidget* inspector = window ? window->findChild<QWidget*>("EditorInspector") : nullptr;
    if (!inspector) {
        return result;
    }
    auto note = inspector->findChild<QLabel*>("EditorInspectorLockNote");
    result["lockNote"] = note && note->isVisibleTo(inspector) ? note->text() : QString();
    for (auto field : inspector->findChildren<PropertyEditor*>()) {
        if (field->spec().key != key) {
            continue;
        }
        result["found"] = true;
        result["shown"] = field->isVisibleTo(inspector);
        result["enabled"] = field->isEnabled();
        result["inHeader"] = field->parentWidget() && field->parentWidget()->objectName() == "EditorInspectorHead";
        result["value"] = field->currentValue();
        result["mixed"] = field->isMixed();
        result["tip"] = field->toolTip();
        break;
    }
    return result;
}

QVariantMap TestScriptingInterface::inspectorToggle(const QString& name, bool click) {
    if (QThread::currentThread() != thread()) {
        QVariantMap result;
        BLOCKING_INVOKE_METHOD(this, "inspectorToggle", Q_RETURN_ARG(QVariantMap, result), Q_ARG(QString, name),
                               Q_ARG(bool, click));
        return result;
    }
    QVariantMap result;
    auto window = qApp->getWindow();
    QWidget* inspector = window ? window->findChild<QWidget*>("EditorInspector") : nullptr;
    auto icon = inspector ? inspector->findChild<QAbstractButton*>("EditorInspectorToggle" + name) : nullptr;
    if (!icon) {
        return result;
    }
    result["found"] = true;
    result["shown"] = icon->isVisibleTo(inspector);
    result["look"] = icon->property("look");
    result["usable"] = icon->property("usable");
    result["tip"] = icon->toolTip();
    if (click) {
        icon->click();
    }
    return result;
}

bool TestScriptingInterface::saveInspectorPicture(const QString& path) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "saveInspectorPicture", Q_RETURN_ARG(bool, result), Q_ARG(QString, path));
        return result;
    }
    QPixmap picture = DependencyManager::get<Editor>()->inspectorPicture();
    return !picture.isNull() && picture.save(path);
}

void TestScriptingInterface::setPresentLoad(int microseconds) {
    OpenGLDisplayPlugin::setPresentLoad(microseconds);
}

float TestScriptingInterface::getPresentPeriod() {
    return qApp->getPresentPeriod();
}

void TestScriptingInterface::holdPresents(int milliseconds) {
    OpenGLDisplayPlugin::holdPresents(milliseconds);
}

void TestScriptingInterface::chooseTrackPoint(int index) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "chooseTrackPoint", Q_ARG(int, index));
        return;
    }
    DependencyManager::get<Editor>()->chooseTrackPoint(index, false);
}

int TestScriptingInterface::chosenTrackPoint() {
    if (QThread::currentThread() != thread()) {
        int result = -1;
        BLOCKING_INVOKE_METHOD(this, "chosenTrackPoint", Q_RETURN_ARG(int, result));
        return result;
    }
    return DependencyManager::get<Editor>()->chosenTrackPoint();
}

void TestScriptingInterface::insertTrackPoint(float arcLength) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "insertTrackPoint", Q_ARG(float, arcLength));
        return;
    }
    DependencyManager::get<Editor>()->insertTrackPoint(arcLength);
}

void TestScriptingInterface::deleteTrackPoint() {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "deleteTrackPoint");
        return;
    }
    DependencyManager::get<Editor>()->deleteTrackPoint();
}

void TestScriptingInterface::editorUploadFiles(const QStringList& paths) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "editorUploadFiles", Q_ARG(const QStringList&, paths));
        return;
    }
    DependencyManager::get<Editor>()->uploadFilesForTest(paths);
}

QString TestScriptingInterface::editorStatus() {
    if (QThread::currentThread() != thread()) {
        QString result;
        BLOCKING_INVOKE_METHOD(this, "editorStatus", Q_RETURN_ARG(QString, result));
        return result;
    }
    return DependencyManager::get<Editor>()->statusForTest();
}

void TestScriptingInterface::importAvatar(const QString& path) {
    // queued, not blocking: the question it asks waits for its answer
    QMetaObject::invokeMethod(qApp, [path] { AvatarImporter::importFile(path); }, Qt::QueuedConnection);
}

void TestScriptingInterface::setBuildMode(bool on) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "setBuildMode", Q_ARG(bool, on));
        return;
    }
    DependencyManager::get<Editor>()->setActive(on);
}

QVariantMap TestScriptingInterface::getHierarchyCounts() {
    if (QThread::currentThread() != thread()) {
        QVariantMap result;
        BLOCKING_INVOKE_METHOD(this, "getHierarchyCounts", Q_RETURN_ARG(QVariantMap, result));
        return result;
    }
    return DependencyManager::get<Editor>()->hierarchyCounts();
}

QVariantMap TestScriptingInterface::terrainRandomAngle(bool on) {
    if (QThread::currentThread() != thread()) {
        QVariantMap result;
        BLOCKING_INVOKE_METHOD(this, "terrainRandomAngle", Q_RETURN_ARG(QVariantMap, result), Q_ARG(bool, on));
        return result;
    }
    return DependencyManager::get<Editor>()->setTerrainRandomAngleForTest(on);
}

float TestScriptingInterface::terrainStroke(const QUuid& id) {
    if (QThread::currentThread() != thread()) {
        float result = -1.0f;
        BLOCKING_INVOKE_METHOD(this, "terrainStroke", Q_RETURN_ARG(float, result), Q_ARG(QUuid, id));
        return result;
    }
    return DependencyManager::get<Editor>()->terrainStrokeForTest(id);
}

float TestScriptingInterface::terrainNextAngle() {
    if (QThread::currentThread() != thread()) {
        float result = 0.0f;
        BLOCKING_INVOKE_METHOD(this, "terrainNextAngle", Q_RETURN_ARG(float, result));
        return result;
    }
    return DependencyManager::get<Editor>()->terrainNextAngleForTest();
}

bool TestScriptingInterface::hierarchyToggle(const QUuid& id, const QString& which, bool onlyThis) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "hierarchyToggle", Q_RETURN_ARG(bool, result), Q_ARG(QUuid, id),
                               Q_ARG(QString, which), Q_ARG(bool, onlyThis));
        return result;
    }
    return DependencyManager::get<Editor>()->clickHierarchyToggle(which, id, onlyThis);
}

bool TestScriptingInterface::hierarchyStroke(const QString& which, const QVariantList& ids, bool onlyThis) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "hierarchyStroke", Q_RETURN_ARG(bool, result), Q_ARG(QString, which),
                               Q_ARG(QVariantList, ids), Q_ARG(bool, onlyThis));
        return result;
    }
    QList<QUuid> rows;
    for (const auto& id : ids) {
        rows << QUuid(id.toString());
    }
    return DependencyManager::get<Editor>()->strokeHierarchyToggle(which, rows, onlyThis);
}

QVariantMap TestScriptingInterface::hierarchyToggles(const QUuid& id) {
    if (QThread::currentThread() != thread()) {
        QVariantMap result;
        BLOCKING_INVOKE_METHOD(this, "hierarchyToggles", Q_RETURN_ARG(QVariantMap, result), Q_ARG(QUuid, id));
        return result;
    }
    return DependencyManager::get<Editor>()->hierarchyToggles(id);
}

void TestScriptingInterface::hierarchyShowAll() {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "hierarchyShowAll");
        return;
    }
    DependencyManager::get<Editor>()->showAllHere();
}

QUuid TestScriptingInterface::editorPickAt(const QUuid& id) {
    if (QThread::currentThread() != thread()) {
        QUuid result;
        BLOCKING_INVOKE_METHOD(this, "editorPickAt", Q_RETURN_ARG(QUuid, result), Q_ARG(QUuid, id));
        return result;
    }
    return DependencyManager::get<Editor>()->pickAtEntityForTest(id);
}

QVariantList TestScriptingInterface::editorMarqueeIds() {
    if (QThread::currentThread() != thread()) {
        QVariantList result;
        BLOCKING_INVOKE_METHOD(this, "editorMarqueeIds", Q_RETURN_ARG(QVariantList, result));
        return result;
    }
    QVariantList result;
    for (const auto& id : DependencyManager::get<Editor>()->marqueeAllForTest()) {
        result << id;
    }
    return result;
}

void TestScriptingInterface::clickHierarchyRow(const QUuid& id) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "clickHierarchyRow", Q_ARG(QUuid, id));
        return;
    }
    DependencyManager::get<Editor>()->clickHierarchyRow(id);
}

void TestScriptingInterface::commitInspectorEdit(const QString& key, const QVariant& value) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "commitInspectorEdit", Q_ARG(QString, key), Q_ARG(QVariant, value));
        return;
    }
    // a script's { x, y, z } is a Vec3 field's value (Scale, Size, Position)
    const QVariantMap map = value.toMap();
    if (map.size() == 3 && map.contains("x") && map.contains("y") && map.contains("z")) {
        DependencyManager::get<Editor>()->testInspectorEdit(key, QVector3D(map["x"].toFloat(), map["y"].toFloat(),
                                                                           map["z"].toFloat()));
        return;
    }
    DependencyManager::get<Editor>()->testInspectorEdit(key, value);
}

void TestScriptingInterface::editorUndo() {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "editorUndo");
        return;
    }
    DependencyManager::get<Editor>()->undo();
}

QString TestScriptingInterface::hierarchyRowColor(const QUuid& id) {
    if (QThread::currentThread() != thread()) {
        QString result;
        BLOCKING_INVOKE_METHOD(this, "hierarchyRowColor", Q_RETURN_ARG(QString, result), Q_ARG(QUuid, id));
        return result;
    }
    QColor color = DependencyManager::get<Editor>()->hierarchyRowColor(id);
    return color.isValid() ? color.name() : QString();
}

QString TestScriptingInterface::setSkin(const QString& id) {
    if (QThread::currentThread() != thread()) {
        QString result;
        BLOCKING_INVOKE_METHOD(this, "setSkin", Q_RETURN_ARG(QString, result), Q_ARG(QString, id));
        return result;
    }
    return DependencyManager::get<Skins>()->setCurrent(id);
}

bool TestScriptingInterface::editorImportEntities(const QString& url) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "editorImportEntities", Q_RETURN_ARG(bool, result), Q_ARG(QString, url));
        return result;
    }
    return DependencyManager::get<Editor>()->importEntities(url);
}

int TestScriptingInterface::editorExportSelection(const QString& path) {
    if (QThread::currentThread() != thread()) {
        int result = -1;
        BLOCKING_INVOKE_METHOD(this, "editorExportSelection", Q_RETURN_ARG(int, result), Q_ARG(QString, path));
        return result;
    }
    return DependencyManager::get<Editor>()->exportSelectionTo(path);
}

QString TestScriptingInterface::editorLoadProblem(const QUuid& id) {
    if (QThread::currentThread() != thread()) {
        QString result;
        BLOCKING_INVOKE_METHOD(this, "editorLoadProblem", Q_RETURN_ARG(QString, result), Q_ARG(QUuid, id));
        return result;
    }
    return DependencyManager::get<Editor>()->loadProblem(id);
}

void TestScriptingInterface::showSignInScreen() {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "showSignInScreen");
        return;
    }
    // as the logout path does (ApplicationStartup.cpp, AccountManager::logoutComplete)
    qApp->getSignInGate().pauseUntilLoginDetermined();
    qApp->getSignInGate().showLoginScreen();
}

// a QML panel's item by its objectName: in the desktop (the floating panels are its windows), then in the docked ones
static QObject* findPanelItem(const QString& objectName) {
    auto offscreenUi = DependencyManager::get<OffscreenUi>();
    QQuickItem* desktop = offscreenUi ? offscreenUi->getDesktop() : nullptr;
    QObject* found = desktop ? desktop->findChild<QObject*>(objectName) : nullptr;
    auto window = qApp->getWindow();
    if (!found && window) {
        for (QmlDock* dock : window->findChildren<QmlDock*>()) {
            QQuickItem* page = dock->panel();
            found = page ? page->findChild<QObject*>(objectName) : nullptr;
            if (found) {
                break;
            }
        }
    }
    return found;
}

bool TestScriptingInterface::setPanelText(const QString& objectName, const QString& text) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "setPanelText", Q_RETURN_ARG(bool, result), Q_ARG(QString, objectName), Q_ARG(QString, text));
        return result;
    }
    QObject* field = findPanelItem(objectName);
    return field && field->setProperty("text", text);
}

bool TestScriptingInterface::acceptPanelText(const QString& objectName) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "acceptPanelText", Q_RETURN_ARG(bool, result), Q_ARG(QString, objectName));
        return result;
    }
    QObject* field = findPanelItem(objectName);
    return field && QMetaObject::invokeMethod(field, "accepted");
}

void TestScriptingInterface::editorCreate(const QString& key) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "editorCreate", Q_ARG(QString, key));
        return;
    }
    DependencyManager::get<Editor>()->create(key);
}

bool TestScriptingInterface::saveWindowPicture(const QString& path) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "saveWindowPicture", Q_RETURN_ARG(bool, result), Q_ARG(QString, path));
        return result;
    }
    auto window = qApp->getWindow();
    if (!window) {
        return false;
    }
    QPixmap picture = window->grab();
    return !picture.isNull() && picture.save(path);
}

QVariantMap TestScriptingInterface::windowLayout() {
    if (QThread::currentThread() != thread()) {
        QVariantMap result;
        BLOCKING_INVOKE_METHOD(this, "windowLayout", Q_RETURN_ARG(QVariantMap, result));
        return result;
    }
    QVariantMap layout;
    auto window = qApp->getWindow();
    if (!window) {
        return layout;
    }
    // a dock area and a toolbar area have the same values (Qt::LeftDockWidgetArea == Qt::LeftToolBarArea, ...)
    auto areaName = [](int area) -> QString {
        switch (area) {
            case Qt::LeftDockWidgetArea: return "left";
            case Qt::RightDockWidgetArea: return "right";
            case Qt::TopDockWidgetArea: return "top";
            case Qt::BottomDockWidgetArea: return "bottom";
            default: return "none";
        }
    };
    const QList<QPair<QString, QString>> PANELS { { "chat", "ChatDock" }, { "hierarchy", "EditorHierarchy" },
                                                  { "inspector", "EditorInspector" }, { "assets", "EditorAssets" },
                                                  { "settings", "SettingsDock" }, { "avatar", "AvatarDock" },
                                                  { "people", "PeopleDock" }, { "explore", "ExploreDock" },
                                                  { "console", "ConsoleDock" } };
    for (const auto& panel : PANELS) {
        auto dock = window->findChild<QDockWidget*>(panel.second);
        if (!dock) {
            continue;
        }
        const QRect place(dock->mapToGlobal(QPoint(0, 0)), dock->size());
        QVariantMap entry {
            { "open", !dock->isHidden() },
            // in sight: tabbed behind another, a docked panel is visible to Qt but moved out of sight
            { "visible", dock->isVisible() && !dock->visibleRegion().isEmpty() },
            { "floating", dock->isFloating() },
            { "area", dock->isFloating() ? QString("none") : areaName(window->dockWidgetArea(dock)) },
            { "x", place.x() }, { "y", place.y() }, { "width", place.width() }, { "height", place.height() },
            // a native window of its own: Qt won't drag a docked panel that has one on macOS
            { "native", dock->internalWinId() != 0 },
        };
        // a QML panel's page: made the first time it shows, hidden with its panel (ui/QmlDock.h)
        if (auto qmlDock = qobject_cast<QmlDock*>(dock)) {
            entry["made"] = qmlDock->panel() != nullptr;
            entry["pageVisible"] = qmlDock->panel() && qmlDock->panel()->isVisible();
        } else if (panel.second == "ConsoleDock") {
            // the console itself, and with it its own script engine: made as the panel first shows
            entry["made"] = dock->widget() != nullptr;
        }
        layout[panel.first] = entry;
    }
    if (auto bar = window->findChild<QToolBar*>("systemToolbar")) {
        layout["bar"] = bar->isVisible() ? areaName(window->toolBarArea(bar)) : QString("hidden");
    }
    // the width of the edges between the panels and the 3D view, which a drag resizes them by
    layout["separator"] = window->style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent, nullptr, window);
    return layout;
}

bool TestScriptingInterface::dockAction(const QString& objectName, const QString& action) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "dockAction", Q_RETURN_ARG(bool, result), Q_ARG(QString, objectName),
                               Q_ARG(QString, action));
        return result;
    }
    auto window = qApp->getWindow();
    auto dock = window ? window->findChild<QDockWidget*>(objectName) : nullptr;
    auto titleBar = dock ? qobject_cast<DockTitleBar*>(dock->titleBarWidget()) : nullptr;
    if (!titleBar) {
        return false;
    }
    if (action == "close") {
        titleBar->closePanel();
    } else if (action.startsWith("tab:")) {
        // docked as a tab behind another docked panel, as a drag onto its title does
        auto other = window->findChild<QDockWidget*>(action.mid(4));
        if (!other || other->isFloating()) {
            return false;
        }
        gl::ViewChangeGuard guard;
        window->tabifyDockWidget(other, dock);
        other->raise();
    } else if (action.startsWith("drag:")) {
        // pressed on its title, moved in steps and let go, as the mouse does but in Qt's events only: Qt's own drag,
        // undocking, hovering and docking as a real one does, the real mouse untouched
        const QStringList by = action.mid(5).split(',');
        if (by.size() != 2) {
            return false;
        }
        const QPoint start = titleBar->mapToGlobal(QPoint(titleBar->width() / 2, titleBar->height() / 2));
        const QPoint end = start + QPoint(by[0].toInt(), by[1].toInt());
        auto send = [](QEvent::Type type, QWidget* target, const QPoint& at, Qt::MouseButtons buttons) {
            QMouseEvent event(type, target->mapFromGlobal(at), at, Qt::LeftButton, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(target, &event);
        };
        auto wait = [](int milliseconds) {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < milliseconds) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            }
        };
        send(QEvent::MouseButtonPress, titleBar, start, Qt::LeftButton);
        const int STEPS = 12;
        for (int step = 1; step <= STEPS; step++) {
            QWidget* target = QWidget::mouseGrabber() ? QWidget::mouseGrabber() : dock;
            send(QEvent::MouseMove, target, start + (end - start) * step / STEPS, Qt::LeftButton);
            wait(30);
        }
        wait(400);   // the place it would dock opens up (QMainWindow's animated docks)
        send(QEvent::MouseButtonRelease, QWidget::mouseGrabber() ? QWidget::mouseGrabber() : dock, end, Qt::NoButton);
        wait(300);
    } else if ((action == "dock" && dock->isFloating()) || (action == "float" && !dock->isFloating())) {
        titleBar->toggleFloating();
    }
    return true;
}

bool TestScriptingInterface::consoleRun(const QString& command) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "consoleRun", Q_RETURN_ARG(bool, result), Q_ARG(QString, command));
        return result;
    }
    auto console = DependencyManager::get<StandAloneJSConsole>()->console();
    if (console) {
        console->executeCommand(command);
    }
    return console != nullptr;
}

QStringList TestScriptingInterface::windowLayoutAction(const QString& action, const QString& name) {
    if (QThread::currentThread() != thread()) {
        QStringList result;
        BLOCKING_INVOKE_METHOD(this, "windowLayoutAction", Q_RETURN_ARG(QStringList, result), Q_ARG(QString, action),
                               Q_ARG(QString, name));
        return result;
    }
    auto layout = WindowLayout::instance();
    if (action == "save") {
        layout->saveAs(name);
    } else if (action == "apply") {
        layout->apply(name);
    } else if (action == "forget") {
        layout->forget(name);
    } else if (action == "reset") {
        layout->resetToDefault();
    }
    return layout->savedNames();
}

QVariantMap TestScriptingInterface::barButton(const QString& text, bool click) {
    if (QThread::currentThread() != thread()) {
        QVariantMap result;
        BLOCKING_INVOKE_METHOD(this, "barButton", Q_RETURN_ARG(QVariantMap, result), Q_ARG(QString, text),
                               Q_ARG(bool, click));
        return result;
    }
    auto window = qApp->getWindow();
    auto bar = window ? window->findChild<QToolBar*>("systemToolbar") : nullptr;
    if (!bar) {
        return QVariantMap();
    }
    for (auto button : bar->findChildren<QToolButton*>()) {
        if (button->text() == text) {
            if (click) {
                button->click();
            }
            // its cell in the bar, for the kits that check every button has the same (toolbar-a)
            return QVariantMap { { "active", button->property("active").toBool() },
                                 { "badge", button->property("badge").toInt() },
                                 { "x", button->x() }, { "y", button->y() },
                                 { "width", button->width() }, { "height", button->height() },
                                 { "barWidth", bar->width() }, { "barHeight", bar->height() } };
        }
    }
    return QVariantMap();
}

// an item's texts that are shown, depth first (Test.panelTexts)
static void collectTexts(QQuickItem* item, QStringList& texts) {
    if (!item->isVisible()) {
        return;
    }
    if (item->inherits("QQuickText")) {
        const QString text = item->property("text").toString();
        if (!text.isEmpty()) {
            texts << text;
        }
    }
    for (QQuickItem* child : item->childItems()) {
        collectTexts(child, texts);
    }
}

QStringList TestScriptingInterface::panelTexts(const QString& name) {
    if (QThread::currentThread() != thread()) {
        QStringList result;
        BLOCKING_INVOKE_METHOD(this, "panelTexts", Q_RETURN_ARG(QStringList, result), Q_ARG(QString, name));
        return result;
    }
    auto window = qApp->getWindow();
    auto dock = window ? window->findChild<QmlDock*>(name + "Dock") : nullptr;
    QQuickItem* page = dock && !dock->isHidden() ? dock->panel() : nullptr;
    QStringList texts;
    if (page) {
        collectTexts(page, texts);
    }
    return texts;
}

bool TestScriptingInterface::saveDockPicture(const QString& objectName, const QString& path) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "saveDockPicture", Q_RETURN_ARG(bool, result), Q_ARG(QString, objectName),
                               Q_ARG(QString, path));
        return result;
    }
    auto window = qApp->getWindow();
    auto dock = window ? window->findChild<QDockWidget*>(objectName) : nullptr;
    if (!dock || dock->isHidden()) {
        return false;
    }
    QPixmap picture = dock->grab();
    if (picture.isNull()) {
        return false;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // on Qt 6 a QML panel draws in its own window inside the dock (QmlDock), which the widget grab leaves blank: its
    // Qt Quick window's own picture goes in on top, where its container sits
    auto qmlDock = qobject_cast<QmlDock*>(dock);
    auto item = qmlDock ? qmlDock->panel() : nullptr;
    auto quickWindow = item ? item->window() : nullptr;
    if (quickWindow && dock->widget()) {
        const QImage content = quickWindow->grabWindow();
        if (!content.isNull()) {
            QPainter painter(&picture);
            painter.drawImage(QRect(dock->widget()->mapTo(dock, QPoint(0, 0)), dock->widget()->size()), content);
        }
    }
#endif
    return picture.save(path);
}

bool TestScriptingInterface::openInCode(const QUuid& id, const QString& which) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "openInCode", Q_RETURN_ARG(bool, result), Q_ARG(QUuid, id), Q_ARG(QString, which));
        return result;
    }
    return DependencyManager::get<CodeEditor>()->open(id, which);
}

QVariantMap TestScriptingInterface::codePanel() {
    if (QThread::currentThread() != thread()) {
        QVariantMap result;
        BLOCKING_INVOKE_METHOD(this, "codePanel", Q_RETURN_ARG(QVariantMap, result));
        return result;
    }
    auto code = DependencyManager::get<CodeEditor>();
    return {
        { "hasDocument", code->hasDocument() }, { "pageReady", code->isPageReady() }, { "fileName", code->fileName() },
        { "kind", code->kind() }, { "where", code->where() }, { "canSave", code->canSave() },
        { "readOnlyReason", code->readOnlyReason() }, { "problem", code->problem() }, { "saving", code->isSaving() },
        { "savedText", code->savedText() }, { "note", code->note() }, { "status", code->status() },
        { "statusState", code->statusState() }, { "errorLine", code->errorLine() }, { "errorText", code->errorText() },
        { "claudeWriting", code->isClaudeWriting() }, { "showingChange", code->isShowingChange() },
        { "nearby", code->nearby()->property("count") },
    };
}

int TestScriptingInterface::codePageRun(const QString& source) {
    if (QThread::currentThread() != thread()) {
        int result = 0;
        BLOCKING_INVOKE_METHOD(this, "codePageRun", Q_RETURN_ARG(int, result), Q_ARG(QString, source));
        return result;
    }
    return DependencyManager::get<CodeEditor>()->testRun(source);
}

QVariant TestScriptingInterface::codePageAnswer(int run) {
    if (QThread::currentThread() != thread()) {
        QVariant result;
        BLOCKING_INVOKE_METHOD(this, "codePageAnswer", Q_RETURN_ARG(QVariant, result), Q_ARG(int, run));
        return result;
    }
    return DependencyManager::get<CodeEditor>()->testAnswerFor(run);
}

void TestScriptingInterface::codeSave() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "codeSave");
        return;
    }
    DependencyManager::get<CodeEditor>()->testSave();
}

bool TestScriptingInterface::saveCodeMenuPicture(const QString& which, const QString& path) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "saveCodeMenuPicture", Q_RETURN_ARG(bool, result), Q_ARG(QString, which),
                               Q_ARG(QString, path));
        return result;
    }
    return DependencyManager::get<CodeEditor>()->saveMenuPicture(which, path);
}

void TestScriptingInterface::codeShowNearby() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "codeShowNearby");
        return;
    }
    DependencyManager::get<CodeEditor>()->testShowNearby();
}

// the widget that has the keyboard; in a test client running behind other windows (no window active), the one that
// would have it: chat's floating window's, else the main window's
static QWidget* keyboardWidget() {
    if (QWidget* focus = QApplication::focusWidget()) {
        return focus;
    }
    auto window = qApp->getWindow();
    auto dock = window ? window->findChild<QDockWidget*>("ChatDock") : nullptr;
    if (dock && dock->isVisible() && dock->isFloating() && dock->focusWidget()) {
        return dock->focusWidget();
    }
    return window ? window->focusWidget() : nullptr;
}

QVariantMap TestScriptingInterface::worldChat() {
    if (QThread::currentThread() != thread()) {
        QVariantMap result;
        BLOCKING_INVOKE_METHOD(this, "worldChat", Q_RETURN_ARG(QVariantMap, result));
        return result;
    }
    auto world = WorldChat::instance();
    auto window = qApp->getWindow();
    auto dock = window ? window->findChild<QDockWidget*>("ChatDock") : nullptr;
    QWidget* focus = keyboardWidget();
    auto box = qobject_cast<QPlainTextEdit*>(focus);
    return { { "items", world->items().toVariantList() },
             { "unread", world->unread() },
             { "chatOpen", dock && dock->isVisible() },
             { "focus", focus ? (focus->objectName().isEmpty() ? QString(focus->metaObject()->className()) : focus->objectName())
                              : QString() },
             { "focusText", box ? box->toPlainText() : QString() } };
}

void TestScriptingInterface::typeKeys(const QString& text, bool toView) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, "typeKeys", Q_ARG(QString, text), Q_ARG(bool, toView));
        return;
    }
    for (QChar c : text) {
        // the 3D view's keys go to the application, as GLWidget hands them on; the rest to the focused widget
        QObject* target = toView ? static_cast<QObject*>(qApp) : static_cast<QObject*>(keyboardWidget());
        if (!target) {
            return;
        }
        int key = c == '\n' ? Qt::Key_Return : c == QChar(0x1b) ? Qt::Key_Escape : (int)c.toUpper().unicode();
        QString typed = c == '\n' ? QString("\r") : c == QChar(0x1b) ? QString(QChar(0x1b)) : QString(c);
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, typed);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, typed);
        QCoreApplication::sendEvent(target, &press);
        QCoreApplication::sendEvent(target, &release);
    }
}

bool TestScriptingInterface::setChatBadge(int count) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "setChatBadge", Q_RETURN_ARG(bool, result), Q_ARG(int, count));
        return result;
    }
    auto tablets = DependencyManager::get<TabletScriptingInterface>();
    auto tablet = tablets ? tablets->getTablet("com.highfidelity.interface.tablet.system") : nullptr;
    if (!tablet) {
        return false;
    }
    auto model = tablet->getButtons();
    for (int row = 0; row < model->rowCount(); row++) {
        auto button = model->data(model->index(row), Qt::UserRole).value<TabletButtonProxy*>();
        if (button && button->getProperties().value("text").toString() == "Chat") {
            button->editProperties({ { "badge", count } });
            return true;
        }
    }
    return false;
}

void TestScriptingInterface::answerNextAlert(const QString& button, const QString& fieldText, const QString& picturePath) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "answerNextAlert", Q_ARG(QString, button), Q_ARG(QString, fieldText),
                               Q_ARG(QString, picturePath));
        return;
    }
    // looked for every 100 ms on this thread, where an open alert's own event loop runs it; answered once it has been
    // up 0.5 s, so the picture shows it drawn
    auto timer = new QTimer(this);
    auto seen = std::make_shared<int>(0);
    auto started = QDateTime::currentMSecsSinceEpoch();
    QObject::connect(timer, &QTimer::timeout, this, [timer, seen, started, button, fieldText, picturePath]() {
        auto alert = dynamic_cast<popup::Alert*>(QApplication::activeModalWidget());
        if (!alert || !alert->isVisible()) {
            if (QDateTime::currentMSecsSinceEpoch() - started > 15000) {
                qCWarning(interfaceapp) << "Test.answerNextAlert: no alert in 15 s";
                timer->stop();
                timer->deleteLater();
            }
            return;
        }
        if (++*seen < 5) {
            return;
        }
        timer->stop();
        timer->deleteLater();
        auto field = alert->findChild<QLineEdit*>();
        if (field && !fieldText.isEmpty()) {
            field->setText(fieldText);
        }
        if (!picturePath.isEmpty()) {
            alert->grab().save(picturePath);
        }
        for (auto pushButton : alert->findChildren<QPushButton*>()) {
            if (pushButton->text() == button) {
                qCInfo(interfaceapp) << "Test.answerNextAlert: pressing" << button << "on" << alert->windowTitle();
                pushButton->click();
                return;
            }
        }
        qCWarning(interfaceapp) << "Test.answerNextAlert: no button" << button << "on" << alert->windowTitle();
    });
    timer->start(100);
}

void TestScriptingInterface::answerNextPrompt(const QString& text) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "answerNextPrompt", Q_ARG(QString, text));
        return;
    }
    // looked for every 100 ms on this thread, where a waiting prompt's own loop runs it, as answerNextAlert does
    auto timer = new QTimer(this);
    auto started = QDateTime::currentMSecsSinceEpoch();
    QObject::connect(timer, &QTimer::timeout, this, [timer, started, text]() {
        QQuickItem* dialog = nullptr;
        if (QQuickItem* desktop = DependencyManager::get<OffscreenUi>()->getDesktop()) {
            for (QQuickItem* item : desktop->findChildren<QQuickItem*>()) {
                if (QString(item->metaObject()->className()).startsWith("QueryDialog")) {
                    dialog = item;
                    break;
                }
            }
        }
        if (!dialog) {
            if (QDateTime::currentMSecsSinceEpoch() - started > 15000) {
                qCWarning(interfaceapp) << "Test.answerNextPrompt: no prompt in 15 s";
                timer->stop();
                timer->deleteLater();
            }
            return;
        }
        timer->stop();
        timer->deleteLater();
        qCInfo(interfaceapp) << "Test.answerNextPrompt: answering" << text;
        // as the prompt's own accept() (dialogs/QueryDialog.qml): its answer, then it goes
        QMetaObject::invokeMethod(dialog, "selected", Q_ARG(QVariant, QVariant(text)));
        dialog->deleteLater();
    });
    timer->start(100);
}

int TestScriptingInterface::showAlert(const QString& title, const QString& text, const QString& details,
                                      const QVariantList& buttons, const QString& fieldPlaceholder) {
    if (QThread::currentThread() != thread()) {
        int result = -1;
        BLOCKING_INVOKE_METHOD(this, "showAlert", Q_RETURN_ARG(int, result), Q_ARG(QString, title), Q_ARG(QString, text),
                               Q_ARG(QString, details), Q_ARG(QVariantList, buttons), Q_ARG(QString, fieldPlaceholder));
        return result;
    }
    std::vector<popup::Button> list;
    for (const auto& entry : buttons) {
        const auto map = entry.toMap();
        const QString role = map.value("role").toString();
        list.push_back({ map.value("label").toString(), role == "default" ? popup::Role::Default
                                                        : role == "cancel" ? popup::Role::Cancel
                                                        : role == "destructive" ? popup::Role::Destructive
                                                        : popup::Role::Plain });
    }
    const popup::Field field { fieldPlaceholder, QString() };
    popup::Alert alert(popup::appIcon(), title, text, details, list, qApp->getWindow(),
                       fieldPlaceholder.isEmpty() ? nullptr : &field);
    alert.exec();
    return alert.answer();
}

void TestScriptingInterface::dumpDraws(int frames) {
    MeshPartPayload::dumpDrawFrames.store(std::max(0, frames));
}

bool TestScriptingInterface::traceRenderedEntity(const QUuid& entityID) {
    if (QThread::currentThread() != thread()) {
        bool result = false;
        BLOCKING_INVOKE_METHOD(this, "traceRenderedEntity", Q_RETURN_ARG(bool, result), Q_ARG(QUuid, entityID));
        return result;
    }
    render::ItemID item = render::Item::INVALID_ITEM_ID;
    if (!entityID.isNull()) {
        auto renderable = qApp->getEntities()->renderableForEntityId(entityID);
        if (!renderable) {
            return false;
        }
        item = renderable->getRenderItemID();
    }
    qApp->getGraphicsEngine().setRenderTraceItem(item);
    return render::Item::isValidID(item);
}

QVariantList TestScriptingInterface::takeRenderTrace() {
    QVariantList list;
    for (const auto& sample : qApp->getGraphicsEngine().takeRenderTrace()) {
        list << QVariant(QVariantList { (double)sample.time * 1.0e-3, (double)sample.camera.x, (double)sample.camera.y, (double)sample.camera.z,
                                        (double)sample.item.x, (double)sample.item.y, (double)sample.item.z, (double)sample.frameIndex });
    }
    return list;
}

void TestScriptingInterface::tracePresents(bool on) {
    auto displayPlugin = qApp->getActiveDisplayPlugin();
    if (displayPlugin) {
        displayPlugin->setPresentTracing(on);
    }
}

QVariantList TestScriptingInterface::takePresentTrace() {
    QVariantList list;
    auto displayPlugin = qApp->getActiveDisplayPlugin();
    if (displayPlugin) {
        for (const auto& sample : displayPlugin->takePresentTrace()) {
            list << QVariant(QVariantList { (double)sample.time * 1.0e-3, (double)sample.frameIndex, sample.newFrame ? 1.0 : 0.0,
                                            (double)sample.presentUsecs * 1.0e-3, (double)sample.swapUsecs * 1.0e-3, (double)sample.gpuMs });
        }
    }
    return list;
}

// FROZEN (vehicle path): see the note at the top of this file.
void TestScriptingInterface::frameTrace(bool on) {
    if (on) {
        FrameTrace::start();
    } else {
        FrameTrace::stop();
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
int TestScriptingInterface::dumpFrameTrace(const QString& path) {
    return FrameTrace::dump(path);
}

// FROZEN (vehicle path): see the note at the top of this file.
QVariantList TestScriptingInterface::takeFrameTrace() {
    QVariantList list;
    QVariantList header;
    for (const QString& column : FrameTrace::columns()) {
        header << column;
    }
    list << QVariant(header);
    for (const QVariantList& row : FrameTrace::take()) {
        list << QVariant(row);
    }
    return list;
}

void TestScriptingInterface::traceRefreshes(bool on) {
    DisplayLink::instance().setTracing(on);
}

QVariantList TestScriptingInterface::takeRefreshTrace() {
    QVariantList list;
    for (uint64_t time : DisplayLink::instance().takeTrace()) {
        list << QVariant((double)time * 1.0e-3);
    }
    return list;
}

float TestScriptingInterface::getDisplayRefreshRate() {
    auto window = qApp->getWindow();
    if (!window || !window->windowHandle() || !window->windowHandle()->screen()) {
        return 0.0f;
    }
    return (float)window->windowHandle()->screen()->refreshRate();
}

bool TestScriptingInterface::isVsyncEnabled() {
    auto displayPlugin = qApp->getActiveDisplayPlugin();
    return displayPlugin && displayPlugin->isVsyncEnabled();
}

void TestScriptingInterface::editorDrag(const glm::vec3& delta, float yawDegrees, float rollDegrees, bool begin, bool final) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "editorDrag", Q_ARG(glm::vec3, delta), Q_ARG(float, yawDegrees), Q_ARG(float, rollDegrees),
                               Q_ARG(bool, begin), Q_ARG(bool, final));
        return;
    }
    // a rotate ring's drag when any rotation is asked for: the kind is the handle's, not the size of the first step
    // (the first step of a slow drag is a degree or two)
    bool rotate = yawDegrees != 0.0f || rollDegrees != 0.0f;
    glm::quat rotation = glm::quat(glm::radians(glm::vec3(0.0f, yawDegrees, rollDegrees)));
    DependencyManager::get<Editor>()->dragSelectionForTest(delta, rotation, rotate, begin, final);
}

void TestScriptingInterface::editorScaleDrag(const glm::vec3& scale, bool begin, bool final) {
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "editorScaleDrag", Q_ARG(glm::vec3, scale), Q_ARG(bool, begin), Q_ARG(bool, final));
        return;
    }
    DependencyManager::get<Editor>()->dragSelectionForTest(glm::vec3(0.0f), glm::quat(), false, begin, final, scale, true);
}
