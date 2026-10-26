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

#include "Display.h"
#include "TestClientIcon.h"

#include <thread>
#include <glm/glm.hpp>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QProcessEnvironment>
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
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
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

// The field-of-view setting is a horizontal angle and the vertical one is derived from the aspect
// ratio, which runs away on a tall, narrow viewport (build mode's panels) and flips past 180
// degrees. Build mode keeps the vertical angle at its 16:9 value, so a narrower viewport crops
// the sides instead, as Unity's scene view does.
float verticalFieldOfView(float horizontalFov, float aspectRatio) {   // the frame loop's render arguments read it too
    auto editor = DependencyManager::get<Editor>();
    if (editor && editor->isActive()) {
        return horizontalFov / (16.0f / 9.0f);
    }
    return horizontalFov / aspectRatio;
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
    auto action = menu->addActionToQMenuAndActionHash(parent,
        name, QKeySequence(Qt::CTRL + (Qt::Key_0 + index)), qApp,
        SLOT(updateDisplayMode()),
        QAction::NoRole, Menu::UNSPECIFIED_POSITION, groupingMenu);

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

void Display::activeChanged(Qt::ApplicationState state) {
    switch (state) {
        case Qt::ApplicationActive:
            _isForeground = true;
            if (!qApp->isAboutToQuit() && qApp->getSignInGate().startUpFinished()) {
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
