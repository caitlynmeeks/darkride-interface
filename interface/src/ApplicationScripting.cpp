//
//  ApplicationScripting.cpp
//  interface/src
//
//  Application's script API: the context properties the desktop's root QML context gets (onDesktopRootContextCreated,
//  with the menu bar made once the context exists; onDesktopRootItemCreated, with the stats and the overlays shown),
//  the ones every other QML surface gets (setupQmlSurface) and the globals every script engine gets
//  (registerScriptEngineWithApplicationServices: the same names, bound to the same services). The members as they
//  stood in Application.cpp, in their order; a home of their own until an owner takes them (ARCHITECTURE.md §5.4
//  step 4).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "Application.h"
#include <thread>
#include <QtQml/QQmlContext>
#include <shared/QtHelpers.h>
#include <shared/GlobalAppProperties.h>
#include <AddressManager.h>
#include <AnimationCacheScriptingInterface.h>
#include <AvatarBookmarks.h>
#include <EntityScriptServerLogClient.h>
#include <EntityScriptingInterface.h>
#include <FileScriptingInterface.h>
#include <controllers/UserInputMapper.h>
#include <controllers/ScriptingInterface.h>
#include "LocationBookmarks.h"
#include <LocationScriptingInterface.h>
#include <MainWindow.h>
#include <MessagesClient.h>
#include <model-networking/ModelCacheScriptingInterface.h>
#include <procedural/MaterialCacheScriptingInterface.h>
#include <material-networking/TextureCacheScriptingInterface.h>
#include <OffscreenUi.h>
#include <PathUtils.h>
#include <plugins/InputConfiguration.h>
#include <RecordingScriptingInterface.h>
#include <SceneScriptingInterface.h>
#include <ScriptEngines.h>
#include <SoundCacheScriptingInterface.h>
#include <ui/TabletScriptingInterface.h>
#include <ui/ToolbarScriptingInterface.h>
#include <FileDialogHelper.h>
#include <UsersScriptingInterface.h>
#include <QmlWebWindowClass.h>
#include <QmlFragmentClass.h>
#include <Preferences.h>
#include <display-plugins/CompositorHelper.h>
#include "AudioClient.h"
#include "audio/AudioScope.h"
#include "avatar/AvatarManager.h"
#include "LODManager.h"
#include "scripting/Audio.h"
#include "scripting/TestScriptingInterface.h"
#include "scripting/PlatformInfoScriptingInterface.h"
#include "scripting/AssetMappingsScriptingInterface.h"
#include "scripting/ClipboardScriptingInterface.h"
#include "scripting/DesktopScriptingInterface.h"
#include "scripting/AccountServicesScriptingInterface.h"
#include "scripting/HMDScriptingInterface.h"
#include "scripting/MenuScriptingInterface.h"
#include "graphics-scripting/GraphicsScriptingInterface.h"
#include "scripting/SettingsScriptingInterface.h"
#include "scripting/WindowScriptingInterface.h"
#include "scripting/RatesScriptingInterface.h"
#include "scripting/SelectionScriptingInterface.h"
#include "editor/Editor.h"
#include "editor/ImpostorBaker.h"
#include "scripting/TTSScriptingInterface.h"
#include "scripting/KeyboardScriptingInterface.h"
#include "scripting/PerformanceScriptingInterface.h"
#include "scripting/RenderScriptingInterface.h"
#include "scripting/ChatScriptingInterface.h"
#include "scripting/DiskCacheScriptingInterface.h"
#include "avatar/AvatarThumbnails.h"
#include "ui/MetaverseRequests.h"
#include "ui/Skins.h"
#include "ui/SystemToolbar.h"
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
#endif
#include "ui/AvatarInputs.h"
#include "ui/DialogsManager.h"
#include "ui/Snapshot.h"
#include "ui/Stats.h"
#include "ui/AnimStats.h"
#include "ui/DomainConnectionModel.h"
#include "ui/Keyboard.h"
#include "ui/OctreeStatsProvider.h"
#include <raypick/RayPickScriptingInterface.h>
#include <raypick/LaserPointerScriptingInterface.h>
#include <raypick/PickScriptingInterface.h>
#include <raypick/PointerScriptingInterface.h>
#include <raypick/RayPick.h>
#include "ResourceRequestObserver.h"
#include <DesktopPreviewProvider.h>
#include "AboutUtil.h"
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

QSharedPointer<OffscreenUi> getOffscreenUI();   // Application.cpp's

void Application::onDesktopRootContextCreated(QQmlContext* surfaceContext) {
    auto engine = surfaceContext->engine();
    // in Qt 5.10.0 there is already an "Audio" object in the QML context
    // though I failed to find it (from QtMultimedia??). So..  let it be "AudioScriptingInterface"
    surfaceContext->setContextProperty("AudioScriptingInterface", DependencyManager::get<AudioScriptingInterface>().data());

    surfaceContext->setContextProperty("AudioStats", DependencyManager::get<AudioClient>()->getStats().data());
    surfaceContext->setContextProperty("AudioScope", DependencyManager::get<AudioScope>().data());

    surfaceContext->setContextProperty("Controller", DependencyManager::get<controller::ScriptingInterface>().data());
    surfaceContext->setContextProperty("Entities", DependencyManager::get<EntityScriptingInterface>().data());
    surfaceContext->setContextProperty("Performance", new PerformanceScriptingInterface());
    // Darkride's own panels (the Settings panel first): the disk cache's size and place, and the metaverse's API
    surfaceContext->setContextProperty("DiskCache", DependencyManager::get<DiskCacheScriptingInterface>().data());
    surfaceContext->setContextProperty("MetaverseRequests", DependencyManager::get<MetaverseRequests>().data());
    surfaceContext->setContextProperty("AvatarThumbnails", DependencyManager::get<AvatarThumbnails>().data());
    surfaceContext->setContextProperty("SystemToolbar", SystemToolbar::instance());
    surfaceContext->setContextProperty("Skins", DependencyManager::get<Skins>().data());
    _fileDownload = new FileScriptingInterface(engine);
    surfaceContext->setContextProperty("File", _fileDownload);
    connect(_fileDownload, &FileScriptingInterface::unzipResult, &_assetImport, &AssetImport::handleUnzip);
    surfaceContext->setContextProperty("MyAvatar", getMyAvatar().get());
    surfaceContext->setContextProperty("Messages", DependencyManager::get<MessagesClient>().data());
    surfaceContext->setContextProperty("Recording", DependencyManager::get<RecordingScriptingInterface>().data());
    surfaceContext->setContextProperty("Preferences", DependencyManager::get<Preferences>().data());
    surfaceContext->setContextProperty("AddressManager", DependencyManager::get<AddressManager>().data());
    surfaceContext->setContextProperty("FrameTimings", &_graphicsEngine._frameTimingsScriptingInterface);
    surfaceContext->setContextProperty("Rates", new RatesScriptingInterface(this));

    surfaceContext->setContextProperty("TREE_SCALE", TREE_SCALE);
    // FIXME Quat and Vec3 won't work with QJSEngine used by QML
    surfaceContext->setContextProperty("Quat", new Quat());
    surfaceContext->setContextProperty("Vec3", new Vec3());
    surfaceContext->setContextProperty("Uuid", new ScriptUUID());
    surfaceContext->setContextProperty("Assets", DependencyManager::get<AssetMappingsScriptingInterface>().data());
    surfaceContext->setContextProperty("Keyboard", DependencyManager::get<KeyboardScriptingInterface>().data());

    surfaceContext->setContextProperty("AvatarList", DependencyManager::get<AvatarManager>().data());
    surfaceContext->setContextProperty("Users", DependencyManager::get<UsersScriptingInterface>().data());

    surfaceContext->setContextProperty("Camera", &_viewState.camera());

    surfaceContext->setContextProperty("Overlays", &_overlays);
    surfaceContext->setContextProperty("Window", DependencyManager::get<WindowScriptingInterface>().data());
    surfaceContext->setContextProperty("Desktop", DependencyManager::get<DesktopScriptingInterface>().data());
    surfaceContext->setContextProperty("MenuInterface", MenuScriptingInterface::getInstance());
    surfaceContext->setContextProperty("Settings", new QMLSettingsScriptingInterface(surfaceContext));
    surfaceContext->setContextProperty("ScriptDiscoveryService", DependencyManager::get<ScriptEngines>().data());
    surfaceContext->setContextProperty("AvatarBookmarks", DependencyManager::get<AvatarBookmarks>().data());
    surfaceContext->setContextProperty("LocationBookmarks", DependencyManager::get<LocationBookmarks>().data());

    // Caches
    surfaceContext->setContextProperty("AnimationCache", DependencyManager::get<AnimationCacheScriptingInterface>().data());
    surfaceContext->setContextProperty("TextureCache", DependencyManager::get<TextureCacheScriptingInterface>().data());
    surfaceContext->setContextProperty("MaterialCache", DependencyManager::get<MaterialCacheScriptingInterface>().data());
    surfaceContext->setContextProperty("ModelCache", DependencyManager::get<ModelCacheScriptingInterface>().data());
    surfaceContext->setContextProperty("SoundCache", DependencyManager::get<SoundCacheScriptingInterface>().data());

    surfaceContext->setContextProperty("InputConfiguration", DependencyManager::get<InputConfiguration>().data());

    surfaceContext->setContextProperty("Account", AccountServicesScriptingInterface::getInstance()); // DEPRECATED - TO BE REMOVED
    surfaceContext->setContextProperty("GlobalServices", AccountServicesScriptingInterface::getInstance()); // DEPRECATED - TO BE REMOVED
    surfaceContext->setContextProperty("AccountServices", AccountServicesScriptingInterface::getInstance());

    surfaceContext->setContextProperty("DialogsManager", _dialogsManagerScriptingInterface);
    surfaceContext->setContextProperty("AvatarManager", DependencyManager::get<AvatarManager>().data());
    surfaceContext->setContextProperty("LODManager", DependencyManager::get<LODManager>().data());
    surfaceContext->setContextProperty("HMD", DependencyManager::get<HMDScriptingInterface>().data());
    surfaceContext->setContextProperty("Scene", DependencyManager::get<SceneScriptingInterface>().data());
    surfaceContext->setContextProperty("Render", RenderScriptingInterface::getInstance());
    surfaceContext->setContextProperty("PlatformInfo", PlatformInfoScriptingInterface::getInstance());
    surfaceContext->setContextProperty("Workload", _gameWorkload._engine->getConfiguration().get());
    surfaceContext->setContextProperty("Reticle", getApplicationCompositor().getReticleInterface());
    surfaceContext->setContextProperty("Snapshot", DependencyManager::get<Snapshot>().data());

    surfaceContext->setContextProperty("ApplicationCompositor", &getApplicationCompositor());

    surfaceContext->setContextProperty("AvatarInputs", AvatarInputs::getInstance());
    surfaceContext->setContextProperty("Selection", DependencyManager::get<SelectionScriptingInterface>().data());
    surfaceContext->setContextProperty("About", AboutUtil::getInstance());
    surfaceContext->setContextProperty("ResourceRequestObserver", DependencyManager::get<ResourceRequestObserver>().data());

    _window->setMenuBar(new Menu());
}

void Application::onDesktopRootItemCreated(QQuickItem* rootItem) {
    Stats::show();
    AnimStats::show();
    auto surfaceContext = getOffscreenUI()->getSurfaceContext();
    surfaceContext->setContextProperty("Stats", Stats::getInstance());
    surfaceContext->setContextProperty("AnimStats", AnimStats::getInstance());
    surfaceContext->setContextProperty("Editor", DependencyManager::get<Editor>().data());

#if !defined(Q_OS_ANDROID)
    auto offscreenUi = getOffscreenUI();
    auto qml = PathUtils::qmlUrl("AvatarInputsBar.qml");
    offscreenUi->show(qml, "AvatarInputsBar");
    // build mode's scene gizmo and view strip; hidden until build mode is on
    offscreenUi->show(PathUtils::qmlUrl("editor/ViewportOverlay.qml"), "EditorViewportOverlay");
#endif
}

void Application::setupQmlSurface(QQmlContext* surfaceContext, bool setAdditionalContextProperties) {
    surfaceContext->setContextProperty("Users", DependencyManager::get<UsersScriptingInterface>().data());
    surfaceContext->setContextProperty("HMD", DependencyManager::get<HMDScriptingInterface>().data());
    surfaceContext->setContextProperty("Preferences", DependencyManager::get<Preferences>().data());
    surfaceContext->setContextProperty("Vec3", new Vec3());
    surfaceContext->setContextProperty("Quat", new Quat());
    surfaceContext->setContextProperty("MyAvatar", DependencyManager::get<AvatarManager>()->getMyAvatar().get());
    surfaceContext->setContextProperty("Entities", DependencyManager::get<EntityScriptingInterface>().data());
    surfaceContext->setContextProperty("Snapshot", DependencyManager::get<Snapshot>().data());
    surfaceContext->setContextProperty("KeyboardScriptingInterface", DependencyManager::get<KeyboardScriptingInterface>().data());

    if (setAdditionalContextProperties) {
        qDebug() << "setting additional context properties!";
        auto tabletScriptingInterface = DependencyManager::get<TabletScriptingInterface>();
        auto flags = tabletScriptingInterface->getFlags();

        surfaceContext->setContextProperty("offscreenFlags", flags);
        surfaceContext->setContextProperty("AddressManager", DependencyManager::get<AddressManager>().data());
        surfaceContext->setContextProperty("Messages", DependencyManager::get<MessagesClient>().data());

        surfaceContext->setContextProperty("Settings", new QMLSettingsScriptingInterface(surfaceContext));
        surfaceContext->setContextProperty("MenuInterface", MenuScriptingInterface::getInstance());
        surfaceContext->setContextProperty("Performance", new PerformanceScriptingInterface());
        surfaceContext->setContextProperty("DiskCache", DependencyManager::get<DiskCacheScriptingInterface>().data());
        surfaceContext->setContextProperty("MetaverseRequests", DependencyManager::get<MetaverseRequests>().data());
        surfaceContext->setContextProperty("AvatarThumbnails", DependencyManager::get<AvatarThumbnails>().data());
        surfaceContext->setContextProperty("SystemToolbar", SystemToolbar::instance());
        surfaceContext->setContextProperty("Skins", DependencyManager::get<Skins>().data());

        surfaceContext->setContextProperty("Account", AccountServicesScriptingInterface::getInstance()); // DEPRECATED - TO BE REMOVED
        surfaceContext->setContextProperty("GlobalServices", AccountServicesScriptingInterface::getInstance()); // DEPRECATED - TO BE REMOVED
        surfaceContext->setContextProperty("AccountServices", AccountServicesScriptingInterface::getInstance());

        // in Qt 5.10.0 there is already an "Audio" object in the QML context
        // though I failed to find it (from QtMultimedia??). So..  let it be "AudioScriptingInterface"
        surfaceContext->setContextProperty("AudioScriptingInterface", DependencyManager::get<AudioScriptingInterface>().data());

        surfaceContext->setContextProperty("AudioStats", DependencyManager::get<AudioClient>()->getStats().data());
        surfaceContext->setContextProperty("fileDialogHelper", new FileDialogHelper());
        surfaceContext->setContextProperty("ScriptDiscoveryService", DependencyManager::get<ScriptEngines>().data());
        surfaceContext->setContextProperty("Assets", DependencyManager::get<AssetMappingsScriptingInterface>().data());
        surfaceContext->setContextProperty("LODManager", DependencyManager::get<LODManager>().data());
        surfaceContext->setContextProperty("OctreeStats", DependencyManager::get<OctreeStatsProvider>().data());
        surfaceContext->setContextProperty("DCModel", DependencyManager::get<DomainConnectionModel>().data());
        surfaceContext->setContextProperty("AvatarInputs", AvatarInputs::getInstance());
        surfaceContext->setContextProperty("AvatarList", DependencyManager::get<AvatarManager>().data());
        surfaceContext->setContextProperty("DialogsManager", DialogsManagerScriptingInterface::getInstance());
        surfaceContext->setContextProperty("InputConfiguration", DependencyManager::get<InputConfiguration>().data());
        surfaceContext->setContextProperty("SoundCache", DependencyManager::get<SoundCacheScriptingInterface>().data());
        surfaceContext->setContextProperty("AvatarBookmarks", DependencyManager::get<AvatarBookmarks>().data());
        surfaceContext->setContextProperty("Render", RenderScriptingInterface::getInstance());
        surfaceContext->setContextProperty("Workload", qApp->getGameWorkload()._engine->getConfiguration().get());
        surfaceContext->setContextProperty("Controller", DependencyManager::get<controller::ScriptingInterface>().data());
        surfaceContext->setContextProperty("Pointers", DependencyManager::get<PointerScriptingInterface>().data());
        surfaceContext->setContextProperty("Window", DependencyManager::get<WindowScriptingInterface>().data());
        surfaceContext->setContextProperty("Reticle", qApp->getApplicationCompositor().getReticleInterface());
        surfaceContext->setContextProperty("About", AboutUtil::getInstance());
        surfaceContext->setContextProperty("ResourceRequestObserver", DependencyManager::get<ResourceRequestObserver>().data());
        surfaceContext->setContextProperty("PlatformInfo", PlatformInfoScriptingInterface::getInstance());
        // This `module` context property is blank for the QML scripting interface so that we don't get log errors when importing
        // certain JS files from both scripts (in the JS context) and QML (in the QML context).
        surfaceContext->setContextProperty("module", "");
    }
}

void Application::registerScriptEngineWithApplicationServices(const ScriptEnginePointer& scriptEngine) {

    scriptEngine->setEmitScriptUpdatesFunction([this]() {
        SharedNodePointer entityServerNode = DependencyManager::get<NodeList>()->soloNodeOfType(NodeType::EntityServer);
        return !entityServerNode || isPhysicsEnabled();
    });

    // setup the packet sender of the script engine's scripting interfaces so
    // we can use the same ones from the application.
    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    entityScriptingInterface->setPacketSender(&_entityEditSender);
    entityScriptingInterface->setEntityTree(getEntities()->getTree());

    if (property(hifi::properties::TEST).isValid()) {
        scriptEngine->registerGlobalObject("Test", TestScriptingInterface::getInstance());
    }

    scriptEngine->registerGlobalObject("PlatformInfo", PlatformInfoScriptingInterface::getInstance());
    scriptEngine->registerGlobalObject("Rates", new RatesScriptingInterface(this));

    // hook our avatar and avatar hash map object into this script engine
    getMyAvatar()->registerMetaTypes(scriptEngine);

    scriptEngine->registerGlobalObject("AvatarList", DependencyManager::get<AvatarManager>().data());

    scriptEngine->registerGlobalObject("Camera", &_viewState.camera());

    ClipboardScriptingInterface* clipboardScriptable = new ClipboardScriptingInterface();
    scriptEngine->registerGlobalObject("Clipboard", clipboardScriptable);
    connect(scriptEngine.data(), &ScriptEngine::finished, clipboardScriptable, &ClipboardScriptingInterface::deleteLater);

    scriptEngine->registerGlobalObject("Overlays", &_overlays);
    qScriptRegisterMetaType(scriptEngine.data(), RayToOverlayIntersectionResultToScriptValue,
                            RayToOverlayIntersectionResultFromScriptValue);

    bool clientScript = scriptEngine->isClientScript();

    scriptEngine->registerGlobalObject("OffscreenFlags", getOffscreenUI()->getFlags());
    if (clientScript) {
        scriptEngine->registerGlobalObject("Desktop", DependencyManager::get<DesktopScriptingInterface>().data());
    } else {
        auto desktopScriptingInterface = new DesktopScriptingInterface(nullptr, true);
        scriptEngine->registerGlobalObject("Desktop", desktopScriptingInterface);
        if (QThread::currentThread() != thread()) {
            desktopScriptingInterface->moveToThread(thread());
        }
    }

    qScriptRegisterMetaType(scriptEngine.data(), wrapperToScriptValue<ToolbarProxy>, wrapperFromScriptValue<ToolbarProxy>);
    qScriptRegisterMetaType(scriptEngine.data(),
                            wrapperToScriptValue<ToolbarButtonProxy>, wrapperFromScriptValue<ToolbarButtonProxy>);
    scriptEngine->registerGlobalObject("Toolbars", DependencyManager::get<ToolbarScriptingInterface>().data());

    qScriptRegisterMetaType(scriptEngine.data(), wrapperToScriptValue<TabletProxy>, wrapperFromScriptValue<TabletProxy>);
    qScriptRegisterMetaType(scriptEngine.data(),
                            wrapperToScriptValue<TabletButtonProxy>, wrapperFromScriptValue<TabletButtonProxy>);
    scriptEngine->registerGlobalObject("Tablet", DependencyManager::get<TabletScriptingInterface>().data());
    // FIXME remove these deprecated names for the tablet scripting interface
    scriptEngine->registerGlobalObject("tabletInterface", DependencyManager::get<TabletScriptingInterface>().data());

    auto toolbarScriptingInterface = DependencyManager::get<ToolbarScriptingInterface>().data();
    DependencyManager::get<TabletScriptingInterface>().data()->setToolbarScriptingInterface(toolbarScriptingInterface);

    scriptEngine->registerGlobalObject("Window", DependencyManager::get<WindowScriptingInterface>().data());
    scriptEngine->registerGetterSetter("location", LocationScriptingInterface::locationGetter,
                        LocationScriptingInterface::locationSetter, "Window");
    // register `location` on the global object.
    scriptEngine->registerGetterSetter("location", LocationScriptingInterface::locationGetter,
                                       LocationScriptingInterface::locationSetter);

    scriptEngine->registerFunction("OverlayWindow", clientScript ? QmlWindowClass::constructor : QmlWindowClass::restricted_constructor);
#if !defined(Q_OS_ANDROID)
    scriptEngine->registerFunction("OverlayWebWindow", clientScript ? QmlWebWindowClass::constructor : QmlWebWindowClass::restricted_constructor);
#endif
    scriptEngine->registerFunction("QmlFragment", clientScript ? QmlFragmentClass::constructor : QmlFragmentClass::restricted_constructor);

    scriptEngine->registerGlobalObject("Menu", MenuScriptingInterface::getInstance());
    scriptEngine->registerGlobalObject("DesktopPreviewProvider", DependencyManager::get<DesktopPreviewProvider>().data());
    scriptEngine->registerGlobalObject("Stats", Stats::getInstance());
    scriptEngine->registerGlobalObject("Settings", SettingsScriptingInterface::getInstance());
    scriptEngine->registerGlobalObject("Snapshot", DependencyManager::get<Snapshot>().data());
    scriptEngine->registerGlobalObject("AudioStats", DependencyManager::get<AudioClient>()->getStats().data());
    scriptEngine->registerGlobalObject("AudioScope", DependencyManager::get<AudioScope>().data());
    scriptEngine->registerGlobalObject("AvatarBookmarks", DependencyManager::get<AvatarBookmarks>().data());
    scriptEngine->registerGlobalObject("LocationBookmarks", DependencyManager::get<LocationBookmarks>().data());

    scriptEngine->registerGlobalObject("RayPick", DependencyManager::get<RayPickScriptingInterface>().data());
    scriptEngine->registerGlobalObject("LaserPointers", DependencyManager::get<LaserPointerScriptingInterface>().data());
    scriptEngine->registerGlobalObject("Picks", DependencyManager::get<PickScriptingInterface>().data());
    scriptEngine->registerGlobalObject("Pointers", DependencyManager::get<PointerScriptingInterface>().data());
    scriptEngine->registerGlobalObject("TextToSpeech", DependencyManager::get<TTSScriptingInterface>().data());
    scriptEngine->registerGlobalObject("Chat", DependencyManager::get<ChatScriptingInterface>().data());
    scriptEngine->registerGlobalObject("DiskCache", DependencyManager::get<DiskCacheScriptingInterface>().data());

    // Caches
    scriptEngine->registerGlobalObject("AnimationCache", DependencyManager::get<AnimationCacheScriptingInterface>().data());
    scriptEngine->registerGlobalObject("TextureCache", DependencyManager::get<TextureCacheScriptingInterface>().data());
    scriptEngine->registerGlobalObject("MaterialCache", DependencyManager::get<MaterialCacheScriptingInterface>().data());
    scriptEngine->registerGlobalObject("ModelCache", DependencyManager::get<ModelCacheScriptingInterface>().data());
    scriptEngine->registerGlobalObject("SoundCache", DependencyManager::get<SoundCacheScriptingInterface>().data());

    scriptEngine->registerGlobalObject("DialogsManager", _dialogsManagerScriptingInterface);

    scriptEngine->registerGlobalObject("Account", AccountServicesScriptingInterface::getInstance()); // DEPRECATED - TO BE REMOVED
    scriptEngine->registerGlobalObject("GlobalServices", AccountServicesScriptingInterface::getInstance()); // DEPRECATED - TO BE REMOVED
    scriptEngine->registerGlobalObject("AccountServices", AccountServicesScriptingInterface::getInstance());
    qScriptRegisterMetaType(scriptEngine.data(), DownloadInfoResultToScriptValue, DownloadInfoResultFromScriptValue);

    scriptEngine->registerGlobalObject("AvatarManager", DependencyManager::get<AvatarManager>().data());

    scriptEngine->registerGlobalObject("LODManager", DependencyManager::get<LODManager>().data());
    qScriptRegisterMetaType(scriptEngine.data(), worldDetailQualityToScriptValue, worldDetailQualityFromScriptValue);

    scriptEngine->registerGlobalObject("Keyboard", DependencyManager::get<KeyboardScriptingInterface>().data());
    scriptEngine->registerGlobalObject("Performance", new PerformanceScriptingInterface());

    scriptEngine->registerGlobalObject("Paths", DependencyManager::get<PathUtils>().data());

    scriptEngine->registerGlobalObject("HMD", DependencyManager::get<HMDScriptingInterface>().data());
    scriptEngine->registerFunction("HMD", "getHUDLookAtPosition2D", HMDScriptingInterface::getHUDLookAtPosition2D, 0);
    scriptEngine->registerFunction("HMD", "getHUDLookAtPosition3D", HMDScriptingInterface::getHUDLookAtPosition3D, 0);

    scriptEngine->registerGlobalObject("Scene", DependencyManager::get<SceneScriptingInterface>().data());
    scriptEngine->registerGlobalObject("Render", RenderScriptingInterface::getInstance());
    scriptEngine->registerGlobalObject("Impostors", DependencyManager::get<ImpostorBaker>().data());
    scriptEngine->registerGlobalObject("Workload", _gameWorkload._engine->getConfiguration().get());

    GraphicsScriptingInterface::registerMetaTypes(scriptEngine.data());
    scriptEngine->registerGlobalObject("Graphics", DependencyManager::get<GraphicsScriptingInterface>().data());

    scriptEngine->registerGlobalObject("ScriptDiscoveryService", DependencyManager::get<ScriptEngines>().data());
    scriptEngine->registerGlobalObject("Reticle", getApplicationCompositor().getReticleInterface());

    scriptEngine->registerGlobalObject("Users", DependencyManager::get<UsersScriptingInterface>().data());

    auto scriptingInterface = DependencyManager::get<controller::ScriptingInterface>();
    scriptEngine->registerGlobalObject("Controller", scriptingInterface.data());
    UserInputMapper::registerControllerTypes(scriptEngine.data());

    auto recordingInterface = DependencyManager::get<RecordingScriptingInterface>();
    scriptEngine->registerGlobalObject("Recording", recordingInterface.data());

    auto entityScriptServerLog = DependencyManager::get<EntityScriptServerLogClient>();
    scriptEngine->registerGlobalObject("EntityScriptServerLog", entityScriptServerLog.data());
    scriptEngine->registerGlobalObject("AvatarInputs", AvatarInputs::getInstance());
    scriptEngine->registerGlobalObject("Selection", DependencyManager::get<SelectionScriptingInterface>().data());
    scriptEngine->registerGlobalObject("AddressManager", DependencyManager::get<AddressManager>().data());
    scriptEngine->registerGlobalObject("About", AboutUtil::getInstance());
    scriptEngine->registerGlobalObject("ResourceRequestObserver", DependencyManager::get<ResourceRequestObserver>().data());

    registerInteractiveWindowMetaType(scriptEngine.data());

    auto pickScriptingInterface = DependencyManager::get<PickScriptingInterface>();
    pickScriptingInterface->registerMetaTypes(scriptEngine.data());

    // connect this script engines printedMessage signal to the global ScriptEngines these various messages
    auto scriptEngines = DependencyManager::get<ScriptEngines>().data();
    connect(scriptEngine.data(), &ScriptEngine::printedMessage, scriptEngines, &ScriptEngines::onPrintedMessage);
    connect(scriptEngine.data(), &ScriptEngine::errorMessage, scriptEngines, &ScriptEngines::onErrorMessage);
    connect(scriptEngine.data(), &ScriptEngine::warningMessage, scriptEngines, &ScriptEngines::onWarningMessage);
    connect(scriptEngine.data(), &ScriptEngine::infoMessage, scriptEngines, &ScriptEngines::onInfoMessage);
    connect(scriptEngine.data(), &ScriptEngine::clearDebugWindow, scriptEngines, &ScriptEngines::onClearDebugWindow);

}
