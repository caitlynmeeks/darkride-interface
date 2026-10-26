//
//  Application.cpp
//  interface/src
//
//  Created by Andrzej Kapolka on 5/10/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Modified by Caitlyn Meeks on 12/24/19
//  Copyright 2019 Tivoli Cloud VR
//


#include "Application.h"
#include "ui/DialogsManager.h"
#include "GLCanvas.h"
#include "ImportExtensions.h"
#include "ApplicationConstants.h"
#include "LambdaEvent.h"
#include "DeadlockWatchdog.h"

#include <thread>

#include <QtCore/QThreadPool>

#include <QtGui/QClipboard>

#include <shared/QtHelpers.h>
#include <AccountManager.h>
#include <BuildInfo.h>
#include <MainWindow.h>
#include <OffscreenUi.h>
#include <plugins/CodecPlugin.h>
#include <plugins/PluginManager.h>
#include <display-plugins/CompositorHelper.h>
#include "AudioClient.h"
#include <AddressManager.h>
#include <NetworkingConstants.h>
#include <PathUtils.h>
#include "ui/Snapshot.h"

#include "InterfaceLogging.h"

#include "Util.h"

#include <GPUIdent.h>

#if defined(Q_OS_WIN)
#include <Windows.h>
#include <VersionHelpers.h>

// On Windows PC, NVidia Optimus laptop, we want to enable NVIDIA GPU
// FIXME seems to be broken.
extern "C" {
 _declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}
#endif

#if defined(Q_OS_ANDROID)
#include <android/log.h>
#include "AndroidHelper.h"
#endif

#if defined(Q_OS_MACOS)
// On Mac OS, disable App Nap to prevent audio glitches while running in the background
#include "AppNapDisabler.h"
static AppNapDisabler appNapDisabler;   // disabled, while in scope
#endif

Q_LOGGING_CATEGORY(trace_app_input_mouse, "trace.app.input.mouse")

using namespace std;



// For processing on QThreadPool, we target a number of threads after reserving some
// based on how many are being consumed by the application and the display plugin.  However,
// we will never drop below the 'min' value
const int MIN_PROCESSING_THREAD_POOL_SIZE = 2;   // declared in ApplicationConstants.h

// The import extensions (ImportExtensions.h declares them) and the table of importers by extension, for the URL dispatch
// (canAcceptURL, acceptURL) below.
const QString SNAPSHOT_EXTENSION = ".jpg";
const QString JPG_EXTENSION = ".jpg";
const QString PNG_EXTENSION = ".png";
const QString SVO_EXTENSION = ".svo";
const QString SVO_JSON_EXTENSION = ".svo.json";
const QString JSON_EXTENSION = ".json";
const QString JS_EXTENSION = ".js";
const QString FST_EXTENSION = ".fst";
const QString FBX_EXTENSION = ".fbx";
const QString OBJ_EXTENSION = ".obj";
const QString AVA_JSON_EXTENSION = ".ava.json";
static const QString WEB_VIEW_TAG = "noDownload=true";
const QString ZIP_EXTENSION = ".zip";

const std::vector<std::pair<QString, Application::AcceptURLMethod>> Application::_acceptedExtensions {
    { SVO_EXTENSION, &Application::importSVOFromURL },
    { SVO_JSON_EXTENSION, &Application::importSVOFromURL },
    { AVA_JSON_EXTENSION, &Application::askToWearAvatarAttachmentUrl },
    { JSON_EXTENSION, &Application::importJSONFromURL },
    { JS_EXTENSION, &Application::askToLoadScript },
    { FST_EXTENSION, &Application::askToSetAvatarUrl },
    { ZIP_EXTENSION, &Application::importFromZIP },
    { JPG_EXTENSION, &Application::importImage },
    { PNG_EXTENSION, &Application::importImage }
};


const QString KEEP_ME_LOGGED_IN_SETTING_NAME = "keepMeLoggedIn";   // declared in ApplicationConstants.h
const QUrl AVATAR_INPUTS_BAR_QML = PathUtils::qmlUrl("AvatarInputsBar.qml");   // declared in ApplicationConstants.h; the start-up's QML-surface operator reads it

#if defined(Q_OS_ANDROID)
#endif

// MyNativeEventFilter (the Windows messages that identify and raise the running instance) is in ApplicationStartup.cpp.


// messageHandler (Qt's messages into the log) is in ApplicationStartup.cpp.



// setupEssentials (what runs before the members exist: the version switches, the crash check, the resources, the
// plug-ins, the registrations) is in ApplicationStartup.cpp, with the statically provided plug-in lists it needs.



// The settings' defaults are in ApplicationStartup.cpp, beside the constructor that reads them.

QSharedPointer<OffscreenUi> getOffscreenUI() {
    return DependencyManager::get<OffscreenUi>();
}

// The constructor is in ApplicationStartup.cpp.

// The session with a domain (the connection's events, the queries, the octree accounting, the serverless and interstitial
// modes, the physics gate, where the client goes first, the content reload, the hand-off URL) is a DomainSession
// (DomainSession.h), the member _domainSession, reached by getDomainSession(); isServerlessMode, isInterstitialMode,
// isPhysicsEnabled, the octree accessors and the refreshes the menu and the Test API reach by name forward to it.

void Application::updateVerboseLogging() {
    auto menu = Menu::getInstance();
    if (!menu) {
        return;
    }
    bool enable = menu->isOptionChecked(MenuOption::VerboseLogging);

    QString rules =
        "hifi.*.info=%1\n"
        "hifi.audio-stream.debug=false\n"
        "hifi.audio-stream.info=false";
    rules = rules.arg(enable ? "true" : "false");
    QLoggingCategory::setFilterRules(rules);
}

QString Application::getUserAgent() {
    if (QThread::currentThread() != thread()) {
        QString userAgent;

        BLOCKING_INVOKE_METHOD(this, "getUserAgent", Q_RETURN_ARG(QString, userAgent));

        return userAgent;
    }

    QString userAgent = (
        "Darkride/" + 
        (BuildInfo::BUILD_TYPE == BuildInfo::BuildType::Stable ? BuildInfo::VERSION : "dev")
    );

    auto formatPluginName = [](QString name) -> QString { return name.trimmed().replace(" ", "-");  };

    // For each plugin, add to userAgent
    auto displayPlugins = PluginManager::getInstance()->getDisplayPlugins();
    for (auto& dp : displayPlugins) {
        if (dp->isActive() && dp->isHmd()) {
            userAgent += " " + formatPluginName(dp->getName());
        }
    }
    auto inputPlugins= PluginManager::getInstance()->getInputPlugins();
    for (auto& ip : inputPlugins) {
        if (ip->isActive()) {
            userAgent += " " + formatPluginName(ip->getName());
        }
    }
    // for codecs, we include all of them, even if not active
    auto codecPlugins = PluginManager::getInstance()->getCodecPlugins();
    for (auto& cp : codecPlugins) {
        userAgent += " " + formatPluginName(cp->getName());
    }

    return userAgent;
}

// toggleTabletUI is in ApplicationUi.cpp.

// The window's cursor is a WindowCursor (WindowCursor.h), the member _windowCursor, reached by getWindowCursor().

void Application::updateHeartbeat() const {
    DeadlockWatchdogThread::updateHeartbeat();
}

// The shutdown sequence (onAboutToQuit, cleanupBeforeQuit, ~Application) is in ApplicationShutdown.cpp.

// The GL context and its share contexts, the display plug-ins' start and the render engine's start (initializeGL,
// initializeDisplayPlugins, initializeRenderEngine) are in ApplicationStartup.cpp; the display plug-ins, the window's geometry
// and title and the headset are a Display (Display.h), the member _display, reached by getDisplay(); getActiveDisplayPlugin,
// isHMDMode and the other display queries forward to it.

// The interface's bring-up (initializeUi) is in ApplicationUi.cpp.


// The desktop's QML context (onDesktopRootContextCreated, onDesktopRootItemCreated) is in ApplicationScripting.cpp.

// userKickConfirmation (the ban question) is in ApplicationDialogs.cpp.

// The context properties of every other QML surface (setupQmlSurface) are in ApplicationScripting.cpp.

void Application::runTests() {
    runTimingTests();
    runUnitTests();
}

// The preferences' setters (setFieldOfView to setAwayStateWhenFocusLostInVREnabled) are in ApplicationSettings.cpp.

// showHelp (the controls reference on the tablet) is in ApplicationUi.cpp.

// The import wrappers for a dropped or opened file, the URL dispatch's targets (with the asks in ApplicationDialogs.cpp).
bool Application::importJSONFromURL(const QString& urlString) {
    // we only load files that terminate in just .json (not .svo.json and not .ava.json)
    QUrl jsonURL { urlString };

    emit svoImportRequested(urlString);
    return true;
}

bool Application::importSVOFromURL(const QString& urlString) {
    emit svoImportRequested(urlString);
    return true;
}

bool Application::importFromZIP(const QString& filePath) {
    qDebug() << "A zip file has been dropped in: " << filePath;
    QUrl empty;
    qApp->getFileDownloadInterface()->runUnzip(filePath, empty, true, true, false);
    return true;
}

bool Application::importImage(const QString& urlString) {
    qCDebug(interfaceapp) << "An image file has been dropped in";
    QString filepath(urlString);
    filepath.remove("file:///");
    _assetImport.addAssetToWorld(filepath, "", false, false);
    return true;
}

// thread-safe
// onPresent (the display plug-in's present callback) is in ApplicationFrame.cpp.

// The event routing (notify, event, eventFilter, the key, mouse, touch, wheel and drop handlers) is in
// ApplicationEvents.cpp.

// This is currently not used, but could be invoked if the user wants to go to the place embedded in an
// Interface-taken snapshot. (It was developed for drag and drop, before we had asset-server loading or in-world browsers.)
bool Application::acceptSnapshot(const QString& urlString) {
    QUrl url(urlString);
    QString snapshotPath = url.toLocalFile();

    SnapshotMetaData* snapshotData = DependencyManager::get<Snapshot>()->parseSnapshotData(snapshotPath);
    if (snapshotData) {
        if (!snapshotData->getURL().toString().isEmpty()) {
            DependencyManager::get<AddressManager>()->handleLookupString(snapshotData->getURL().toString());
        }
    } else {
        OffscreenUi::asyncWarning("", "No location details were found in the file\n" +
                             snapshotPath + "\nTry dragging in a Darkride Snap saved on this computer.");
    }
    return true;
}

// The frame loop's tick (idle, with the Windows CPU monitor it starts) is in ApplicationFrame.cpp.

// The entities' clipboard (export, import, paste) is an EntityClipboard (EntityClipboard.h), the member _entityClipboard,
// reached by getEntityClipboard().

// loadSettings and saveSettings are in ApplicationSettings.cpp.

// init (the avatar manager, the entity tree renderer, the physics engine, the entity simulation and the game workload
// brought up; the interface stage calls it) is in ApplicationStartup.cpp.

// The sign-in gate is a SignInGate (SignInGate.h), the member _signInGate, reached by getSignInGate();
// getLoginDialogPoppedUp() and isSignInPending() forward to it.

// The avatar's scripts (loadAvatarScripts, unloadAvatarScripts) are in ApplicationAvatar.cpp.

// The per-frame helpers (updateLOD, pushPostUpdateLambda, updateMyAvatarLookAtPosition, updateThreads) are in ApplicationFrame.cpp.

// The overlays (toggleOverlays, setOverlaysVisible, centerUI) are in ApplicationUi.cpp.

// The camera and the view frustums are a ViewState (ViewState.h), the member _viewState, reached by getViewState();
// getCamera(), the view-state overrides and the two View menu slots forward to it.

// The keyboard focus and its highlight are a KeyboardFocus (KeyboardFocus.h), the member _keyboardFocus, reached by
// getKeyboardFocus().

// The simulation step and what frames it (updateDialogs, the physics gate setArrivalWaitsForNearby/tryToEnablePhysics,
// update, updateRenderArgs) is in ApplicationFrame.cpp.

float Application::getNumCollisionObjects() const {
    return _physicsEngine ? _physicsEngine->getNumCollisionObjects() : 0;
}

// getMyAvatar and getAvatarPosition are in ApplicationAvatar.cpp.

// resentSensors() is a bit of vestigial feature. It used to be used for Oculus DK2 to recenter the view around
// the current head orientation.  With the introduction of "room scale" tracking we no longer need that particular
// feature.  However, we still use this to reset face trackers, eye trackers, audio and to optionally re-load the avatar
// rig and animations from scratch.
void Application::resetSensors(bool andReload) {
    _overlayConductor.centerUI();
    getActiveDisplayPlugin()->resetSensors();
    getMyAvatar()->reset(true, andReload);
    QMetaObject::invokeMethod(DependencyManager::get<AudioClient>().data(), "reset", Qt::QueuedConnection);
}

// The script API's globals (registerScriptEngineWithApplicationServices) are in ApplicationScripting.cpp.

// The URL dispatch (AbstractUriHandler): a dropped or opened URL goes to the address manager (a hifi: URL) or to the
// importer for its extension, from the table at the top of this file.
bool Application::canAcceptURL(const QString& urlString) const {
    QUrl url(urlString);
    if (url.query().contains(WEB_VIEW_TAG)) {
        return false;
    } else if (urlString.startsWith(URL_SCHEME_HIFI) || isWorldAddressScheme(url.scheme())) {
        return true;
    }
    QString lowerPath = url.path().toLower();
    for (auto& pair : _acceptedExtensions) {
        if (lowerPath.endsWith(pair.first, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

bool Application::acceptURL(const QString& urlString, bool defaultUpload) {
    QUrl url(urlString);

    if (isWorldAddressScheme(url.scheme())) {
        // a world's address (hifi://, or tivoli:// or darkride:// from a link) - have the AddressManager handle it
        QMetaObject::invokeMethod(DependencyManager::get<AddressManager>().data(), "handleLookupString",
                                  Qt::AutoConnection, Q_ARG(const QString&, urlString));
        return true;
    }

    QString lowerPath = url.path().toLower();
    for (auto& pair : _acceptedExtensions) {
        if (lowerPath.endsWith(pair.first, Qt::CaseInsensitive)) {
            AcceptURLMethod method = pair.second;
            return (this->*method)(urlString);
        }
    }

    
    return defaultUpload;
}

// The asks for a dropped or opened URL (askToSetAvatarUrl, askToLoadScript, askToWearAvatarAttachmentUrl,
// displayAvatarAttachmentWarning), showDialog, showScriptLogs and showAssetServerWidget are in ApplicationDialogs.cpp.

// A file from a URL or a drop becoming an asset and an entity is an AssetImport (AssetImport.h), the member _assetImport,
// reached by getAssetImport(); addAssetToWorldFromURL forwards to it for QML.

// The dialogs and windows (packageModel, loadDialog, importAvatarDialog, putAvatarInMyFiles, the previous script location,
// loadScriptURLDialog, getSampleSound, the LOD tools, entity statistics and domain connection dialogs, the log windows) are
// in ApplicationDialogs.cpp.

// The snapshots asked of the next frame are a SnapshotRequests (SnapshotRequests.h), the member _snapshotRequests, reached
// by getSnapshotRequests().

// checkSkeleton is in ApplicationAvatar.cpp.

void Application::postLambdaEvent(const std::function<void()>& f) {
    if (this->thread() == QThread::currentThread()) {
        f();
    } else {
        QCoreApplication::postEvent(this, new LambdaEvent(f));
    }
}

void Application::sendLambdaEvent(const std::function<void()>& f) {
    if (this->thread() == QThread::currentThread()) {
        f();
    } else {
        LambdaEvent event(f);
        QCoreApplication::sendEvent(this, &event);
    }
}

// initPlugins (the display and input plug-ins' enable lists from the command line) is in ApplicationStartup.cpp.

void Application::setShowBulletWireframe(bool value) {
    _physicsEngine->setShowBulletWireframe(value);
}

void Application::setShowBulletAABBs(bool value) {
    _physicsEngine->setShowBulletAABBs(value);
}

void Application::setShowBulletContactPoints(bool value) {
    _physicsEngine->setShowBulletContactPoints(value);
}

void Application::setShowBulletConstraints(bool value) {
    _physicsEngine->setShowBulletConstraints(value);
}

void Application::setShowBulletConstraintLimits(bool value) {
    _physicsEngine->setShowBulletConstraintLimits(value);
}

void Application::setShowTrackedObjects(bool value) {
    _showTrackedObjects = value;
}

void Application::deadlockApplication() {
    qCDebug(interfaceapp) << "Intentionally deadlocked Interface";
    // Using a loop that will *technically* eventually exit (in ~600 billion years)
    // to avoid compiler warnings about a loop that will never exit
    for (uint64_t i = 1; i != 0; ++i) {
        QThread::sleep(1);
    }
}

// cause main thread to be unresponsive for 35 seconds
void Application::unresponsiveApplication() {
    // to avoid compiler warnings about a loop that will never exit
    uint64_t start = usecTimestampNow();
    uint64_t UNRESPONSIVE_FOR_SECONDS = 35;
    uint64_t UNRESPONSIVE_FOR_USECS = UNRESPONSIVE_FOR_SECONDS * USECS_PER_SECOND;
    qCDebug(interfaceapp) << "Intentionally cause Interface to be unresponsive for " << UNRESPONSIVE_FOR_SECONDS << " seconds";
    while (usecTimestampNow() - start < UNRESPONSIVE_FOR_USECS) {
        QThread::sleep(1);
    }
}

CompositorHelper& Application::getApplicationCompositor() const {
    return *DependencyManager::get<CompositorHelper>();
}


// virtual functions required for PluginContainer
ui::Menu* Application::getPrimaryMenu() {
    auto appMenu = _window->menuBar();
    auto uiMenu = dynamic_cast<ui::Menu*>(appMenu);
    return uiMenu;
}


// FIXME?  perhaps two, one for the main thread and one for the offscreen UI rendering thread?
static const int UI_RESERVED_THREADS = 1;
// Windows won't let you have all the cores
static const int OS_RESERVED_THREADS = 1;

void Application::updateThreadPoolCount() const {
    auto reservedThreads = UI_RESERVED_THREADS + OS_RESERVED_THREADS + _display.plugin()->getRequiredThreadCount();
    auto availableThreads = QThread::idealThreadCount() - reservedThreads;
    auto threadPoolSize = std::max(MIN_PROCESSING_THREAD_POOL_SIZE, availableThreads);
    qCDebug(interfaceapp) << "Ideal Thread Count " << QThread::idealThreadCount();
    qCDebug(interfaceapp) << "Reserved threads " << reservedThreads;
    qCDebug(interfaceapp) << "Setting thread pool size to " << threadPoolSize;
    QThreadPool::globalInstance()->setMaxThreadCount(threadPoolSize);
}

// The system tablet's mode and ids (updateSystemTabletMode, getTabletScreenID, getTabletHomeButtonID, getTabletFrameID,
// getTabletIDs) are in ApplicationUi.cpp.

// setAvatarOverrideUrl is in ApplicationAvatar.cpp.

void Application::saveNextPhysicsStats(QString filename) {
    _physicsEngine->saveNextPhysicsStats(filename);
}

void Application::copyToClipboard(const QString& text) {
    if (QThread::currentThread() != qApp->thread()) {
        QMetaObject::invokeMethod(this, "copyToClipboard", Q_ARG(QString, text));
        return;
    }

    // assume that the address is being copied because the user wants a shareable address
    QApplication::clipboard()->setText(text);
}

QString Application::getGraphicsCardType() {
    return GPUIdent::getInstance()->getName();
}

// openDirectory and showUrlHandler (the URL handler's "open this file?" ask) are in ApplicationDialogs.cpp.

// used to test "shutdown" crash annotation.
void Application::crashOnShutdown() {
    qDebug() << "crashOnShutdown(), ON PURPOSE!";
    _crashOnShutdown = true;
    quit();
}

void Application::overrideEntry(){
    _domainSession.setOverrideEntry();
}
void Application::forceDisplayName(const QString& displayName) {
    getMyAvatar()->setDisplayName(displayName);
}
void Application::forceLoginWithTokens(const QString& tokens) {
    DependencyManager::get<AccountManager>()->setAccessTokens(tokens);
    Setting::Handle<bool>(KEEP_ME_LOGGED_IN_SETTING_NAME, true).set(true);
}

// The Android lifecycle (beforeEnterBackground, enterBackground, enterForeground, toggleAwayMode) is in ApplicationEvents.cpp.

// The PluginContainer's window and context, and the display plug-ins' tools dialog.
GLWidget* Application::getPrimaryWidget() {
    return _glWidget;
}

MainWindow* Application::getPrimaryWindow() {
    return getWindow();
}

QOpenGLContext* Application::getPrimaryContext() {
    return _glWidget->qglContext();
}

bool Application::makeRenderingContextCurrent() {
    return true;
}

void Application::showDisplayPluginsTools(bool show) {
    DependencyManager::get<DialogsManager>()->hmdTools(show);
}
