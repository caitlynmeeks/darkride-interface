//
//  ScriptApiPolicy.cpp
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ScriptApiPolicy.h"

#include <QtCore/QHash>
#include <QtCore/QMetaObject>
#include <QtCore/QSet>

namespace {

// The globals a world's entity scripts get. Not here, so a stand-in: ScriptDiscoveryService (install, stop, reload the
// visitor's scripts), Menu, Users (ignore, mute, kick), Snapshot, Impostors, Recording, Clipboard, DialogsManager,
// Account/GlobalServices/AccountServices, AvatarBookmarks, LocationBookmarks, DiskCache, Keyboard, Render, LODManager,
// Performance, Workload, Scene, Reticle, Friends, Toolbars, File, AvatarInputs, Panels, AudioScope,
// DesktopPreviewProvider, OffscreenFlags, Test, Desktop and the OverlayWindow/OverlayWebWindow/QmlFragment
// constructors (QML and web windows on the visitor's desktop), StackTest, Resources (for the visitor's own scripts),
// Paths, ResourceRequestObserver.
const QSet<QString> ALLOWED_GLOBALS {
    // the engine's own
    "Script", "Vec3", "Quat", "Mat4", "Uuid", "console", "print", "atob", "btoa", "TREE_SCALE", "debugEntityID",
    "XMLHttpRequest", "WebSocket", "request", "AudioEffectOptions",
    "Http", "Sockets",                 // the WebAssembly engine's factories under XMLHttpRequest and WebSocket (the guest hides them)
    // the world and its content
    "Entities", "Messages", "Audio", "Assets", "Resource", "Model", "DebugDraw", "Overlays", "Picks",
    "Pointers", "RayPick", "LaserPointers", "Selection", "Camera", "Graphics", "TextToSpeech", "SoundCache",
    "AnimationCache", "TextureCache", "MaterialCache", "ModelCache",
    // the visitor as the world sees them
    "MyAvatar", "AvatarList", "AvatarManager", "Controller", "HMD",
    // the app, the part a page would get
    "Window", "Window.location", "Tablet", "tabletInterface", "AddressManager", "location", "Chat", "Settings",
    // reads
    "Stats", "Rates", "AudioStats", "PlatformInfo", "About", "EntityScriptServerLog"
};

// The classes whose objects a world's script may use (by ancestry). The globals' classes, and the classes their
// allowed members hand back.
const QSet<QByteArray> ALLOWED_CLASSES {
    // the globals
    "ScriptEngine", "Vec3", "Quat", "Mat4", "ScriptUUID", "ConsoleScriptingInterface", "AudioEffectOptions",
    "EntityScriptingInterface", "WorldMessages", "AudioScriptingInterface", "AssetScriptingInterface",
    "ScriptableResourceCache", "ModelScriptingInterface", "DebugDraw", "Overlays",
    "PickScriptingInterface", "PointerScriptingInterface", "RayPickScriptingInterface",
    "LaserPointerScriptingInterface", "SelectionScriptingInterface", "Camera", "GraphicsScriptingInterface",
    "TTSScriptingInterface", "SoundCacheScriptingInterface", "AnimationCacheScriptingInterface",
    "TextureCacheScriptingInterface", "MaterialCacheScriptingInterface", "ModelCacheScriptingInterface",
    "MyAvatar", "AvatarManager", "controller::ScriptingInterface", "HMDScriptingInterface",
    "WindowScriptingInterface", "TabletScriptingInterface", "AddressManager", "ChatScriptingInterface",
    "ScriptSettingsInterface", "Stats", "RatesScriptingInterface", "AudioStats", "PlatformInfoScriptingInterface",
    "AboutUtil", "EntityScriptServerLogClient",
    // what allowed members hand back
    "QTimer",                                                       // Script.setTimeout / setInterval
    "TabletProxy",                                                  // Tablet.getTablet
    "controller::MappingBuilderProxy", "controller::RouteBuilderProxy",  // Controller.newMapping and its chain
    "ScriptAudioInjector",                                          // Audio.playSound
    "ScriptableResource",                                           // the caches' prefetch
    "ScriptAvatarData",                                             // AvatarList.getAvatar
    "AssetRequest", "AssetUpload", "GetMappingRequest", "SetMappingRequest", "DeleteMappingsRequest",
    "RenameMappingRequest", "GetAllMappingsRequest",                // Assets
    "ModalDialogListener",                                          // Window.promptAsync
    "HttpRequestObject", "WebSocketObject",                         // the WebAssembly engine's XMLHttpRequest / WebSocket
    "HttpRequestFactory", "WebSocketFactory", "AudioEffectOptionsFactory", "ResourceStates",  // its internal globals
    "AnimationObject", "AnimationFrameObject",                      // AnimationCache
    "scriptable::ScriptableModel", "scriptable::ScriptableMesh", "scriptable::ScriptableMeshPart",  // Graphics
    "Overlay",                                                      // Overlays.getOverlayObject
    "RefusedGlobal",                                                // the stand-in: its name only, for the refusal message
    "SoundScriptingInterface"                                       // SoundCache.getSound: a sound (downloaded, duration, ready)
};

// Kept classes with an ALLOW-list of methods: anything else on them is refused.
const QHash<QByteArray, QSet<QByteArray>> ALLOWED_METHODS {
    { "WindowScriptingInterface", {
        // the app's own dialogs, as a web page may; sizes and focus reads; physics and loading reads
        "alert", "confirm", "prompt", "promptAsync", "hasFocus", "isPhysicsEnabled", "checkForPhysics", "getDeviceSize",
        "getInnerWidth", "getInnerHeight", "getX", "getY", "isPointOnDesktopWindow", "getActiveDisplayPlugin",
        "getDisplayPluginCount", "getDisplayPluginName", "isDisplayPluginHmd", "getWorldLoading",
        "domainLoadingProgress", "getLastDomainConnectionError" } },
    { "TabletScriptingInterface", { "getTablet", "playSound" } },
    { "TabletProxy", {
        // a world's page in the visitor's tablet (a pop-up; later with item 7's prompt); not the buttons, not QML,
        // not a message into whatever page the tablet shows
        "gotoWebScreen", "loadWebScreenOnTop", "popFromStack", "returnToPreviousApp", "gotoHomeScreen", "onHomeScreen",
        "isPathLoaded", "isMessageDialogOpen", "closeDialog", "getLandscape" } },
    { "HMDScriptingInterface", {
        "calculateRayUICollisionPoint", "overlayFromWorldPoint", "worldPointFromOverlay", "sphericalToOverlay",
        "overlayToSpherical", "isHMDAvailable", "isHeadControllerAvailable", "isHandControllerAvailable",
        "isSubdeviceContainingNameAvailable", "isKeyboardVisible", "shouldShowHandControllers", "isHMDMode",
        "getHUDLookAtPosition2DValue", "getHUDLookAtPosition3DValue" } },
    { "scripting::Audio", {
        // sounds and reverb are content; mute, devices, volumes and recordings are the visitor's
        "playSound", "playSystemSound", "playLocalSound", "setReverb", "setReverbOptions", "isStereoInput",
        "getAvatarGain", "getInjectorGain", "getLocalInjectorGain", "getSystemInjectorGain",
        "getPushingToTalkOutputGainDesktop", "getRecording", "getPTT", "getPushingToTalk", "loudnessToLevel" } },
    { "AddressManager", {
        // a world may send you to another world (portals); not your clipboard or history
        "handleLookupString", "goToUser", "goToEntry", "goToViewpointForPath", "isConnected", "getHost" } },
    { "ChatScriptingInterface", { "showMessage" } }
};

// Kept classes with a DENY-list of methods instead: too large and too much of them content's to allow-list, so a
// member added later is reachable until it is judged. MyAvatar: what persists the visitor's avatar identity and
// points the loaders at a world's content on every start. Controller: the recordings on the visitor's disk, and taking
// the visitor's input for itself (keys, mouse, wheel, touch, joysticks, clicks on entities, the avatar's actions); a
// world's script may still hear the 3D view's keys and use mappings.
const QHash<QByteArray, QSet<QByteArray>> DENIED_METHODS {
    { "MyAvatar", {
        "setSkeletonModelURL", "setSkeletonModelURLFromScript", "useFullAvatarURL", "setAnimGraphUrl",
        "setAnimGraphOverrideUrl", "setCollisionSoundURL", "attach", "detachOne", "detachAll", "setAttachmentData",
        "setAttachmentsVariant", "setAvatarEntityData", "updateAvatarEntity", "clearAvatarEntity", "saveData",
        "loadData", "setDisplayName" } },
    { "controller::ScriptingInterface", {
        "startInputRecording", "stopInputRecording", "startInputPlayback", "stopInputPlayback", "saveInputRecording",
        "loadInputRecording", "getInputRecorderSaveDirectory", "captureMouseEvents", "releaseMouseEvents",
        "captureTouchEvents", "releaseTouchEvents", "captureWheelEvents", "releaseWheelEvents", "captureActionEvents",
        "releaseActionEvents" } },
    { "ControllerScriptingInterface", {
        "captureKeyEvents", "releaseKeyEvents", "captureJoystick", "releaseJoystick", "captureEntityClickEvents",
        "releaseEntityClickEvents" } }
};

// Properties a world's script may write on an allow-listed class (every other write on such a class is refused).
const QHash<QByteArray, QSet<QByteArray>> ALLOWED_PROPERTY_WRITES {
    { "AddressManager", { "href" } }    // a teleport
};

// Properties a world's script may not write on a deny-listed class.
const QHash<QByteArray, QSet<QByteArray>> DENIED_PROPERTY_WRITES {
    { "MyAvatar", { "skeletonModelURL", "attachmentData", "collisionSoundURL", "displayName" } }
};

// Properties a world's script may not read.
const QHash<QByteArray, QSet<QByteArray>> DENIED_PROPERTY_READS {
    { "scripting::Audio", { "devices" } },   // the visitor's audio devices
    { "TabletProxy", { "buttons" } }          // the visitor's buttons
};

template <typename Table>
const QSet<QByteArray>* entryFor(const Table& table, const QMetaObject* meta, const QMetaObject** owner = nullptr) {
    for (const QMetaObject* cls = meta; cls; cls = cls->superClass()) {
        auto found = table.constFind(QByteArray(cls->className()));
        if (found != table.constEnd()) {
            if (owner) {
                *owner = cls;
            }
            return &found.value();
        }
    }
    return nullptr;
}

template <typename Table>
bool listed(const Table& table, const QMetaObject* meta, const QByteArray& name) {
    for (const QMetaObject* cls = meta; cls; cls = cls->superClass()) {
        auto found = table.constFind(QByteArray(cls->className()));
        if (found != table.constEnd() && found->contains(name)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool ScriptApiPolicy::allowsGlobal(const QString& name) {
    return ALLOWED_GLOBALS.contains(name);
}

bool ScriptApiPolicy::allowsClass(const QMetaObject* meta) {
    for (const QMetaObject* cls = meta; cls; cls = cls->superClass()) {
        if (ALLOWED_CLASSES.contains(QByteArray(cls->className()))) {
            return true;
        }
    }
    return false;
}

bool ScriptApiPolicy::deniesMethod(const QMetaObject* meta, const QByteArray& name) {
    if (!allowsClass(meta)) {
        return true;
    }
    if (entryFor(ALLOWED_METHODS, meta)) {
        return !listed(ALLOWED_METHODS, meta, name);
    }
    return listed(DENIED_METHODS, meta, name);
}

bool ScriptApiPolicy::deniesPropertyWrite(const QMetaObject* meta, const QByteArray& name) {
    if (!allowsClass(meta)) {
        return true;
    }
    if (entryFor(ALLOWED_METHODS, meta)) {
        return !listed(ALLOWED_PROPERTY_WRITES, meta, name);
    }
    return listed(DENIED_PROPERTY_WRITES, meta, name);
}

bool ScriptApiPolicy::deniesPropertyRead(const QMetaObject* meta, const QByteArray& name) {
    return !allowsClass(meta) || listed(DENIED_PROPERTY_READS, meta, name);
}

QString ScriptApiPolicy::refusalLine(const QString& object, const QString& member) {
    return QString("%1%2 is for the visitor's own scripts; a world's script may not use it")
        .arg(object, member.isEmpty() ? QString() : "." + member);
}
