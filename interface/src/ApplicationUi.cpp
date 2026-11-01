//
//  ApplicationUi.cpp
//  interface/src
//
//  Application's user-interface bring-up: the QML desktop, the system tablet, the menu and the preferences brought up
//  in order (initializeUi); the tablet toggled, its toolbar mode and its entity ids (toggleTabletUI,
//  updateSystemTabletMode, getTabletScreenID, getTabletHomeButtonID, getTabletFrameID, getTabletIDs); the overlays
//  (toggleOverlays, setOverlaysVisible, centerUI); the controls reference (showHelp). The members as they stood in
//  Application.cpp, in their order; a home of their own until an owner takes them (ARCHITECTURE.md §5.4 step 4). The
//  sign-in gate is a SignInGate (SignInGate.h).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "Application.h"
#include "ApplicationConstants.h"
#include "DeadlockWatchdog.h"
#include <thread>
#include <glm/glm.hpp>
#include <QtQml/QQmlContext>
#include <shared/QtHelpers.h>
#include <shared/GlobalAppProperties.h>
#include <StartupProfiler.h>
#include <EntityScriptingInterface.h>
#include <ErrorDialog.h>
#include <MainWindow.h>
#include <OffscreenUi.h>
#include <ui/OffscreenQmlSurfaceCache.h>
#include <PathUtils.h>
#include <plugins/PluginManager.h>
#include <plugins/PluginUtils.h>
#include <ui/TabletScriptingInterface.h>
#include <ui/types/ContextAwareProfile.h>
#include <Tooltip.h>
#include <Preferences.h>
#include <display-plugins/CompositorHelper.h>
#include <RenderableEntityItem.h>
#include <RenderableTextEntityItem.h>
#include <RenderableWebEntityItem.h>
#include "GLCanvas.h"
#include "scripting/Audio.h"
#include "scripting/PlatformInfoScriptingInterface.h"
#include "scripting/AccountServicesScriptingInterface.h"
#include "scripting/HMDScriptingInterface.h"
#include "scripting/TTSScriptingInterface.h"
#include "ui/Skins.h"
#include "ui/SystemToolbar.h"
#include "ui/ResourceImageItem.h"
#include "ui/AddressBarDialog.h"
#include "ui/DialogsManager.h"
#include "ui/LoginDialog.h"
#include "ui/Stats.h"
#include "ui/Keyboard.h"
#include "ui/VrScreen.h"
#include "webbrowser/WebBrowserSuggestionsEngine.h"
#if defined(Q_OS_ANDROID)
#include <android/log.h>
#include "AndroidHelper.h"
#endif

using namespace std;

QSharedPointer<OffscreenUi> getOffscreenUI();   // Application.cpp's
QString labelledTitle(const QString& title);     // Display.cpp's: a window title with the client label
extern void setupPreferences();                  // ui/PreferencesDialog.cpp's
void addDisplayPluginToMenu(const DisplayPluginPointer& displayPlugin, int index, bool active = false);   // Display.cpp's; initializeUi reads it

// a constant only this code reads
static const QString INFO_HELP_PATH = "html/tabletHelp.html";

void Application::toggleTabletUI(bool shouldOpen) const {
    if (VrScreen::isOn()) {
        auto screen = DependencyManager::get<VrScreen>();
        if (!(shouldOpen && screen->isShown())) {
            screen->toggle();
        }
        return;
    }
    auto hmd = DependencyManager::get<HMDScriptingInterface>();
    if (!(shouldOpen && hmd->getShouldShowTablet())) {
        auto HMD = DependencyManager::get<HMDScriptingInterface>();
        HMD->toggleShouldShowTablet();

        if (!HMD->getShouldShowTablet()) {
            DependencyManager::get<Keyboard>()->setRaised(false);
            _window->activateWindow();
            auto tablet = DependencyManager::get<TabletScriptingInterface>()->getTablet(SYSTEM_TABLET);
            tablet->unfocus();
        }
    }
}

void Application::initializeUi() {

    // QML is the app's own interface code: it comes from the app (qrc:), a path relative to it, or a file on this
    // computer. An address that names a host, or whose path starts with two slashes, is on another machine or a
    // network share (a relative one too, once resolved against a file base) and is refused, as is any other scheme,
    // written with its slashes or without.
    {
        auto newValidator = [=](const QUrl& url)->bool {
            // (backslashes read as slashes, as Windows reads them: /\server and \\server are shares too)
            const QString path = QString(url.path()).replace('\\', '/');
            if (!url.host().isEmpty() || path.startsWith("//")) {
                return false;
            }
            // (a one-letter scheme is a Windows drive: a path on this computer)
            const QString scheme = url.scheme().toLower();
            return scheme.isEmpty() || scheme == "qrc" || scheme == "file" || scheme.length() == 1;
        };
        OffscreenQmlSurface::setUrlValidator(newValidator);
    }

    AddressBarDialog::registerType();
    ErrorDialog::registerType();
    LoginDialog::registerType();
    Tooltip::registerType();
    QmlContextCallback platformInfoCallback = [](QQmlContext* context) {
        context->setContextProperty("PlatformInfo", new PlatformInfoScriptingInterface());
    };
    OffscreenQmlSurface::addWhitelistContextHandler({
        QUrl{ "hifi/tablet/TabletAddressDialog.qml" },
        QUrl{ "hifi/Card.qml" },
        }, platformInfoCallback);

    QmlContextCallback ttsCallback = [](QQmlContext* context) {
        context->setContextProperty("TextToSpeech", DependencyManager::get<TTSScriptingInterface>().data());
    };
    OffscreenQmlSurface::addWhitelistContextHandler({
        QUrl{ "hifi/tts/TTS.qml" }
    }, ttsCallback);
    qmlRegisterType<ResourceImageItem>("Hifi", 1, 0, "ResourceImageItem");
    qmlRegisterType<Preference>("Hifi", 1, 0, "Preference");
    qmlRegisterType<WebBrowserSuggestionsEngine>("HifiWeb", 1, 0, "WebBrowserSuggestionsEngine");

    {
        StartupProfiler::Scope step("initializeUi: the system tablet");
        auto tabletScriptingInterface = DependencyManager::get<TabletScriptingInterface>();
        auto systemTablet = tabletScriptingInterface->getTablet(SYSTEM_TABLET);
        // the bar's buttons on a side of the window, when Settings puts them there (ui/SystemToolbar.h)
        SystemToolbar::instance()->attach(_window, systemTablet);
    }
    // the skin chosen last time, now the main window and the bar exist (ui/Skins.h)
    DependencyManager::get<Skins>()->start(_window);

    auto offscreenUi = getOffscreenUI();
    connect(offscreenUi.data(), &hifi::qml::OffscreenSurface::rootContextCreated,
        this, &Application::onDesktopRootContextCreated);
    connect(offscreenUi.data(), &hifi::qml::OffscreenSurface::rootItemCreated,
        this, &Application::onDesktopRootItemCreated);
    // BUGZ-1365 - the root context should explicitly default to being unable to load local HTML content. Here, as the
    // context is made: before Desktop.qml loads, so nothing in it has bound a web profile yet (see restrictContext)
    connect(offscreenUi.data(), &hifi::qml::OffscreenSurface::rootContextCreated, this, [](QQmlContext* surfaceContext) {
        ContextAwareProfile::restrictContext(surfaceContext, true);
    });

    offscreenUi->setProxyWindow(_window->windowHandle());
    // OffscreenUi is a subclass of OffscreenQmlSurface specifically designed to
    // support the window management and scripting proxies for VR use
    DeadlockWatchdogThread::withPause([&] {
        StartupProfiler::Scope step("initializeUi: Desktop.qml (OffscreenUi::createDesktop)");
        offscreenUi->createDesktop(PathUtils::qmlUrl("hifi/Desktop.qml"));
    });
    // FIXME either expose so that dialogs can set this themselves or
    // do better detection in the offscreen UI of what has focus
    offscreenUi->setNavigationFocused(false);

    {
        StartupProfiler::Scope step("initializeUi: preferences");
        setupPreferences();
    }

    _glWidget->installEventFilter(offscreenUi.data());
    offscreenUi->setMouseTranslator([=](const QPointF& pt) {
        QPointF result = pt;
        auto displayPlugin = getActiveDisplayPlugin();
        if (displayPlugin->isHmd()) {
            getApplicationCompositor().handleRealMouseMoveEvent(false);
            auto resultVec = getApplicationCompositor().getReticlePosition();
            result = QPointF(resultVec.x, resultVec.y);
        }
        return result.toPoint();
    });

    // Again, on purpose: the same flag and the same objects, so nothing changes, unless the call above ever stops running
    // before Desktop.qml loads. Then this closes the door late rather than leaving it open.
    ContextAwareProfile::restrictContext(offscreenUi->getSurfaceContext(), true);
    offscreenUi->resume();
    connect(_window, &MainWindow::windowGeometryChanged, [this](const QRect& r){
        _display.resizeGL();
        if (_touchscreenVirtualPadDevice) {
            _touchscreenVirtualPadDevice->resize();
        }
    });

    // This will set up the input plugins UI
    foreach(auto inputPlugin, PluginManager::getInstance()->getInputPlugins()) {
        if (KeyboardMouseDevice::NAME == inputPlugin->getName()) {
            _keyboardMouseDevice = std::dynamic_pointer_cast<KeyboardMouseDevice>(inputPlugin);
        }
        if (TouchscreenDevice::NAME == inputPlugin->getName()) {
            _touchscreenDevice = std::dynamic_pointer_cast<TouchscreenDevice>(inputPlugin);
        }
        if (TouchscreenVirtualPadDevice::NAME == inputPlugin->getName()) {
            _touchscreenVirtualPadDevice = std::dynamic_pointer_cast<TouchscreenVirtualPadDevice>(inputPlugin);
#if defined(ANDROID_APP_INTERFACE)
            auto& virtualPadManager = VirtualPad::Manager::instance();
            connect(&virtualPadManager, &VirtualPad::Manager::hapticFeedbackRequested,
                    this, [](int duration) {
                        AndroidHelper::instance().performHapticFeedback(duration);
                    });
#endif
        }
    }

    // in headset mode the desktop mouse drives the headset's pointer and the window shows none, unless capture is off or
    // the headset is off the player's head: then the system arrow, so the window's menus can be reached
    auto updateHmdCursor = [=] {
        if (isHMDMode()) {
            auto compositorHelper = DependencyManager::get<CompositorHelper>(); // don't capture outer smartpointer
            bool headsetsPointer = compositorHelper->getAllowMouseCapture() && !getActiveDisplayPlugin()->isHeadsetOff();
            _windowCursor.show(headsetsPointer ? Cursor::Manager::lookupIcon(_preferredCursor.get()) : Cursor::Icon::SYSTEM);
        }
    };
    auto compositorHelper = DependencyManager::get<CompositorHelper>();
    connect(compositorHelper.data(), &CompositorHelper::allowMouseCaptureChanged, this, updateHmdCursor);
    connect(DependencyManager::get<HMDScriptingInterface>().data(), &HMDScriptingInterface::mountedChanged, this, updateHmdCursor);
    // after start-up's own handler, which sets the preferred cursor for the new mode
    connect(this, &Application::activeDisplayPluginChanged, this, updateHmdCursor);

    // Pre-create a couple of offscreen surfaces to speed up tablet UI
    auto offscreenSurfaceCache = DependencyManager::get<OffscreenQmlSurfaceCache>();
    offscreenSurfaceCache->setOnRootContextCreated([&](const QString& rootObject, QQmlContext* surfaceContext) {
        if (rootObject == TabletScriptingInterface::QML) {
            // in Qt 5.10.0 there is already an "Audio" object in the QML context
            // though I failed to find it (from QtMultimedia??). So..  let it be "AudioScriptingInterface"
            surfaceContext->setContextProperty("AudioScriptingInterface", DependencyManager::get<AudioScriptingInterface>().data());
            surfaceContext->setContextProperty("Account", AccountServicesScriptingInterface::getInstance()); // DEPRECATED - TO BE REMOVED
        }
        // the pool's surfaces show HTML Web entities: here, before the surface's QML loads
        if (rootObject == render::entities::WebEntityRenderer::QML) {
            ContextAwareProfile::separateWebEntitySurface(surfaceContext);
        }
    });

    {
        StartupProfiler::Scope step("initializeUi: offscreen surfaces reserved (one tablet, two web)");
        offscreenSurfaceCache->reserve(TabletScriptingInterface::QML, 1);
        offscreenSurfaceCache->reserve(render::entities::WebEntityRenderer::QML, 2);
    }

    {
        StartupProfiler::Scope step("initializeUi: the menu (flushMenuUpdates)");
        flushMenuUpdates();
    }

    // Now that the menu is instantiated, ensure the display plugin menu is properly updated
    {
        auto displayPlugins = PluginManager::getInstance()->getDisplayPlugins();
        // first sort the plugins into groupings: standard, advanced, developer
        std::stable_sort(displayPlugins.begin(), displayPlugins.end(),
            [](const DisplayPluginPointer& a, const DisplayPluginPointer& b) -> bool { return a->getGrouping() < b->getGrouping(); });
        int dpIndex = 1;
        // concatenate the groupings into a single list in the order: standard, advanced, developer
        for(const auto& displayPlugin : displayPlugins) {
            addDisplayPluginToMenu(displayPlugin, dpIndex, _display.plugin() == displayPlugin);
            dpIndex++;
        }

        // after all plugins have been added to the menu, add a separator to the menu
        auto parent = getPrimaryMenu()->getMenu(MenuOption::OutputMenu);
        parent->addSeparator();
    }

    // The display plugins are created before the menu now, so we need to do this here to hide the menu bar
    // now that it exists
    if (_window && _window->isFullScreen()) {
        setFullscreen(nullptr, true);
    }

    _domainSession.setIsInterstitialMode(true);
}

void Application::showHelp() {
    static const QString HAND_CONTROLLER_NAME_VIVE = "vive";
    static const QString HAND_CONTROLLER_NAME_OCULUS_TOUCH = "oculus";
    static const QString HAND_CONTROLLER_NAME_WINDOWS_MR = "windowsMR";

    static const QString VIVE_PLUGIN_NAME = "HTC Vive";
    static const QString OCULUS_RIFT_PLUGIN_NAME = "Oculus Rift";
    static const QString WINDOWS_MR_PLUGIN_NAME = "WindowsMR";

    static const QString TAB_KEYBOARD_MOUSE = "kbm";
    static const QString TAB_GAMEPAD = "gamepad";
    static const QString TAB_HAND_CONTROLLERS = "handControllers";

    QString handControllerName;
    QString defaultTab = TAB_KEYBOARD_MOUSE;

    if (PluginUtils::isHMDAvailable(WINDOWS_MR_PLUGIN_NAME)) {
        defaultTab = TAB_HAND_CONTROLLERS;
        handControllerName = HAND_CONTROLLER_NAME_WINDOWS_MR;
    } else if (PluginUtils::isHMDAvailable(VIVE_PLUGIN_NAME)) {
        defaultTab = TAB_HAND_CONTROLLERS;
        handControllerName = HAND_CONTROLLER_NAME_VIVE;
    } else if (PluginUtils::isHMDAvailable(OCULUS_RIFT_PLUGIN_NAME)) {
        if (PluginUtils::isOculusTouchControllerAvailable()) {
            defaultTab = TAB_HAND_CONTROLLERS;
            handControllerName = HAND_CONTROLLER_NAME_OCULUS_TOUCH;
        } else if (PluginUtils::isXboxControllerAvailable()) {
            defaultTab = TAB_GAMEPAD;
        } else {
            defaultTab = TAB_KEYBOARD_MOUSE;
        }
    } else if (PluginUtils::isXboxControllerAvailable()) {
        defaultTab = TAB_GAMEPAD;
    } else {
        defaultTab = TAB_KEYBOARD_MOUSE;
    }

    QUrlQuery queryString;
    queryString.addQueryItem("handControllerName", handControllerName);
    queryString.addQueryItem("defaultTab", defaultTab);
    TabletProxy* tablet = dynamic_cast<TabletProxy*>(DependencyManager::get<TabletScriptingInterface>()->getTablet(SYSTEM_TABLET));
    tablet->gotoWebScreen(PathUtils::resourcesUrl() + INFO_HELP_PATH + "?" + queryString.toString());
    DependencyManager::get<HMDScriptingInterface>()->openTablet();
}

void Application::toggleOverlays() {
    auto menu = Menu::getInstance();
    menu->setIsOptionChecked(MenuOption::Overlays, !menu->isOptionChecked(MenuOption::Overlays));
}

void Application::setOverlaysVisible(bool visible) {
    auto menu = Menu::getInstance();
    menu->setIsOptionChecked(MenuOption::Overlays, visible);
}

void Application::centerUI() {
    _overlayConductor.centerUI();
}

void Application::updateSystemTabletMode() {
    if (_settingsLoaded && !getLoginDialogPoppedUp()) {
        qApp->setProperty(hifi::properties::HMD, isHMDMode());
        // the 3D tablet is gone: the Tablet API's window is on the desktop's QML engine in both modes, and in the headset
        // Panels moves its content onto the VR screen
        DependencyManager::get<TabletScriptingInterface>()->setToolbarMode(true);
    }
}

QUuid Application::getTabletScreenID() const {
    auto HMD = DependencyManager::get<HMDScriptingInterface>();
    return HMD->getCurrentTabletScreenID();
}

QUuid Application::getTabletHomeButtonID() const {
    auto HMD = DependencyManager::get<HMDScriptingInterface>();
    return HMD->getCurrentHomeButtonID();
}

QUuid Application::getTabletFrameID() const {
    auto HMD = DependencyManager::get<HMDScriptingInterface>();
    return HMD->getCurrentTabletFrameID();
}

QVector<QUuid> Application::getTabletIDs() const {
    // Most important first.
    QVector<QUuid> result;
    auto HMD = DependencyManager::get<HMDScriptingInterface>();
    result << HMD->getCurrentTabletScreenID();
    result << HMD->getCurrentHomeButtonID();
    result << HMD->getCurrentTabletFrameID();
    return result;
}
