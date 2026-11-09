//
//  Display.cpp
//  interface/src
//
//  The display (see Display.h). The code of the display members of ApplicationDisplay.cpp, with Application reached
//  through qApp for the window, the GL widget, the view, the compositor, the refresh-rate manager, the field of view
//  and the clips, the sign-in gate and the quit flag; with the window title's helpers (labelledTitle, read by the
//  start-up, the UI bring-up and the sign-in gate), the vertical field of view (read by the frame loop) and the
//  Output menu's entries (read by the UI bring-up).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
// FROZEN: DARKRIDE VEHICLE PATH (2026-10-31). DO NOT EDIT WITHOUT THE MASTER PLAN.
// Frozen here: Display::setDisplayPlugin's hand-over to the refresh-rate manager (the refresh operator, the present
// cadence the governor reads, the ride-in-view source) and the regime changes on focus and on the display's rate
// (followScreenRefreshRate, activeChanged, foregroundChanged, checkForeground).
// This code is tuned to the millisecond. The monorail, karts and every ride depend on it, and small "harmless"
// changes here have broken the ride before (strobing, judder, frame drops). Before changing anything frozen:
//   1. the physics, performance and rendering owners work on it together, all three on Claude Fable (not a
//      lighter model), with one written master plan agreed before any code;
//   2. run the full vehicle check (team/VEHICLE-PERFORMANCE.md), ending with Caity's own ride at the display's
//      real rate in a heavy world;
//   3. release merges it only as checked.
// When unsure whether a change counts, it counts.

#include "Display.h"
#include "TestClientIcon.h"

#include <thread>
#include <glm/glm.hpp>
#include <QtCore/QAbstractNativeEventFilter>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QProcessEnvironment>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtGui/QActionGroup>   // Qt 6 moved QActionGroup from QtWidgets to QtGui
#endif
#include <AccountManager.h>
#include <AddressManager.h>
#include <BuildInfo.h>
#include <FramebufferCache.h>
#include <MainWindow.h>
#include <MessagesClient.h>
#include <OffscreenUi.h>
#include <plugins/PluginManager.h>
#include <plugins/PluginUtils.h>
#include <display-plugins/CompositorHelper.h>
#include <display-plugins/OpenGLDisplayPlugin.h>
#include <display-plugins/hmd/HmdDisplayPlugin.h>

#include "Application.h"
#include "ApplicationConstants.h"   // declares ACTIVE_DISPLAY_PLUGIN_SETTING_NAME extern, so the definition below has external linkage
#include "avatar/AvatarManager.h"
#include "ClientOptions.h"
#include "editor/Editor.h"
#include "graphics/RenderEventHandler.h"
#include "InterfaceLogging.h"
#include "scripting/HMDScriptingInterface.h"
#include "scripting/RenderScriptingInterface.h"
#include "ui/DialogsManager.h"
#include "ui/HMDToolsDialog.h"
#if defined(Q_OS_WIN)
#include <Windows.h>
#include <VersionHelpers.h>
#endif

using namespace std;

QSharedPointer<OffscreenUi> getOffscreenUI();   // Application.cpp's
extern DisplayPluginList getDisplayPlugins();   // the statically provided display plug-ins (plugins/src); updateDisplayMode reads them

// constants only the display reads, and the active plug-in's setting (declared in ApplicationConstants.h: the shutdown writes it)
static const unsigned int THROTTLED_SIM_FRAMERATE = 15;
static const int THROTTLED_SIM_FRAME_PERIOD_MS = MSECS_PER_SECOND / THROTTLED_SIM_FRAMERATE;
static const int INTERVAL_TO_CHECK_HMD_WORN_STATUS = 500; // milliseconds
static const QString DESKTOP_DISPLAY_PLUGIN_NAME = "Desktop";
const QString ACTIVE_DISPLAY_PLUGIN_SETTING_NAME = "activeDisplayPlugin";

// Application's getMyAvatar is private; this is the same call
static std::shared_ptr<MyAvatar> getMyAvatar() {
    return DependencyManager::get<AvatarManager>()->getMyAvatar();
}

#if defined(Q_OS_WIN)
// Windows sends every top-level window WM_ACTIVATEAPP when the front app changes. Qt's application state misses some of
// those changes: a window activated while another app is in front (a start-up activateWindow, Window.setFocus) only
// flashes the taskbar button, yet Qt calls the app active, and when the player later clicks it, Qt has no change to
// report. So the refresh-rate regime takes Windows' word, checked against the foreground window (foregroundChanged):
// that same background activation sends a WM_ACTIVATEAPP too (laptop, test-1028-win-11, 10-28).
class ForegroundFilter : public QAbstractNativeEventFilter {
public:
    explicit ForegroundFilter(Display* display) : _display(display) {}

    bool nativeEventFilter(const QByteArray& eventType, void* message, long*) override {
        auto msg = static_cast<MSG*>(message);
        if (eventType == "windows_generic_MSG" && msg->message == WM_ACTIVATEAPP) {
            bool inFront = msg->wParam != FALSE;
            // queued: this runs inside the window procedure; each top-level window gets its own copy, and a repeat changes nothing
            QMetaObject::invokeMethod(_display, [display = _display, inFront] { display->foregroundChanged(inFront); }, Qt::QueuedConnection);
        }
        return false;
    }

private:
    Display* _display;
};
#endif

Display::Display() {
#if defined(Q_OS_WIN)
    _foregroundFilter = std::make_unique<ForegroundFilter>(this);
    QCoreApplication::instance()->installNativeEventFilter(_foregroundFilter.get());
#endif
}

Display::~Display() {
    if (_foregroundFilter && QCoreApplication::instance()) {
        QCoreApplication::instance()->removeNativeEventFilter(_foregroundFilter.get());
    }
}

// The field-of-view setting is a horizontal angle and the vertical one is derived from the aspect
// ratio, which runs away on a tall, narrow viewport (build mode's panels) and flips past 180
// degrees. Build mode keeps the vertical angle at its 16:9 value, so a narrower viewport crops
// the sides instead, as Unity's scene view does.
// Outside build mode the vertical angle is capped. A setting of 120 (kept from 1.0, where the number
// was the vertical angle) in a 1190 x 953 window with Chat docked at the side (view 676 x 953) gave
// 169 degrees: a near-fisheye picture, everything tiny at the centre, the first-person camera inside
// the avatar's hair, and past 180 the picture turns inside out (Caity, the 1.1 beta zip, 10-29). At
// the cap the docked view looks like the same window undocked (103 degrees); at the default 45 the
// cap is reached only below an aspect ratio of 0.45.
const float MAX_VERTICAL_FOV_DEGREES = 100.0f;

float verticalFieldOfView(float horizontalFov, float aspectRatio) {   // the frame loop's render arguments read it too
    auto editor = DependencyManager::get<Editor>();
    if (editor && editor->isActive()) {
        return horizontalFov / (16.0f / 9.0f);
    }
    return glm::min(horizontalFov / glm::max(aspectRatio, 0.01f), MAX_VERTICAL_FOV_DEGREES);
}

static const char* EXCLUSION_GROUP_KEY = "exclusionGroup";

void addDisplayPluginToMenu(const DisplayPluginPointer& displayPlugin, int index, bool active) {   // the UI bring-up (initializeUi) reads it
    auto menu = Menu::getInstance();
    QString name = displayPlugin->getName();
    auto grouping = displayPlugin->getGrouping();
    QString groupingMenu { "" };
    Q_ASSERT(!menu->menuItemExists(MenuOption::OutputMenu, name));

    // assign the meny grouping based on plugin grouping
    switch (grouping) {
        case Plugin::ADVANCED:
            groupingMenu = "Advanced";
            break;
        case Plugin::DEVELOPER:
            groupingMenu = "Developer";
            break;
        default:
            groupingMenu = "Standard";
            break;
    }

    static QActionGroup* displayPluginGroup = nullptr;
    if (!displayPluginGroup) {
        displayPluginGroup = new QActionGroup(menu);
        displayPluginGroup->setExclusive(true);
    }
    auto parent = menu->getMenu(MenuOption::OutputMenu);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Qt 6 deleted modifier + int: the shortcut is a key combination (the same keys, Ctrl+0.. by the plugin's index)
    auto action = menu->addActionToQMenuAndActionHash(parent,
        name, QKeySequence(Qt::CTRL | Qt::Key(Qt::Key_0 + index)), qApp,
        SLOT(updateDisplayMode()),
        QAction::NoRole, Menu::UNSPECIFIED_POSITION, groupingMenu);
#else
    auto action = menu->addActionToQMenuAndActionHash(parent,
        name, QKeySequence(Qt::CTRL + (Qt::Key_0 + index)), qApp,
        SLOT(updateDisplayMode()),
        QAction::NoRole, Menu::UNSPECIFIED_POSITION, groupingMenu);
#endif

    action->setCheckable(true);
    action->setChecked(active);
    displayPluginGroup->addAction(action);

    action->setProperty(EXCLUSION_GROUP_KEY, QVariant::fromValue(displayPluginGroup));
    Q_ASSERT(menu->menuItemExists(MenuOption::OutputMenu, name));
}

// The window title's client label, first in the title when there is one: --clientLabel "<text>" on the command line,
// else the DARKRIDE_CLIENT_LABEL environment variable, else the build's (cmake -DCLIENT_LABEL=...). With several
// sessions' clients on one machine, a label set per launch names the session and the run ("Darkride - Physics - Card
// Run Test") without a rebuild, release builds included, so Caity doesn't close one for her own (2026-10-22).
static const QString& clientLabel() {
    static const QString label = []() -> QString {
        const QString& fromCommandLine = ClientOptions::get().clientLabel;
        if (!fromCommandLine.isEmpty()) {
            return fromCommandLine;
        }
        const QString fromEnvironment = QProcessEnvironment::systemEnvironment().value("DARKRIDE_CLIENT_LABEL").trimmed();
        return fromEnvironment.isEmpty() ? BuildInfo::CLIENT_LABEL : fromEnvironment;
    }();
    return label;
}

QString labelledTitle(const QString& title) {   // the start-up, the UI bring-up and the sign-in gate read it too
    return clientLabel().isEmpty() ? title : clientLabel() + " | " + title;
}

void applyClientLabelIcon() {   // the start-up calls it before the window shows: a test client's Dock icon (TestClientIcon.h)
    setTestClientIcon(clientLabel());
}

void Display::initializePlugins() {
    auto displayPlugins = PluginManager::getInstance()->getDisplayPlugins();
    Setting::Handle<QString> activeDisplayPluginSetting{ ACTIVE_DISPLAY_PLUGIN_SETTING_NAME, displayPlugins.at(0)->getName() };
    auto lastActiveDisplayPluginName = activeDisplayPluginSetting.get();

    auto defaultDisplayPlugin = displayPlugins.at(0);
    // Once time initialization code
    DisplayPluginPointer targetDisplayPlugin;
    foreach(auto displayPlugin, displayPlugins) {
        displayPlugin->setContext(qApp->getGraphicsEngine().getGPUContext());
        if (displayPlugin->getName() == lastActiveDisplayPluginName) {
            targetDisplayPlugin = displayPlugin;
        }

        if (!_autoSwitchDisplayModeSupportedHMDPlugin) {
            if (displayPlugin->isHmd() && displayPlugin->getSupportsAutoSwitch()) {
                _autoSwitchDisplayModeSupportedHMDPlugin = displayPlugin;
                _autoSwitchDisplayModeSupportedHMDPluginName = _autoSwitchDisplayModeSupportedHMDPlugin->getName();
                _previousHMDWornStatus = _autoSwitchDisplayModeSupportedHMDPlugin->isDisplayVisible() && _autoSwitchDisplayModeSupportedHMDPlugin->isActive();
            }
        }

        QObject::connect(displayPlugin.get(), &DisplayPlugin::recommendedFramebufferSizeChanged,
            [this](const QSize& size) { resizeGL(); });
        QObject::connect(displayPlugin.get(), &DisplayPlugin::resetSensorsRequested, qApp, &Application::requestReset);

        if (displayPlugin->isHmd()) {
            auto hmdDisplayPlugin = dynamic_cast<HmdDisplayPlugin*>(displayPlugin.get());
            QObject::connect(hmdDisplayPlugin, &HmdDisplayPlugin::hmdMountedChanged,
                DependencyManager::get<HMDScriptingInterface>().data(), &HMDScriptingInterface::mountedChanged);
        }
    }

    // The default display plugin needs to be activated first, otherwise the display plugin thread
    // may be launched by an external plugin, which is bad
    {
        StartupProfiler::Scope step("initializeDisplayPlugins: default display plugin activated (the present thread)");
        setDisplayPlugin(defaultDisplayPlugin);
    }

    // Now set the desired plugin if it's not the same as the default plugin
    if (targetDisplayPlugin && (targetDisplayPlugin != defaultDisplayPlugin)) {
        StartupProfiler::Scope step("initializeDisplayPlugins: the saved display plugin activated");
        setDisplayPlugin(targetDisplayPlugin);
    }

    if (_autoSwitchDisplayModeSupportedHMDPlugin) {
        if (getActiveDisplayPlugin() != _autoSwitchDisplayModeSupportedHMDPlugin && !_autoSwitchDisplayModeSupportedHMDPlugin->isSessionActive()) {
            startHMDStandBySession();
        }
        // Poll periodically to check whether the user has worn HMD or not. Switch Display mode accordingly.
        // If the user wears HMD then switch to VR mode. If the user removes HMD then switch to Desktop mode.
        QTimer* autoSwitchDisplayModeTimer = new QTimer(this);
        connect(autoSwitchDisplayModeTimer, SIGNAL(timeout()), this, SLOT(switchDisplayMode()));
        autoSwitchDisplayModeTimer->start(INTERVAL_TO_CHECK_HMD_WORN_STATUS);
    }

#if defined(Q_OS_WIN)
    // the safety net under WM_ACTIVATEAPP: whatever Windows sent or didn't, the regime catches up within half a second.
    // Started here, once a plug-in is active: started with the constructor it ran during the start-up's own event
    // processing, and asking for the active plug-in then made one before the GL context (test-1028-win-12's crash).
    static const int FOREGROUND_CHECK_INTERVAL_MS = 500;
    _foregroundCheck.setInterval(FOREGROUND_CHECK_INTERVAL_MS);
    connect(&_foregroundCheck, &QTimer::timeout, this, &Display::checkForeground);
    _foregroundCheck.start();
#endif
}

DisplayPluginPointer Display::getActiveDisplayPlugin() const {
    if (QThread::currentThread() != thread()) {
        std::unique_lock<std::mutex> lock(_displayPluginLock);
        return _displayPlugin;
    }

    if (!qApp->isAboutToQuit() && !_displayPlugin) {
        const_cast<Display*>(this)->updateDisplayMode();
        Q_ASSERT(_displayPlugin);
    }
    return _displayPlugin;
}

bool Display::isHMDMode() const {
    return getActiveDisplayPlugin()->isHmd();
}

float Display::getTargetRenderFrameRate() const { return getActiveDisplayPlugin()->getTargetFrameRate(); }

float Display::getPresentPeriod() const {
    return _displayPlugin ? _displayPlugin->getPresentPeriod() : 0.0f;
}

float Display::getRenderResolutionScale() const {
    return RenderScriptingInterface::getInstance()->getViewportResolutionScale();
}

bool Display::isThrottleRendering() const {
    // In the background (another app in front, or minimized) and at rest (in front, untouched and still: the resting
    // rate) the refresh rate is lowered on purpose: automatic LOD holds still rather than read the slow frames as time
    // to spare and add detail that would make the return stutter.
    auto regime = qApp->getRefreshRateManager().getRefreshRateRegime();
    if (regime == RefreshRateManager::RefreshRateRegime::UNFOCUS ||
        regime == RefreshRateManager::RefreshRateRegime::MINIMIZED ||
        regime == RefreshRateManager::RefreshRateRegime::FOCUS_INACTIVE) {
        return true;
    }
    if (_displayPlugin) {
        return getActiveDisplayPlugin()->isThrottled();
    }
    return false;
}

glm::uvec2 Display::getCanvasSize() const {
    return glm::uvec2(qApp->getPrimaryWidget()->width(), qApp->getPrimaryWidget()->height());
}

QRect Display::getRenderingGeometry() const {
    auto geometry = qApp->getPrimaryWidget()->geometry();
    auto topLeft = geometry.topLeft();
    auto topLeftScreen = qApp->getPrimaryWidget()->mapToGlobal(topLeft);
    geometry.moveTopLeft(topLeftScreen);
    return geometry;
}

glm::uvec2 Display::getUiSize() const {
    static const uint MIN_SIZE = 1;
    glm::uvec2 result(MIN_SIZE);
    if (_displayPlugin) {
        result = getActiveDisplayPlugin()->getRecommendedUiSize();
    }
    return result;
}

QRect Display::getRecommendedHUDRect() const {
    auto uiSize = getUiSize();
    QRect result(0, 0, uiSize.x, uiSize.y);
    if (_displayPlugin) {
        result = getActiveDisplayPlugin()->getRecommendedHUDRect();
    }
    return result;
}

glm::vec2 Display::getDeviceSize() const {
    static const int MIN_SIZE = 1;
    glm::vec2 result(MIN_SIZE);
    if (_displayPlugin) {
        result = getActiveDisplayPlugin()->getRecommendedRenderSize();
    }
    return result;
}

QRect Display::getDesirableApplicationGeometry() const {
    QRect applicationGeometry = qApp->getWindow()->geometry();

    // If our parent window is on the HMD, then don't use its geometry, instead use
    // the "main screen" geometry.
    HMDToolsDialog* hmdTools = DependencyManager::get<DialogsManager>()->getHMDToolsDialog();
    if (hmdTools && hmdTools->hasHMDScreen()) {
        QScreen* hmdScreen = hmdTools->getHMDScreen();
        QWindow* appWindow = qApp->getWindow()->windowHandle();
        QScreen* appScreen = appWindow->screen();

        // if our app's screen is the hmd screen, we don't want to place the
        // running scripts widget on it. So we need to pick a better screen.
        // we will use the screen for the HMDTools since it's a guaranteed
        // better screen.
        if (appScreen == hmdScreen) {
            QScreen* betterScreen = hmdTools->windowHandle()->screen();
            applicationGeometry = betterScreen->geometry();
        }
    }
    return applicationGeometry;
}

qreal Display::getDevicePixelRatio() const {
    return (qApp->getWindow() && qApp->getWindow()->windowHandle()) ? qApp->getWindow()->windowHandle()->devicePixelRatio() : 1.0;
}

mat4 Display::getHMDSensorPose() const {
    if (isHMDMode()) {
        return getActiveDisplayPlugin()->getHeadPose();
    }
    return mat4();
}

mat4 Display::getEyeOffset(int eye) const {
    // FIXME invert?
    return getActiveDisplayPlugin()->getEyeToHeadTransform((Eye)eye);
}

mat4 Display::getEyeProjection(int eye) const {
    QMutexLocker viewLocker(&qApp->getViewState().mutex());
    if (isHMDMode()) {
        return getActiveDisplayPlugin()->getEyeProjection((Eye)eye, qApp->getViewState().viewFrustum().getProjection());
    }
    return qApp->getViewState().viewFrustum().getProjection();
}

bool Display::hasRiftControllers() {
    return PluginUtils::isOculusTouchControllerAvailable();
}

bool Display::hasViveControllers() {
    return PluginUtils::isViveControllerAvailable();
}

bool Display::hasFocus() const {
    bool result = (QApplication::activeWindow() != nullptr);

    
#if defined(Q_OS_WIN)
    // On Windows, QWidget::activateWindow() - as called in setFocus() - makes the application's taskbar icon flash but doesn't
    // take user focus away from their current window. So also check whether the application is the user's current foreground
    // window.
    result = result && (HWND)QApplication::activeWindow()->winId() == GetForegroundWindow();
#endif
    return result;
}

void Display::setFocus() {
    // Note: Windows doesn't allow a user focus to be taken away from another application. Instead, it changes the color of and
    // flashes the taskbar icon.
    auto window = qApp->getWindow();
    window->activateWindow();
}

void Display::raise() {
    auto windowState = qApp->getWindow()->windowState();
    if (windowState & Qt::WindowMinimized) {
        if (windowState & Qt::WindowMaximized) {
            qApp->getWindow()->showMaximized();
        } else if (windowState & Qt::WindowFullScreen) {
            qApp->getWindow()->showFullScreen();
        } else {
            qApp->getWindow()->showNormal();
        }
    }
    qApp->getWindow()->raise();
}

void Display::raiseWindow() const {
    qApp->getWindow()->raise();
    qApp->getWindow()->activateWindow();
}

bool Display::isForeground() const {
    return _isForeground && !qApp->getWindow()->isMinimized();
}

bool Display::isAppInFront() const {
#if defined(Q_OS_WIN)
    // Qt can call the app active while another app is in front (see ForegroundFilter), so ask Windows whose window is in front
    if (HWND foreground = GetForegroundWindow()) {
        DWORD process = 0;
        GetWindowThreadProcessId(foreground, &process);
        return process == GetCurrentProcessId();
    }
#endif
    return QGuiApplication::applicationState() == Qt::ApplicationActive;
}

void Display::resizeGL() {
    PROFILE_RANGE(render, __FUNCTION__);
    if (nullptr == _displayPlugin) {
        return;
    }

    auto displayPlugin = getActiveDisplayPlugin();
    // Set the desired FBO texture size. If it hasn't changed, this does nothing.
    // Otherwise, it must rebuild the FBOs
    uvec2 framebufferSize = displayPlugin->getRecommendedRenderSize();
    uvec2 renderSize = uvec2(framebufferSize);
    if (_renderResolution != renderSize) {
        _renderResolution = renderSize;
        DependencyManager::get<FramebufferCache>()->setFrameBufferSize(fromGlm(renderSize));
    }

    // FIXME the aspect ratio for stereo displays is incorrect based on this.
    float aspectRatio = displayPlugin->getRecommendedAspectRatio();
    float fovY = verticalFieldOfView(qApp->getFieldOfView(), aspectRatio);
    qApp->getViewState().camera().setProjection(
        glm::perspective(
            glm::radians(fovY), aspectRatio, qApp->getNearClip(), qApp->getFarClip()
        )
    );
    // Possible change in aspect ratio
    qApp->getViewState().loadViewFrustum();

    getOffscreenUI()->resize(fromGlm(displayPlugin->getRecommendedUiSize()));
}

void Display::updateWindowTitle() const {

    const auto nodeList = DependencyManager::get<NodeList>();
    const auto accountManager = DependencyManager::get<AccountManager>();
    const auto addressManager = DependencyManager::get<AddressManager>();

    // Like a Mac document window, the title names what you're looking at: the world you're in, by its name, or
    // Darkride when you're in none or it has no name to show (an address is not a name). The account is in its
    // menu, the build in About and in the log line below, and a connection problem is shown in the window itself.
    QString title = "Darkride";
    auto& domainHandler = nodeList->getDomainHandler();
    if (!qApp->isServerlessMode() && domainHandler.isConnected() && !domainHandler.isInErrorState()) {
        const QString worldName = addressManager->getDomainLabel().trimmed();
        if (!worldName.isEmpty()) {
            title = worldName;
        }
    }
    // the client label first, when there is one (see clientLabel): with several sessions' clients on one machine, it
    // tells them apart at a glance (Caity, 2026-10-19)
    title = labelledTitle(title);
    // With several branches built on this machine, one per session, the title says which build this client is
    // ("Darkride - physics-net"), so the right window gets closed. Dev builds only; a release shows no branch.
    if (BuildInfo::BUILD_TYPE == BuildInfo::Dev && !BuildInfo::BUILD_BRANCH.isEmpty() && BuildInfo::BUILD_BRANCH != "HEAD") {
        title += " - " + BuildInfo::BUILD_BRANCH;
    }

#ifndef WIN32
    // crashes with vs2013/win32
    qCDebug(interfaceapp, "Application title set to: %s (build %s %s %s)", title.toStdString().c_str(),
            QCoreApplication::applicationVersion().toStdString().c_str(), BuildInfo::BUILD_BRANCH.toStdString().c_str(),
            BuildInfo::BUILD_COMMIT.toStdString().c_str());
#endif
    qApp->getWindow()->setWindowTitle(title);

    // updateTitleWindow gets called whenever there's a change regarding the domain, so rather
    // than placing this within domainURLChanged, it's placed here to cover the other potential cases.
    DependencyManager::get< MessagesClient >()->sendLocalMessage("Toolbar-DomainChanged", "");


}

void Display::setActiveDisplayPlugin(const QString& pluginName) {
    DisplayPluginPointer newDisplayPlugin;
    for (DisplayPluginPointer displayPlugin : PluginManager::getInstance()->getDisplayPlugins()) {
        QString name = displayPlugin->getName();
        if (pluginName == name) {
            newDisplayPlugin = displayPlugin;
            break;
        }
    }

    if (newDisplayPlugin) {
        setDisplayPlugin(newDisplayPlugin);
    }
}

void Display::updateDisplayMode() {
    // Unsafe to call this method from anything but the main thread
    if (QThread::currentThread() != thread()) {
        qFatal("Attempted to switch display plugins from a non-main thread");
    }

    // Once time initialization code that depends on the UI being available
    auto displayPlugins = getDisplayPlugins();

    // Default to the first item on the list, in case none of the menu items match

    DisplayPluginPointer newDisplayPlugin = displayPlugins.at(0);
    auto menu = qApp->getPrimaryMenu();
    if (menu) {
        foreach(DisplayPluginPointer displayPlugin, PluginManager::getInstance()->getDisplayPlugins()) {
            QString name = displayPlugin->getName();
            QAction* action = menu->getActionForOption(name);
            // Menu might have been removed if the display plugin lost
            if (!action) {
                continue;
            }
            if (action->isChecked()) {
                newDisplayPlugin = displayPlugin;
                break;
            }
        }
    }

    if (newDisplayPlugin == _displayPlugin) {
        return;
    }

    setDisplayPlugin(newDisplayPlugin);
}

// FROZEN (vehicle path): see the note at the top of this file.
void Display::setDisplayPlugin(DisplayPluginPointer newDisplayPlugin) {
    if (newDisplayPlugin == _displayPlugin) {
        return;
    }

    // FIXME don't have the application directly set the state of the UI,
    // instead emit a signal that the display plugin is changing and let
    // the desktop lock itself.  Reduces coupling between the UI and display
    // plugins
    auto offscreenUi = getOffscreenUI();
    auto desktop = offscreenUi ? offscreenUi->getDesktop() : nullptr;
    auto menu = Menu::getInstance();

    // Make the switch atomic from the perspective of other threads
    {
        std::unique_lock<std::mutex> lock(_displayPluginLock);
        bool wasRepositionLocked = false;
        if (desktop) {
            // Tell the desktop to no reposition (which requires plugin info), until we have set the new plugin, below.
            wasRepositionLocked = desktop->property("repositionLocked").toBool();
            desktop->setProperty("repositionLocked", true);
        }

        if (_displayPlugin) {
            disconnect(_displayPlugin.get(), &DisplayPlugin::presented, qApp, &Application::onPresent);
            _displayPlugin->deactivate();
        }

        auto oldDisplayPlugin = _displayPlugin;
        bool active = newDisplayPlugin->activate();

        if (!active) {
            auto displayPlugins = PluginManager::getInstance()->getDisplayPlugins();

            // If the new plugin fails to activate, fallback to last display
            qWarning() << "Failed to activate display: " << newDisplayPlugin->getName();
            newDisplayPlugin = oldDisplayPlugin;

            if (newDisplayPlugin) {
                qWarning() << "Falling back to last display: " << newDisplayPlugin->getName();
                active = newDisplayPlugin->activate();
            }

            // If there is no last display, or
            // If the last display fails to activate, fallback to desktop
            if (!active) {
                newDisplayPlugin = displayPlugins.at(0);
                qWarning() << "Falling back to display: " << newDisplayPlugin->getName();
                active = newDisplayPlugin->activate();
            }

            if (!active) {
                qFatal("Failed to activate fallback plugin");
            }
        }

        if (offscreenUi) {
            offscreenUi->resize(fromGlm(newDisplayPlugin->getRecommendedUiSize()));
        }
        qApp->getApplicationCompositor().setDisplayPlugin(newDisplayPlugin);
        _displayPlugin = newDisplayPlugin;
        connect(_displayPlugin.get(), &DisplayPlugin::presented, qApp, &Application::onPresent, Qt::DirectConnection);
        if (desktop) {
            desktop->setProperty("repositionLocked", wasRepositionLocked);
        }

        RefreshRateManager& refreshRateManager = qApp->getRefreshRateManager();
        refreshRateManager.setRefreshRateOperator(OpenGLDisplayPlugin::getRefreshRateOperator());
        // the present cadence's last second, for the refresh governor
        // a ride in view keeps the full rate through the unfocused regime (Caity, 2026-10-31)
        refreshRateManager.setRideInViewSource([]() { return qApp->isRideInView(); });
        refreshRateManager.setCadenceSource([counters = OpenGLDisplayPlugin::getCadenceCountersOperator()]() {
            RefreshRateController::CadenceCounters c = counters();
            RefreshRateManager::CadenceSample sample;
            sample.displayRate = c.displayRefreshRate;
            sample.presents = c.presents;
            sample.repeated = c.repeated;
            sample.onTwoOrMoreRefreshes = c.onTwoRefreshes + c.onThreeRefreshes + c.onMoreRefreshes;
            sample.onOneRefresh = c.onOneRefresh;
            sample.missed = c.missed;
            sample.minSlackUs = c.minSlackUs;
            sample.newFrames = c.newFrames;
            sample.slowMainFrames = c.slowMainFrames;
            sample.slowBuildFrames = c.slowBuildFrames;
            sample.slowExecutes = c.slowExecutes;
            sample.maxMainUs = c.maxMainUs;
            sample.maxBuildUs = c.maxBuildUs;
            sample.maxExecuteUs = c.maxExecuteUs;
            sample.gpuMs = c.gpuMs;
            sample.rolledAtNs = c.rolledAtNs;
            sample.startedAtNs = c.startedAtNs;
            return sample;
        });
        bool isHmd = newDisplayPlugin->isHmd();
        RefreshRateManager::UXMode uxMode = isHmd ? RefreshRateManager::UXMode::VR :
            RefreshRateManager::UXMode::DESKTOP;

        refreshRateManager.setUXMode(uxMode);
        followScreenRefreshRate();
    }

    bool isHmd = _displayPlugin->isHmd();
    qCDebug(interfaceapp) << "Entering into" << (isHmd ? "HMD" : "Desktop") << "Mode";

    // Only emit after a successful change
    emit activeDisplayPluginChanged();

    // reset the avatar, to set head and hand palms back to a reasonable default pose.
    getMyAvatar()->reset(false);

    // switch to first person if entering hmd and setting is checked
    if (menu) {
        QAction* action = menu->getActionForOption(newDisplayPlugin->getName());
        if (action) {
            action->setChecked(true);
        }

        if (isHmd && menu->isOptionChecked(MenuOption::FirstPersonHMD)) {
            menu->setIsOptionChecked(MenuOption::FirstPersonLookAt, true);
            qApp->getViewState().cameraMenuChanged();
        }

        // Remove the selfie camera options from menu if in HMD mode
        auto selfieAction = menu->getActionForOption(MenuOption::SelfieCamera);
        selfieAction->setVisible(!isHmd);
    }

    Q_ASSERT_X(_displayPlugin, "Display::updateDisplayMode", "could not find an activated display plugin");
}

// FROZEN (vehicle path): see the note at the top of this file.
void Display::followScreenRefreshRate() {
    QWindow* handle = qApp->getWindow() ? qApp->getWindow()->windowHandle() : nullptr;
    if (!handle || _screenChangedConnection) {
        return;
    }
    auto followScreen = [this, handle] {
        QObject::disconnect(_screenRateConnection);
        if (QScreen* screen = handle->screen()) {
            _screenRateConnection = connect(screen, &QScreen::refreshRateChanged, this, [](qreal) {
                qApp->getRefreshRateManager().updateRefreshRateController();
            });
        }
        qApp->getRefreshRateManager().updateRefreshRateController();
    };
    _screenChangedConnection = connect(handle, &QWindow::screenChanged, this, [followScreen](QScreen*) { followScreen(); });
    followScreen();
}

void Display::startHMDStandBySession() {
    _autoSwitchDisplayModeSupportedHMDPlugin->startStandBySession();
}

void Display::endHMDSession() {
    _autoSwitchDisplayModeSupportedHMDPlugin->endSession();
}

// FROZEN (vehicle path): see the note at the top of this file.
void Display::activeChanged(Qt::ApplicationState state) {
    switch (state) {
        case Qt::ApplicationActive:
            // on Windows, Qt says active when one of our windows is activated behind another app's; then nothing changes
            _isForeground = isAppInFront();
            if (_isForeground && !qApp->isAboutToQuit() && qApp->getSignInGate().startUpFinished()) {
                qApp->getRefreshRateManager().setRefreshRateRegime(RefreshRateManager::RefreshRateRegime::FOCUS_ACTIVE);
            }
            break;

        case Qt::ApplicationSuspended:
            break;
        case Qt::ApplicationHidden:
            break;
        case Qt::ApplicationInactive:
            if (!qApp->isAboutToQuit() && qApp->getSignInGate().startUpFinished()) {
                qApp->getRefreshRateManager().setRefreshRateRegime(RefreshRateManager::RefreshRateRegime::UNFOCUS);
            }
            break;
        default:
            _isForeground = false;
            break;
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
void Display::foregroundChanged(bool inFront) {
    // a window of ours activated behind another app's gets "came to the front" too: only our own foreground window counts
    if (inFront && !isAppInFront()) {
        qCDebug(interfaceapp) << "Activated behind another app's window: the refresh regime stays";
        return;
    }
    _isForeground = inFront;
    // a minimized window keeps the minimized regime; restoring it sets the full rate (Application's WindowStateChange)
    if (!qApp->isAboutToQuit() && qApp->getSignInGate().startUpFinished() && !qApp->getWindow()->isMinimized()) {
        qApp->getRefreshRateManager().setRefreshRateRegime(inFront ? RefreshRateManager::RefreshRateRegime::FOCUS_ACTIVE :
            RefreshRateManager::RefreshRateRegime::UNFOCUS);
    }
}

// FROZEN (vehicle path): see the note at the top of this file.
void Display::checkForeground() {
#if defined(Q_OS_WIN)
    // one call to Windows; no decision before start-up has finished, while no window is in front (between two windows)
    // or in a headset, whose rate doesn't follow the regime. plugin(), not isHMDMode(): asking for the active plug-in
    // makes one when there is none
    if (qApp->isAboutToQuit() || !qApp->getSignInGate().startUpFinished() || !_displayPlugin || _displayPlugin->isHmd() ||
        !GetForegroundWindow()) {
        return;
    }
    auto regime = qApp->getRefreshRateManager().getRefreshRateRegime();
    bool regimeInFront = regime == RefreshRateManager::RefreshRateRegime::FOCUS_ACTIVE ||
                         regime == RefreshRateManager::RefreshRateRegime::FOCUS_INACTIVE;
    bool regimeBehind = regime == RefreshRateManager::RefreshRateRegime::UNFOCUS;
    bool inFront = isAppInFront();
    if ((inFront && regimeBehind) || (!inFront && regimeInFront)) {
        qCDebug(interfaceapp) << "The app is" << (inFront ? "in front" : "behind") << "(the foreground window's process); the refresh regime follows";
        foregroundChanged(inFront);
    }
#endif
}

void Display::windowMinimizedChanged(bool minimized) {
    // initialize the _minimizedWindowTimer
    static std::once_flag once;
    std::call_once(once, [&] {
        connect(&_minimizedWindowTimer, &QTimer::timeout, this, [] {
            QCoreApplication::postEvent(QCoreApplication::instance(), new QEvent(static_cast<QEvent::Type>(Idle)), Qt::HighEventPriority);
        });
    });

    // avoid rendering to the display plugin but continue posting Idle events,
    // so that physics continues to simulate and the deadlock watchdog knows we're alive
    if (!minimized && !getActiveDisplayPlugin()->isActive()) {
        _minimizedWindowTimer.stop();
        getActiveDisplayPlugin()->activate();
    } else if (minimized && getActiveDisplayPlugin()->isActive()) {
        getActiveDisplayPlugin()->deactivate();
        _minimizedWindowTimer.start(THROTTLED_SIM_FRAME_PERIOD_MS);
    }
}

void Display::switchDisplayMode() {
    if (!_autoSwitchDisplayModeSupportedHMDPlugin) {
        return;
    }

    bool currentHMDWornStatus = _autoSwitchDisplayModeSupportedHMDPlugin->isDisplayVisible();
    if (currentHMDWornStatus != _previousHMDWornStatus) {
        // Switch to respective mode as soon as currentHMDWornStatus changes
        if (currentHMDWornStatus) {
            qCDebug(interfaceapp) << "Switching from Desktop to HMD mode";
            endHMDSession();
            setActiveDisplayPlugin(_autoSwitchDisplayModeSupportedHMDPluginName);
        } else {
            qCDebug(interfaceapp) << "Switching from HMD to desktop mode";
            setActiveDisplayPlugin(DESKTOP_DISPLAY_PLUGIN_NAME);
            startHMDStandBySession();
        }
    }
    _previousHMDWornStatus = currentHMDWornStatus;
}

void Display::deactivate() {
    getActiveDisplayPlugin()->deactivate();
    if (_autoSwitchDisplayModeSupportedHMDPlugin
        && _autoSwitchDisplayModeSupportedHMDPlugin->isSessionActive()) {
        _autoSwitchDisplayModeSupportedHMDPlugin->endSession();
    }
}

void Display::releasePlugin() {
    _displayPlugin.reset();
}
