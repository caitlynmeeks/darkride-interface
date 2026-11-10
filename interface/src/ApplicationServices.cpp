//
//  ApplicationServices.cpp
//  interface/src
//
//  The client's services, registered with the DependencyManager in this order at start-up. This is the list that
//  setupEssentials ran inline since High Fidelity's days (ARCHITECTURE.md §3.1, §5.4 step 3), now in one place with
//  its groups named: what a group is for, and where the order matters, why. The start-up profile times each line.
//  Nothing here starts a thread or touches the window: that is the constructor's, afterwards.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ApplicationServices.h"

#include <AccountManager.h>
#include <ChatService.h>
#include <FriendsService.h>
#include <AddressManager.h>
#include <AnimationCacheScriptingInterface.h>
#include <AudioClient.h>
#include <AudioInjectorManager.h>
#include "AvatarBookmarks.h"
#include <AvatarHashMap.h>
#include <DeferredLightingEffect.h>
#include "DiscoverabilityManager.h"
#include <EntityDynamicFactoryInterface.h>
#include <EntityScriptClient.h>
#include <EntityScriptServerLogClient.h>
#include <EntityScriptingInterface.h>
#include <EntityTreeRenderer.h>
#include <FadeEffect.h>
#include <FramebufferCache.h>
#include <GeometryCache.h>
#include "LODManager.h"
#include "LocationBookmarks.h"
#include <MessagesClient.h>
#include <NodeList.h>
#include <OffscreenUi.h>
#include <PathUtils.h>
#include <PickManager.h>
#include <PointerManager.h>
#include <RealClicks.h>
#include <ScriptCaller.h>
#include <Preferences.h>
#include <RecordingScriptingInterface.h>
#include <ResourceManager.h>
#include <ResourceRequestObserver.h>
#include <ResourceScriptingInterface.h>
#include <SceneScriptingInterface.h>
#include <ScriptCache.h>
#include <ScriptEngines.h>
#include <SoundCacheScriptingInterface.h>
#include <SpatialParentFinder.h>
#include <StatTracker.h>
#include <UsersScriptingInterface.h>
#include <controllers/ScriptingInterface.h>
#include <controllers/StateController.h>
#include <controllers/UserInputMapper.h>
#include <display-plugins/CompositorHelper.h>
#include <graphics-scripting/GraphicsScriptingInterface.h>
#include <hfm/ModelFormatRegistry.h>
#include <material-networking/TextureCacheScriptingInterface.h>
#include <model-networking/ModelCacheScriptingInterface.h>
#include <plugins/InputConfiguration.h>
#include <procedural/MaterialCacheScriptingInterface.h>
#include <recording/ClipCache.h>
#include <recording/Deck.h>
#include <recording/Recorder.h>
#include <ui/OffscreenQmlSurfaceCache.h>

#include "Application.h"
#include "ApplicationMeshProvider.h"
#include "DesktopPreviewProvider.h"
#include "InterfaceDynamicFactory.h"
#include "InterfaceParentFinder.h"
#include "audio/AudioScope.h"
#include "avatar/AvatarManager.h"
#include "avatar/GrabManager.h"
#include "editor/Editor.h"
#include "editor/ImpostorBaker.h"
#include "raypick/LaserPointerScriptingInterface.h"
#include "raypick/PickScriptingInterface.h"
#include "raypick/PointerScriptingInterface.h"
#include "raypick/RayPickScriptingInterface.h"
#include "scripting/AssetMappingsScriptingInterface.h"
#include "scripting/ChatScriptingInterface.h"
#include "scripting/ControllerScriptingInterface.h"
#include "scripting/DesktopScriptingInterface.h"
#include "scripting/DiskCacheScriptingInterface.h"
#include "avatar/AvatarThumbnails.h"
#include "ui/Skins.h"
#include "ui/PanelTheme.h"
#include "ui/CodeEditor.h"
#include "ui/Panels.h"
#include "ui/VrScreen.h"
#include "chat/FriendsScriptingInterface.h"
#include "scripting/HMDScriptingInterface.h"
#include "scripting/KeyboardScriptingInterface.h"
#include "scripting/SelectionScriptingInterface.h"
#include "scripting/TTSScriptingInterface.h"
#include "scripting/WindowScriptingInterface.h"
#include "ui/DialogsManager.h"
#include "ui/Keyboard.h"
#include "ui/DomainConnectionModel.h"
#include "ui/OctreeStatsProvider.h"
#include "ui/Snapshot.h"
#include "ui/StandAloneJSConsole.h"
#include "ui/TabletScriptingInterface.h"
#include "ui/ToolbarScriptingInterface.h"
#include "make/MakeLink.h"
#include "make/MakeDock.h"

// the controller's state names (Application's state device reads them too)
const QString STATE_IN_HMD = "InHMD";
const QString STATE_CAMERA_FULL_SCREEN_MIRROR = "CameraFSM";
const QString STATE_CAMERA_FIRST_PERSON = "CameraFirstPerson";
const QString STATE_CAMERA_FIRST_PERSON_LOOK_AT = "CameraFirstPersonLookat";
const QString STATE_CAMERA_THIRD_PERSON = "CameraThirdPerson";
const QString STATE_CAMERA_ENTITY = "CameraEntity";
const QString STATE_CAMERA_INDEPENDENT = "CameraIndependent";
const QString STATE_CAMERA_LOOK_AT = "CameraLookAt";
const QString STATE_CAMERA_SELFIE = "CameraSelfie";
const QString STATE_SNAP_TURN = "SnapTurn";
const QString STATE_ADVANCED_MOVEMENT_CONTROLS = "AdvancedMovement";
const QString STATE_GROUNDED = "Grounded";
const QString STATE_NAV_FOCUSED = "NavigationFocused";
const QString STATE_PLATFORM_WINDOWS = "PlatformWindows";
const QString STATE_PLATFORM_MAC = "PlatformMac";
const QString STATE_PLATFORM_ANDROID = "PlatformAndroid";
const QString STATE_LEFT_HAND_DOMINANT = "LeftHandDominant";
const QString STATE_RIGHT_HAND_DOMINANT = "RightHandDominant";
const QString STATE_STRAFE_ENABLED = "StrafeEnabled";

void registerApplicationServices(int listenPort, const QString& defaultScriptsOverride) {
    // Which class answers for which interface: get<LimitedNodeList>() is the NodeList, and so on.
    DependencyManager::registerInheritance<LimitedNodeList, NodeList>();
    DependencyManager::registerInheritance<AvatarHashMap, AvatarManager>();
    DependencyManager::registerInheritance<EntityDynamicFactoryInterface, InterfaceDynamicFactory>();
    DependencyManager::registerInheritance<SpatialParentFinder, InterfaceParentFinder>();

    // Picking and pointers: rays and parabolas into the world, the laser and stylus pointers, their script interfaces.
    DependencyManager::set<PickManager>();
    DependencyManager::set<PointerManager>();
    DependencyManager::set<LaserPointerScriptingInterface>();
    DependencyManager::set<RayPickScriptingInterface>();
    DependencyManager::set<PointerScriptingInterface>();
    DependencyManager::set<PickScriptingInterface>();

    // The cursor, the touch-screen pad, the desktop preview (the HMD's picture of the desktop).
    DependencyManager::set<Cursor::Manager>();
    DependencyManager::set<VirtualPad::Manager>();
    DependencyManager::set<DesktopPreviewProvider>();

    // The metaverse account, with the client's user agent.
#if defined(Q_OS_ANDROID)
    DependencyManager::set<AccountManager>(true); // use the default user agent getter
#else
    DependencyManager::set<AccountManager>(true, std::bind(&Application::getUserAgent, qApp));
#endif

    // The stats, and the scripting: the client-script engines (the default scripts, or the override from the command
    // line) and the preferences they show.
    DependencyManager::set<StatTracker>();
    DependencyManager::set<ScriptEngines>(ScriptEngine::CLIENT_SCRIPT, defaultScriptsOverride);
    DependencyManager::set<Preferences>();

    // Recording and playback of avatar and audio frames.
    DependencyManager::set<recording::Deck>();
    DependencyManager::set<recording::Recorder>();

    // The network: the address manager (places, hops, the address bar) and the node list, on the port the command line
    // gave, if any.
    DependencyManager::set<AddressManager>();
    DependencyManager::set<NodeList>(NodeType::Agent, listenPort);

    // The caches: recorded clips, geometry, the model formats (registered before the model cache, which reads them),
    // models, scripts, sounds; each with its script interface where scripts see it.
    DependencyManager::set<recording::ClipCache>();
    DependencyManager::set<GeometryCache>();
    DependencyManager::set<ModelFormatRegistry>(); // ModelFormatRegistry must be defined before ModelCache. See the ModelCache constructor.
    DependencyManager::set<ModelCache>();
    DependencyManager::set<ModelCacheScriptingInterface>();
    DependencyManager::set<ScriptCache>();
    DependencyManager::set<SoundCache>();
    DependencyManager::set<SoundCacheScriptingInterface>();

    // Audio: the client (its thread starts in the constructor) and the scope.
    DependencyManager::set<AudioClient>();
    DependencyManager::set<AudioScope>();

    // Rendering: the deferred lighting, the textures, materials, framebuffers and animations, the blender that bakes
    // blendshapes on the pool.
    DependencyManager::set<DeferredLightingEffect>();
    DependencyManager::set<TextureCache>();
    DependencyManager::set<MaterialCache>();
    DependencyManager::set<TextureCacheScriptingInterface>();
    DependencyManager::set<MaterialCacheScriptingInterface>();
    DependencyManager::set<FramebufferCache>();
    DependencyManager::set<AnimationCache>();
    DependencyManager::set<AnimationCacheScriptingInterface>();
    DependencyManager::set<ModelBlender>();

    // People: the users API, the avatar manager (every avatar, this one included), the level-of-detail manager.
    DependencyManager::set<UsersScriptingInterface>();
    DependencyManager::set<AvatarManager>();
    DependencyManager::set<LODManager>();

    // The UI: the script console, the dialogs, the shared resource items, the desktop API.
    DependencyManager::set<StandAloneJSConsole>();
    DependencyManager::set<DialogsManager>();
    DependencyManager::set<ResourceCacheSharedItems>();
    DependencyManager::set<DesktopScriptingInterface>();

    // For scripts: entities (the client's instance), graphics and the mesh provider behind it, recording, the window,
    // the HMD, resources, the tablet, input configuration, toolbars, asset mappings, the domain connection model.
    DependencyManager::set<EntityScriptingInterface>(true);
    DependencyManager::set<GraphicsScriptingInterface>();
    DependencyManager::registerInheritance<scriptable::ModelProviderFactory, ApplicationMeshProvider>();
    DependencyManager::set<ApplicationMeshProvider>();
    DependencyManager::set<RecordingScriptingInterface>();
    DependencyManager::set<WindowScriptingInterface>();
    DependencyManager::set<HMDScriptingInterface>();
    DependencyManager::set<ResourceScriptingInterface>();
    DependencyManager::set<TabletScriptingInterface>();
    DependencyManager::set<InputConfiguration>();
    DependencyManager::set<ToolbarScriptingInterface>();
    DependencyManager::set<AssetMappingsScriptingInterface>();
    DependencyManager::set<DomainConnectionModel>();

    // security finding #54, with editor-ui: the tablet opens a world's page only within a short while after a real
    // press on one of the world's entities (RealClicks). A real press (the desktop mouse, the app's lasers) reaches
    // mousePressOnEntity on an unmarked thread; a press a world's script sends in (Entities.sendMousePressOnEntity)
    // is emitted on that world's own thread, where ScriptCaller says so, and doesn't count; nor does a trigger of a
    // pointer a world's script made (a mapping could drive it). The direct connection keeps the emitting thread, so
    // the check reads it; the registry is thread-safe and touches no UI.
    {
        auto entities = DependencyManager::get<EntityScriptingInterface>();
        QObject::connect(entities.data(), &EntityScriptingInterface::mousePressOnEntity, entities.data(),
                         [](const EntityItemID& entityID, const PointerEvent& event) {
            if (ScriptCaller::isWorld()) {
                return;
            }
            auto pointers = DependencyManager::get<PointerManager>();
            if (pointers && pointers->isWorldPointer(event.getID())) {
                return;
            }
            RealClicks::note(entityID);
        }, Qt::DirectConnection);
        RealClicks::setOwnershipCheck([](const QUuid& entityID) {
            auto entities = DependencyManager::get<EntityScriptingInterface>();
            return entities && entities->worldMayUse(entityID);
        });
        // A pointer a world's script made delivers its hover and trigger events only into the world's own entities
        // (Pointer::worldTarget): a domain entity, or one a world's script made. Asked on the main thread, so by the
        // entity itself, not by the caller; an id that isn't an entity is not the world's.
        Pointer::setWorldTargetCheck([](const QUuid& entityID) {
            auto entities = DependencyManager::get<EntityScriptingInterface>();
            auto tree = entities ? entities->getEntityTree() : EntityTreePointer();
            if (!tree || entityID.isNull()) {
                return false;
            }
            EntityItemPointer entity;
            tree->withReadLock([&] { entity = tree->findEntityByEntityItemID(EntityItemID(entityID)); });
            return entity && entity->isMadeByWorld();
        });
    }


    // Speech recognition where the platform has it; discoverability (the metaverse's where-are-you); Darkride chat and the
// account's friends; the scene API; the
    // offscreen QML UI; paths; the entity dynamics factory; audio injectors; the messages client.
    DependencyManager::set<DiscoverabilityManager>();
    DependencyManager::set<ChatService>();       // Darkride chat (libraries/chat), signed in with the account
    DependencyManager::set<FriendsService>();    // the account's friends, for the chat window's Friends page
    DependencyManager::set<FriendsScriptingInterface>();   // and for scripts and the panels ("Friends")
    DependencyManager::set<SceneScriptingInterface>();
    DependencyManager::set<OffscreenUi>();
    DependencyManager::set<PathUtils>();
    DependencyManager::set<InterfaceDynamicFactory>();
    DependencyManager::set<AudioInjectorManager>();
    DependencyManager::set<MessagesClient>();

    // Input: the state variables scripts can route (set before the mapper reads them), the input mapper, the
    // controller API.
    controller::StateController::setStateVariables({ { STATE_IN_HMD, STATE_CAMERA_FULL_SCREEN_MIRROR,
                    STATE_CAMERA_FIRST_PERSON, STATE_CAMERA_FIRST_PERSON_LOOK_AT, STATE_CAMERA_THIRD_PERSON,
                    STATE_CAMERA_ENTITY, STATE_CAMERA_INDEPENDENT, STATE_CAMERA_LOOK_AT, STATE_CAMERA_SELFIE,
                    STATE_SNAP_TURN, STATE_ADVANCED_MOVEMENT_CONTROLS, STATE_GROUNDED, STATE_NAV_FOCUSED,
                    STATE_PLATFORM_WINDOWS, STATE_PLATFORM_MAC, STATE_PLATFORM_ANDROID, STATE_LEFT_HAND_DOMINANT, STATE_RIGHT_HAND_DOMINANT, STATE_STRAFE_ENABLED } });
    DependencyManager::set<UserInputMapper>();
    DependencyManager::set<controller::ScriptingInterface, ControllerScriptingInterface>();

    // The world: the parent finder for nested things, the entity tree renderer (the client's entity tree), the
    // compositor, the QML surface cache, the entity-script clients, the octree stats.
    DependencyManager::set<InterfaceParentFinder>();
    DependencyManager::set<EntityTreeRenderer>(true, qApp, qApp);
    DependencyManager::set<CompositorHelper>();
    DependencyManager::set<OffscreenQmlSurfaceCache>();
    DependencyManager::set<EntityScriptClient>();
    DependencyManager::set<EntityScriptServerLogClient>();
    DependencyManager::set<OctreeStatsProvider>(nullptr, qApp->getOcteeSceneStats());

    // Bookmarks and snapshots, the resource manager, selection, the editor and its impostor baker, text to speech,
    // chat, the disk cache API.
    DependencyManager::set<AvatarBookmarks>();
    // Make's link to the builder's Claude Code: nothing listens or runs until the person connects
    DependencyManager::set<MakeLink>();
    DependencyManager::set<LocationBookmarks>();
    DependencyManager::set<Snapshot>();
    DependencyManager::set<ResourceManager>();
    MyAvatar::useBuiltInDefaultAvatar();   // the default avatar's files from the app's own copy
    DependencyManager::set<SelectionScriptingInterface>();
    DependencyManager::set<Editor>();
    DependencyManager::set<ImpostorBaker>();
    DependencyManager::set<TTSScriptingInterface>();
    DependencyManager::set<ChatScriptingInterface>();
    DependencyManager::set<DiskCacheScriptingInterface>();
    DependencyManager::set<AvatarThumbnails>();    // the QML panels' avatar pictures
    DependencyManager::set<PanelTheme>();          // the QML panels' colour scheme
    DependencyManager::set<Skins>();               // skins (ui/Skins.h), put on once the main window exists
    DependencyManager::set<Panels>();              // the window's panels by name (ui/Panels.h)
    DependencyManager::set<CodeEditor>();          // the Code panel's document (ui/CodeEditor.h), after Build mode's
    DependencyManager::set<MakeDock>();            // the Make dock's one object (make/MakeDock.h), after MakeLink's
    DependencyManager::set<VrScreen>();            // the VR screen's headset side (ui/VrScreen.h)
    PanelTheme::registerWithQml();


    // Last: the fade effect, the resource request observer, the keyboard and its API, grabbing.
    DependencyManager::set<FadeEffect>();
    DependencyManager::set<ResourceRequestObserver>();
    DependencyManager::set<Keyboard>();
    DependencyManager::set<KeyboardScriptingInterface>();
    DependencyManager::set<GrabManager>();
}
