//
//  Application.h
//  interface/src
//
//  Created by Andrzej Kapolka on 5/10/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Modified by Caitlyn Meeks on 12/24/19
//  Copyright 2019 Tivoli Cloud VR
//

#ifndef hifi_Application_h
#define hifi_Application_h

#include <functional>

#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtQuick/QQuickItem>

#include <QtGui/QImage>

#include <QtWidgets/QApplication>

#include <ThreadHelpers.h>
#include <AbstractScriptingServicesInterface.h>
#include <AbstractViewStateInterface.h>
#include <EntityEditPacketSender.h>
#include <EntityTreeRenderer.h>
#include <FileScriptingInterface.h>
#include <input-plugins/KeyboardMouseDevice.h>
#include <input-plugins/TouchscreenDevice.h>
#include <input-plugins/TouchscreenVirtualPadDevice.h>
#include <PhysicalEntitySimulation.h>
#include <PhysicsEngine.h>
#include <plugins/Forward.h>
#include <ui-plugins/PluginContainer.h>
#include <ScriptEngine.h>
#include <ScriptEngines.h>
#include <ShapeManager.h>
#include <SimpleMovingAverage.h>
#include <StDev.h>
#include <ViewFrustum.h>
#include <AbstractUriHandler.h>
#include <shared/RateCounter.h>
#include <ThreadSafeValueCache.h>
#include <shared/ConicalViewFrustum.h>
#include <shared/FileLogger.h>
#include <RunningMarker.h>

class ClientOptions;

#include "avatar/MyAvatar.h"
#include "ViewState.h"
#include "EntityClipboard.h"
#include "AssetImport.h"
#include "SignInGate.h"
#include "DomainSession.h"
#include "Display.h"
#include "CursorManager.h"
#include "WindowCursor.h"
#include "SnapshotRequests.h"
#include "KeyboardFocus.h"
#include "gpu/Context.h"
#include "Menu.h"
#include "PerformanceManager.h"
#include "RefreshRateManager.h"
#include "render/Engine.h"
#include "scripting/ControllerScriptingInterface.h"
#include "scripting/DialogsManagerScriptingInterface.h"
#include "ui/ApplicationOverlay.h"
#include "ui/EntityScriptServerLogDialog.h"
#include "ui/LodToolsDialog.h"
#include "ui/LogDialog.h"
#include "ui/OctreeStatsDialog.h"
#include "ui/OverlayConductor.h"
#include "ui/overlays/Overlays.h"

#include "workload/GameWorkload.h"
#include "graphics/GraphicsEngine.h"

#include <graphics/Skybox.h>
#include <ModelScriptingInterface.h>

#include "Sound.h"
#include "VisionSqueeze.h"

class GLCanvas;
class MainWindow;
class AssetUpload;
class CompositorHelper;
class AudioInjector;

namespace controller {
class StateController;
}

static const QString RUNNING_MARKER_FILENAME = "Interface.running";
static const QString SCRIPTS_SWITCH = "scripts";
// static const QString HIFI_NO_LOGIN_COMMAND_LINE_KEY = "no-login-suggestion";

class Application;
#if defined(qApp)
#undef qApp
#endif
#define qApp (static_cast<Application*>(QCoreApplication::instance()))

class Application : public QApplication,
                    public AbstractViewStateInterface,
                    public AbstractScriptingServicesInterface,
                    public AbstractUriHandler,
                    public PluginContainer {
    Q_OBJECT

public:
    // virtual functions required for PluginContainer
    virtual ui::Menu* getPrimaryMenu() override;
    virtual void requestReset() override { resetSensors(false); }
    virtual void showDisplayPluginsTools(bool show) override;
    virtual GLWidget* getPrimaryWidget() override;
    virtual MainWindow* getPrimaryWindow() override;
    virtual QOpenGLContext* getPrimaryContext() override;
    virtual bool makeRenderingContextCurrent() override;
    virtual bool isForeground() const override { return _display.isForeground(); }

    Display& getDisplay() { return _display; }   // the display plug-ins, the window's geometry and title, the headset (Display.h)
    virtual DisplayPluginPointer getActiveDisplayPlugin() const override { return _display.getActiveDisplayPlugin(); }

    // FIXME? Empty methods, do we still need them?
    static void initPlugins(const ClientOptions& options);

    Application(int& argc, char** argv, QElapsedTimer& startup_time, bool runningMarkerExisted);
    ~Application();

    void postLambdaEvent(const std::function<void()>& f) override;
    void sendLambdaEvent(const std::function<void()>& f) override;

    QString getPreviousScriptLocation();
    void setPreviousScriptLocation(const QString& previousScriptLocation);

    // Return an HTTP User-Agent string with OS and device information.
    Q_INVOKABLE QString getUserAgent();

    void initializeGL();
    void initializeDisplayPlugins();
    void initializeRenderEngine();
    void initializeUi();

    bool notify(QObject*, QEvent*) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;

    glm::uvec2 getCanvasSize() const { return _display.getCanvasSize(); }
    glm::uvec2 getUiSize() const { return _display.getUiSize(); }
    QRect getRecommendedHUDRect() const { return _display.getRecommendedHUDRect(); }
    glm::vec2 getDeviceSize() const { return _display.getDeviceSize(); }
    bool hasFocus() const { return _display.hasFocus(); }
    void setFocus() { _display.setFocus(); }
    void raise() { _display.raise(); }

    WindowCursor& getWindowCursor() { return _windowCursor; }   // the system cursor over the window (WindowCursor.h)

    ViewState& getViewState() { return _viewState; }   // the camera and what it sees: the view frustums, the conical views (ViewState.h)
    const ViewState& getViewState() const { return _viewState; }
    Camera& getCamera() { return _viewState.camera(); }
    const Camera& getCamera() const { return _viewState.camera(); }

    bool isMissingSequenceNumbers() { return _domainSession.isMissingSequenceNumbers(); }

    const ConicalViewFrustums& getConicalViews() const override { return _viewState.conicalViews(); }

    DomainSession& getDomainSession() { return _domainSession; }   // the session with a domain: the queries, the octree accounting, the modes, the physics gate (DomainSession.h)
    const OctreePacketProcessor& getOctreePacketProcessor() const { return _domainSession.octreeProcessor(); }
    QSharedPointer<EntityTreeRenderer> getEntities() const { return DependencyManager::get<EntityTreeRenderer>(); }
    GraphicsEngine& getGraphicsEngine() { return _graphicsEngine; }
    MainWindow* getWindow() const { return _window; }
    EntityClipboard& getEntityClipboard() { return _entityClipboard; }   // the entities' clipboard: export, import, paste (EntityClipboard.h)
    AssetImport& getAssetImport() { return _assetImport; }   // a file from a URL or a drop becomes an asset and an entity (AssetImport.h)
    EntityEditPacketSender* getEntityEditPacketSender() { return &_entityEditSender; }

    ApplicationOverlay& getApplicationOverlay() { return _applicationOverlay; }
    const ApplicationOverlay& getApplicationOverlay() const { return _applicationOverlay; }
    CompositorHelper& getApplicationCompositor() const;

    Overlays& getOverlays() { return _overlays; }

    PerformanceManager& getPerformanceManager() { return _performanceManager; }
    RefreshRateManager& getRefreshRateManager() { return _refreshRateManager; }

    size_t getRenderFrameCount() const { return _graphicsEngine.getRenderFrameCount(); }
    float getRenderLoopRate() const { return _graphicsEngine.getRenderLoopRate(); }
    float getNumCollisionObjects() const;

    static void setupQmlSurface(QQmlContext* surfaceContext, bool setAdditionalContextProperties);

    float getFieldOfView() { return _fieldOfView.get(); }
    void setFieldOfView(float fov);

    float getFarClip() { return _farClip.get(); }
    void setFarClip(float farClip);
    // the desktop camera's near clip; the HMD keeps DEFAULT_NEAR_CLIP for the hands. A longer one gives the depth
    // buffer precision in proportion (REVERSED-Z.md §2)
    float getNearClip() { return _nearClip.get(); }
    void setNearClip(float nearClip);

    float getHMDTabletScale() { return _hmdTabletScale.get(); }
    void setHMDTabletScale(float hmdTabletScale);
    float getDesktopTabletScale() { return _desktopTabletScale.get(); }
    void setDesktopTabletScale(float desktopTabletScale);

    bool getDesktopTabletBecomesToolbarSetting() { return _desktopTabletBecomesToolbarSetting.get(); }
    bool getLogWindowOnTopSetting() { return _keepLogWindowOnTop.get(); }
    void setLogWindowOnTopSetting(bool keepOnTop) { _keepLogWindowOnTop.set(keepOnTop); }
    void setDesktopTabletBecomesToolbarSetting(bool value);
    bool getHmdTabletBecomesToolbarSetting() { return _hmdTabletBecomesToolbarSetting.get(); }
    void setHmdTabletBecomesToolbarSetting(bool value);
    bool getPreferStylusOverLaser() { return _preferStylusOverLaserSetting.get(); }
    void setPreferStylusOverLaser(bool value);

    // TIVOLI new feature.  Activated from Developer > Tivoli >>>
    // TODO: Add this as an option to domain settings and switch on when you visit a domain
    // TODO: Make sure loading screen works
    bool getLoadCompleteEntityTreeSetting() { return _loadCompleteEntityTreeSetting.get(); }

    // Shortcircuits the code that does priority sorting in EntityTreeRenderer for faster
    // load and batch operations like zone culling all at once
    bool getForcedBypassPrioritySorting() { return _bypassPrioritySortingSetting.get(); }
    void setForcedBypassPrioritySorting(bool value) {
        _bypassPrioritySortingSetting.set(value); 
        DependencyManager::get<EntityTreeRenderer>()->setBypassPrioritySorting(value);
    }
    
    // Bypass the script engine throttling
    bool getBypassScriptThrottling() { return _bypassScriptThrottling.get(); }
    void setBypassScriptThrottling(bool value) {
        _bypassScriptThrottling.set(value); 
        DependencyManager::get<ScriptEngines>()->setBypassScriptThrottling(value);
    }

    bool getPreferAvatarFingerOverStylus() { return _preferAvatarFingerOverStylusSetting.get(); }
    void setPreferAvatarFingerOverStylus(bool value);


    float getSettingConstrainToolbarPosition() { return _constrainToolbarPosition.get(); }
    void setSettingConstrainToolbarPosition(bool setting);

    float getAwayStateWhenFocusLostInVREnabled() { return _awayStateWhenFocusLostInVREnabled.get(); }
    void setAwayStateWhenFocusLostInVREnabled(bool setting);

    Q_INVOKABLE void setMinimumGPUTextureMemStabilityCount(int stabilityCount) { _domainSession.setMinimumGPUTextureMemStabilityCount(stabilityCount); }   // the Test API's, by name

    NodeToOctreeSceneStats* getOcteeSceneStats() { return _domainSession.octreeSceneStats(); }


    virtual controller::ScriptingInterface* getControllerScriptingInterface() { return _controllerScriptingInterface; }
    virtual void registerScriptEngineWithApplicationServices(const ScriptEnginePointer& scriptEngine) override;

    virtual void copyCurrentViewFrustum(ViewFrustum& viewOut) const override { _viewState.copyDisplayViewFrustum(viewOut); }
    virtual QThread* getMainThread() override { return thread(); }
    virtual PickRay computePickRay(float x, float y) const override { return _viewState.computePickRay(x, y); }
    virtual glm::vec3 getAvatarPosition() const override;
    virtual qreal getDevicePixelRatio() override { return _display.getDevicePixelRatio(); }

    void setActiveDisplayPlugin(const QString& pluginName) { _display.setActiveDisplayPlugin(pluginName); }

#ifndef Q_OS_ANDROID
    FileLogger* getLogger() const { return _logger; }
#endif

    float getRenderResolutionScale() const { return _display.getRenderResolutionScale(); }

    qint64 getCurrentSessionRuntime() const { return _sessionRunTimer.elapsed(); }

    bool isAboutToQuit() const { return _aboutToQuit; }
    bool isPhysicsEnabled() const { return _domainSession.isPhysicsEnabled(); }
    bool settingsLoaded() const { return _settingsLoaded; }
    Setting::Handle<bool>& firstRun() { return _firstRun; }   // the first run's setting, read and cleared where the client goes first

    // While a loading screen is up, the arrival in a world waits for what is near the avatar to be ready for physics
    // and drawn (as interstitial mode does), even where the zone allows moving at once (ghosting).
    void setArrivalWaitsForNearby(bool wait) { _domainSession.setArrivalWaitsForNearby(wait); }
    PhysicsEnginePointer getPhysicsEngine() { return _physicsEngine; }

    // the isHMDMode is true whenever we use the interface from an HMD and not a standard flat display
    // rendering of several elements depend on that
    // TODO: carry that information on the Camera as a setting
    virtual bool isHMDMode() const override { return _display.isHMDMode(); }
    glm::mat4 getHMDSensorPose() const { return _display.getHMDSensorPose(); }
    glm::mat4 getEyeOffset(int eye) const { return _display.getEyeOffset(eye); }
    glm::mat4 getEyeProjection(int eye) const { return _display.getEyeProjection(eye); }

    virtual bool canAcceptURL(const QString& url) const override;
    virtual bool acceptURL(const QString& url, bool defaultUpload = false) override;

    void setMaxOctreePacketsPerSecond(int maxOctreePPS) { _domainSession.setMaxOctreePacketsPerSecond(maxOctreePPS); }
    int getMaxOctreePacketsPerSecond() const { return _domainSession.getMaxOctreePacketsPerSecond(); }

    render::ScenePointer getMain3DScene() override { return _graphicsEngine.getRenderScene(); }
    render::EnginePointer getRenderEngine() override { return _graphicsEngine.getRenderEngine(); }
    gpu::ContextPointer getGPUContext() const { return _graphicsEngine.getGPUContext(); }

    const GameWorkload& getGameWorkload() const { return _gameWorkload; }

    virtual void pushPostUpdateLambda(void* key, const std::function<void()>& func) override;

    void updateMyAvatarLookAtPosition(float deltaTime);

    float getGameLoopRate() const { return _gameLoopCounter.rate(); }
    // whether a ride is in the picture: this avatar seated or on a frame or vehicle, a moving assembly with parts
    // drawn in the view within the last second, or another avatar riding one in the view (RefreshRateManager: no
    // drop to the unfocused rate while the window is visible and a ride is in view; Caity, 2026-10-31). Main thread.
    bool isRideInView();
    float getPresentPeriod() const { return _display.getPresentPeriod(); }   // the period the presents are paced at, in seconds; 0 while they are not

    SnapshotRequests& getSnapshotRequests() { return _snapshotRequests; }   // the snapshots asked of the next frame (SnapshotRequests.h)

    QUuid getTabletScreenID() const;
    QUuid getTabletHomeButtonID() const;
    QUuid getTabletFrameID() const;
    QVector<QUuid> getTabletIDs() const;


    void setAvatarOverrideUrl(const QUrl& url, bool save);
    void clearAvatarOverrideUrl() {
        _avatarOverrideUrl = QUrl();
        _saveAvatarOverrideUrl = false;
    }
    QUrl getAvatarOverrideUrl() { return _avatarOverrideUrl; }
    bool getSaveAvatarOverrideUrl() { return _saveAvatarOverrideUrl; }
    void saveNextPhysicsStats(QString filename);

    bool isServerlessMode() const { return _domainSession.isServerlessMode(); }
    bool isInterstitialMode() const { return _domainSession.isInterstitialMode(); }
    // whole-world loading (the domain asks for a loading screen until the whole world is in): the domain session's
    bool isWorldLoading() const { return _domainSession.isWorldLoading(); }
    QVariantMap getWorldLoadingState() { return _domainSession.getWorldLoadingState(); }
    void showWorldNow() { _domainSession.showWorldNow(); }

    void loadAvatarScripts(const QVector<QString>& urls);
    void unloadAvatarScripts();

    Q_INVOKABLE void copyToClipboard(const QString& text);

    int getOtherAvatarsReplicaCount() { return DependencyManager::get<AvatarHashMap>()->getReplicaCount(); }
    void setOtherAvatarsReplicaCount(int count) { DependencyManager::get<AvatarHashMap>()->setReplicaCount(count); }

    SignInGate& getSignInGate() { return _signInGate; }   // the sign-in screen, the pause until an account is chosen, the resume (SignInGate.h)
    bool getLoginDialogPoppedUp() const { return _signInGate.poppedUp(); }
    // From the start, and again after signing out, until the viewer goes in (resumeAfterLoginDialogActionTaken): the
    // 3D view isn't drawn then, only Darkride's navy under the user interface (GraphicsEngine::render_performFrame)
    bool isSignInPending() const { return _signInGate.signInPending(); }
    const QDir& getDefaultScriptsLocation() const { return _defaultScriptsLocation; }   // --scripts, when given

    // Check if a headset is connected
    bool hasRiftControllers() { return _display.hasRiftControllers(); }
    bool hasViveControllers() { return _display.hasViveControllers(); }

#if defined(Q_OS_ANDROID)
    void beforeEnterBackground();
    void enterBackground();
    void enterForeground();
    void toggleAwayMode();
#endif

    void openDirectory(const QString& path);

    void overrideEntry();
    void forceDisplayName(const QString& displayName);
    void forceLoginWithTokens(const QString& tokens);

    // used by preferences and HMDScriptingInterface...
    VisionSqueeze& getVisionSqueeze() { return _visionSqueeze; }

signals:
    void svoImportRequested(const QString& url);

    void fullAvatarURLChanged(const QString& newValue, const QString& modelName);

    void beforeAboutToQuit();
    void activeDisplayPluginChanged();   // the Display's, relayed: services connect here in the essentials stage

    void uploadRequest(QString path);

    void interstitialModeChanged(bool isInInterstitialMode);   // the domain session's, relayed: the Window scripting interface connects here before the session exists
    void worldLoadingChanged(bool isWorldLoading);   // the domain session's, relayed likewise

    void awayStateWhenFocusLostInVRChanged(bool enabled);

public slots:
    void updateThreadPoolCount() const;
    void updateSystemTabletMode();
    void updateWindowTitle() const { _display.updateWindowTitle(); }   // the domain session and the start-up call it
    void onPresent(quint32 frameCount, quint64 presentSlot);   // the display plug-in's present callback, on its thread: the present's slot noted, the next idle and render events posted

    Q_INVOKABLE void loadDialog();
    void importAvatarDialog();
    void putAvatarInMyFiles();
    Q_INVOKABLE void loadScriptURLDialog() const;
    void toggleLogDialog();
    void recreateLogWindow(int);
    void toggleEntityScriptServerLogDialog();
    Q_INVOKABLE void showAssetServerWidget(QString filePath = "");
    Q_INVOKABLE SharedSoundPointer getSampleSound() const;

    void showDialog(const QUrl& widgetUrl, const QUrl& tabletUrl, const QString& name) const;

    void addAssetToWorldFromURL(QString url) { _assetImport.addAssetToWorldFromURL(url); }   // QML's ApplicationInterface.addAssetToWorldFromURL (the tablet and window roots, the web view)
    void wearAvatarLink(const QString& link);   // QML's ApplicationInterface.wearAvatarLink: the Avatar panel's Wear from Link…

    FileScriptingInterface* getFileDownloadInterface() { return _fileDownload; }

    // the hand-offs of later launches (SingleInstance): raise this window (the domain session goes to the address handed over)
    void raiseWindow() const { _display.raiseWindow(); }

    static void packageModel();

    void resetSensors(bool andReload = false);


#if (PR_BUILD || DEV_BUILD)
    void sendWrongProtocolVersionsSignature(bool checked) { ::sendWrongProtocolVersionsSignature(checked); }
#endif

    static void showHelp();

    void cameraMenuChanged() { _viewState.cameraMenuChanged(); }   // the View menu's camera items, connected by name in Menu.cpp
    void toggleOverlays();
    void setOverlaysVisible(bool visible);
    Q_INVOKABLE void centerUI();

    Q_INVOKABLE void requeryOctree() { _domainSession.requeryOctree(); }   // Redraw: the menu's, by name; MyAvatar's

    void reloadResourceCaches() { _domainSession.reloadResourceCaches(); }   // Reload Content: the menu's, by name; the Test API's

    void updateHeartbeat() const;

    static void deadlockApplication();
    static void unresponsiveApplication();  // cause main thread to be unresponsive for 35 seconds

    void rotationModeChanged() const { _viewState.rotationModeChanged(); }   // View > Center Player In View, connected by name in Menu.cpp

    static void runTests();

    KeyboardFocus& getKeyboardFocus() { return _keyboardFocus; }   // which entity has the keyboard, and its highlight (KeyboardFocus.h)
    std::shared_ptr<KeyboardMouseDevice> getKeyboardMouseDevice() const { return _keyboardMouseDevice; }

    void loadLODToolsDialog();
    void loadEntityStatisticsDialog();
    void loadDomainConnectionDialog();
    void showScriptLogs();

    const QString getPreferredCursor() const { return _preferredCursor.get(); }
    void setPreferredCursor(const QString& cursor);

    void updateVerboseLogging();

    QString getGraphicsCardType();

    bool gpuTextureMemSizeStable() { return _domainSession.gpuTextureMemSizeStable(); }   // the Test API's, by name
    void showUrlHandler(const QUrl& url);

    // used to test "shutdown" crash annotation.
    void crashOnShutdown();

private slots:
    void onDesktopRootItemCreated(QQuickItem* qmlContext);
    void onDesktopRootContextCreated(QQmlContext* qmlContext);
    void onAboutToQuit();
    void loadSettings();
    void saveSettings() const;

    bool acceptSnapshot(const QString& urlString);
    bool askToSetAvatarUrl(const QString& url);
    void askToWearAvatar(const QString& url, bool askFirst);
    bool askToLoadScript(const QString& scriptFilenameOrURL);

    bool askToWearAvatarAttachmentUrl(const QString& url);
    void displayAvatarAttachmentWarning(const QString& message) const;

    void updateDisplayMode() { _display.updateDisplayMode(); }   // the Output menu's items and the graphics engine reach it by name

    void setShowBulletWireframe(bool value);
    void setShowBulletAABBs(bool value);
    void setShowBulletContactPoints(bool value);
    void setShowBulletConstraints(bool value);
    void setShowBulletConstraintLimits(bool value);

    void setShowTrackedObjects(bool value);

private:
    void init();
    // the constructor's stages, in order (ApplicationStartup.cpp); each groups the start-up profile's rows
    void setupProcess();
    void startServices();
    void connectServices();
    void openWindow();
    void setupInterface();
    void startEntityProcessing();
    void setupInput();
    void setupSettings();
    void installOperators();
    void finishStartup();
    bool handleFileOpenEvent(QFileOpenEvent* event);
    void cleanupBeforeQuit();

    void idle();
    void update(float deltaTime);

    // Various helper functions called during update()
    void updateLOD(float deltaTime) const;
    void updateThreads(float deltaTime);
    void updateDialogs(float deltaTime) const;

    std::shared_ptr<MyAvatar> getMyAvatar() const;

    void checkSkeleton() const;


    bool importJSONFromURL(const QString& urlString);
    bool importSVOFromURL(const QString& urlString);
    bool importFromZIP(const QString& filePath);
    bool importImage(const QString& urlString);

    void keyPressEvent(QKeyEvent* event);
    void keyReleaseEvent(QKeyEvent* event);

    void focusOutEvent(QFocusEvent* event);
    void synthesizeKeyReleasEvents();

    void mouseMoveEvent(QMouseEvent* event);
    void mousePressEvent(QMouseEvent* event);
    void mouseDoublePressEvent(QMouseEvent* event);
    void mouseReleaseEvent(QMouseEvent* event);

    void touchBeginEvent(QTouchEvent* event);
    void touchEndEvent(QTouchEvent* event);
    void touchUpdateEvent(QTouchEvent* event);
    void touchGestureEvent(QGestureEvent* event);

    void wheelEvent(QWheelEvent* event) const;
    void dropEvent(QDropEvent* event);
    static void dragEnterEvent(QDragEnterEvent* event);

    void maybeToggleMenuVisible(QMouseEvent* event) const;
    void toggleTabletUI(bool shouldOpen = false) const;

    void userKickConfirmation(const QUuid& nodeID);

    MainWindow* _window;
    Display _display;   // the display plug-ins, the window's geometry and title, the headset (Display.h); before the essentials stage, which may ask it for the active plug-in
    QElapsedTimer& _sessionRunTimer;

    bool _aboutToQuit{ false };

#ifndef Q_OS_ANDROID
    FileLogger* _logger{ nullptr };
#endif

    bool _previousSessionCrashed;

    // Frame Rate Measurement
    RateCounter<500> _gameLoopCounter;

    QElapsedTimer _lastTimeUpdated;
    std::atomic<uint64_t> _lastIdleDurationUsecs { 0 };   // the main thread's last frame (idle), for the refresh governor's fit

    ShapeManager _shapeManager;
    PhysicalEntitySimulationPointer _entitySimulation;
    PhysicsEnginePointer _physicsEngine;

    EntityClipboard _entityClipboard;   // the entities' clipboard (EntityClipboard.h)

    ViewState _viewState;   // the camera, the view frustums and the conical views (ViewState.h)
    DomainSession _domainSession;   // the session with a domain (DomainSession.h)

    std::shared_ptr<controller::StateController>
        _applicationStateDevice;  // Default ApplicationDevice reflecting the state of different properties of the session
    std::shared_ptr<KeyboardMouseDevice>
        _keyboardMouseDevice;  // Default input device, the good old keyboard mouse and maybe touchpad
    std::shared_ptr<TouchscreenDevice> _touchscreenDevice;  // the good old touchscreen
    std::shared_ptr<TouchscreenVirtualPadDevice> _touchscreenVirtualPadDevice;

    Setting::Handle<QString> _previousScriptLocation;
    Setting::Handle<float> _fieldOfView;
    Setting::Handle<float> _farClip;
    Setting::Handle<float> _nearClip;
    Setting::Handle<float> _hmdTabletScale;
    Setting::Handle<float> _desktopTabletScale;
    Setting::Handle<bool> _firstRun;
    Setting::Handle<bool> _desktopTabletBecomesToolbarSetting;
    Setting::Handle<bool> _hmdTabletBecomesToolbarSetting;
    Setting::Handle<bool> _preferStylusOverLaserSetting;
    Setting::Handle<bool> _preferAvatarFingerOverStylusSetting;
    Setting::Handle<bool> _constrainToolbarPosition;
    Setting::Handle<bool> _awayStateWhenFocusLostInVREnabled;
    Setting::Handle<QString> _preferredCursor;
    Setting::Handle<bool> _keepLogWindowOnTop{ "keepLogWindowOnTop", false };
    // TIVOLI SPECIFIC
    Setting::Handle<bool> _loadCompleteEntityTreeSetting; 
    Setting::Handle<bool> _bypassPrioritySortingSetting;
    Setting::Handle<bool> _bypassScriptThrottling;

    QHash<int, QKeyEvent> _keysPressed;

    bool _enableProcessOctreeThread;
    EntityEditPacketSender _entityEditSender;

    StDev _idleLoopStdev;

    ControllerScriptingInterface* _controllerScriptingInterface{ nullptr };
    QPointer<LogDialog> _logDialog;
    QPointer<EntityScriptServerLogDialog> _entityScriptServerLogDialog;
    QDir _defaultScriptsLocation;

    TouchEvent _lastTouchEvent;

    quint64 _lastSendDownstreamAudioStats;

    ConditionalGuard _settingsGuard;

    GLCanvas* _glWidget{ nullptr };

    typedef bool (Application::*AcceptURLMethod)(const QString&);
    static const std::vector<std::pair<QString, Application::AcceptURLMethod>> _acceptedExtensions;

    SignInGate _signInGate;   // the sign-in screen and the pause and resume around it (SignInGate.h)
    PerformanceManager _performanceManager;
    RefreshRateManager _refreshRateManager;

    GameWorkload _gameWorkload;

    GraphicsEngine _graphicsEngine;
    void updateRenderArgs(float deltaTime);

    Overlays _overlays;
    ApplicationOverlay _applicationOverlay;
    OverlayConductor _overlayConductor;

    DialogsManagerScriptingInterface* _dialogsManagerScriptingInterface = new DialogsManagerScriptingInterface();

    KeyboardFocus _keyboardFocus;
    bool _isGLInitialized{ false };

    // Whole-world loading, per arrival (reset with the physics-ready information). The choice is the domain's
    // ("loading.whole_world" and "loading.time_limit" in its settings, sent to the client when it connects).
    struct WorldLoading {
        bool settingsKnown { false };   // the domain's settings have arrived, or we stopped waiting for them
        bool active { false };          // the loading screen is up and the entity query asks for the whole world
        bool skipRequested { false };   // "Show me now"
        quint64 waitStart { 0 };        // since when we are connected and waiting for the domain's settings
        quint64 started { 0 };
        quint64 deadline { 0 };
        int timeLimit { 0 };            // seconds
        float radius { 0.0f };          // metres round the avatar whose models are waited for; 0 for all
        quint64 lastQuery { 0 };
    } _worldLoading;

    bool _reticleClickPressed{ false };

    int _avatarAttachmentRequest = 0;

    bool _settingsLoaded{ false };

    WindowCursor _windowCursor;

    std::map<void*, std::function<void()>> _postUpdateLambdas;
    std::mutex _postUpdateLambdasLock;

    bool _keyboardDeviceHasFocus{ true };

    AssetImport _assetImport;   // a file from a URL or a drop becomes an asset and an entity (AssetImport.h)

    FileScriptingInterface* _fileDownload;
    SharedSoundPointer _snapshotSound;
    SharedSoundPointer _sampleSound;
    SnapshotRequests _snapshotRequests;

    QUrl _avatarOverrideUrl;
    bool _saveAvatarOverrideUrl{ false };

    std::atomic<bool> _pendingIdleEvent{ true };
    // the slot (the paced refresh interval) of the latest present, from the present thread (onPresent), and the
    // one the last update was for: the simulation advances by the intervals between them (update)
    std::atomic<uint64_t> _presentSlot{ 0 };
    uint64_t _lastUpdateSlot{ 0 };

    bool _showTrackedObjects{ false };
    bool _prevShowTrackedObjects{ false };

    VisionSqueeze _visionSqueeze;

    bool _crashOnShutdown{ false };
};
#endif  // hifi_Application_h
