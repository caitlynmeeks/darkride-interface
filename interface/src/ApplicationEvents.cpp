//
//  ApplicationEvents.cpp
//  interface/src
//
//  Application's event routing: notify() and event() (the main thread's view-change guard, the file-open and
//  lambda events, the keyboard focus of entities), eventFilter() (the offscreen UI's shortcuts and the tablet's
//  focus), the key, mouse, touch, wheel and drop handlers and, on Android, the lifecycle events (the background
//  entered and left, the away mode toggled). The members as they stood in Application.cpp, in their order; a home
//  of their own until an owner takes them (ARCHITECTURE.md §5.4 step 4).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "Application.h"
#if defined(Q_OS_WIN)
// before QQuickWindow, whose qopengl.h includes the whole of windows.h and with it the old winsock.h: AudioClient.h's
// webrtc headers include winsock2.h, which can't come after it
#include <winsock2.h>
#endif
#include <thread>
#include <glm/glm.hpp>
#include <QtCore/QMimeData>
#include <QtQuick/QQuickWindow>
#include <QProcessEnvironment>
#include <gl/GLHelpers.h>
#include <shared/FileUtils.h>
#include <controllers/InputRecorder.h>
#include <MainWindow.h>
#include <OffscreenUi.h>
#include <plugins/PluginManager.h>
#include <ScriptEngines.h>
#include <display-plugins/CompositorHelper.h>
#include <display-plugins/hmd/HmdDisplayPlugin.h>
#include "AudioClient.h"
#include "GLCanvas.h"
#include "scripting/Audio.h"
#include "editor/Editor.h"
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
#endif
#include "ui/StandAloneJSConsole.h"
#if defined(Q_OS_ANDROID)
#include <android/log.h>
#include "AndroidHelper.h"
#endif
#if defined(Q_OS_MACOS)
#include "AppNapDisabler.h"
#endif

Q_DECLARE_LOGGING_CATEGORY(trace_app_input_mouse)

#include "LambdaEvent.h"

using namespace std;

QSharedPointer<OffscreenUi> getOffscreenUI();   // Application.cpp's

static inline bool isKeyEvent(QEvent::Type type) {
    return type == QEvent::KeyPress || type == QEvent::KeyRelease;
}

bool Application::handleFileOpenEvent(QFileOpenEvent* fileEvent) {
    QUrl url = fileEvent->url();
    if (!url.isEmpty()) {
        QString urlString = url.toString();
        if (canAcceptURL(urlString)) {
            return acceptURL(urlString);
        }
    }
    return false;
}

bool Application::notify(QObject * object, QEvent * event) {
    if (thread() == QThread::currentThread()) {
        PROFILE_RANGE_IF_LONGER(app, "notify", 2)
        // The main window's layout passes, its moves and resizes, and the drags of its panel splitters move or resize
        // the GL view: no frame is drawn to it meanwhile, nor just after (see gl::ViewChangeGuard). Build mode's
        // panels showing and hiding crashed in Apple's GL driver.
        if (object == _window) {
            bool changesView = false;
            switch (event->type()) {
                case QEvent::LayoutRequest:
                case QEvent::Resize:
                case QEvent::Move:
                case QEvent::MouseButtonRelease:
                    changesView = true;
                    break;
                case QEvent::MouseMove:
                    changesView = static_cast<QMouseEvent*>(event)->buttons() != Qt::NoButton;   // a drag, not a hover
                    break;
                default:
                    break;
            }
            if (changesView) {
                gl::ViewChangeGuard guard(event->type());
                return QApplication::notify(object, event);
            }
        }
        return QApplication::notify(object, event);
    }

    return QApplication::notify(object, event);
}

bool Application::event(QEvent* event) {

    if (_aboutToQuit) {
        return false;
    }

    // a link (or a file) the app is opened with arrives as it starts, before the menu exists and before it can go
    // anywhere: kept for the starting location (DomainSession::goToStartingLocation), which goes there
    if (event->type() == QEvent::FileOpen && !_domainSession.startingLocationChosen()) {
        QUrl url = static_cast<QFileOpenEvent*>(event)->url();
        if (!url.isEmpty()) {
            _domainSession.keepStartupLink(url.toString());
        }
        return true;
    }

    if (!Menu::getInstance()) {
        return false;
    }

    // Allow focused Entities to handle keyboard input
    if (isKeyEvent(event->type()) && _keyboardFocus.deliverKeyEvent(event)) {
        return true;
    }

    int type = event->type();
    switch (type) {
        case ApplicationEvent::Lambda:
            static_cast<LambdaEvent*>(event)->call();
            return true;

        // Explicit idle keeps the idle running at a lower interval, but without any rendering
        // see (windowMinimizedChanged)
        case ApplicationEvent::Idle:
            idle();

#ifdef DEBUG_EVENT_QUEUE_DEPTH
            // The event queue may very well grow beyond 400, so 
            // this code should only be enabled on local builds
            {
                int count = ::hifi::qt::getEventQueueSize(QThread::currentThread());
                if (count > 400) {
                    ::hifi::qt::dumpEventQueue(QThread::currentThread());
                }
            }
#endif // DEBUG_EVENT_QUEUE_DEPTH

            _pendingIdleEvent.store(false);

            return true;

        case QEvent::MouseMove:
            mouseMoveEvent(static_cast<QMouseEvent*>(event));
            return true;
        case QEvent::MouseButtonPress:
            mousePressEvent(static_cast<QMouseEvent*>(event));
            return true;
        case QEvent::MouseButtonDblClick:
            mouseDoublePressEvent(static_cast<QMouseEvent*>(event));
            return true;
        case QEvent::MouseButtonRelease:
            mouseReleaseEvent(static_cast<QMouseEvent*>(event));
            return true;
        case QEvent::KeyPress:
            keyPressEvent(static_cast<QKeyEvent*>(event));
            return true;
        case QEvent::KeyRelease:
            keyReleaseEvent(static_cast<QKeyEvent*>(event));
            return true;
        case QEvent::FocusOut:
            focusOutEvent(static_cast<QFocusEvent*>(event));
            return true;
        case QEvent::FocusIn:
        { //testing to see if we can set focus when focus is not set to root window.
            _glWidget->activateWindow();
            _glWidget->setFocus();
            return true; 
        }

        case QEvent::TouchBegin:
            touchBeginEvent(static_cast<QTouchEvent*>(event));
            event->accept();
            return true;
        case QEvent::TouchEnd:
            touchEndEvent(static_cast<QTouchEvent*>(event));
            return true;
        case QEvent::TouchUpdate:
            touchUpdateEvent(static_cast<QTouchEvent*>(event));
            return true;
        case QEvent::Gesture:
            touchGestureEvent((QGestureEvent*)event);
            return true;
        case QEvent::Wheel:
            wheelEvent(static_cast<QWheelEvent*>(event));
            return true;
        case QEvent::Drop:
            dropEvent(static_cast<QDropEvent*>(event));
            return true;

        case QEvent::FileOpen:
            if (handleFileOpenEvent(static_cast<QFileOpenEvent*>(event))) {
                return true;
            }
            break;

        default:
            break;
    }

    return QApplication::event(event);
}

bool Application::eventFilter(QObject* object, QEvent* event) {
    auto eventType = event->type();

    if (_aboutToQuit && eventType != QEvent::DeferredDelete && eventType != QEvent::Destroy) {
        return true;
    }

#if defined(Q_OS_MACOS)
    // On Mac OS, Cmd+LeftClick is treated as a RightClick (more specifically, it seems to
    // be Cmd+RightClick without the modifier being dropped). Starting in Qt 5.12, only
    // on Mac, the MouseButtonRelease event for these mouse presses is sent to the top
    // level QWidgetWindow, but are not propagated further. This means that the Application
    // will see a MouseButtonPress, but no MouseButtonRelease, causing the client to get
    // stuck in "mouse-look." The cause of the problem is in the way QWidgetWindow processes
    // events where QMouseEvent::button() is not equal to QMouseEvent::buttons(). In this case
    // QMouseEvent::button() is Qt::RightButton, while QMouseEvent::buttons() is (correctly?)
    // Qt::LeftButton.
    //
    // The change here gets around this problem by capturing these
    // pseudo-RightClicks, and re-emitting them as "pure" RightClicks, where
    // QMouseEvent::button() == QMouseEvent::buttons() == Qt::RightButton.
    //
    if (eventType == QEvent::MouseButtonPress) {
        auto mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::RightButton
            && mouseEvent->buttons() == Qt::LeftButton
            && mouseEvent->modifiers() == Qt::MetaModifier) {

            QMouseEvent* newEvent = new QMouseEvent(
                    QEvent::MouseButtonPress, mouseEvent->localPos(), mouseEvent->windowPos(),
                    mouseEvent->screenPos(), Qt::RightButton, Qt::MouseButtons(Qt::RightButton),
                    mouseEvent->modifiers());
            QApplication::postEvent(object, newEvent);
            return true;
        }
    }
#endif

    if (eventType == QEvent::KeyPress || eventType == QEvent::KeyRelease || eventType == QEvent::MouseMove) {
        getRefreshRateManager().resetInactiveTimer();
    }

    if (event->type() == QEvent::Leave) {
        getApplicationCompositor().handleLeaveEvent();
    }

    if (event->type() == QEvent::ShortcutOverride) {
        // The check passes untouched on its way to the overlay's focused item, below
        static bool forwardingToOverlay = false;
        if (forwardingToOverlay) {
            return false;
        }
        auto offscreenUi = getOffscreenUI();
        if (offscreenUi->shouldSwallowShortcut(event)) {
            // An overlay item has the focus. It keeps the shortcut only if it claims it: a text field claims typing
            // and its edit keys, a web page its edit shortcuts. Anything else (a button, a list, whatever panel was
            // clicked last) leaves it to the menus. A focus left on any overlay item used to swallow every shortcut,
            // so Cmd+B (Build Mode) and the rest worked only now and then (Caity, 10-24: "only toggles every 4th").
            bool claimed = false;
            if (QApplication::focusWidget() == _glWidget && !offscreenUi->isPaused()) {
                // The key press that follows goes on to the overlay's focused item (OffscreenUi filters the GL widget's
                // events), so the item gets to claim the shortcut first, as it would as a widget of its own. A web page
                // acts on an edit shortcut (paste, copy, cut, select all, undo) only if it claimed it here: on macOS it
                // otherwise leaves them to the app's Edit menu (which then asks the focused item, editFocusedItem).
                event->ignore();
                forwardingToOverlay = true;
                QCoreApplication::sendEvent(offscreenUi->getWindow(), event);
                forwardingToOverlay = false;
                claimed = event->isAccepted();
            }
            if (claimed) {
                event->accept();
                return true;
            }
            // with a native widget's focus (build mode's Inspector), the overlay item never gets the keys at all
            event->ignore();
        }

        // Filter out captured keys before they're used for shortcut actions.
        if (_controllerScriptingInterface->isKeyCaptured(static_cast<QKeyEvent*>(event))) {
            event->accept();
            return true;
        }
    }

    if (event->type() == QEvent::WindowStateChange) {
        if (getWindow()->windowState() & Qt::WindowMinimized) {
            getRefreshRateManager().setRefreshRateRegime(RefreshRateManager::RefreshRateRegime::MINIMIZED);
        } else {
            auto* windowStateChangeEvent = static_cast<QWindowStateChangeEvent*>(event);
            if (windowStateChangeEvent->oldState() & Qt::WindowMinimized) {
                getRefreshRateManager().setRefreshRateRegime(RefreshRateManager::RefreshRateRegime::FOCUS_ACTIVE);
                getRefreshRateManager().resetInactiveTimer();
            }
        }
    }

    // for overview overlay
    auto jsConsole = DependencyManager::get<StandAloneJSConsole>();
    if (
        (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) &&
        hasFocus() && OffscreenQmlSurface::viewportFocused &&
        (jsConsole ? !jsConsole->getActive() : true)
    ) {
        QKeyEvent* key = static_cast<QKeyEvent*>(event);

        // only while the 3D view has the keyboard: a field in a native panel (build mode's Inspector)
        // takes Tab itself, to go on to the next field
        QWidget* focusWidget = QApplication::focusWidget();
        bool viewHasKeyboard = !focusWidget || focusWidget == getPrimaryWidget() || focusWidget == getWindow();
        if (key->key() == Qt::Key_Tab && viewHasKeyboard) {
            if (key->isAutoRepeat() == false) {
                _controllerScriptingInterface->tabKeyEvent(event->type() == QEvent::KeyPress);
            }
            return true; 
        }
    }

    return false;
}

void Application::keyPressEvent(QKeyEvent* event) {
    if (!event->isAutoRepeat()) {
        _keysPressed.insert(event->key(), *event);
    }

    if (DependencyManager::get<Editor>()->handleKeyPress(event)) {
        return;
    }

    _controllerScriptingInterface->emitKeyPressEvent(event); // send events to any registered scripts
    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface->isKeyCaptured(event) || isInterstitialMode()) {
        return;
    }

    if (hasFocus() && getLoginDialogPoppedUp()) {
        if (_keyboardMouseDevice->isActive()) {
            _keyboardMouseDevice->keyReleaseEvent(event);
        }

        bool isControlOrCommand = event->modifiers().testFlag(Qt::ControlModifier);
        bool isOption = event->modifiers().testFlag(Qt::AltModifier);
        switch (event->key()) {
            case Qt::Key_4:
            case Qt::Key_5:
            case Qt::Key_6:
            case Qt::Key_7:
                if (isControlOrCommand || isOption) {
                    unsigned int index = static_cast<unsigned int>(event->key() - Qt::Key_1);
                    auto displayPlugins = PluginManager::getInstance()->getDisplayPlugins();
                    if (index < displayPlugins.size()) {
                        auto targetPlugin = displayPlugins.at(index);
                        QString targetName = targetPlugin->getName();
                        auto menu = Menu::getInstance();
                        QAction* action = menu->getActionForOption(targetName);
                        if (action && !action->isChecked()) {
                            action->trigger();
                        }
                    }
                }
                break;
        }
    } else if (hasFocus()) {
        if (_keyboardMouseDevice->isActive()) {
            _keyboardMouseDevice->keyPressEvent(event);
        }

        bool isShifted = event->modifiers().testFlag(Qt::ShiftModifier);
        bool isControlOrCommand = event->modifiers().testFlag(Qt::ControlModifier);
        bool isMetaOrMacControl = event->modifiers().testFlag(Qt::MetaModifier);
        bool isOption = event->modifiers().testFlag(Qt::AltModifier);
        switch (event->key()) {
            case Qt::Key_Enter:
            case Qt::Key_Return:
                if (isOption) {
                    if (_window->isFullScreen()) {
                        unsetFullscreen();
                    } else {
                        setFullscreen(nullptr);
                    }
                }
                break;

            case Qt::Key_4:
            case Qt::Key_5:
            case Qt::Key_6:
            case Qt::Key_7:
                if (isControlOrCommand || isOption) {
                    unsigned int index = static_cast<unsigned int>(event->key() - Qt::Key_1);
                    auto displayPlugins = PluginManager::getInstance()->getDisplayPlugins();
                    if (index < displayPlugins.size()) {
                        auto targetPlugin = displayPlugins.at(index);
                        QString targetName = targetPlugin->getName();
                        auto menu = Menu::getInstance();
                        QAction* action = menu->getActionForOption(targetName);
                        if (action && !action->isChecked()) {
                            action->trigger();
                        }
                    }
                }
                break;

            case Qt::Key_G:
                if (isShifted && isControlOrCommand && isOption && isMetaOrMacControl) {
                    static const QString HIFI_FRAMES_FOLDER_VAR = "HIFI_FRAMES_FOLDER";
                    static const QString GPU_FRAME_FOLDER = QProcessEnvironment::systemEnvironment().contains(HIFI_FRAMES_FOLDER_VAR)
                        ? QProcessEnvironment::systemEnvironment().value(HIFI_FRAMES_FOLDER_VAR)
                        : "hifiFrames";
                    static QString GPU_FRAME_TEMPLATE = GPU_FRAME_FOLDER + "/{DATE}_{TIME}";
                    QString fullPath = FileUtils::computeDocumentPath(FileUtils::replaceDateTimeTokens(GPU_FRAME_TEMPLATE));
                    if (FileUtils::canCreateFile(fullPath)) {
                        getActiveDisplayPlugin()->captureFrame(fullPath.toStdString());
                    }
                }
                break;
            case Qt::Key_X:
                if (isShifted && isControlOrCommand) {
                    auto offscreenUi = getOffscreenUI();
                    offscreenUi->togglePinned();
                }
                break;

            case Qt::Key_Y:
                if (isShifted && isControlOrCommand) {
                    getActiveDisplayPlugin()->cycleDebugOutput();
                }
                break;

            case Qt::Key_B:
                if (isOption) {
                    controller::InputRecorder* inputRecorder = controller::InputRecorder::getInstance();
                    inputRecorder->stopPlayback();
                }
                break;

            case Qt::Key_R:
                if (isShifted) break;
                if (isControlOrCommand && !event->isAutoRepeat()) {
                    DependencyManager::get<ScriptEngines>()->reloadAllScripts();
                    getOffscreenUI()->clearCache();
                }
                break;

            case Qt::Key_M:
                if (isControlOrCommand) {
                    auto audioClient = DependencyManager::get<AudioClient>();
                    audioClient->setMuted(!audioClient->isMuted());
                    QSharedPointer<scripting::Audio> audioScriptingInterface = qSharedPointerDynamicCast<scripting::Audio>(DependencyManager::get<AudioScriptingInterface>());
                    if (audioScriptingInterface && audioScriptingInterface->getPTT()) {
                       audioScriptingInterface->setPushingToTalk(!audioClient->isMuted());
                    }
                }
                break;

            case Qt::Key_Apostrophe: {
                if (isControlOrCommand) {
                    auto cursor = Cursor::Manager::instance().getCursor();
                    auto curIcon = cursor->getIcon();
                    if (curIcon == Cursor::Icon::DEFAULT) {
                        _windowCursor.show(Cursor::Icon::RETICLE);
                    } else if (curIcon == Cursor::Icon::RETICLE) {
                        _windowCursor.show(Cursor::Icon::SYSTEM);
                    } else if (curIcon == Cursor::Icon::SYSTEM) {
                        _windowCursor.show(Cursor::Icon::LINK);
                    } else {
                        _windowCursor.show(Cursor::Icon::DEFAULT);
                    }
                } else if (!event->isAutoRepeat()){
                    resetSensors(true);
                }
                break;
            }
            
            case Qt::Key_Plus: {
                if (isControlOrCommand && event->modifiers().testFlag(Qt::KeypadModifier)) {
                    auto& cursorManager = Cursor::Manager::instance();
                    cursorManager.setScale(cursorManager.getScale() * 1.1f);
                } else {
                    getMyAvatar()->increaseSize();
                }
                break;
            }

            case Qt::Key_Minus: {
                if (isControlOrCommand && event->modifiers().testFlag(Qt::KeypadModifier)) {
                    auto& cursorManager = Cursor::Manager::instance();
                    cursorManager.setScale(cursorManager.getScale() / 1.1f);
                } else {
                    getMyAvatar()->decreaseSize();
                }
                break;
            }

            case Qt::Key_Equal:
                getMyAvatar()->resetSize();
                break;
            case Qt::Key_Escape: {
                getActiveDisplayPlugin()->abandonCalibration();
                break;
            }

            default:
                event->ignore();
                break;
        }
    }
}

void Application::keyReleaseEvent(QKeyEvent* event) {
    if (!event->isAutoRepeat()) {
        _keysPressed.remove(event->key());
    }

    if (DependencyManager::get<Editor>()->handleKeyRelease(event)) {
        return;
    }

#if defined(Q_OS_ANDROID)
    if (event->key() == Qt::Key_Back) {
        event->accept();
        AndroidHelper::instance().requestActivity("Home", false);
    }
#endif
    _controllerScriptingInterface->emitKeyReleaseEvent(event); // send events to any registered scripts

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface->isKeyCaptured(event)) {
        return;
    }

    if (_keyboardMouseDevice->isActive()) {
        _keyboardMouseDevice->keyReleaseEvent(event);
    }

}

void Application::focusOutEvent(QFocusEvent* event) {
    auto inputPlugins = PluginManager::getInstance()->getInputPlugins();
    foreach(auto inputPlugin, inputPlugins) {
        if (inputPlugin->isActive()) {
            inputPlugin->pluginFocusOutEvent();
        }
    }
    synthesizeKeyReleasEvents();
}

void Application::synthesizeKeyReleasEvents() {
    // synthesize events for keys currently pressed, since we may not get their release events
    // Because our key event handlers may manipulate _keysPressed, lets swap the keys pressed into a local copy,
    // clearing the existing list.
    QHash<int, QKeyEvent> keysPressed;
    std::swap(keysPressed, _keysPressed);
    for (auto& ev : keysPressed) {
        QKeyEvent synthesizedEvent { QKeyEvent::KeyRelease, ev.key(), Qt::NoModifier, ev.text() };
        keyReleaseEvent(&synthesizedEvent);
    }
}

void Application::maybeToggleMenuVisible(QMouseEvent* event) const {
#ifndef Q_OS_MACOS
    // If in full screen, and our main windows menu bar is hidden, and we're close to the top of the QMainWindow
    // then show the menubar.
    if (_window->isFullScreen()) {
        QMenuBar* menuBar = _window->menuBar();
        if (menuBar) {
            static const int MENU_TOGGLE_AREA = 10;
            if (!menuBar->isVisible()) {
                if (event->pos().y() <= MENU_TOGGLE_AREA) {
                    menuBar->setVisible(true);
                }
            }  else {
                if (event->pos().y() > MENU_TOGGLE_AREA) {
                    menuBar->setVisible(false);
                }
            }
        }
    }
#endif
}

void Application::mouseMoveEvent(QMouseEvent* event) {
    PROFILE_RANGE(app_input_mouse, __FUNCTION__);

    maybeToggleMenuVisible(event);

    auto& compositor = getApplicationCompositor();
    // if this is a real mouse event, and we're in HMD mode, then we should use it to move the
    // compositor reticle
    // handleRealMouseMoveEvent() will return true, if we shouldn't process the event further
    if (!compositor.fakeEventActive() && compositor.handleRealMouseMoveEvent()) {
        return; // bail
    }

    if (DependencyManager::get<Editor>()->handleMouseMove(event)) {
        return;
    }

    auto offscreenUi = getOffscreenUI();
    auto eventPosition = compositor.getMouseEventPosition(event);
    QPointF transformedPos = offscreenUi ? offscreenUi->mapToVirtualScreen(eventPosition) : QPointF();
    auto button = event->button();
    auto buttons = event->buttons();
    // Determine if the ReticleClick Action is 1 and if so, fake include the LeftMouseButton
    if (_reticleClickPressed) {
        if (button == Qt::NoButton) {
            button = Qt::LeftButton;
        }
        buttons |= Qt::LeftButton;
    }

    QMouseEvent mappedEvent(event->type(),
        transformedPos,
        event->screenPos(), button,
        buttons, event->modifiers());

    if (compositor.getReticleVisible() || !isHMDMode() || !compositor.getReticleOverDesktop() ||
        getOverlays().getOverlayAtPoint(glm::vec2(transformedPos.x(), transformedPos.y())) != UNKNOWN_ENTITY_ID) {
        getEntities()->mouseMoveEvent(&mappedEvent);
    }

    _controllerScriptingInterface->emitMouseMoveEvent(&mappedEvent); // send events to any registered scripts

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface->isMouseCaptured()) {
        return;
    }

    if (_keyboardMouseDevice->isActive()) {
        _keyboardMouseDevice->mouseMoveEvent(event);
    }
}

void Application::mousePressEvent(QMouseEvent* event) {
    auto offscreenUi = getOffscreenUI();
    // If we get a mouse press event it means it wasn't consumed by the offscreen UI,
    // hence, we should defocus all of the offscreen UI windows, in order to allow
    // keyboard shortcuts not to be swallowed by them.  In particular, WebEngineViews
    // will consume all keyboard events.
    offscreenUi->unfocusWindows();

    auto eventPosition = getApplicationCompositor().getMouseEventPosition(event);
    QPointF transformedPos = offscreenUi->mapToVirtualScreen(eventPosition);

    // in build mode the viewport belongs to the editor: no entity clicks, no avatar input
    if (DependencyManager::get<Editor>()->handleMousePress(event)) {
        return;
    }

    QMouseEvent mappedEvent(event->type(), transformedPos, event->screenPos(), event->button(), event->buttons(), event->modifiers());
    QUuid result = getEntities()->mousePressEvent(&mappedEvent);
    _keyboardFocus.setFocusedEntity(getEntities()->wantsKeyboardFocus(result) ? result : UNKNOWN_ENTITY_ID);

    _controllerScriptingInterface->emitMousePressEvent(&mappedEvent); // send events to any registered scripts

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface->isMouseCaptured()) {
        return;
    }

#if defined(Q_OS_MACOS)
    // Fix for macOS right click dragging on window when coming from a native window
    bool isFocussed = hasFocus();
    if (!isFocussed && event->button() == Qt::MouseButton::RightButton) {
        setFocus();
        isFocussed = true;
    }

    if (isFocussed) {
#else
    if (hasFocus()) {
#endif
        if (_keyboardMouseDevice->isActive()) {
            _keyboardMouseDevice->mousePressEvent(event);
        }
    }
}

void Application::mouseDoublePressEvent(QMouseEvent* event) {
    if (DependencyManager::get<Editor>()->handleMouseDoubleClick(event)) {
        return;
    }
    auto offscreenUi = getOffscreenUI();
    auto eventPosition = getApplicationCompositor().getMouseEventPosition(event);
    QPointF transformedPos = offscreenUi->mapToVirtualScreen(eventPosition);
    QMouseEvent mappedEvent(event->type(),
        transformedPos,
        event->screenPos(), event->button(),
        event->buttons(), event->modifiers());
    getEntities()->mouseDoublePressEvent(&mappedEvent);

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface->isMouseCaptured()) {
        return;
    }

    _controllerScriptingInterface->emitMouseDoublePressEvent(event);
}

void Application::mouseReleaseEvent(QMouseEvent* event) {

    auto offscreenUi = getOffscreenUI();
    auto eventPosition = getApplicationCompositor().getMouseEventPosition(event);
    QPointF transformedPos = offscreenUi->mapToVirtualScreen(eventPosition);
    QMouseEvent mappedEvent(event->type(),
        transformedPos,
        event->screenPos(), event->button(),
        event->buttons(), event->modifiers());

    if (DependencyManager::get<Editor>()->handleMouseRelease(event)) {
        return;
    }

    getEntities()->mouseReleaseEvent(&mappedEvent);

    _controllerScriptingInterface->emitMouseReleaseEvent(&mappedEvent); // send events to any registered scripts

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface->isMouseCaptured()) {
        return;
    }

    if (hasFocus()) {
        if (_keyboardMouseDevice->isActive()) {
            _keyboardMouseDevice->mouseReleaseEvent(event);
        }
    }
}

void Application::touchUpdateEvent(QTouchEvent* event) {
    if (event->type() == QEvent::TouchUpdate) {
        TouchEvent thisEvent(*event, _lastTouchEvent);
        _controllerScriptingInterface->emitTouchUpdateEvent(thisEvent); // send events to any registered scripts
        _lastTouchEvent = thisEvent;
    }

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface->isTouchCaptured()) {
        return;
    }

    if (_keyboardMouseDevice->isActive()) {
        _keyboardMouseDevice->touchUpdateEvent(event);
    }
    if (_touchscreenDevice && _touchscreenDevice->isActive()) {
        _touchscreenDevice->touchUpdateEvent(event);
    }
    if (_touchscreenVirtualPadDevice && _touchscreenVirtualPadDevice->isActive()) {
        _touchscreenVirtualPadDevice->touchUpdateEvent(event);
    }
}

void Application::touchBeginEvent(QTouchEvent* event) {
    TouchEvent thisEvent(*event); // on touch begin, we don't compare to last event
    _controllerScriptingInterface->emitTouchBeginEvent(thisEvent); // send events to any registered scripts

    _lastTouchEvent = thisEvent; // and we reset our last event to this event before we call our update
    touchUpdateEvent(event);

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface->isTouchCaptured()) {
        return;
    }

    if (_keyboardMouseDevice->isActive()) {
        _keyboardMouseDevice->touchBeginEvent(event);
    }
    if (_touchscreenDevice && _touchscreenDevice->isActive()) {
        _touchscreenDevice->touchBeginEvent(event);
    }
    if (_touchscreenVirtualPadDevice && _touchscreenVirtualPadDevice->isActive()) {
        _touchscreenVirtualPadDevice->touchBeginEvent(event);
    }

}

void Application::touchEndEvent(QTouchEvent* event) {
    TouchEvent thisEvent(*event, _lastTouchEvent);
    _controllerScriptingInterface->emitTouchEndEvent(thisEvent); // send events to any registered scripts
    _lastTouchEvent = thisEvent;

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface->isTouchCaptured()) {
        return;
    }

    if (_keyboardMouseDevice->isActive()) {
        _keyboardMouseDevice->touchEndEvent(event);
    }
    if (_touchscreenDevice && _touchscreenDevice->isActive()) {
        _touchscreenDevice->touchEndEvent(event);
    }
    if (_touchscreenVirtualPadDevice && _touchscreenVirtualPadDevice->isActive()) {
        _touchscreenVirtualPadDevice->touchEndEvent(event);
    }
    // put any application specific touch behavior below here..
}

void Application::touchGestureEvent(QGestureEvent* event) {
    if (_keyboardMouseDevice->isActive()) {
        _keyboardMouseDevice->touchGestureEvent(event);
    }
    if (_touchscreenDevice && _touchscreenDevice->isActive()) {
        _touchscreenDevice->touchGestureEvent(event);
    }
    if (_touchscreenVirtualPadDevice && _touchscreenVirtualPadDevice->isActive()) {
        _touchscreenVirtualPadDevice->touchGestureEvent(event);
    }
}

void Application::wheelEvent(QWheelEvent* event) const {
    if (DependencyManager::get<Editor>()->handleWheel(event)) {
        return;
    }

    _controllerScriptingInterface->emitWheelEvent(event); // send events to any registered scripts

    // if one of our scripts have asked to capture this event, then stop processing it
    if (_controllerScriptingInterface->isWheelCaptured() || getLoginDialogPoppedUp()) {
        return;
    }

    if (_keyboardMouseDevice->isActive()) {
        _keyboardMouseDevice->wheelEvent(event);
    }
}

void Application::dropEvent(QDropEvent *event) {
    // in build mode a dropped model or image lands in the world where it was dropped
    auto editor = DependencyManager::get<Editor>();
    if (editor && editor->isActive() && editor->handleDrop(event)) {
        event->acceptProposedAction();
        return;
    }
    const QMimeData* mimeData = event->mimeData();
    for (auto& url : mimeData->urls()) {
        QString urlString = url.toString();
        if (acceptURL(urlString, true)) {
            event->acceptProposedAction();
        }
    }
}

void Application::dragEnterEvent(QDragEnterEvent* event) {
    event->acceptProposedAction();
}

// The Android lifecycle: AndroidHelper's signals, connected at the end of the start-up (ApplicationStartup.cpp).
#if defined(Q_OS_ANDROID)
void Application::beforeEnterBackground() {
    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->setSendDomainServerCheckInEnabled(false);
    nodeList->reset("Entering background", true);
    _domainSession.clearDomainOctreeDetails();
}



void Application::enterBackground() {
    QMetaObject::invokeMethod(DependencyManager::get<AudioClient>().data(),
                              "stop", Qt::BlockingQueuedConnection);
// Quest only supports one plugin which can't be deactivated currently
#if !defined(ANDROID_APP_QUEST_INTERFACE)
    if (getActiveDisplayPlugin()->isActive()) {
        getActiveDisplayPlugin()->deactivate();
    }
#endif
}

void Application::enterForeground() {
    QMetaObject::invokeMethod(DependencyManager::get<AudioClient>().data(),
                                  "start", Qt::BlockingQueuedConnection);
// Quest only supports one plugin which can't be deactivated currently
#if !defined(ANDROID_APP_QUEST_INTERFACE)
    if (!getActiveDisplayPlugin() || getActiveDisplayPlugin()->isActive() || !getActiveDisplayPlugin()->activate()) {
        qWarning() << "Could not re-activate display plugin";
    }
#endif
    auto nodeList = DependencyManager::get<NodeList>();
    nodeList->setSendDomainServerCheckInEnabled(true);
}


void Application::toggleAwayMode(){
    QKeyEvent event = QKeyEvent (QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent (this, &event);
}


#endif
