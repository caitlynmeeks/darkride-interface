//
//  ApplicationShutdown.cpp
//  interface/src
//
//  Application's shutdown, in its order: onAboutToQuit (the avatar's entity data saved, the input and display
//  plug-ins deactivated, the active display plug-in's name kept), cleanupBeforeQuit (the domain told, the scripts
//  stopped, the script engines, QML and audio destroyed, the settings saved) and the destructor (the avatars out of
//  physics, the graphics engine, the caches, the node list, the window). The members as they stood in
//  Application.cpp; a home of their own until an owner takes them (ARCHITECTURE.md §5.4 step 4).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "make/MakeDock.h"
#include "make/MakeLink.h"
#include "Application.h"
#include "ApplicationConstants.h"
#include <thread>
#include <QtCore/QThreadPool>
#include <QProcessEnvironment>
#include <shared/QtHelpers.h>
#include <shared/PlatformHelper.h>
#include <shared/GlobalAppProperties.h>
#include <AccountManager.h>
#include <ChatService.h>
#include <FriendsService.h>
#include <AnimDebugDraw.h>
#include <AnimationCacheScriptingInterface.h>
#include <AudioInjectorManager.h>
#include <CrashHelpers.h>
#include <EntityScriptingInterface.h>
#include <FramebufferCache.h>
#include <cstdlib>

#include <LogHandler.h>
#include <RunningMarker.h>
#include <MainWindow.h>
#include <hfm/ModelFormatRegistry.h>
#include <model-networking/ModelCacheScriptingInterface.h>
#include <procedural/MaterialCacheScriptingInterface.h>
#include <material-networking/TextureCacheScriptingInterface.h>
#include <OffscreenUi.h>
#include <ui/OffscreenQmlSurfaceCache.h>
#include <PhysicsEngine.h>
#include <plugins/PluginManager.h>
#include <RecordingScriptingInterface.h>
#include <ScriptEngines.h>
#include <ScriptCache.h>
#include <SoundCacheScriptingInterface.h>
#include <ui/TabletScriptingInterface.h>
#include <ui/ToolbarScriptingInterface.h>
#include <Preferences.h>
#include <display-plugins/CompositorHelper.h>
#include "AudioClient.h"
#include "avatar/AvatarManager.h"
#include "scripting/Audio.h"
#include "scripting/KeyboardScriptingInterface.h"
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
#endif
#include "ui/StandAloneJSConsole.h"
#include "ui/Keyboard.h"
#include "ui/OctreeStatsProvider.h"
#include "ui/Panels.h"
#include "chat/FriendsScriptingInterface.h"
#include "ui/Popup.h"
#include <PickManager.h>
#include <PointerManager.h>
#if defined(Q_OS_ANDROID)
#include <android/log.h>
#include "AndroidHelper.h"
#endif
#if defined(Q_OS_MACOS)
#include "AppNapDisabler.h"
#endif

using namespace std;

QSharedPointer<OffscreenUi> getOffscreenUI();      // Application.cpp's
extern QTimer locationUpdateTimer;                 // ApplicationStartup.cpp's; the constructor starts it

// Quitting takes a moment (scripts stop, the domain is told, settings are saved), and the window stops drawing
// at once, which looks like a hang: a notice says Darkride is closing, until the process ends
static void showClosingNotice(QWidget* window) {
    // A minimized or hidden window has no hang to explain
    if (!window || !window->isVisible() || window->isMinimized()) {
        return;
    }
    // drawn as Darkride's other popups, the logo on top (Caity, 10-27)
    auto notice = new popup::Notice(popup::appIcon(), QString("Closing Darkride") + QChar(0x2026),
                                    "Saving your settings and disconnecting", window);
    notice->show();
    notice->repaint();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void Application::onAboutToQuit() {
    showClosingNotice(_window);
    // quickly save AvatarEntityData before the EntityTree is dismantled
    getMyAvatar()->saveAvatarEntityDataToSettings();

    emit beforeAboutToQuit();

    if (getLoginDialogPoppedUp() && _firstRun.get()) {
        _firstRun.set(false);
    }

    foreach(auto inputPlugin, PluginManager::getInstance()->getInputPlugins()) {
        if (inputPlugin->isActive()) {
            inputPlugin->deactivate();
        }
    }

    // The active display plugin needs to be loaded before the menu system is active,
    // so its persisted explicitly here
    Setting::Handle<QString>{ ACTIVE_DISPLAY_PLUGIN_SETTING_NAME }.set(getActiveDisplayPlugin()->getName());

    _signInGate.shutdown();   // the sign-in setting cleared

    _display.deactivate();   // the plug-in deactivated, the headset's stand-by session ended
    // Hide Running Scripts dialog so that it gets destroyed in an orderly manner; prevents warnings at shutdown.
    getOffscreenUI()->hide("RunningScripts");

    _aboutToQuit = true;

    cleanupBeforeQuit();

    if (_crashOnShutdown) {
        // triggered by crash menu
        crash::nullDeref();
    }

    getRefreshRateManager().setRefreshRateRegime(RefreshRateManager::RefreshRateRegime::SHUTDOWN);
}

void Application::cleanupBeforeQuit() {
    // add a logline indicating if QTWEBENGINE_REMOTE_DEBUGGING is set or not
    QString webengineRemoteDebugging = QProcessEnvironment::systemEnvironment().value("QTWEBENGINE_REMOTE_DEBUGGING", "false");
    qCDebug(interfaceapp) << "QTWEBENGINE_REMOTE_DEBUGGING =" << webengineRemoteDebugging;

    DependencyManager::prepareToExit();

    if (tracing::enabled()) {
        auto tracer = DependencyManager::get<tracing::Tracer>();
        tracer->stopTracing();
        auto outputFile = property(hifi::properties::TRACING).toString();
        tracer->serialize(outputFile);
    }

    // Stop third party processes so that they're not left running in the event of a subsequent shutdown crash.
    AnimDebugDraw::getInstance().shutdown();

    // FIXME: once we move to shared pointer for the INputDevice we shoud remove this naked delete:
    _applicationStateDevice.reset();

    _keyboardFocus.deleteHighlight();

    {
        auto nodeList = DependencyManager::get<NodeList>();

        // send the domain a disconnect packet, force stoppage of domain-server check-ins
        nodeList->getDomainHandler().disconnect("Quitting");
        nodeList->setIsShuttingDown(true);

        // tell the packet receiver we're shutting down, so it can drop packets
        nodeList->getPacketReceiver().setShouldDropPackets(true);
    }

    getEntities()->shutdown(); // tell the entities system we're shutting down, so it will stop running scripts

    // Clear any queued processing (I/O, FBX/OBJ/Texture parsing)
    QThreadPool::globalInstance()->clear();
    QThreadPool::globalInstance()->waitForDone();

    DependencyManager::destroy<RecordingScriptingInterface>();

    // FIXME: Something is still holding on to the ScriptEnginePointers contained in ScriptEngines, and they hold backpointers to ScriptEngines,
    // so this doesn't shut down properly

    // Make first: Claude Code's connection, a running Blender and the building tools' engine end with it
    DependencyManager::destroy<MakeDock>();   // the dock's object first: it listens to MakeLink
    DependencyManager::destroy<MakeLink>();
    DependencyManager::get<ScriptEngines>()->shutdownScripting(); // stop all currently running global scripts

    // These classes hold ScriptEnginePointers, so they must be destroyed before ScriptEngines
    // Must be done after shutdownScripting in case any scripts try to access these things
    {
        DependencyManager::destroy<StandAloneJSConsole>();
        EntityTreePointer tree = getEntities()->getTree();
        tree->setSimulation(nullptr);
        DependencyManager::destroy<EntityTreeRenderer>();
    }
    DependencyManager::destroy<ScriptEngines>();

    bool keepMeLoggedIn = Setting::Handle<bool>(KEEP_ME_LOGGED_IN_SETTING_NAME, true).get();
    if (!keepMeLoggedIn) {
        DependencyManager::get<AccountManager>()->removeAccountFromFile();
    }
    // an account not kept on this computer leaves no chat session on it either
    DependencyManager::get<ChatService>()->stop(!keepMeLoggedIn);
    DependencyManager::destroy<FriendsScriptingInterface>();
    DependencyManager::destroy<ChatService>();
    DependencyManager::destroy<FriendsService>();

    _display.releasePlugin();
    PluginManager::getInstance()->shutdown();

    // Cleanup all overlays after the scripts, as scripts might add more
    _overlays.cleanupAllOverlays();

    // first stop all timers directly or by invokeMethod
    // depending on what thread they run in
    locationUpdateTimer.stop();

    // Wait for the settings thread to shut down, and save the settings one last time when it's safe
    if (_settingsGuard.wait()) {
        // save state
        saveSettings();
    }

    _window->saveGeometry();

    // stop QML: the panels' pages made on its engine (ui/Panels.h) go first
    DependencyManager::get<Panels>()->releaseQml();
    DependencyManager::destroy<TabletScriptingInterface>();
    DependencyManager::destroy<ToolbarScriptingInterface>();
    DependencyManager::destroy<OffscreenUi>();

    DependencyManager::destroy<OffscreenQmlSurfaceCache>();

    // destroy Audio so it and its threads have a chance to go down safely
    // this must happen after QML, as there are unexplained audio crashes originating in qtwebengine
    AudioInjector::setLocalAudioInterface(nullptr);
    DependencyManager::destroy<AudioClient>();
    DependencyManager::destroy<AudioScriptingInterface>();

    // The PointerManager must be destroyed before the PickManager because when a Pointer is deleted,
    // it accesses the PickManager to delete its associated Pick
    DependencyManager::destroy<PointerManager>();
    DependencyManager::destroy<PickManager>();
    DependencyManager::destroy<KeyboardScriptingInterface>();
    DependencyManager::destroy<Keyboard>();

    qCDebug(interfaceapp) << "Application::cleanupBeforeQuit() complete";
}

namespace Setting {
    void cleanupSettingsSaveThread();   // SettingInterface.cpp: stops the settings thread and saves every setting
}

// The Windows quit crash (qa + windowz 11-09: 2 of Caity's 3 quits of 1.1.4's cand-4): Qt WebEngine's own teardown, a
// post routine inside ~QApplication, deletes our shared web profiles while web views made on libraries/qml's surfaces
// are still on them, and the heap is corrupted. (Those views' lifetimes are frozen code; deleting the profiles ourselves
// crashes on the same views: editor-ui's Mac quit kit, 11-09.) Everything of ours is done by the end of ~Application, so
// on Windows the process ends there: the settings saved now (the post routine that would save them, taken off Qt's
// list), the running marker removed (as main's scope would), the log written out. main() already ends release builds
// with _exit, only after that teardown. Compiled everywhere so every build checks it; called on Windows only.
[[maybe_unused]] static void endBeforeQtTeardown(FileLogger* logger) {
    qCDebug(interfaceapp) << "Quit: settings saved and the process ended before Qt's late teardown (Windows)";
    qRemovePostRoutine(Setting::cleanupSettingsSaveThread);
    Setting::cleanupSettingsSaveThread();
    RunningMarker(RUNNING_MARKER_FILENAME).deleteRunningMarkerFile();
    if (logger) {
        logger->sync();
    }
    std::_Exit(0);   // C++'s _exit: no static destructors, as main's
}

Application::~Application() {
    // remove avatars from physics engine
    auto avatarManager = DependencyManager::get<AvatarManager>();
    avatarManager->clearOtherAvatars();
    auto myCharacterController = getMyAvatar()->getCharacterController();
    myCharacterController->clearDetailedMotionStates();

    PhysicsEngine::Transaction transaction;
    avatarManager->buildPhysicsTransaction(transaction);
    _physicsEngine->processTransaction(transaction);
    avatarManager->handleProcessedPhysicsTransaction(transaction);
    avatarManager->deleteAllAvatars();

    _physicsEngine->setCharacterController(nullptr);

    // the _shapeManager should have zero references
    _shapeManager.collectGarbage();
    assert(_shapeManager.getNumShapes() == 0);

    // shutdown graphics engine
    _graphicsEngine.shutdown();

    _gameWorkload.shutdown();

    DependencyManager::destroy<Preferences>();
    PlatformHelper::shutdown();

    _entityClipboard.shutdown();

    _domainSession.shutdown();
    _entityEditSender.terminate();

    DependencyManager::destroy<PluginManager>();

    DependencyManager::destroy<CompositorHelper>(); // must be destroyed before the FramebufferCache

    DependencyManager::destroy<SoundCacheScriptingInterface>();

    DependencyManager::destroy<AudioInjectorManager>();
    DependencyManager::destroy<AvatarManager>();
    DependencyManager::destroy<AnimationCacheScriptingInterface>();
    DependencyManager::destroy<AnimationCache>();
    DependencyManager::destroy<FramebufferCache>();
    DependencyManager::destroy<MaterialCacheScriptingInterface>();
    DependencyManager::destroy<MaterialCache>();
    DependencyManager::destroy<TextureCacheScriptingInterface>();
    DependencyManager::destroy<TextureCache>();
    DependencyManager::destroy<ModelCacheScriptingInterface>();
    DependencyManager::destroy<ModelCache>();
    DependencyManager::destroy<ModelFormatRegistry>();
    DependencyManager::destroy<ScriptCache>();
    DependencyManager::destroy<SoundCacheScriptingInterface>();
    DependencyManager::destroy<SoundCache>();
    DependencyManager::destroy<OctreeStatsProvider>();
    DependencyManager::destroy<GeometryCache>();

    DependencyManager::get<ResourceManager>()->cleanup();

    // remove the NodeList from the DependencyManager
    DependencyManager::destroy<NodeList>();
    // The window takes ownership of the menu, so this has the side effect of destroying it.
    _window->setMenuBar(nullptr);

    _window->deleteLater();

#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    endBeforeQtTeardown(_logger);   // still logging to the file here
#endif

    // Can't log to file past this point, FileLogger about to be deleted
    qInstallMessageHandler(LogHandler::verboseMessageHandler);

#ifdef Q_OS_MACOS
    // 10/16/2019 - Disabling this call. This causes known crashes (A), and it is not
    // fully understood whether it might cause other unknown crashes (B).
    //
    // (A) Although we try to shutdown the ScriptEngine threads in onAboutToQuit, there is
    // currently no guarantee that they have stopped. Waiting on them to stop has so far appeared to
    // never return on Mac, causing the application to hang on shutdown. Because ScriptEngines
    // may still be running, they may end up receiving events that are triggered from this processEvents call,
    // and then try to access resources that are no longer available at this point in time.
    // If the ScriptEngine threads were fully destroyed before getting here, this would
    // not be an issue.
    //
    // (B) It seems likely that a bunch of potential event handlers are dependent on Application
    // and other common dependencies to be available and not destroyed or in the middle of being
    // destroyed.
#endif
}
