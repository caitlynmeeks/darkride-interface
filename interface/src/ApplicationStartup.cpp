//
//  ApplicationStartup.cpp
//  interface/src
//
//  Application's start-up: setupEssentials (what runs before the members exist: the version switches, the crash
//  check, the resources, the plug-in manager and the registrations) and the constructor, which brings the client up
//  in ten named stages (the process, the services and their connections, the window and the renderer, the
//  interface, the entity processing, input, settings, the operators, then the sign-in screen), each a function
//  here whose body is the constructor's former text between two of the start-up profile's phase marks. With the
//  pieces only the start-up uses: the Qt message handler, the Windows native event filter, the settings' defaults;
//  and init, which the interface stage calls once the UI is up, to bring up the avatar manager, the entity tree
//  renderer, the physics engine, the entity simulation and the game workload (ARCHITECTURE.md §5.4 step 4).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "Application.h"
#include <shared/NsightHelpers.h>
#include <QtGui/QWindow>
#include <gl/OffscreenGLCanvas.h>
#include "ApplicationServices.h"
#include "ApplicationConstants.h"
#include "ClientOptions.h"
#include "DeadlockWatchdog.h"
#include <thread>
#include <glm/glm.hpp>
#include <gl/Config.h>
#include <QtCore/QResource>
#include <QtCore/QAbstractNativeEventFilter>
#include <QtCore/QThreadPool>
#include <QtGui/QDesktopServices>
#include <QtQml/QQmlContext>
#include <QtQuick/QQuickWindow>
#include <QtWidgets/QDesktopWidget>
#include <QFontDatabase>
#include <gl/GLHelpers.h>
#include <shared/QtHelpers.h>
#include <shared/PlatformHelper.h>
#include <shared/GlobalAppProperties.h>
#include <StartupProfiler.h>
#include <AccountManager.h>
#include <ChatService.h>
#include <AddressManager.h>
#include <BuildInfo.h>
#include <DebugDraw.h>
#include <EntityScriptingInterface.h>
#include <controllers/UserInputMapper.h>
#include <controllers/ScriptingInterface.h>
#include <LogHandler.h>
#include "LocationBookmarks.h"
#include <MainWindow.h>
#include <MessagesClient.h>
#include <NetworkingConstants.h>
#include <ObjectMotionState.h>
#include <OffscreenUi.h>
#include <ui/OffscreenQmlSurfaceCache.h>
#include <PathUtils.h>
#include <PhysicsEngine.h>
#include <plugins/PluginManager.h>
#include <RecordingScriptingInterface.h>
#include <ResourceCache.h>
#include <ScriptEngines.h>
#include <ScriptCache.h>
#include <ShapeEntityItem.h>
#include <ui/TabletScriptingInterface.h>
#include <FileDialogHelper.h>
#include <UsersScriptingInterface.h>
#include <recording/Recorder.h>
#include <display-plugins/CompositorHelper.h>
#include <display-plugins/hmd/HmdDisplayPlugin.h>
#include <RenderableEntityItem.h>
#include <RenderableTextEntityItem.h>
#include <RenderableWebEntityItem.h>
#include <StencilMaskPass.h>
#include "AudioClient.h"
#include "avatar/AvatarManager.h"
#include "CrashRecoveryHandler.h"
#include "DiscoverabilityManager.h"
#include "GLCanvas.h"
#include "scripting/Audio.h"
#include "scripting/TestScriptingInterface.h"
#include "scripting/HMDScriptingInterface.h"
#include "scripting/ControllerScriptingInterface.h"
#include "scripting/SelectionScriptingInterface.h"
#include "editor/Editor.h"
#include "editor/ui/ViewportContainer.h"
#include "scripting/KeyboardScriptingInterface.h"
#include "scripting/RenderScriptingInterface.h"
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
#endif
#include "ui/DialogsManager.h"
#include "ui/LoginDialog.h"
#include "ui/Keyboard.h"
#include "Util.h"
#include <GPUIdent.h>
#include <PickManager.h>
#include <PointerManager.h>
#include <raypick/PickScriptingInterface.h>
#include <raypick/RayPick.h>
#include <raypick/MouseTransformNode.h>
#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <VersionHelpers.h>
#endif
#if defined(Q_OS_ANDROID)
#include <android/log.h>
#include "AndroidHelper.h"
#endif
#if defined(Q_OS_MACOS)
#include "AppNapDisabler.h"
#endif

using namespace std;

QSharedPointer<OffscreenUi> getOffscreenUI();            // Application.cpp's
QString labelledTitle(const QString& title);             // Display.cpp's: a window title with the client label
void applyClientLabelIcon();                            // Display.cpp's: a test client's Dock icon, from the label

QTimer locationUpdateTimer;   // started by the constructor, stopped in ApplicationShutdown.cpp

// constants only the start-up reads
#if !defined(Q_OS_ANDROID)
static const uint32_t MAX_CONCURRENT_RESOURCE_DOWNLOADS = 16;
#else
static const uint32_t MAX_CONCURRENT_RESOURCE_DOWNLOADS = 4;
#endif
static const QString DESKTOP_LOCATION = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
static const QUrl BUBBLE_ICON_QML = PathUtils::qmlUrl("BubbleIcon.qml");

#ifdef Q_OS_WIN
static const UINT UWM_IDENTIFY_INSTANCES =
    RegisterWindowMessage("UWM_IDENTIFY_INSTANCES_{8AB82783-B74A-4258-955B-8188C22AA0D6}_" + qgetenv("USERNAME"));
static const UINT UWM_SHOW_APPLICATION =
    RegisterWindowMessage("UWM_SHOW_APPLICATION_{71123FD6-3DA8-4DC1-9C27-8A12A6250CBA}_" + qgetenv("USERNAME"));

class MyNativeEventFilter : public QAbstractNativeEventFilter {
public:
    static MyNativeEventFilter& getInstance() {
        static MyNativeEventFilter staticInstance;
        return staticInstance;
    }

    bool nativeEventFilter(const QByteArray &eventType, void* msg, long* result) Q_DECL_OVERRIDE {
        if (eventType == "windows_generic_MSG") {
            MSG* message = (MSG*)msg;

            if (message->message == UWM_IDENTIFY_INSTANCES) {
                *result = UWM_IDENTIFY_INSTANCES;
                return true;
            }

            if (message->message == UWM_SHOW_APPLICATION) {
                MainWindow* applicationWindow = qApp->getWindow();
                if (applicationWindow->isMinimized()) {
                    applicationWindow->showNormal();  // Restores to windowed or maximized state appropriately.
                }
                qApp->setActiveWindow(applicationWindow);  // Flashes the taskbar icon if not focus.
                return true;
            }
        }
        return false;
    }
};
#endif

void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    QString logMessage = LogHandler::getInstance().printMessage((LogMsgType) type, context, message);

    if (!logMessage.isEmpty()) {
#ifdef Q_OS_ANDROID
        const char * local=logMessage.toStdString().c_str();
        switch (type) {
            case QtDebugMsg:
                __android_log_write(ANDROID_LOG_DEBUG,"Interface",local);
                break;
            case QtInfoMsg:
                __android_log_write(ANDROID_LOG_INFO,"Interface",local);
                break;
            case QtWarningMsg:
                __android_log_write(ANDROID_LOG_WARN,"Interface",local);
                break;
            case QtCriticalMsg:
                __android_log_write(ANDROID_LOG_ERROR,"Interface",local);
                break;
            case QtFatalMsg:
            default:
                __android_log_write(ANDROID_LOG_FATAL,"Interface",local);
                abort();
        }
#else
        qApp->getLogger()->addMessage(qPrintable(logMessage));
#endif
    }
}

/**jsdoc
 * <p>The <code>Controller.Hardware.Application</code> object has properties representing Interface's state. The property
 * values are integer IDs, uniquely identifying each output. <em>Read-only.</em></p>
 * <p>These states can be mapped to actions or functions or <code>Controller.Standard</code> items in a {@link RouteObject} 
 * mapping (e.g., using the {@link RouteObject#when} method). Each data value is either <code>1.0</code> for "true" or 
 * <code>0.0</code> for "false".</p>
 * <table>
 *   <thead>
 *     <tr><th>Property</th><th>Type</th><th>Data</th><th>Description</th></tr>
 *   </thead>
 *   <tbody>
 *     <tr><td><code>CameraFirstPerson</code></td><td>number</td><td>number</td><td>The camera is in first-person mode.
 *       <em>Legacy first person camera mode.</em></td></tr>
 *     <tr><td><code>CameraFirstPersonLookAt</code></td><td>number</td><td>number</td><td>The camera is in first-person mode.
 *       <em>Default first person camera mode.</em></td></tr>
 *     <tr><td><code>CameraThirdPerson</code></td><td>number</td><td>number</td><td>The camera is in third-person mode.
 *       <em>Legacy third person camera mode.</em></td></tr>
 *     <tr><td><code>CameraLookAt</code></td><td>number</td><td>number</td><td>The camera is in third-person mode.
 *       <em>Default third person camera mode.</em></td></tr>
 *     <tr><td><code>CameraFSM</code></td><td>number</td><td>number</td><td>The camera is in full screen mirror mode.
 *       <em>Legacy "look at myself" behavior.</em></td></tr>
 *     <tr><td><code>CameraSelfie</code></td><td>number</td><td>number</td><td>The camera is in selfie mode.
 *       <em>Default "look at myself" camera mode.</em></td></tr>
 *     <tr><td><code>CameraIndependent</code></td><td>number</td><td>number</td><td>The camera is in independent mode.</td></tr>
 *     <tr><td><code>CameraEntity</code></td><td>number</td><td>number</td><td>The camera is in entity mode.</td></tr>
 *     <tr><td><code>InHMD</code></td><td>number</td><td>number</td><td>The user is in HMD mode.</td></tr>
 *     <tr><td><code>AdvancedMovement</code></td><td>number</td><td>number</td><td>Advanced movement (walking) controls are 
 *       enabled.</td></tr>
 *     <tr><td><code>StrafeEnabled</code></td><td>number</td><td>number</td><td>Strafing is enabled</td></tr>
 *     <tr><td><code>LeftHandDominant</code></td><td>number</td><td>number</td><td>Dominant hand set to left.</td></tr>
 *     <tr><td><code>RightHandDominant</code></td><td>number</td><td>number</td><td>Dominant hand set to right.</td></tr>
 *     <tr><td><code>SnapTurn</code></td><td>number</td><td>number</td><td>Snap turn is enabled.</td></tr>
 *     <tr><td><code>Grounded</code></td><td>number</td><td>number</td><td>The user's avatar is on the ground.</td></tr>
 *     <tr><td><code>NavigationFocused</code></td><td>number</td><td>number</td><td><em>Not used.</em></td></tr>
 *     <tr><td><code>PlatformWindows</code></td><td>number</td><td>number</td><td>The operating system is Windows.</td></tr>
 *     <tr><td><code>PlatformMac</code></td><td>number</td><td>number</td><td>The operating system is Mac.</td></tr>
 *     <tr><td><code>PlatformAndroid</code></td><td>number</td><td>number</td><td>The operating system is Android.</td></tr>
 *   </tbody>
 * </table>
 * @typedef {object} Controller.Hardware-Application
 */


// Statically provided display and input plugins
extern DisplayPluginList getDisplayPlugins();
extern InputPluginList getInputPlugins();
extern void saveInputPluginSettings(const InputPluginList& plugins);

bool setupEssentials(bool runningMarkerExisted) {
    const ClientOptions& options = ClientOptions::get();

    qInstallMessageHandler(messageHandler);

    StartupProfiler::phase("setupEssentials: version switches, crash check, resources");

    // --protocolVersion <file>: the protocol signature into that file, then out, before anything else is set up.
    // It reports to a file because stdout is captured for the log. (A launcher asking this should also pass
    // --allowMultipleInstances, since this runs after the single-instance check.)
    if (!options.protocolVersionFile.isEmpty()) {
        FILE* fp = fopen(options.protocolVersionFile.toLocal8Bit().constData(), "w");
        if (fp) {
            fputs(protocolVersionsSignatureBase64().toLatin1().data(), fp);
            fclose(fp);
            if (!runningMarkerExisted) { // don't leave ours around
                RunningMarker runingMarker(RUNNING_MARKER_FILENAME);
                runingMarker.deleteRunningMarkerFile(); // happens in deleter, but making the side-effect explicit.
            }
            _exit(0);
        }
    }

    const int listenPort = options.listenPort >= 0 ? options.listenPort : INVALID_PORT;

    // emulate standalone device
    bool isStandalone = options.standalone;
    qApp->setProperty(hifi::properties::STANDALONE, isStandalone);

    // Ignore any previous crashes if running from command line with a test script.
    bool inTestMode = !options.testScript.isEmpty();

    bool previousSessionCrashed { false };
    if (!inTestMode) {
        // previousSessionCrashed = CrashRecoveryHandler::checkForResetSettings(runningMarkerExisted, suppressPrompt);
        previousSessionCrashed = CrashRecoveryHandler::checkForResetSettings(runningMarkerExisted, true);
    }

    // get dir to use for cache
    QString cacheDir = options.cacheDirectory;
    if (!cacheDir.isEmpty()) {
        qApp->setProperty(hifi::properties::APP_LOCAL_DATA_PATH, cacheDir);
    }

    {
        const QString resourcesBinaryFile = PathUtils::getRccPath();
        if (!QFile::exists(resourcesBinaryFile)) {
            throw std::runtime_error("Unable to find primary resources");
        }
        if (!QResource::registerResource(resourcesBinaryFile)) {
            throw std::runtime_error("Unable to load primary resources");
        }
    }

    StartupProfiler::phase("setupEssentials: plug-in manager, runtime plug-ins loaded");
    // Tell the plugin manager about our statically linked plugins
    DependencyManager::set<ScriptInitializers>();
    DependencyManager::set<PluginManager>();
    auto pluginManager = PluginManager::getInstance();
    pluginManager->setInputPluginProvider([] { return getInputPlugins(); });
    pluginManager->setDisplayPluginProvider([] { return getDisplayPlugins(); });
    pluginManager->setInputPluginSettingsPersister([](const InputPluginList& plugins) { saveInputPluginSettings(plugins); });
    // The runtime plug-ins load here, on the main thread, before the audio client exists: left to the first lookup,
    // the audio client's codec search would load them from its own thread and the plug-ins' objects (the MIDI
    // plug-in's among them) would live there. The Steam and Oculus init that stood here did this load as a side effect.
    pluginManager->loadPlugins();

    PROFILE_SET_THREAD_NAME("Main Thread");

#if defined(Q_OS_WIN)
    // Select appropriate audio DLL
    QString audioDLLPath = QCoreApplication::applicationDirPath();
    if (IsWindows8OrGreater()) {
        audioDLLPath += "/audioWin8";
    } else {
        audioDLLPath += "/audioWin7";
    }
    QCoreApplication::addLibraryPath(audioDLLPath);
#endif 

    QString defaultScriptsOverrideOption = options.defaultScriptsOverride;

    StartupProfiler::phase("setupEssentials: the registrations (each row a service's constructor)");
    registerApplicationServices(listenPort, defaultScriptsOverrideOption);
    StartupProfiler::phase("setupEssentials: platform helper, bookmarks; then the members (physics engine, clipboard tree, settings)");
    PlatformHelper::setup();
    
    QObject::connect(PlatformHelper::instance(), &PlatformHelper::systemWillWake, [] {
        QMetaObject::invokeMethod(DependencyManager::get<NodeList>().data(), "noteAwakening", Qt::QueuedConnection);
        QMetaObject::invokeMethod(DependencyManager::get<AudioClient>().data(), "noteAwakening", Qt::QueuedConnection);
    });


    QString setBookmarkValue = options.setBookmark;
    if (!setBookmarkValue.isEmpty()) {
        // Bookmarks are expected to be in a name=url form.
        // An `=` character in the name or url is unsupported.
        auto parts = setBookmarkValue.split("=");
        if (parts.length() != 2) {
            qWarning() << "Malformed setBookmark argument: " << setBookmarkValue;
        } else {
            qDebug() << "Setting bookmark" << parts[0] << "to" << parts[1];
            DependencyManager::get<LocationBookmarks>()->insert(parts[0], parts[1]);
        }
    }

    return previousSessionCrashed;
}

const float DEFAULT_HMD_TABLET_SCALE_PERCENT = 100.0f;
const float DEFAULT_DESKTOP_TABLET_SCALE_PERCENT = 75.0f;
const bool DEFAULT_DESKTOP_TABLET_BECOMES_TOOLBAR = true;
const bool DEFAULT_HMD_TABLET_BECOMES_TOOLBAR = false;
const bool DEFAULT_PREFER_STYLUS_OVER_LASER = false;
const bool DEFAULT_PREFER_AVATAR_FINGER_OVER_STYLUS = false;
const QString DEFAULT_CURSOR_NAME = "DEFAULT";   // declared in ApplicationConstants.h
const bool DEFAULT_AWAY_STATE_WHEN_FOCUS_LOST_IN_VR_ENABLED = true;
const bool DEFAULT_LOAD_COMPLETE_ENTITY_TREE = true; 
const bool DEFAULT_BYPASS_PRIORITY_SORTING = false; 
const bool DEFAULT_BYPASS_SCRIPT_THROTTLING = false;

Application::Application(int& argc, char** argv, QElapsedTimer& startupTimer, bool runningMarkerExisted) :
    QApplication(argc, argv),
    _window(new MainWindow(desktop())),
    _sessionRunTimer(startupTimer),
#ifndef Q_OS_ANDROID
    _logger(new FileLogger(this)),
#endif
    _previousSessionCrashed(setupEssentials(runningMarkerExisted)),
    _entitySimulation(new PhysicalEntitySimulation()),
    _physicsEngine(new PhysicsEngine(Vectors::ZERO)),
    _previousScriptLocation("LastScriptLocation", DESKTOP_LOCATION),
    _fieldOfView("fieldOfView", DEFAULT_FIELD_OF_VIEW_DEGREES),
    _farClip("farClip", DEFAULT_FAR_CLIP),
    _nearClip("nearClip", DEFAULT_NEAR_CLIP),
    _hmdTabletScale("hmdTabletScale", DEFAULT_HMD_TABLET_SCALE_PERCENT),
    _desktopTabletScale("desktopTabletScale", DEFAULT_DESKTOP_TABLET_SCALE_PERCENT),
    _firstRun(Settings::firstRun, true),
    _desktopTabletBecomesToolbarSetting("desktopTabletBecomesToolbar", DEFAULT_DESKTOP_TABLET_BECOMES_TOOLBAR),
    _hmdTabletBecomesToolbarSetting("hmdTabletBecomesToolbar", DEFAULT_HMD_TABLET_BECOMES_TOOLBAR),
    _preferStylusOverLaserSetting("preferStylusOverLaser", DEFAULT_PREFER_STYLUS_OVER_LASER),
    _preferAvatarFingerOverStylusSetting("preferAvatarFingerOverStylus", DEFAULT_PREFER_AVATAR_FINGER_OVER_STYLUS),
    _constrainToolbarPosition("toolbar/constrainToolbarToCenterX", true),
    _awayStateWhenFocusLostInVREnabled("awayStateWhenFocusLostInVREnabled", DEFAULT_AWAY_STATE_WHEN_FOCUS_LOST_IN_VR_ENABLED),
    _preferredCursor("preferredCursor", DEFAULT_CURSOR_NAME),
    _loadCompleteEntityTreeSetting("loadCompleteEntityTree", DEFAULT_LOAD_COMPLETE_ENTITY_TREE),
    _bypassPrioritySortingSetting("bypassPrioritySorting", DEFAULT_BYPASS_PRIORITY_SORTING), 
    _bypassScriptThrottling("bypassScriptThrottling", DEFAULT_BYPASS_SCRIPT_THROTTLING), 
    _enableProcessOctreeThread(true),
    _lastSendDownstreamAudioStats(usecTimestampNow()),
    _snapshotSound(nullptr),
    _sampleSound(nullptr)
{
    setProperty(hifi::properties::CRASHED, _previousSessionCrashed);

    // The client comes up in ten stages, in this order; each is a function below and a group of rows in the start-up
    // profile (the phase marks inside it). A stage reads what it needs from the DependencyManager rather than from a
    // local of an earlier stage, so each one says what it depends on.
    setupProcess();           // the log handler, test arguments, the GPU's identity, the plug-in container, the thread pool, fonts
    startServices();          // the node list and address manager threads, the watchdog, the account manager, the domain check-in, the audio client
    connectServices();        // the messages client, the domain handler's and node list's signals, discoverability, the script engine initializer
    openWindow();             // the GL canvas and the window, the GL context, the display plug-ins, the audio scripting interface, the render engine
    setupInterface();         // the overlays, the sign-in connections, initializeUi, init
    startEntityProcessing();  // the octree processor and edit sender threads, the first update
    setupInput();             // controller actions and the input devices
    setupSettings();          // loadSettings, the settings thread, the audio interfaces, menu options
    installOperators();       // the callbacks the entity, render and pick libraries make into the application
    finishStartup();          // tablet sounds, the keyboard, URL handlers, the graphics engine, then the sign-in screen
}

// The log handler, test arguments, the GPU's identity, the plug-in container, the thread pool, fonts.
void Application::setupProcess() {
    connect(&_display, &Display::activeDisplayPluginChanged, this, &Application::activeDisplayPluginChanged);   // relayed: services connect to Application's signal in the essentials stage
    const ClientOptions& options = ClientOptions::get();

    StartupProfiler::phase("constructor: log handler, test arguments");

    LogHandler::getInstance().moveToThread(thread());
    LogHandler::getInstance().setupRepeatedMessageFlusher();

    if (!options.testScript.isEmpty()) {
        const QString& testScriptPath = options.testScript;
        // If the URL scheme is http(s) or ftp, then use as is, else - treat it as a local file
        // This is done so as not break previous command line scripts
        if (testScriptPath.left(HIFI_URL_SCHEME_HTTP.length()) == HIFI_URL_SCHEME_HTTP ||
            testScriptPath.left(HIFI_URL_SCHEME_FTP.length()) == HIFI_URL_SCHEME_FTP) {
            setProperty(hifi::properties::TEST, QUrl::fromUserInput(testScriptPath));
        } else if (QFileInfo(testScriptPath).exists()) {
            setProperty(hifi::properties::TEST, QUrl::fromLocalFile(testScriptPath));
        }
    }
    if (!options.testResultsLocation.isEmpty()) {
        // Set test snapshot location only if it is a writeable directory
        QFileInfo fileInfo(options.testResultsLocation);
        if (fileInfo.isDir() && fileInfo.isWritable()) {
            TestScriptingInterface::getInstance()->setTestResultsLocation(options.testResultsLocation);
        }
    }

    StartupProfiler::phase("constructor: GPU identification (GPUIdent)");
    {
        // identify gpu as early as possible to help identify OpenGL initialization errors.
        GPUIdent::getInstance();
    }

    StartupProfiler::phase("constructor: debug draw, plug-in container, thread pool, controller interface, clipboard");
    // make sure the debug draw singleton is initialized on the main thread.
    DebugDraw::getInstance().removeMarker("");

    PluginContainer* pluginContainer = dynamic_cast<PluginContainer*>(this); // set the container for any plugins that care
    PluginManager::getInstance()->setContainer(pluginContainer);

    QThreadPool::globalInstance()->setMaxThreadCount(MIN_PROCESSING_THREAD_POOL_SIZE);
    thread()->setPriority(QThread::HighPriority);
    thread()->setObjectName("Main Thread");

    setInstance(this);

    auto controllerScriptingInterface = DependencyManager::get<controller::ScriptingInterface>().data();
    _controllerScriptingInterface = dynamic_cast<ControllerScriptingInterface*>(controllerScriptingInterface);
    connect(PluginManager::getInstance().data(), &PluginManager::inputDeviceRunningChanged,
        controllerScriptingInterface, &controller::ScriptingInterface::updateRunningInputDevices);

    EntityTree::setEntityClicksCapturedOperator([this] {
        return _controllerScriptingInterface->areEntityClicksCaptured();
    });

#ifdef Q_OS_WIN
    installNativeEventFilter(&MyNativeEventFilter::getInstance());
#endif

    StartupProfiler::phase("constructor: 16 fonts");
    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/Cairo-SemiBold.ttf");
    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/fontawesome-webfont.ttf");
    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/Graphik-Medium.ttf");
    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/Graphik-Regular.ttf");
    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/Graphik-SemiBold.ttf");
    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/hifi-glyphs.ttf");
    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/rawline-500.ttf");

    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/Roboto-Bold.ttf");
    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/Roboto-Light.ttf");
    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/Roboto-Medium.ttf");
    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/Roboto-Regular.ttf");

    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/RobotoCondensed-Bold.ttf");
    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/RobotoCondensed-Light.ttf");
    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/RobotoCondensed-Regular.ttf");
    
    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "fonts/RobotoMono-Regular.ttf");

    QFontDatabase::addApplicationFont(PathUtils::resourcesPath() + "styles/Inconsolata.otf");
}

// The node list and address manager threads, the watchdog, the account manager, the domain check-in, the audio client.
void Application::startServices() {
    const ClientOptions& options = ClientOptions::get();

    StartupProfiler::phase("constructor: node list thread, address manager");
    _window->setWindowTitle(labelledTitle("Darkride"));
    applyClientLabelIcon();

    Model::setAbstractViewStateInterface(this); // The model class will sometimes need to know view state details from us

    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->startThread();
    nodeList->setFlagTimeForConnectionStep(true);

    // move the AddressManager to the NodeList thread so that domain resets due to domain changes always occur
    // before we tell MyAvatar to go to a new location in the new domain
    auto addressManager = DependencyManager::get<AddressManager>();
    addressManager->moveToThread(nodeList->thread());

    StartupProfiler::phase("constructor: deadlock watchdog");
    if (!options.disableWatchdog && !DeadlockWatchdogThread::disabledByEnvironment()) {
        DeadlockWatchdogThread::startWatching(this);
    }

    StartupProfiler::phase("constructor: avatar manager space, account manager and keypair, version log");
    int avatarSpaceStep = StartupProfiler::begin("avatar manager's workload space");
    // Set File Logger Session UUID
    auto avatarManager = DependencyManager::get<AvatarManager>();
    auto myAvatar = avatarManager ? avatarManager->getMyAvatar() : nullptr;
    if (avatarManager) {
        workload::SpacePointer space = getEntities()->getWorkloadSpace();
        avatarManager->setSpace(space);
    }
    StartupProfiler::end(avatarSpaceStep);
    auto accountManager = DependencyManager::get<AccountManager>();

    // set the account manager's root URL and trigger a login request if we don't have the access token
    int authStep = StartupProfiler::begin("account manager: auth URL, saved account, profile request");
    accountManager->setIsAgent(true);
    accountManager->setAuthURL(NetworkingConstants::METAVERSE_SERVER_URL());
    StartupProfiler::end(authStep);
    int keypairStep = StartupProfiler::begin("keypair check (a new pair is generated on the pool)");
    if (!accountManager->hasKeyPair()) {
        accountManager->generateNewUserKeypair();
    }
    StartupProfiler::end(keypairStep);
    int sessionStep = StartupProfiler::begin("session id, crash annotations, version log");

#ifndef Q_OS_ANDROID
    _logger->setSessionID(accountManager->getSessionID());
#endif

    qCDebug(interfaceapp) << "[VERSION] Build sequence:" << qPrintable(applicationVersion());
    qCDebug(interfaceapp) << "[VERSION] MODIFIED_ORGANIZATION:" << BuildInfo::MODIFIED_ORGANIZATION;
    qCDebug(interfaceapp) << "[VERSION] VERSION:" << BuildInfo::VERSION;
    qCDebug(interfaceapp) << "[VERSION] BUILD_TYPE_STRING:" << BuildInfo::BUILD_TYPE_STRING;
    qCDebug(interfaceapp) << "[VERSION] BUILD_GLOBAL_SERVICES:" << BuildInfo::BUILD_GLOBAL_SERVICES;
#if USE_STABLE_GLOBAL_SERVICES
    qCDebug(interfaceapp) << "[VERSION] We will use STABLE global services.";
#else
    qCDebug(interfaceapp) << "[VERSION] We will use DEVELOPMENT global services.";
#endif

    updateHeartbeat();

    StartupProfiler::end(sessionStep);
    StartupProfiler::phase("constructor: domain check-in timer, audio client thread");
    // setup a timer for domain-server check ins
    QTimer* domainCheckInTimer = new QTimer(this);
    connect(domainCheckInTimer, &QTimer::timeout, nodeList.data(), &NodeList::sendDomainServerCheckIn);
    domainCheckInTimer->start(DOMAIN_SERVER_CHECK_IN_MSECS);
    connect(this, &QCoreApplication::aboutToQuit, [domainCheckInTimer] {
        domainCheckInTimer->stop();
        domainCheckInTimer->deleteLater();
    });

    {
        auto audioIO = DependencyManager::get<AudioClient>().data();
        audioIO->setPositionGetter([] {
            auto avatarManager = DependencyManager::get<AvatarManager>();
            auto myAvatar = avatarManager ? avatarManager->getMyAvatar() : nullptr;

            return myAvatar ? myAvatar->getPositionForAudio() : Vectors::ZERO;
        });
        audioIO->setOrientationGetter([] {
            auto avatarManager = DependencyManager::get<AvatarManager>();
            auto myAvatar = avatarManager ? avatarManager->getMyAvatar() : nullptr;

            return myAvatar ? myAvatar->getOrientationForAudio() : Quaternions::IDENTITY;
        });

        recording::Frame::registerFrameHandler(AudioConstants::getAudioFrameName(), [audioIO](recording::Frame::ConstPointer frame) {
            audioIO->handleRecordedAudioInput(frame->data);
        });

        connect(audioIO, &AudioClient::inputReceived, [](const QByteArray& audio) {
            static auto recorder = DependencyManager::get<recording::Recorder>();
            if (recorder->isRecording()) {
                static const recording::FrameType AUDIO_FRAME_TYPE = recording::Frame::registerFrameType(AudioConstants::getAudioFrameName());
                recorder->recordFrame(AUDIO_FRAME_TYPE, audio);
            }
        });
        audioIO->startThread();
    }

    // Make sure we don't time out during slow operations at startup
    updateHeartbeat();
}

// The messages client, the domain handler's and node list's signals, discoverability, the script engine initializer.
void Application::connectServices() {
    const ClientOptions& options = ClientOptions::get();
    auto nodeList = DependencyManager::get<NodeList>();
    auto addressManager = DependencyManager::get<AddressManager>();
    auto accountManager = DependencyManager::get<AccountManager>();
    auto myAvatar = getMyAvatar();

    StartupProfiler::phase("constructor: messages client thread, domain handler connections");
    // Setup MessagesClient
    DependencyManager::get<MessagesClient>()->startThread();

    const DomainHandler& domainHandler = nodeList->getDomainHandler();

    connect(&domainHandler, &DomainHandler::domainURLChanged, &_domainSession, &DomainSession::domainURLChanged);
    connect(&domainHandler, &DomainHandler::redirectToErrorDomainURL, &_domainSession, &DomainSession::goToErrorDomainURL);
    connect(&domainHandler, &DomainHandler::resetting, &_domainSession, &DomainSession::resettingDomain);
    connect(&_domainSession, &DomainSession::worldLoadingChanged, this, &Application::worldLoadingChanged);   // relayed likewise
    connect(&_domainSession, &DomainSession::interstitialModeChanged, this, &Application::interstitialModeChanged);   // relayed: the Window scripting interface connects to Application in the essentials stage, before the session exists
    connect(&domainHandler, &DomainHandler::connectedToDomain, &_display, &Display::updateWindowTitle);
    connect(&domainHandler, &DomainHandler::connectedToDomain, this, [] { StartupProfiler::instant("connected to a domain"); });
    connect(&domainHandler, &DomainHandler::disconnectedFromDomain, &_display, &Display::updateWindowTitle);
    connect(&domainHandler, &DomainHandler::disconnectedFromDomain, this, [this]() {
        auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
        entityScriptingInterface->deleteEntity(getTabletScreenID());
        entityScriptingInterface->deleteEntity(getTabletHomeButtonID());
        entityScriptingInterface->deleteEntity(getTabletFrameID());
        _domainSession.disconnectedFromDomain();
    });

    connect(&domainHandler, &DomainHandler::connectedToDomain, &_domainSession, &DomainSession::connectedToDomain);
    connect(&domainHandler, &DomainHandler::domainConnectionRefused, &_domainSession, &DomainSession::domainConnectionRefused);

    nodeList->getDomainHandler().setErrorDomainURL(QUrl(REDIRECT_HIFI_ADDRESS));

    // We could clear ATP assets only when changing domains, but it's possible that the domain you are connected
    // to has gone down and switched to a new content set, so when you reconnect the cached ATP assets will no longer be valid.
    connect(&domainHandler, &DomainHandler::disconnectedFromDomain, DependencyManager::get<ScriptCache>().data(), &ScriptCache::clearATPScriptsFromCache);

    StartupProfiler::phase("constructor: discoverability, node list and account connections");
    // update our location every 5 seconds in the metaverse server, assuming that we are authenticated with one
    const qint64 DATA_SERVER_LOCATION_CHANGE_UPDATE_MSECS = 5 * MSECS_PER_SECOND;

    auto discoverabilityManager = DependencyManager::get<DiscoverabilityManager>();
    connect(&locationUpdateTimer, &QTimer::timeout, discoverabilityManager.data(), &DiscoverabilityManager::updateLocation);
    connect(&locationUpdateTimer, &QTimer::timeout, this, [this] {
        // nothing to remember while the sign-in screen is up, since no world is joined until someone signs in
        // (pauseUntilLoginDetermined); the last place stays saved for after
        if (!getLoginDialogPoppedUp()) {
            DependencyManager::get<AddressManager>()->storeCurrentAddress();
        }
    });
    locationUpdateTimer.start(DATA_SERVER_LOCATION_CHANGE_UPDATE_MSECS);

    // if we get a domain change, immediately attempt update location in metaverse server
    connect(&nodeList->getDomainHandler(), &DomainHandler::connectedToDomain,
        discoverabilityManager.data(), &DiscoverabilityManager::updateLocation);

    // send a location update immediately
    discoverabilityManager->updateLocation();

    connect(nodeList.data(), &NodeList::nodeAdded, &_domainSession, &DomainSession::nodeAdded);
    connect(nodeList.data(), &NodeList::nodeKilled, &_domainSession, &DomainSession::nodeKilled);
    connect(nodeList.data(), &NodeList::nodeActivated, &_domainSession, &DomainSession::nodeActivated);
    connect(nodeList.data(), &NodeList::uuidChanged, myAvatar.get(), &MyAvatar::setSessionUUID);
    connect(nodeList.data(), &NodeList::uuidChanged, &_domainSession, &DomainSession::setSessionUUID);
    connect(nodeList.data(), &NodeList::packetVersionMismatch, &_domainSession, &DomainSession::notifyPacketVersionMismatch);

    // you might think we could just do this in NodeList but we only want this connection for Interface
    connect(&nodeList->getDomainHandler(), &DomainHandler::limitOfSilentDomainCheckInsReached,
        nodeList.data(), [nodeList]() {nodeList->reset("Domain checkin limit"); });

    auto dialogsManager = DependencyManager::get<DialogsManager>();
#if defined(Q_OS_ANDROID)
    connect(accountManager.data(), &AccountManager::authRequired, this, []() {
        auto addressManager = DependencyManager::get<AddressManager>();
        AndroidHelper::instance().showLoginDialog(addressManager->currentAddress());
    });
#else
    connect(accountManager.data(), &AccountManager::authRequired, dialogsManager.data(), &DialogsManager::showLoginDialog);
#endif
    connect(accountManager.data(), &AccountManager::usernameChanged, &_display, &Display::updateWindowTitle);
    connect(accountManager.data(), &AccountManager::usernameChanged, this, [&](const QString username){
        auto avatarManager = DependencyManager::get<AvatarManager>();
        if (avatarManager) avatarManager->getMyAvatar()->setDisplayName(username);
    });

    // use our MyAvatar position and quat for address manager path
    addressManager->setPositionGetter([] {
        auto avatarManager = DependencyManager::get<AvatarManager>();
        auto myAvatar = avatarManager ? avatarManager->getMyAvatar() : nullptr;
        return myAvatar ? myAvatar->getWorldFeetPosition() : Vectors::ZERO;
    });
    addressManager->setOrientationGetter([] {
        auto avatarManager = DependencyManager::get<AvatarManager>();
        auto myAvatar = avatarManager ? avatarManager->getMyAvatar() : nullptr;
        return myAvatar ? myAvatar->getWorldOrientation() : glm::quat();
    });

    connect(addressManager.data(), &AddressManager::hostChanged, &_display, &Display::updateWindowTitle);

    // a hop to a place or a person the metaverse doesn't find goes nowhere: say why. It finds a world by its address (a
    // world's name is no address), and for a person it answers the same whether they are offline, out of sight or unknown
    connect(addressManager.data(), &AddressManager::lookupResultIsNotFound, this, [](const QString& lookedUp, bool isUser) {
        const QString name = lookedUp.trimmed();
        if (isUser) {
            OffscreenUi::asyncWarning(QString("Couldn't join %1").arg(name).trimmed(), "They're offline, or somewhere you can't join.");
        } else {
            OffscreenUi::asyncWarning(name.isEmpty() ? QString("Couldn't find that world")
                                                     : QString::fromUtf8("Couldn't find “%1”").arg(name),
                                      "Check the address, or choose a world in Explore.");
        }
    });
    connect(addressManager.data(), &AddressManager::lookupResultIsOffline, this, [](const QString& lookedUp) {
        const QString name = lookedUp.trimmed();
        OffscreenUi::asyncWarning((name.isEmpty() ? QString("That world") : name) + " is offline", "Try again later.");
    });
    connect(this, &QCoreApplication::aboutToQuit, addressManager.data(), &AddressManager::storeCurrentAddress);

    connect(this, &Application::activeDisplayPluginChanged, this, &Application::updateThreadPoolCount);
    connect(this, &Application::activeDisplayPluginChanged, this, [=](){
        qApp->setProperty(hifi::properties::HMD, qApp->isHMDMode());
        auto displayPlugin = qApp->getActiveDisplayPlugin();

        if (displayPlugin->isHmd()) {
            if (_preferredCursor.get() == Cursor::Manager::getIconName(Cursor::Icon::RETICLE)) {
                setPreferredCursor(Cursor::Manager::getIconName(Cursor::Icon::RETICLE));
            }
            else {
                setPreferredCursor(Cursor::Manager::getIconName(Cursor::Icon::ARROW));
            }
        }
        else {
            setPreferredCursor(Cursor::Manager::getIconName(Cursor::Icon::SYSTEM));
        }
    });
    connect(this, &Application::activeDisplayPluginChanged, this, &Application::updateSystemTabletMode);
    connect(this, &Application::activeDisplayPluginChanged, this, [&](){
        if (_signInGate.poppedUp()) {
            if (_firstRun.get()) {
                // display mode changed.  Don't allow auto-switch to work after this session.
                _firstRun.set(false);
            }
            _signInGate.displayPluginChanged();
        }
    });

    // Save avatar location immediately after a teleport.
    connect(myAvatar.get(), &MyAvatar::positionGoneTo,
        DependencyManager::get<AddressManager>().data(), &AddressManager::storeCurrentAddress);

    connect(myAvatar.get(), &MyAvatar::positionGoneTo, &_domainSession, &DomainSession::arrivedWithoutPhysics, Qt::QueuedConnection);

    StartupProfiler::phase("constructor: sample sound, script engine initializer, node interest set, options");
    // Inititalize sample before registering
    _sampleSound = DependencyManager::get<SoundCache>()->getSound(PathUtils::resourcesUrl("sounds/sample.wav"));

    {
        auto scriptEngines = DependencyManager::get<ScriptEngines>().data();
        scriptEngines->registerScriptInitializer([this](ScriptEnginePointer engine) {
            registerScriptEngineWithApplicationServices(engine);
        });

        connect(scriptEngines, &ScriptEngines::scriptCountChanged, this, [this] {
            auto scriptEngines = DependencyManager::get<ScriptEngines>();
            if (scriptEngines->getRunningScripts().isEmpty()) {
                getMyAvatar()->clearScriptableSettings();
            }
        }, Qt::QueuedConnection);

        connect(scriptEngines, &ScriptEngines::scriptsReloading, this, [this] {
            getEntities()->reloadEntityScripts();
            loadAvatarScripts(getMyAvatar()->getScriptUrls());
        }, Qt::QueuedConnection);
    }

#ifdef _WIN32
    WSADATA WsaData;
    int wsaresult = WSAStartup(MAKEWORD(2, 2), &WsaData);
#endif

    // tell the NodeList instance who to tell the domain server we care about
    nodeList->addSetOfNodeTypesToNodeInterestSet(NodeSet() << NodeType::AudioMixer << NodeType::AvatarMixer
        << NodeType::EntityServer << NodeType::AssetServer << NodeType::MessagesMixer << NodeType::EntityScriptServer
        << NodeType::PhysicsServer);


    ResourceCache::setRequestLimit(options.concurrentDownloads >= 0 ? (uint32_t)options.concurrentDownloads
                                                                   : MAX_CONCURRENT_RESOURCE_DOWNLOADS);

    // perhaps override the avatar url.  Since we will test later for validity
    // we don't need to do so here.
    QString avatarURL = options.avatarUrl;
    _avatarOverrideUrl = QUrl::fromUserInput(avatarURL);

    // If someone specifies both --avatarURL and --replaceAvatarURL,
    // the replaceAvatarURL wins.  So only set the _overrideUrl if this
    // does have a non-empty string.
    QString replaceURL = options.replaceAvatarUrl;
    if (!replaceURL.isEmpty()) {
        _avatarOverrideUrl = QUrl::fromUserInput(replaceURL);
        _saveAvatarOverrideUrl = true;
    }
}

// The GL canvas and the window, the GL context, the display plug-ins, the audio scripting interface, the render engine.
void Application::openWindow() {
    const ClientOptions& options = ClientOptions::get();

    StartupProfiler::phase("constructor: GL canvas, window restored and shown");
    // setDefaultFormat has no effect after the platform window has been created, so call it here.
    QSurfaceFormat::setDefaultFormat(getDefaultOpenGLSurfaceFormat());

    int canvasStep = StartupProfiler::begin("GL canvas and viewport container");
    _glWidget = new GLCanvas();
    getApplicationCompositor().setRenderingWidget(_glWidget);
    // the GL view sits in a container that can hold its size while build mode's panels are dragged
    _window->setCentralWidget(new ViewportContainer(_glWidget, _window));
    StartupProfiler::end(canvasStep);
    int shownStep = StartupProfiler::begin("window geometry restored and shown");

    _window->restoreGeometry();
    _window->setVisible(true);
    StartupProfiler::end(shownStep);
    // the window has been seen to end up at Qt's bare 640 x 480 after the start on macOS (2026-10-18), cause unknown:
    // where it is is logged once it has been shown and a little later, and a shrunk window is put back
    QTimer::singleShot(0, _window, [this] { _window->checkStartGeometry(); });
    QTimer::singleShot(3000, _window, [this] { _window->checkStartGeometry(); });

    _glWidget->setFocusPolicy(Qt::StrongFocus);
    _glWidget->setFocus();

    if (options.systemCursor) {
        _preferredCursor.set(Cursor::Manager::getIconName(Cursor::Icon::SYSTEM));
    }
    _windowCursor.show(Cursor::Manager::lookupIcon(_preferredCursor.get()));

    // enable mouse tracking; otherwise, we only get drag events
    _glWidget->setMouseTracking(true);
    // Make sure the window is set to the correct size by processing the pending events
    {
        StartupProfiler::Scope step("pending events processed (the window's first layout)");
        QCoreApplication::processEvents();
    }

    StartupProfiler::phase("constructor: initializeGL");
    // Create the main thread context, the GPU backend
    initializeGL();
    qCDebug(interfaceapp, "Initialized GL");

    StartupProfiler::phase("constructor: initializeDisplayPlugins");
    // Initialize the display plugin architecture
    initializeDisplayPlugins();
    qCDebug(interfaceapp, "Initialized Display");

    if (_display.plugin() && !_display.plugin()->isHmd()) {
        _preferredCursor.set(Cursor::Manager::getIconName(Cursor::Icon::SYSTEM));
        _windowCursor.show(Cursor::Manager::lookupIcon(_preferredCursor.get()));
    }
    StartupProfiler::phase("constructor: audio scripting interface");
    // An audio device changed signal received before the display plugins are set up will cause a crash,
    // so we defer the setup of the `scripting::Audio` class until this point
    {
        auto audioScriptingInterface = DependencyManager::set<AudioScriptingInterface, scripting::Audio>();
        auto audioIO = DependencyManager::get<AudioClient>().data();
        connect(audioIO, &AudioClient::mutedByMixer, audioScriptingInterface.data(), &AudioScriptingInterface::mutedByMixer);
        connect(audioIO, &AudioClient::receivedFirstPacket, audioScriptingInterface.data(), &AudioScriptingInterface::receivedFirstPacket);
        connect(audioIO, &AudioClient::disconnected, audioScriptingInterface.data(), &AudioScriptingInterface::disconnected);
        connect(audioIO, &AudioClient::muteEnvironmentRequested, [](glm::vec3 position, float radius) {
            auto audioClient = DependencyManager::get<AudioClient>();
            auto audioScriptingInterface = DependencyManager::get<AudioScriptingInterface>();
            auto myAvatarPosition = DependencyManager::get<AvatarManager>()->getMyAvatar()->getWorldPosition();
            float distance = glm::distance(myAvatarPosition, position);

            if (distance < radius) {
                audioClient->setMuted(true);
                audioScriptingInterface->environmentMuted();
            }
        });
        QSharedPointer<scripting::Audio> scriptingAudioSharedPointer = qSharedPointerDynamicCast<scripting::Audio>(DependencyManager::get<AudioScriptingInterface>());
        if (scriptingAudioSharedPointer) {
            connect(this, &Application::activeDisplayPluginChanged,
                scriptingAudioSharedPointer.data(), &scripting::Audio::onContextChanged);
        }
    }

    StartupProfiler::phase("constructor: initializeRenderEngine");
    // Create the rendering engine.  This can be slow on some machines due to lots of
    // GPU pipeline creation.
    initializeRenderEngine();
    qCDebug(interfaceapp, "Initialized Render Engine.");
}

// The overlays, the sign-in connections, initializeUi, init.
void Application::setupInterface() {
    const ClientOptions& options = ClientOptions::get();

    StartupProfiler::phase("constructor: overlays, sign-in connections");
    _overlays.init(); // do this before scripts load

    auto offscreenUi = getOffscreenUI();
    connect(offscreenUi.data(), &OffscreenUi::desktopReady, []() {
        // Now that we've loaded the menu and thus switched to the previous display plugin
        // we can unlock the desktop repositioning code, since all the positions will be
        // relative to the desktop size for this plugin
        auto offscreenUi = getOffscreenUI();
        auto desktop = offscreenUi->getDesktop();
        if (desktop) {
            desktop->setProperty("repositionLocked", false);
        }
    });

    connect(offscreenUi.data(), &OffscreenUi::keyboardFocusActive, [this]() {
#if !defined(Q_OS_ANDROID)
        // Darkride starts on the sign-in screen: until someone signs in, or continues as the remembered account, the
        // avatar is hidden, no world is joined and no scripts run (pauseUntilLoginDetermined), and the screen covers
        // the window. A launcher passing --tokens has signed in for us.
        if (ClientOptions::get().hasTokens) {
            _signInGate.resumeAfterLoginDialogActionTaken();
            return;
        }

        _signInGate.showLoginScreen();
#else
        _signInGate.resumeAfterLoginDialogActionTaken();
#endif
    });

#if !defined(Q_OS_ANDROID)
    // Signing out of a running viewer goes back to the sign-in screen, as at the start: out of the world, the avatar
    // hidden, until someone signs in, and then on as at the start (resumeAfterLoginDialogActionTaken).
    if (!options.hasTokens) {
        connect(DependencyManager::get<AccountManager>().data(), &AccountManager::logoutComplete, this, [this]() {
            if (getLoginDialogPoppedUp()) {
                return;
            }
            _signInGate.pauseUntilLoginDetermined();
            _signInGate.showLoginScreen();
        }, Qt::QueuedConnection);
    }
#endif

    StartupProfiler::phase("constructor: chat started");
    // Darkride chat (libraries/chat) follows the account from here: signed in to chat while it is signed in
    DependencyManager::get<ChatService>()->start();

    StartupProfiler::phase("constructor: initializeUi");
    // Initialize the user interface and menu system
    // Needs to happen AFTER the render engine initialization to access its configuration
    initializeUi();

    StartupProfiler::phase("constructor: init");
    init();
    qCDebug(interfaceapp, "init() complete.");
}

// The octree processor and edit sender threads, the first update.
void Application::startEntityProcessing() {
    auto myAvatar = getMyAvatar();

    StartupProfiler::phase("constructor: octree processor and edit sender threads, first update");
    // create thread for parsing of octree data independent of the main network and rendering threads
    _domainSession.octreeProcessor().initialize(_enableProcessOctreeThread);
    _entityEditSender.initialize(_enableProcessOctreeThread);

    _idleLoopStdev.reset();

    // update before the first render
    update(0);

    // Make sure we don't time out during slow operations at startup
    updateHeartbeat();

    _entityEditSender.setMyAvatar(myAvatar.get());

    // The entity octree will have to know about MyAvatar for the parentJointName import
    getEntities()->getTree()->setMyAvatar(myAvatar);
    _entityClipboard.tree()->setMyAvatar(myAvatar);

    // For now we're going to set the PPS for outbound packets to be super high, this is
    // probably not the right long term solution. But for now, we're going to do this to
    // allow you to move an entity around in your hand
    _entityEditSender.setPacketsPerSecond(3000); // super high!!

    // Make sure we don't time out during slow operations at startup
    updateHeartbeat();

    connect(this, SIGNAL(aboutToQuit()), this, SLOT(onAboutToQuit()));

    // FIXME -- I'm a little concerned about this.
    connect(myAvatar->getSkeletonModel().get(), &SkeletonModel::skeletonLoaded,
        this, &Application::checkSkeleton, Qt::QueuedConnection);
}

// Controller actions and the input devices.
void Application::setupInput() {
    const ClientOptions& options = ClientOptions::get();

    StartupProfiler::phase("constructor: controller actions, input devices");
    // Setup the userInputMapper with the actions
    auto userInputMapper = DependencyManager::get<UserInputMapper>();
    connect(userInputMapper.data(), &UserInputMapper::actionEvent, [this](int action, float state) {
        using namespace controller;
        auto tabletScriptingInterface = DependencyManager::get<TabletScriptingInterface>();
        QSharedPointer<scripting::Audio> audioScriptingInterface = qSharedPointerDynamicCast<scripting::Audio>(DependencyManager::get<AudioScriptingInterface>());
        {
            auto actionEnum = static_cast<Action>(action);
            int key = Qt::Key_unknown;
            static int lastKey = Qt::Key_unknown;
            bool navAxis = false;
            switch (actionEnum) {
                case Action::TOGGLE_PUSHTOTALK:
                    if (audioScriptingInterface) {
                        if (state > 0.0f) {
                            audioScriptingInterface->setPushingToTalk(true);
                        } else if (state <= 0.0f) {
                            audioScriptingInterface->setPushingToTalk(false);
                        }
                    }
                    break;

                case Action::UI_NAV_VERTICAL:
                    navAxis = true;
                    if (state > 0.0f) {
                        key = Qt::Key_Up;
                    } else if (state < 0.0f) {
                        key = Qt::Key_Down;
                    }
                    break;

                case Action::UI_NAV_LATERAL:
                    navAxis = true;
                    if (state > 0.0f) {
                        key = Qt::Key_Right;
                    } else if (state < 0.0f) {
                        key = Qt::Key_Left;
                    }
                    break;

                case Action::UI_NAV_GROUP:
                    navAxis = true;
                    if (state > 0.0f) {
                        key = Qt::Key_Tab;
                    } else if (state < 0.0f) {
                        key = Qt::Key_Backtab;
                    }
                    break;

                case Action::UI_NAV_BACK:
                    key = Qt::Key_Escape;
                    break;

                case Action::UI_NAV_SELECT:
                    key = Qt::Key_Return;
                    break;
                default:
                    break;
            }

            auto window = tabletScriptingInterface->getTabletWindow();
            if (navAxis && window) {
                if (lastKey != Qt::Key_unknown) {
                    QKeyEvent event(QEvent::KeyRelease, lastKey, Qt::NoModifier);
                    sendEvent(window, &event);
                    lastKey = Qt::Key_unknown;
                }

                if (key != Qt::Key_unknown) {
                    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
                    sendEvent(window, &event);
                    tabletScriptingInterface->processEvent(&event);
                    lastKey = key;
                }
            } else if (key != Qt::Key_unknown && window) {
                if (state) {
                    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
                    sendEvent(window, &event);
                    tabletScriptingInterface->processEvent(&event);
                } else {
                    QKeyEvent event(QEvent::KeyRelease, key, Qt::NoModifier);
                    sendEvent(window, &event);
                }
                return;
            }
        }

        if (action == controller::toInt(controller::Action::RETICLE_CLICK)) {
            auto reticlePos = getApplicationCompositor().getReticlePosition();
            QPoint localPos(reticlePos.x, reticlePos.y); // both hmd and desktop already handle this in our coordinates.
            if (state) {
                QMouseEvent mousePress(QEvent::MouseButtonPress, localPos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                sendEvent(_glWidget, &mousePress);
                _reticleClickPressed = true;
            } else {
                QMouseEvent mouseRelease(QEvent::MouseButtonRelease, localPos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                sendEvent(_glWidget, &mouseRelease);
                _reticleClickPressed = false;
            }
            return; // nothing else to do
        }

        if (state) {
            if (action == controller::toInt(controller::Action::TOGGLE_MUTE)) {
                auto audioClient = DependencyManager::get<AudioClient>();
                audioClient->setMuted(!audioClient->isMuted());
            } else if (action == controller::toInt(controller::Action::CYCLE_CAMERA)) {
                _viewState.cycleCamera();
            } else if (action == controller::toInt(controller::Action::CONTEXT_MENU) && !isInterstitialMode()) {
                toggleTabletUI();
            } else if (action == controller::toInt(controller::Action::RETICLE_X)) {
                auto oldPos = getApplicationCompositor().getReticlePosition();
                getApplicationCompositor().setReticlePosition({ oldPos.x + state, oldPos.y });
            } else if (action == controller::toInt(controller::Action::RETICLE_Y)) {
                auto oldPos = getApplicationCompositor().getReticlePosition();
                getApplicationCompositor().setReticlePosition({ oldPos.x, oldPos.y + state });
            } else if (action == controller::toInt(controller::Action::TOGGLE_OVERLAY)) {
                toggleOverlays();
            }
        }
    });

    _applicationStateDevice = userInputMapper->getStateDevice();

    _applicationStateDevice->setInputVariant(STATE_IN_HMD, []() -> float {
        return qApp->isHMDMode() ? 1 : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_CAMERA_FULL_SCREEN_MIRROR, []() -> float {
        return qApp->getCamera().getMode() == CAMERA_MODE_MIRROR ? 1 : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_CAMERA_FIRST_PERSON, []() -> float {
        return qApp->getCamera().getMode() == CAMERA_MODE_FIRST_PERSON ? 1 : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_CAMERA_FIRST_PERSON_LOOK_AT, []() -> float {
        return qApp->getCamera().getMode() == CAMERA_MODE_FIRST_PERSON_LOOK_AT ? 1 : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_CAMERA_THIRD_PERSON, []() -> float {
        return qApp->getCamera().getMode() == CAMERA_MODE_THIRD_PERSON ? 1 : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_CAMERA_LOOK_AT, []() -> float {
        return qApp->getCamera().getMode() == CAMERA_MODE_LOOK_AT ? 1 : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_CAMERA_SELFIE, []() -> float {
        return qApp->getCamera().getMode() == CAMERA_MODE_SELFIE ? 1 : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_CAMERA_ENTITY, []() -> float {
        return qApp->getCamera().getMode() == CAMERA_MODE_ENTITY ? 1 : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_CAMERA_INDEPENDENT, []() -> float {
        return qApp->getCamera().getMode() == CAMERA_MODE_INDEPENDENT ? 1 : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_SNAP_TURN, []() -> float {
        return qApp->getMyAvatar()->getSnapTurn() ? 1 : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_ADVANCED_MOVEMENT_CONTROLS, []() -> float {
        return qApp->getMyAvatar()->useAdvancedMovementControls() ? 1 : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_LEFT_HAND_DOMINANT, []() -> float {
        return qApp->getMyAvatar()->getDominantHand() == "left" ? 1 : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_RIGHT_HAND_DOMINANT, []() -> float {
        return qApp->getMyAvatar()->getDominantHand() == "right" ? 1 : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_STRAFE_ENABLED, []() -> float {
        return qApp->getMyAvatar()->getStrafeEnabled() ? 1 : 0;
    });

    _applicationStateDevice->setInputVariant(STATE_GROUNDED, []() -> float {
        return qApp->getMyAvatar()->getCharacterController()->onGround() ? 1 : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_NAV_FOCUSED, []() -> float {
        auto offscreenUi = getOffscreenUI();
        return offscreenUi ? (offscreenUi->navigationFocused() ? 1 : 0) : 0;
    });
    _applicationStateDevice->setInputVariant(STATE_PLATFORM_WINDOWS, []() -> float {
#if defined(Q_OS_WIN)
        return 1;
#else
        return 0;
#endif
    });
    _applicationStateDevice->setInputVariant(STATE_PLATFORM_MAC, []() -> float {
#if defined(Q_OS_MACOS)
        return 1;
#else
        return 0;
#endif
    });
    _applicationStateDevice->setInputVariant(STATE_PLATFORM_ANDROID, []() -> float {
#if defined(Q_OS_ANDROID)
        return 1 ;
#else
        return 0;
#endif
    });


    getRefreshRateManager().setRefreshRateRegime(RefreshRateManager::RefreshRateRegime::STARTUP);

    // Setup the _keyboardMouseDevice, _touchscreenDevice, _touchscreenVirtualPadDevice and the user input mapper with the default bindings
    userInputMapper->registerDevice(_keyboardMouseDevice->getInputDevice());
    // if the _touchscreenDevice is not supported it will not be registered
    if (_touchscreenDevice) {
        userInputMapper->registerDevice(_touchscreenDevice->getInputDevice());
    }
    if (_touchscreenVirtualPadDevice) {
        userInputMapper->registerDevice(_touchscreenVirtualPadDevice->getInputDevice());
    }

    _defaultScriptsLocation.setPath(options.scriptsPath);

    // Make sure we don't time out during slow operations at startup
    updateHeartbeat();
}

// loadSettings, the settings thread, the audio interfaces, menu options.
void Application::setupSettings() {
    StartupProfiler::phase("constructor: loadSettings");
    loadSettings();
    // the menu has just replayed its saved state (which build mode ignored); from here on it may be entered
    DependencyManager::get<Editor>()->setReady();

    updateVerboseLogging();

    // Make sure we don't time out during slow operations at startup
    updateHeartbeat();
    StartupProfiler::phase("constructor: settings thread, audio interfaces, menu options");
    QTimer* settingsTimer = new QTimer();
    moveToNewNamedThread(settingsTimer, "Settings Thread", [this, settingsTimer]{
        // This needs to run on the settings thread, so we need to pass the `settingsTimer` as the
        // receiver object, otherwise it will run on the application thread and trigger a warning
        // about trying to kill the timer on the main thread.
        connect(qApp, &Application::beforeAboutToQuit, settingsTimer, [this, settingsTimer]{
            // Disconnect the signal from the save settings
            QObject::disconnect(settingsTimer, &QTimer::timeout, this, &Application::saveSettings);
            // Stop the settings timer
            settingsTimer->stop();
            // Delete it (this will trigger the thread destruction
            settingsTimer->deleteLater();
            // Mark the settings thread as finished, so we know we can safely save in the main application
            // shutdown code
            _settingsGuard.trigger();
        });

        int SAVE_SETTINGS_INTERVAL = 10 * MSECS_PER_SECOND; // Let's save every seconds for now
        settingsTimer->setSingleShot(false);
        settingsTimer->setInterval(SAVE_SETTINGS_INTERVAL); // 10s, Qt::CoarseTimer acceptable
        QObject::connect(settingsTimer, &QTimer::timeout, this, &Application::saveSettings);
        settingsTimer->start();
    }, QThread::LowestPriority);

    if (Menu::getInstance()->isOptionChecked(MenuOption::FirstPersonLookAt)) {
        getMyAvatar()->setBoomLength(MyAvatar::ZOOM_MIN);  // So that camera doesn't auto-switch to third person.
    }

    {
        auto audioIO = DependencyManager::get<AudioClient>().data();
        // set the local loopback interface for local sounds
        AudioInjector::setLocalAudioInterface(audioIO);
        auto audioScriptingInterface = DependencyManager::get<AudioScriptingInterface>();
        audioScriptingInterface->setLocalAudioInterface(audioIO);
        connect(audioIO, &AudioClient::noiseGateOpened, audioScriptingInterface.data(), &AudioScriptingInterface::noiseGateOpened);
        connect(audioIO, &AudioClient::noiseGateClosed, audioScriptingInterface.data(), &AudioScriptingInterface::noiseGateClosed);
        connect(audioIO, &AudioClient::inputReceived, audioScriptingInterface.data(), &AudioScriptingInterface::inputReceived);
    }

    this->installEventFilter(this);


    Menu::getInstance()->setIsOptionChecked(MenuOption::ActionMotorControl, true);
    Menu::getInstance()->setIsOptionChecked(
        MenuOption::MaterialProceduralShaders,
        MeshPartPayload::DEFAULT_ENABLE_MATERIAL_PROCEDURAL_SHADERS
    );
}

// The callbacks the entity, render and pick libraries make into the application.
void Application::installOperators() {
    auto myAvatar = getMyAvatar();
    const DomainHandler& domainHandler = DependencyManager::get<NodeList>()->getDomainHandler();

    StartupProfiler::phase("constructor: pointer, entity and material operators");
    // If the user clicks on an object, we will check that it's a web surface, and if so, set the focus to it
    auto pointerManager = DependencyManager::get<PointerManager>();
    auto keyboardFocusOperator = [this](const QUuid& id, const PointerEvent& event) {
        if (event.shouldFocus()) {
            auto keyboard = DependencyManager::get<Keyboard>();
            if (getEntities()->wantsKeyboardFocus(id)) {
                _keyboardFocus.setFocusedEntity(id);
            } else if (!keyboard->containsID(id)) { // FIXME: this is a hack to make the keyboard work for now, since the keys would otherwise steal focus
                _keyboardFocus.setFocusedEntity(UNKNOWN_ENTITY_ID);
            }
        }
    };
    connect(pointerManager.data(), &PointerManager::triggerBeginEntity, keyboardFocusOperator);
    connect(pointerManager.data(), &PointerManager::triggerBeginOverlay, keyboardFocusOperator);

    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    connect(entityScriptingInterface.data(), &EntityScriptingInterface::deletingEntity, this, [this](const EntityItemID& entityItemID) {
        if (entityItemID == _keyboardFocus.focusedEntity()) {
            _keyboardFocus.setFocusedEntity(UNKNOWN_ENTITY_ID);
        }
    }, Qt::QueuedConnection);

    EntityTreeRenderer::setAddMaterialToEntityOperator([this](const QUuid& entityID, graphics::MaterialLayer material, const std::string& parentMaterialName) {
        if (_aboutToQuit) {
            return false;
        }

        auto renderable = getEntities()->renderableForEntityId(entityID);
        if (renderable) {
            renderable->addMaterial(material, parentMaterialName);
            return true;
        }

        return false;
    });
    EntityTreeRenderer::setRemoveMaterialFromEntityOperator([this](const QUuid& entityID, graphics::MaterialPointer material, const std::string& parentMaterialName) {
        if (_aboutToQuit) {
            return false;
        }

        auto renderable = getEntities()->renderableForEntityId(entityID);
        if (renderable) {
            renderable->removeMaterial(material, parentMaterialName);
            return true;
        }

        return false;
    });

    EntityTreeRenderer::setAddMaterialToAvatarOperator([](const QUuid& avatarID, graphics::MaterialLayer material, const std::string& parentMaterialName) {
        auto avatarManager = DependencyManager::get<AvatarManager>();
        auto avatar = static_pointer_cast<Avatar>(avatarManager->getAvatarBySessionID(avatarID));
        if (avatar) {
            avatar->addMaterial(material, parentMaterialName);
            return true;
        }
        return false;
    });
    EntityTreeRenderer::setRemoveMaterialFromAvatarOperator([](const QUuid& avatarID, graphics::MaterialPointer material, const std::string& parentMaterialName) {
        auto avatarManager = DependencyManager::get<AvatarManager>();
        auto avatar = static_pointer_cast<Avatar>(avatarManager->getAvatarBySessionID(avatarID));
        if (avatar) {
            avatar->removeMaterial(material, parentMaterialName);
            return true;
        }
        return false;
    });
    EntityTree::setGetUnscaledDimensionsForIDOperator([this](const QUuid& id) {
        if (_aboutToQuit) {
            return glm::vec3(1.0f);
        }

        auto entity = getEntities()->getEntity(id);
        if (entity) {
            return entity->getUnscaledDimensions();
        }

        auto avatarManager = DependencyManager::get<AvatarManager>();
        auto avatar = static_pointer_cast<Avatar>(avatarManager->getAvatarBySessionID(id));
        if (avatar) {
            return avatar->getSNScale();
        }
        return glm::vec3(1.0f);
    });
    Procedural::opaqueStencil = [](gpu::StatePointer state) { PrepareStencil::testMaskDrawShape(*state); };
    Procedural::transparentStencil = [](gpu::StatePointer state) { PrepareStencil::testMask(*state); };

    EntityTree::setGetEntityObjectOperator([this](const QUuid& id) -> QObject* {
        auto entities = getEntities();
        if (auto entity = entities->renderableForEntityId(id)) {
            return qobject_cast<QObject*>(entity.get());
        }
        return nullptr;
    });

    EntityTree::setEmitScriptEventOperator([this](const QUuid& id, const QVariant& message) {
        auto entities = getEntities();
        if (auto entity = entities->renderableForEntityId(id)) {
            entity->emitScriptEvent(message);
        }
    });

    EntityTree::setSendToQmlOperator([this](const QUuid& id, const QVariant& message) {
        auto entities = getEntities();
        if (auto entity = entities->renderableForEntityId(id)) {
            entity->sendToQml(message);
        }
    });

    EntityTree::setTextSizeOperator([this](const QUuid& id, const QString& text) {
        auto entities = getEntities();
        if (auto entity = entities->renderableForEntityId(id)) {
            if (auto renderable = std::dynamic_pointer_cast<render::entities::TextEntityRenderer>(entity)) {
                return renderable->textSize(text);
            }
        }
        return QSizeF(0.0f, 0.0f);
    });

    connect(this, &Application::aboutToQuit, [this]() {
        _keyboardFocus.setFocusedEntity(UNKNOWN_ENTITY_ID);
    });

    // Make sure we don't time out during slow operations at startup
    updateHeartbeat();

    OctreeEditPacketSender* packetSender = entityScriptingInterface->getPacketSender();
    EntityEditPacketSender* entityPacketSender = static_cast<EntityEditPacketSender*>(packetSender);
    entityPacketSender->setMyAvatar(myAvatar.get());

    connect(this, &Application::applicationStateChanged, &_display, &Display::activeChanged);
    connect(_window, &MainWindow::windowMinimizedChanged, &_display, &Display::windowMinimizedChanged);
    qCDebug(interfaceapp, "Startup time: %4.2f seconds.", (double)_sessionRunTimer.elapsed() / 1000.0);
    StartupProfiler::phase("constructor: fade function, snapshot sound, asset timers, picks, web-surface operators");

    EntityTreeRenderer::setEntitiesShouldFadeFunction([this]() {
        SharedNodePointer entityServerNode = DependencyManager::get<NodeList>()->soloNodeOfType(NodeType::EntityServer);
        return entityServerNode && !isPhysicsEnabled();
    });

    _snapshotSound = DependencyManager::get<SoundCache>()->getSound(PathUtils::resourcesUrl("sounds/snapshot/snap.wav"));

    connect(this, &QCoreApplication::aboutToQuit, &_assetImport, &AssetImport::addAssetToWorldMessageClose);
    connect(&domainHandler, &DomainHandler::domainURLChanged, &_assetImport, &AssetImport::addAssetToWorldMessageClose);
    connect(&domainHandler, &DomainHandler::redirectToErrorDomainURL, &_assetImport, &AssetImport::addAssetToWorldMessageClose);

    updateSystemTabletMode();

    connect(&_viewState.camera(), &Camera::modeUpdated, this, [this] { _viewState.cameraModeChanged(); });

    DependencyManager::get<PickManager>()->setShouldPickHUDOperator([]() { return DependencyManager::get<HMDScriptingInterface>()->isHMDMode(); });
    DependencyManager::get<PickManager>()->setCalculatePos2DFromHUDOperator([this](const glm::vec3& intersection) {
        const glm::vec2 MARGIN(25.0f);
        glm::vec2 maxPos = _controllerScriptingInterface->getViewportDimensions() - MARGIN;
        glm::vec2 pos2D = DependencyManager::get<HMDScriptingInterface>()->overlayFromWorldPoint(intersection);
        return glm::max(MARGIN, glm::min(pos2D, maxPos));
    });

    // Setup the mouse ray pick and related operators
    {
        auto mouseRayPick = std::make_shared<RayPick>(Vectors::ZERO, Vectors::UP, PickFilter(PickScriptingInterface::PICK_ENTITIES() | PickScriptingInterface::PICK_LOCAL_ENTITIES()), 0.0f, true);
        mouseRayPick->parentTransform = std::make_shared<MouseTransformNode>();
        mouseRayPick->setJointState(PickQuery::JOINT_STATE_MOUSE);
        auto mouseRayPickID = DependencyManager::get<PickManager>()->addPick(PickQuery::Ray, mouseRayPick);
        DependencyManager::get<EntityTreeRenderer>()->setMouseRayPickID(mouseRayPickID);
    }
    DependencyManager::get<EntityTreeRenderer>()->setMouseRayPickResultOperator([](unsigned int rayPickID) {
        RayToEntityIntersectionResult entityResult;
        entityResult.intersects = false;
        auto pickResult = DependencyManager::get<PickManager>()->getPrevPickResultTyped<RayPickResult>(rayPickID);
        if (pickResult) {
            entityResult.intersects = pickResult->type != IntersectionType::NONE;
            if (entityResult.intersects) {
                entityResult.intersection = pickResult->intersection;
                entityResult.distance = pickResult->distance;
                entityResult.surfaceNormal = pickResult->surfaceNormal;
                entityResult.entityID = pickResult->objectID;
                entityResult.extraInfo = pickResult->extraInfo;
            }
        }
        return entityResult;
    });
    DependencyManager::get<EntityTreeRenderer>()->setSetPrecisionPickingOperator([](unsigned int rayPickID, bool value) {
        DependencyManager::get<PickManager>()->setPrecisionPicking(rayPickID, value);
    });

    EntityItem::setBillboardRotationOperator([](const glm::vec3& position, const glm::quat& rotation, BillboardMode billboardMode, const glm::vec3& frustumPos) {
        if (billboardMode == BillboardMode::YAW) {
            //rotate about vertical to face the camera
            glm::vec3 dPosition = frustumPos - position;
            // If x and z are 0, atan(x, z) is undefined, so default to 0 degrees
            float yawRotation = dPosition.x == 0.0f && dPosition.z == 0.0f ? 0.0f : glm::atan(dPosition.x, dPosition.z);
            return glm::quat(glm::vec3(0.0f, yawRotation, 0.0f));
        } else if (billboardMode == BillboardMode::FULL) {
            // use the referencial from the avatar, y isn't always up
            glm::vec3 avatarUP = DependencyManager::get<AvatarManager>()->getMyAvatar()->getWorldOrientation() * Vectors::UP;
            // check to see if glm::lookAt will work / using glm::lookAt variable name
            glm::highp_vec3 s(glm::cross(position - frustumPos, avatarUP));

            // make sure s is not NaN for any component
            if (glm::length2(s) > 0.0f) {
                return glm::conjugate(glm::toQuat(glm::lookAt(frustumPos, position, avatarUP)));
            }
        }
        return rotation;
    });
    EntityItem::setPrimaryViewFrustumPositionOperator([this]() {
        ViewFrustum viewFrustum;
        _viewState.copyViewFrustum(viewFrustum);
        return viewFrustum.getPosition();
    });

    DependencyManager::get<UsersScriptingInterface>()->setKickConfirmationOperator([this] (const QUuid& nodeID) { userKickConfirmation(nodeID); });

    render::entities::WebEntityRenderer::setAcquireWebSurfaceOperator([=](const QString& url, bool htmlContent, QSharedPointer<OffscreenQmlSurface>& webSurface, bool& cachedWebSurface) {
        bool isTablet = url == TabletScriptingInterface::QML;
        if (htmlContent) {
            webSurface = DependencyManager::get<OffscreenQmlSurfaceCache>()->acquire(render::entities::WebEntityRenderer::QML);
            cachedWebSurface = true;
            auto rootItemLoadedFunctor = [url, webSurface] {
                webSurface->getRootItem()->setProperty(render::entities::WebEntityRenderer::URL_PROPERTY, url);
            };
            if (webSurface->getRootItem()) {
                rootItemLoadedFunctor();
            } else {
                QObject::connect(webSurface.data(), &hifi::qml::OffscreenSurface::rootContextCreated, rootItemLoadedFunctor);
            }
            auto surfaceContext = webSurface->getSurfaceContext();
            surfaceContext->setContextProperty("KeyboardScriptingInterface", DependencyManager::get<KeyboardScriptingInterface>().data());
        } else {
            // FIXME: the tablet should use the OffscreenQmlSurfaceCache
            webSurface = QSharedPointer<OffscreenQmlSurface>(new OffscreenQmlSurface(), [](OffscreenQmlSurface* webSurface) {
                AbstractViewStateInterface::instance()->sendLambdaEvent([webSurface] {
                    // WebEngineView may run other threads (wasapi), so they must be deleted for a clean shutdown
                    // if the application has already stopped its event loop, delete must be explicit
                    delete webSurface;
                });
            });
            auto rootItemLoadedFunctor = [webSurface, url, isTablet] {
                Application::setupQmlSurface(webSurface->getSurfaceContext(), isTablet || url == LOGIN_DIALOG.toString() || url == AVATAR_INPUTS_BAR_QML.toString() ||
                   url == BUBBLE_ICON_QML.toString());
            };
            if (webSurface->getRootItem()) {
                rootItemLoadedFunctor();
            } else {
                QObject::connect(webSurface.data(), &hifi::qml::OffscreenSurface::rootContextCreated, rootItemLoadedFunctor);
            }
            webSurface->load(url);
            cachedWebSurface = false;
        }
        
        // average target framerate for a 60 hz monitor 
        const uint8_t DEFAULT_MAX_FPS = 120;
        const uint8_t TABLET_FPS = 120;
        webSurface->setMaxFps(isTablet ? TABLET_FPS : DEFAULT_MAX_FPS);
    });
    render::entities::WebEntityRenderer::setReleaseWebSurfaceOperator([=](QSharedPointer<OffscreenQmlSurface>& webSurface, bool& cachedWebSurface, std::vector<QMetaObject::Connection>& connections) {
        QQuickItem* rootItem = webSurface->getRootItem();

        // Fix for crash in QtWebEngineCore when rapidly switching domains
        // Call stop on the QWebEngineView before destroying OffscreenQMLSurface.
        if (rootItem && !cachedWebSurface) {
            // stop loading
            QMetaObject::invokeMethod(rootItem, "stop");
        }

        webSurface->pause();

        for (auto& connection : connections) {
            QObject::disconnect(connection);
        }
        connections.clear();

        // If the web surface was fetched out of the cache, release it back into the cache
        if (cachedWebSurface) {
            // If it's going back into the cache make sure to explicitly set the URL to a blank page
            // in order to stop any resource consumption or audio related to the page.
            if (rootItem) {
                rootItem->setProperty("url", "about:blank");
            }
            auto offscreenCache = DependencyManager::get<OffscreenQmlSurfaceCache>();
            if (offscreenCache) {
                offscreenCache->release(render::entities::WebEntityRenderer::QML, webSurface);
            }
            cachedWebSurface = false;
        }
        webSurface.reset();
    });
}

// Tablet sounds, the keyboard, URL handlers, the graphics engine, then the sign-in screen.
void Application::finishStartup() {
    auto accountManager = DependencyManager::get<AccountManager>();

    StartupProfiler::phase("constructor: tablet sounds, keyboard, URL handlers, graphics engine start");
    // Preload Tablet sounds
    DependencyManager::get<EntityScriptingInterface>()->setEntityTree(qApp->getEntities()->getTree());
    DependencyManager::get<TabletScriptingInterface>()->preloadSounds();
    DependencyManager::get<Keyboard>()->createKeyboard();

    FileDialogHelper::setOpenDirectoryOperator([this](const QString& path) { openDirectory(path); });
    QDesktopServices::setUrlHandler("file", this, "showUrlHandler");
    QDesktopServices::setUrlHandler("", this, "showUrlHandler");
    auto drives = QDir::drives();
    for (auto drive : drives) {
        QDesktopServices::setUrlHandler(QUrl(drive.absolutePath()).scheme(), this, "showUrlHandler");
    }

    _pendingIdleEvent = false;
    _graphicsEngine.startup();

    qCDebug(interfaceapp) << "Metaverse session ID is" << uuidStringWithoutCurlyBraces(accountManager->getSessionID());

#if defined(Q_OS_ANDROID)
    connect(&AndroidHelper::instance(), &AndroidHelper::beforeEnterBackground, this, &Application::beforeEnterBackground);
    connect(&AndroidHelper::instance(), &AndroidHelper::enterBackground, this, &Application::enterBackground);
    connect(&AndroidHelper::instance(), &AndroidHelper::enterForeground, this, &Application::enterForeground);
    connect(&AndroidHelper::instance(), &AndroidHelper::toggleAwayMode, this, &Application::toggleAwayMode);
    AndroidHelper::instance().notifyLoadComplete();
#endif
    StartupProfiler::phase("constructor: pauseUntilLoginDetermined");
    _signInGate.pauseUntilLoginDetermined();
}

// init, which the interface stage (setupInterface) calls once the UI is up: the avatar manager, the entity tree
// renderer, the physics engine, the entity simulation and the game workload brought up, in that order.
void Application::init() {
    // Make sure Login state is up to date
    DependencyManager::get<DialogsManager>()->toggleLoginDialog();
    {
        StartupProfiler::Scope step("init: avatar manager");
        DependencyManager::get<AvatarManager>()->init();
    }

    _lastTimeUpdated.start();

    qCDebug(interfaceapp) << "Loaded settings";

    // fire off an immediate domain-server check in now that settings are loaded
    QMetaObject::invokeMethod(DependencyManager::get<NodeList>().data(), "sendDomainServerCheckIn");

    // This allows collision to be set up properly for shape entities supported by GeometryCache.
    // This is before entity setup to ensure that it's ready for whenever instance collision is initialized.
    ShapeEntityItem::setShapeInfoCalulator(ShapeEntityItem::ShapeInfoCalculator(&shapeInfoCalculator));

    {
        StartupProfiler::Scope step("init: entity tree renderer");
        getEntities()->init();
    }
    getEntities()->setEntityLoadingPriorityFunction([this, editor = DependencyManager::get<Editor>()](const EntityItem& item) {
        auto dims = item.getScaledDimensions();
        auto maxSize = glm::compMax(dims);

        if (maxSize <= 0.0f) {
            return 0.0f;
        }

        auto distance = glm::distance(getMyAvatar()->getWorldPosition(), item.getWorldPosition());
        // in build mode the camera roams far from the avatar: what it looks at loads first
        if (editor && editor->isActive()) {
            distance = std::min(distance, glm::distance(_viewState.camera().getPosition(), item.getWorldPosition()));
        }
        return atan2(maxSize, distance);
    });

    ObjectMotionState::setShapeManager(&_shapeManager);
    {
        StartupProfiler::Scope step("init: physics engine");
        _physicsEngine->init();
    }

    EntityTreePointer tree = getEntities()->getTree();
    {
        StartupProfiler::Scope step("init: entity simulation");
        _entitySimulation->init(tree, _physicsEngine, &_entityEditSender);
    }
    tree->setSimulation(_entitySimulation);

    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();

    // connect the _entityCollisionSystem to our EntityTreeRenderer since that's what handles running entity scripts
    connect(_entitySimulation.get(), &PhysicalEntitySimulation::entityCollisionWithEntity,
            getEntities().data(), &EntityTreeRenderer::entityCollisionWithEntity);

     // connect the _entities (EntityTreeRenderer) to our script engine's EntityScriptingInterface for firing
    // of events related clicking, hovering over, and entering entities
    getEntities()->connectSignalsToSlots(entityScriptingInterface.data());

    // Make sure any new sounds are loaded as soon as know about them.
    connect(tree.get(), &EntityTree::newCollisionSoundURL, this, [this](QUrl newURL, EntityItemID id) {
        getEntities()->setCollisionSound(id, DependencyManager::get<SoundCache>()->getSound(newURL));
    }, Qt::QueuedConnection);
    connect(getMyAvatar().get(), &MyAvatar::newCollisionSoundURL, this, [this](QUrl newURL) {
        if (auto avatar = getMyAvatar()) {
            auto sound = DependencyManager::get<SoundCache>()->getSound(newURL);
            avatar->setCollisionSound(sound);
        }
    }, Qt::QueuedConnection);

    {
        StartupProfiler::Scope step("init: game workload");
        _gameWorkload.startup(getEntities()->getWorkloadSpace(), _graphicsEngine.getRenderScene(), _entitySimulation);
    }
    _entitySimulation->setWorkloadSpace(getEntities()->getWorkloadSpace());
}

// The GL context and its share contexts, the display plug-ins' start and the render engine's start, which the window
// and interface stages call, and the plug-in lists from the command line (main calls initPlugins first).
OffscreenGLCanvas* _qmlShareContext { nullptr };

// FIXME hack access to the internal share context for the Chromium helper
// Normally we'd want to use QWebEngine::initialize(), but we can't because
// our primary context is a QGLWidget, which can't easily be initialized to share
// from a QOpenGLContext.
//
// So instead we create a new offscreen context to share with the QGLWidget,
// and manually set THAT to be the shared context for the Chromium helper
OffscreenGLCanvas* _chromiumShareContext { nullptr };

Q_GUI_EXPORT void qt_gl_set_global_share_context(QOpenGLContext *context);
Q_GUI_EXPORT QOpenGLContext *qt_gl_global_share_context();

void Application::initializeGL() {
    qCDebug(interfaceapp) << "Created Display Window.";


    // initialize glut for shape drawing; Qt apparently initializes it on OS X
    if (_isGLInitialized) {
        return;
    } else {
        _isGLInitialized = true;
    }

    _glWidget->windowHandle()->setFormat(getDefaultOpenGLSurfaceFormat());

    // When loading QtWebEngineWidgets, it creates a global share context on startup.
    // We have to account for this possibility by checking here for an existing
    // global share context
    auto globalShareContext = qt_gl_global_share_context();

    // Build a shared canvas / context for the Chromium processes
    if (!globalShareContext) {
        StartupProfiler::Scope step("initializeGL: Chromium share context");
        // Chromium rendering uses some GL functions that prevent nSight from capturing
        // frames, so we only create the shared context if nsight is NOT active.
        if (!nsightActive()) {
            _chromiumShareContext = new OffscreenGLCanvas();
            _chromiumShareContext->setObjectName("ChromiumShareContext");
            auto format =QSurfaceFormat::defaultFormat();
#ifdef Q_OS_MACOS
            // On mac, the primary shared OpenGL context must be a 3.2 core context,
            // or chromium flips out and spews error spam (but renders fine)
            format.setMajorVersion(3);
            format.setMinorVersion(2);
#endif
            _chromiumShareContext->setFormat(format);
            _chromiumShareContext->create();
            if (!_chromiumShareContext->makeCurrent()) {
                qCWarning(interfaceapp, "Unable to make chromium shared context current");
            }
            globalShareContext = _chromiumShareContext->getContext();
            qt_gl_set_global_share_context(globalShareContext);
            _chromiumShareContext->doneCurrent();
        }
    }


    {
        StartupProfiler::Scope step("initializeGL: window context");
        _glWidget->createContext(globalShareContext);

        if (!_glWidget->makeCurrent()) {
            qCWarning(interfaceapp, "Unable to make window context current");
        }

        // Populate the global OpenGL context based on the information for the primary window GL context
        gl::ContextInfo::get(true);
    }

    QStringList chromiumFlags;
    // HACK: re-expose mic and camera to prevent crash on domain-change in chromium's media::FakeAudioInputStream::ReadAudioFromSource()
    // This isn't necessary because there's a permission popup when the webcam is requested 
    // chromiumFlags << "--use-fake-device-for-media-stream";

    // Disable signed distance field font rendering on ATI/AMD GPUs, due to
    // a High Fidelity bug report: text drawn white in the web views on those GPUs
    std::string vendor{ (const char*)glGetString(GL_VENDOR) };
    if ((vendor.find("AMD") != std::string::npos) || (vendor.find("ATI") != std::string::npos)) {
        chromiumFlags << "--disable-distance-field-text";
    }

    // Enable autoplay
    chromiumFlags << "--autoplay-policy=no-user-gesture-required";

#ifdef Q_OS_MACOS
    // The web views' GPU start-up opened VideoToolbox's decoder and encoder services to list the hardware's video
    // formats, and that pair (~30 MB) stayed alive after the client quit, one per launch, until logout (bug #29,
    // performance 2026-10-25). Nothing is lost: this QtWebEngine has no H.264, and VP8/VP9 decode in software anyway
    // (a 1080p VP9 Web entity measured the same with and without).
    chromiumFlags << "--disable-accelerated-video-decode" << "--disable-accelerated-video-encode";
#endif

    // Enable this for debugging chromium
    // chromiumFlags << "--enable-logging" << "--log-level=0" << "--v=1";

    // DARKRIDE_CHROMIUM_FLAGS adds switches of the caller's: QTWEBENGINE_CHROMIUM_FLAGS is set here, so switches on the
    // command line or in that variable from outside never reach the web views (performance's VT-service tests, 10-25)
    const QString extraChromiumFlags = qEnvironmentVariable("DARKRIDE_CHROMIUM_FLAGS").trimmed();
    if (!extraChromiumFlags.isEmpty()) {
        chromiumFlags << extraChromiumFlags.split(' ', Qt::SkipEmptyParts);
    }

    // Ensure all Qt webengine processes launched from us have the appropriate command line flags
    if (!chromiumFlags.empty()) {
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", chromiumFlags.join(' ').toLocal8Bit());
    }

    if (!globalShareContext) {
        globalShareContext = _glWidget->qglContext();
        qt_gl_set_global_share_context(globalShareContext);
    }

    // Build a shared canvas / context for the QML rendering
    {
        StartupProfiler::Scope step("initializeGL: QML share context");
        _qmlShareContext = new OffscreenGLCanvas();
        _qmlShareContext->setObjectName("QmlShareContext");
        _qmlShareContext->create(globalShareContext);
        if (!_qmlShareContext->makeCurrent()) {
            qCWarning(interfaceapp, "Unable to make QML shared context current");
        }
        OffscreenQmlSurface::setSharedContext(_qmlShareContext->getContext());
        _qmlShareContext->doneCurrent();
        if (!_glWidget->makeCurrent()) {
            qCWarning(interfaceapp, "Unable to make window context current");
        }
    }


    // Build an offscreen GL context for the main thread.
    _glWidget->makeCurrent();
    glClearColor(0.2f, 0.2f, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    _glWidget->swapBuffers();

    {
        StartupProfiler::Scope step("initializeGL: GPU context and render thread (GraphicsEngine::initializeGPU)");
        _graphicsEngine.initializeGPU(_glWidget);
    }
}

// The display plug-ins' start, with the first frame: the plug-ins themselves are the Display's (Display.cpp).
void Application::initializeDisplayPlugins() {
    _display.initializePlugins();
    // Submit a default frame to render until the engine starts up
    updateRenderArgs(0.0f);
}

void Application::initializeRenderEngine() {
    // FIXME: on low end systems os the shaders take up to 1 minute to compile, so we pause the deadlock watchdog thread.
    DeadlockWatchdogThread::withPause([&] {
        {
            StartupProfiler::Scope step("initializeRenderEngine: the render engine and its pipelines (GraphicsEngine::initializeRender)");
            _graphicsEngine.initializeRender();
        }
        DependencyManager::get<Keyboard>()->registerKeyboardHighlighting();
    });
}

void Application::initPlugins(const ClientOptions& options) {
    if (!options.displays.isEmpty()) {
        qInfo() << "Setting prefered display plugins:" << options.displays;
        PluginManager::getInstance()->setPreferredDisplayPlugins(options.displays);
    }
    if (!options.disabledDisplays.isEmpty()) {
        qInfo() << "Disabling following display plugins:"  << options.disabledDisplays;
        PluginManager::getInstance()->disableDisplays(options.disabledDisplays);
    }
    if (!options.disabledInputs.isEmpty()) {
        qInfo() << "Disabling following input plugins:" << options.disabledInputs;
        PluginManager::getInstance()->disableInputs(options.disabledInputs);
    }
}
