//
//  Panels.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "Panels.h"

#include <QtGui/QGuiApplication>
#include <QtCore/QThread>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtQml/QQmlEngine>
#include <QtCore/QFileInfo>
#include <QtQml/QQmlProperty>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtWidgets/QDockWidget>

#include <gl/GLHelpers.h>
#include <shared/QtHelpers.h>
#include <ui/TabletScriptingInterface.h>
#include <MainWindow.h>
#include <OffscreenUi.h>
#include <SettingHandle.h>

#include "Application.h"
#include "CodeEditor.h"
#include "ApplicationConstants.h"
#include "InterfaceLogging.h"
#include "PanelTheme.h"
#include "QmlDock.h"
#include "StandAloneJSConsole.h"
#include "WindowLayout.h"
#include "chat/ChatWindow.h"
#include "editor/Editor.h"
#include "make/MakeBadge.h"
#include "make/MakeDock.h"

namespace {
    const QString CHAT = "Chat";
    const QString MY_FILES = "My Files";
    const QString CONSOLE = "Console";
    const QString CODE = "Code";   // the Code panel (Make v1): Qt 6's web engine only (ui/CodeEditor.h)
    const QString MAKE = "Make";   // the Make dock (Make v1, make/MakeDock.h): Qt 6 only, with the Code panel
    const QString PEOPLE = "People";
    // People's saved person and tab (PeoplePanel.qml), and its Profile tab
    const QString PEOPLE_PROFILE = "io.darkride.peoplePanel.profile";
    const QString PEOPLE_TAB = "io.darkride.peoplePanel.tab";
    const int PROFILE_TAB = 2;
    // Build mode's own, there only while it's on
    const QString RADIO = "Radio";

    const QString SLOT_LEFT = "left";
    const QString SLOT_MIDDLE = "middle";
    const QString SLOT_RIGHT = "right";
    const QString MIDDLE_FIRST = "Explore";   // the middle slot's page when nothing else has been in front
    const QString TABLET_TAB = "Tablet";   // the middle's tab for the tablet's content (its label says what it shows)
    const int SLOT_HEIGHT = 720;
    const int MIDDLE_WIDTH = 760;   // Explore's cards (the widest panel)
    std::function<bool(QQuickWindow*)> screenWindowCheck;
    std::function<void(bool)> screenShowInFront;
    std::function<void()> screenDialogOnlyDone;

    const QString HIERARCHY = "Entities";   // Build mode's Hierarchy panel (Caity, 10-27: "Entities")
    const QString INSPECTOR = "Inspector";

    const int FLOATING_HEIGHT = 720;
    struct QmlPanel {
        QString name;
        QString source;
        int width;
        int height { FLOATING_HEIGHT };
    };
    // the bar's order; at the overlay windows' widths, which the panels were laid out for: a column, or wider for
    // Explore's cards; the radio at its player's size (Webamp's two windows, 275 px wide) and its native controls
    const QList<QmlPanel> QML_PANELS {
        { "Settings", "qrc:///qml/darkride/SettingsPanel.qml", 560 },
        { "Avatar", "qrc:///qml/darkride/AvatarPanel.qml", 560 },
        { "People", "qrc:///qml/darkride/PeoplePanel.qml", 560 },
        { "Explore", "qrc:///qml/darkride/ExplorePanel.qml", 760 },
        { "Radio", "qrc:///qml/darkride/RadioPanel.qml", 295, 640 },   // the player's 275 + the margin each side
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        { CODE, "qrc:///qml/darkride/CodePanel.qml", 640 },
        { MAKE, "qrc:///qml/darkride/MakePanel.qml", 420 },
#endif
    };

    const QString TABS_BEGIN = "/* dock tabs: begin */";
    const QString TABS_END = "/* dock tabs: end */";
    // the tabs of panels docked together (the main window's own tab bars) and the edges between the panels: flat, in
    // the panels' colours (the title bars' roles, ui/DockTitleBar.cpp), the skin's where it gives them
    QString tabsSheet(const QVariantMap& skin) {
        const QVariantMap colors = skin.value("colors").toMap();
        auto role = [&colors](const QString& key, const QString& otherwise) {
            const QColor color(colors.value(key).toString());
            return color.isValid() ? color.name(QColor::HexArgb) : otherwise;
        };
        return QString(
            "QMainWindow > QTabBar { background: %1; border: none; }\n"
            "QMainWindow > QTabBar::tab { background: transparent; color: %2; border: none; border-right: 1px solid %3;"
            " padding: 4px 12px; font-size: 12px; min-width: 48px; }\n"
            "QMainWindow > QTabBar::tab:selected { background: %4; color: %5; }\n"
            "QMainWindow > QTabBar::tab:hover:!selected { background: %6; }\n"
            // the edges between the panels and the 3D view (libraries/ui MainWindow.cpp gives their width)
            "QMainWindow::separator { background: %3; }\n"
            "QMainWindow::separator:hover { background: %7; }\n")
            .arg(role("fill", "#2a2a2a"), role("secondaryText", "#9a9a9a"), role("separator", "#232323"),
                 role("card", "#3c3c3c"), role("text", "#d2d2d2"), role("cardPressed", "#323232"),
                 role("tint", "#2c5d87"));
    }
}

// floating at its first size in the middle of the main window, no bigger than the overlay window was in it (the
// window less the bar's room), kept on the main window's screen (as chat's)
void Panels::placeFloating(QDockWidget* panel, QWidget* window, QSize size) {
    size = size.boundedTo(QSize(window->width() - 16, window->height() - 96)).expandedTo(QSize(320, 240));
    QRect screen = window->screen() ? window->screen()->availableGeometry() : QRect();
    QRect rect(QPoint(), screen.isValid() ? size.boundedTo(screen.size()) : size);
    rect.moveCenter(window->geometry().center());
    if (screen.isValid()) {
        rect.moveLeft(std::max(screen.left(), std::min(rect.left(), screen.right() + 1 - rect.width())));
        rect.moveTop(std::max(screen.top(), std::min(rect.top(), screen.bottom() + 1 - rect.height())));
    }
    panel->resize(rect.size());
    panel->move(rect.topLeft());
}

Panels::Panels() {
}

void Panels::styleTabs() {
    auto window = qApp->getWindow();
    if (!window) {
        return;
    }
    // the window's sheet holds other parts too (Build mode's, Editor::applyStyle): ours goes between its markers
    QString sheet = window->styleSheet();
    const QString block = TABS_BEGIN + "\n" + tabsSheet(DependencyManager::get<PanelTheme>()->getSkin()) + TABS_END;
    int begin = sheet.indexOf(TABS_BEGIN);
    int end = sheet.indexOf(TABS_END);
    if (begin >= 0 && end > begin) {
        sheet.replace(begin, end + TABS_END.size() - begin, block);
    } else {
        sheet += "\n" + block;
    }
    window->setStyleSheet(sheet);
}

QStringList Panels::names() const {
    // chat's button sits between People's and Explore's; then My Files, and Build mode's own
    QStringList result;
    for (const auto& panel : QML_PANELS) {
        if (panel.name == CODE || panel.name == MAKE) {
            continue;   // with the tools, after My Files
        }
        if (panel.name == "Explore") {
            result << CHAT;
        }
        result << panel.name;
    }
    result << MY_FILES;
    if (MakeDock::isAvailable()) {
        result << MAKE;
    }
    if (CodeEditor::isAvailable()) {
        result << CODE;
    }
    result << CONSOLE << HIERARCHY << INSPECTOR;
    return result;
}

bool Panels::isBuildModeOnly(const QString& name) {
    return name == HIERARCHY || name == INSPECTOR;
}

bool Panels::isOpen(const QString& name) const {
    QMutexLocker lock(&_openMutex);
    return _open.contains(name);
}

void Panels::open(const QString& name) {
    QMetaObject::invokeMethod(this, [this, name] { change(name, Change::Open); });
}

void Panels::close(const QString& name) {
    QMetaObject::invokeMethod(this, [this, name] { change(name, Change::Close); });
}

void Panels::toggle(const QString& name) {
    QMetaObject::invokeMethod(this, [this, name] { change(name, Change::Toggle); });
}

void Panels::showProfile(const QString& username) {
    QMetaObject::invokeMethod(this, [this, username] {
        // the person and the tab as People reads them when it shows (PeoplePanel.qml readSettings)
        Setting::Handle<QVariant>(PEOPLE_PROFILE).set(username);
        Setting::Handle<QVariant>(PEOPLE_TAB).set(PROFILE_TAB);
        // in sight already, it hears it at once; else it reads them as it shows
        auto people = qobject_cast<QmlDock*>(_restored ? dock(PEOPLE) : nullptr);
        QQuickItem* page = people ? people->panel() : nullptr;
        if (page && page->isVisible()) {
            QMetaObject::invokeMethod(page, "showProfile", Q_ARG(QVariant, username));
        }
        change(PEOPLE, Change::Open);
    });
}

QDockWidget* Panels::dock(const QString& name) {
    if (name == CHAT || name == MY_FILES || name == CONSOLE || isBuildModeOnly(name)) {
        auto editor = DependencyManager::get<Editor>();
        QDockWidget* made = name == CHAT ? ChatWindow::dock()
            : name == MY_FILES ? editor->myFilesPanel()
            : name == CONSOLE ? DependencyManager::get<StandAloneJSConsole>()->dock()
            : name == HIERARCHY ? editor->hierarchyPanel() : editor->inspectorPanel();
        watch(name, made);
        return made;
    }
    if (QDockWidget* made = _docks.value(name)) {
        return made;
    }
    auto window = qApp->getWindow();
    if (!window) {
        return nullptr;
    }
    for (const auto& panel : QML_PANELS) {
        if (panel.name != name) {
            continue;
        }
        gl::ViewChangeGuard guard;
        auto made = new QmlDock(panel.name, QUrl(panel.source), window);
        if (name == CODE) {
            made->setContextObject("CodeEditor", DependencyManager::get<CodeEditor>().data());
        }
        if (name == MAKE) {
            // MakeDock on the Make dock's own context alone; it watches the dock's window for real input and gives
            // the engine its picture provider once the view is made
            made->setContextObject("MakeDock", DependencyManager::get<MakeDock>().data());
            // the Make badge shows only while this panel is out of sight (closed, or a tab behind another)
            connect(made, &QDockWidget::visibilityChanged, this, [](bool visible) {
                if (auto dock = DependencyManager::get<MakeDock>()) {
                    dock->badge()->setPanelInSight(visible);
                }
            });
            connect(made, &QmlDock::contentMade, this, [made] {
                if (auto dock = DependencyManager::get<MakeDock>()) {
                    dock->watchWindow(qobject_cast<QQuickWindow*>(made->contentWindow()),
                                      made->panel() ? qmlEngine(made->panel()) : nullptr);
                }
            });
        }
        // hidden until it's opened or the saved layout shows it
        made->hide();
        // where the saved layout had it (restored after sign-in), open or closed as it was; asked before it's added
        // anywhere, as adding it drops the layout's place for it. Else floating, so opening it never resizes the 3D
        // view
        if (!WindowLayout::instance()->restoreDock(made)) {
            window->addDockWidget(Qt::RightDockWidgetArea, made);
            made->setFloating(true);
            placeFloating(made, window, QSize(panel.width, panel.height));
        }
        WindowLayout::instance()->watch(made);
        _docks[name] = made;
        watch(name, made);
        return made;
    }
    return nullptr;
}

void Panels::restore() {
    _restored = true;
    if (auto window = qApp->getWindow()) {
        // panels docked together show their tabs along the top, as a Mac app's do; and a side can hold panels side
        // by side as well as one above another
        window->setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
        window->setDockOptions(window->dockOptions() | QMainWindow::AllowNestedDocks);
        styleTabs();
        connect(DependencyManager::get<PanelTheme>().data(), &PanelTheme::changed, this, &Panels::styleTabs,
                Qt::UniqueConnection);
    }
    for (const QString& name : names()) {
        dock(name);
    }
    connect(qApp, &Application::activeDisplayPluginChanged, this, &Panels::displayChanged, Qt::UniqueConnection);
    hookTabletWindow();
    // test builds only (team/editor-ui/test-kit/vr-screen): slots a test script made stand in for vr's, any window but
    // the desktop's own; set only from the environment, which no world reaches
    if (!screenWindowCheck && qEnvironmentVariableIsSet("DARKRIDE_SCREEN_TEST_SLOTS")) {
        screenWindowCheck = [](QQuickWindow* window) {
            auto offscreenUi = DependencyManager::get<OffscreenUi>();
            return window && (!offscreenUi || window != offscreenUi->getWindow());
        };
        qCWarning(interfaceapp) << "Panels: TEST screen slots allowed (DARKRIDE_SCREEN_TEST_SLOTS)";
    }
    // Build mode's own panels are made as it first comes on: watched from then
    connect(DependencyManager::get<Editor>().data(), &Editor::activeChanged, this, [this] {
        dock(HIERARCHY);
        dock(INSPECTOR);
    });
}

void Panels::resetDocks() {
    auto window = qApp->getWindow();
    if (!window) {
        return;
    }
    gl::ViewChangeGuard guard;
    for (const auto& panel : QML_PANELS) {
        if (QDockWidget* made = _docks.value(panel.name)) {
            made->hide();
            made->setFloating(true);
            placeFloating(made, window, QSize(panel.width, panel.height));
        }
    }
    // the console closed, as on a first start (Caity, 10-27), which stops what was typed into it, and back in its
    // first place
    if (QDockWidget* console = DependencyManager::get<StandAloneJSConsole>()->dock()) {
        if (!console->isHidden()) {
            console->close();
        }
        console->setFloating(true);
        placeFloating(console, window, QSize(CONSOLE_WIDTH, CONSOLE_HEIGHT));
    }
}

void Panels::releaseQml() {
    for (const auto& made : _docks) {
        if (auto qmlDock = qobject_cast<QmlDock*>(made.data())) {
            qmlDock->releaseContent();
        }
    }
}

void Panels::change(const QString& name, Change how) {
    // a page made before start-up is done finds the avatar and the rest not ready (a crash, 10-26)
    if (!_restored) {
        qCDebug(interfaceapp) << "Panels: not yet, before sign-in:" << name;
        return;
    }
    // the tablet's tab on the VR screen (its tab clicked)
    if (name == TABLET_TAB) {
        if (how != Change::Close && !_screenSlots.isEmpty()) {
            putTabletOnScreen();
        }
        return;
    }
    // Build mode's own panels, only while it's on
    if (isBuildModeOnly(name) && !DependencyManager::get<Editor>()->isActive()) {
        return;
    }
    QDockWidget* panel = dock(name);
    if (!panel) {
        qCWarning(interfaceapp) << "There is no panel called" << name;
        return;
    }
    // chat's own toggle also puts the keyboard in its message box
    if (name == CHAT && how == Change::Toggle) {
        ChatWindow::toggle();
        return;
    }
    // the VR screen, while it's up: a page with a slot opens there, and closes back into its dock
    if (!_screenSlots.isEmpty() && !screenSlotFor(name).isEmpty()) {
        auto qmlDock = qobject_cast<QmlDock*>(panel);
        const bool inFront = qmlDock && qmlDock->isAway() &&
            (screenSlotFor(name) != SLOT_MIDDLE || (!_middleTabs.isEmpty() && _middleTabs.last() == name));
        if (how == Change::Close || (how == Change::Toggle && inFront)) {
            if (qmlDock && qmlDock->isAway()) {
                qmlDock->moveBack(false);
            }
            _middleTabs.removeAll(name);
            panel->close();
            setOpen(name, false);
            if (name == RADIO) {
                emit screenSlotSizeChanged(SLOT_RIGHT);
            } else if (screenSlotFor(name) == SLOT_MIDDLE && !_middleTabs.isEmpty()) {
                putOnScreen(_middleTabs.last());
            }
            return;
        }
        if (putOnScreen(name)) {
            return;
        }
        // the radio turned on while the screen is up and has no right slot yet: on now (its page hears it's open),
        // and vr makes the slot from the size, which moves it in (screenSlotReady)
        if (name == RADIO && qmlDock && qmlDock->makePage()) {
            setOpen(RADIO, true);
            emit screenSlotSizeChanged(SLOT_RIGHT);
            return;
        }
    }
    // tabbed behind another, a docked panel is still visible to Qt, moved out of sight (QWidgetAnimator)
    const bool inSight = panel->isVisible() && !panel->visibleRegion().isEmpty();
    const bool inFront = inSight && (!panel->isFloating() || panel->isActiveWindow());
    // a window coming or going beside the viewer's, or a panel in it, changes the GL view
    gl::ViewChangeGuard guard;
    if (how == Change::Close || (how == Change::Toggle && inFront)) {
        // closed, not only hidden: a panel tells the two apart (floating and docking hide it for a moment; the
        // console stops what was typed into it only on a close)
        panel->close();
        return;
    }
    panel->show();
    // in front of the other windows when floating; docked among others as tabs, the tab in front. Raising or activating
    // a floating window brings the whole app to the front on the Mac, so a floating one only while Darkride is the app
    // in use (a click on the bar is), never from a script while Darkride is behind (a background test client took the
    // focus from the person's app, 11-07)
    if (!panel->isFloating()) {
        panel->raise();
    } else if (QGuiApplication::applicationState() == Qt::ApplicationActive) {
        panel->raise();
        panel->activateWindow();
    }
    showDockPages();
}

QString Panels::screenSlotFor(const QString& name) const {
    if (name == PEOPLE) {
        return SLOT_LEFT;
    }
    if (name == RADIO) {
        return SLOT_RIGHT;
    }
    for (const auto& panel : QML_PANELS) {
        if (panel.name == name) {
            return SLOT_MIDDLE;
        }
    }
    return QString();   // Chat, My Files, the console, Build mode's: not on the screen (yet)
}

bool Panels::isScreenSlotUrl(const QString& url) {
    static const QStringList SLOT_URLS {
        "qrc:///qml/darkride/ScreenSlotLeft.qml", "qrc:///qml/darkride/ScreenSlotMiddle.qml",
        "qrc:///qml/darkride/ScreenSlotRight.qml"
    };
    return SLOT_URLS.contains(url);
}

void Panels::setScreenShowInFront(std::function<void(bool dialogOnly)> showInFront, std::function<void()> dialogOnlyDone) {
    screenShowInFront = showInFront;
    screenDialogOnlyDone = dialogOnlyDone;
}

bool Panels::cancelScreenDialog() {
    QQuickItem* host = _screenSlots.value(SLOT_MIDDLE);
    QQuickItem* dialogs = host ? host->findChild<QQuickItem*>("dialogs") : nullptr;
    const QList<QQuickItem*> waiting = dialogs ? dialogs->childItems() : QList<QQuickItem*>();
    if (waiting.isEmpty()) {
        return false;
    }
    // the newest, as Escape answers it on the desktop: its Cancel (or No, Close: the dialogs' own Escape order)
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(waiting.last(), &press);
    return true;
}

void Panels::setScreenWindowCheck(std::function<bool(QQuickWindow*)> check) {
    screenWindowCheck = check;
}

void Panels::screenSlotReady(const QString& slot, QQuickItem* host) {
    // the slot's QML is made while the Web entity's renderer holds its lock (buildWebSurface), which vr's window check
    // takes too: the slot is taken on the next turn of the event loop, and no page moves in under that lock
    QPointer<QQuickItem> guarded(host);
    QMetaObject::invokeMethod(this, [this, slot, guarded] { takeScreenSlot(slot, guarded); }, Qt::QueuedConnection);
}

void Panels::takeScreenSlot(const QString& slot, QQuickItem* host) {
    if (slot != SLOT_LEFT && slot != SLOT_MIDDLE && slot != SLOT_RIGHT) {
        return;
    }
    // only the screen's own surfaces, made by the app (a world's script could make a Web entity showing the same QML)
    QQuickWindow* window = host ? host->window() : nullptr;
    if (!window || !screenWindowCheck || !screenWindowCheck(window)) {
        qCWarning(interfaceapp) << "Panels: a screen slot from a window that isn't the screen's, refused:" << slot;
        return;
    }
    if (_screenSlots.value(slot) == host) {
        return;
    }
    _screenSlots[slot] = host;
    qCDebug(interfaceapp) << "Panels: screen slot ready:" << slot << host->size();
    if (slot == SLOT_MIDDLE) {
        // the app's dialogs show over the middle slot, not the HUD (it can be behind you, or hidden on a ride: vr)
        if (auto offscreenUi = DependencyManager::get<OffscreenUi>()) {
            offscreenUi->setDialogHost(host->findChild<QQuickItem*>("dialogs"));
        }
    }
    if (slot == SLOT_LEFT) {
        putOnScreen(PEOPLE);
    } else if (slot == SLOT_RIGHT) {
        // the radio comes along only while it's on (from its first showing until it's closed: RadioPanel.qml)
        auto radio = qobject_cast<QmlDock*>(dock(RADIO));
        QQuickItem* page = radio ? radio->panel() : nullptr;
        if (page && page->property("on").toBool()) {
            putOnScreen(RADIO);
        }
    } else if (!_middleTabs.isEmpty() && _middleTabs.last() == TABLET_TAB) {
        putTabletOnScreen();
    } else {
        putOnScreen(_middleTabs.isEmpty() ? MIDDLE_FIRST : _middleTabs.last());
    }
}

bool Panels::putOnScreen(const QString& name) {
    const QString slot = screenSlotFor(name);
    QQuickItem* host = _screenSlots.value(slot);
    auto qmlDock = qobject_cast<QmlDock*>(slot.isEmpty() ? nullptr : dock(name));
    if (!host || !qmlDock) {
        return false;
    }
    // the middle slot holds one page in front (its tabs, the host's): the one there goes back to its dock, unseen
    if (slot == SLOT_MIDDLE) {
        if (!_middleTabs.isEmpty() && _middleTabs.last() != name) {
            if (_middleTabs.last() == TABLET_TAB) {
                takeTabletOffScreen();
            } else if (auto front = qobject_cast<QmlDock*>(dock(_middleTabs.last()))) {
                if (front->isAway()) {
                    front->moveBack(false);
                }
            }
        }
        _middleTabs.removeAll(name);
        _middleTabs.append(name);
        showMiddleTabs();
    }
    QQuickItem* content = host->findChild<QQuickItem*>("content");
    if (!qmlDock->makePage() || !qmlDock->moveTo(content ? content : host)) {
        return false;
    }
    if (qmlDock->isVisible()) {
        _hiddenForScreen.insert(name);
        qmlDock->hide();   // an empty dock; the page is on the screen
    }
    setOpen(name, true);
    if (slot == SLOT_RIGHT) {
        emit screenSlotSizeChanged(SLOT_RIGHT);
    }
    return true;
}

QSize Panels::screenSlotSize(const QString& slot) const {
    if (slot == SLOT_LEFT) {
        return QSize(560, SLOT_HEIGHT);
    }
    if (slot == SLOT_MIDDLE) {
        return QSize(MIDDLE_WIDTH, SLOT_HEIGHT);
    }
    if (slot == SLOT_RIGHT) {
        // while the radio is on (from its first showing until it's closed), moved in yet or not: vr makes the slot
        // from this, and the radio moves in once it's there
        auto radio = qobject_cast<QmlDock*>(_docks.value(RADIO).data());
        QQuickItem* page = radio ? radio->panel() : nullptr;
        return page && page->property("on").toBool() ? QSize(320, 640) : QSize();
    }
    return QSize();
}

void Panels::setScreenState(const QString& state) {
    if ((state == "down" || state == "parked") && state != _screenState) {
        _screenState = state;
        reportScreenShown(true);
        emit screenStateChanged(state);
    }
}

void Panels::reportScreenShown(bool shown) {
    // scripts ask the system tablet whether it's shown (a world's air-draw stops drawing while it is): the screen now
    auto tablets = DependencyManager::get<TabletScriptingInterface>();
    QObject* tablet = tablets ? tablets->getTablet(SYSTEM_TABLET) : nullptr;
    if (tablet && tablet->property("tabletShown").toBool() != shown) {
        tablet->setProperty("tabletShown", shown);
    }
}

void Panels::releaseScreenSlots() {
    const bool shown = !qApp->isHMDMode();
    if (auto offscreenUi = DependencyManager::get<OffscreenUi>()) {
        offscreenUi->setDialogHost(nullptr);   // a dialog still waiting goes back to the desktop
    }
    takeTabletOffScreen();
    hideTabletWindow(!shown);
    QStringList movedBack;
    for (auto it = _docks.cbegin(); it != _docks.cend(); ++it) {
        auto qmlDock = qobject_cast<QmlDock*>(it.value().data());
        if (qmlDock && qmlDock->isAway()) {
            qmlDock->moveBack(shown);
            movedBack.append(it.key());
        }
    }
    // the middle's tabs behind the front one are in their docks already, unseen
    for (const QString& name : _middleTabs) {
        if (name != TABLET_TAB && !movedBack.contains(name)) {
            movedBack.append(name);
        }
    }
    _screenSlots.clear();
    _middleTabs.clear();
    // out of the headset, the docks that were showing before the screen took their pages show again, and a panel
    // that was only ever on the screen is closed; in the headset they wait unseen (displayChanged, as it comes off)
    if (shown) {
        showHiddenForScreen();
        for (const QString& name : movedBack) {
            QDockWidget* panel = _docks.value(name);
            setOpen(name, panel && !panel->isHidden());
        }
    }
    if (!_screenState.isEmpty()) {
        _screenState.clear();
        reportScreenShown(false);
        emit screenStateChanged(_screenState);
    }
}

void Panels::testReleaseScreenSlots() {
    if (!qEnvironmentVariableIsSet("DARKRIDE_SCREEN_TEST_SLOTS")) {
        return;
    }
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "testReleaseScreenSlots");
        return;
    }
    releaseScreenSlots();
}

void Panels::testSetScreenState(const QString& state) {
    if (!qEnvironmentVariableIsSet("DARKRIDE_SCREEN_TEST_SLOTS")) {
        return;
    }
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "testSetScreenState", Q_ARG(QString, state));
        return;
    }
    setScreenState(state);
}

void Panels::displayChanged() {
    // leaving the headset, the screen's pages come home first (vr releases them before deleting its slots; this is
    // the fallback)
    if (!qApp->isHMDMode()) {
        if (!_screenSlots.isEmpty()) {
            releaseScreenSlots();
        }
        showHiddenForScreen();
    }
    hideTabletWindow(qApp->isHMDMode());
    showDockPages();
}

void Panels::showHiddenForScreen() {
    for (const QString& name : _hiddenForScreen) {
        QDockWidget* panel = _docks.value(name);
        auto qmlDock = qobject_cast<QmlDock*>(panel);
        if (panel && !(qmlDock && qmlDock->isAway())) {
            panel->show();
        }
    }
    _hiddenForScreen.clear();
}

void Panels::showMiddleTabs() {
    QQuickItem* host = _screenSlots.value(SLOT_MIDDLE);
    if (!host) {
        return;
    }
    host->setProperty("labels", QVariantMap { { TABLET_TAB, _tabletLabel } });
    host->setProperty("tabs", _middleTabs);
}

void Panels::hookTabletWindow() {
    auto tablets = DependencyManager::get<TabletScriptingInterface>();
    auto tablet = tablets ? tablets->getTablet(SYSTEM_TABLET) : nullptr;
    if (!tablet) {
        return;
    }
    connect(tablet, &TabletProxy::toolbarModeChanged, this, &Panels::hookTabletWindow, Qt::UniqueConnection);
    connect(tablet, &TabletProxy::screenChanged, this, &Panels::tabletScreenChanged, Qt::UniqueConnection);
    QQuickItem* window = tablet->getToolbarWindowItem();
    if (window == _tabletWindow) {
        return;
    }
    // a new window (toolbar mode turned on or off): the old one went with its content
    if (_tabletOnScreen || _middleTabs.contains(TABLET_TAB)) {
        _tabletOnScreen = false;
        _middleTabs.removeAll(TABLET_TAB);
        showMiddleTabs();
    }
    _tabletWindow = window;
    _tabletLoader = window ? window->findChild<QQuickItem*>("loader") : nullptr;
    _tabletLoaderParent = _tabletLoader ? _tabletLoader->parentItem() : nullptr;
    _tabletWindowParent = window ? window->parentItem() : nullptr;
    if (window) {
        QQmlProperty(window, "shown").connectNotifySignal(this, SLOT(tabletWindowShownChanged()));
        hideTabletWindow(qApp->isHMDMode());
    }
}

void Panels::tabletWindowShownChanged() {
    if (!_tabletWindow) {
        return;
    }
    const bool shown = _tabletWindow->property("shown").toBool();
    if (shown && _screenSlots.value(SLOT_MIDDLE)) {
        putTabletOnScreen();
    } else if (!shown && _middleTabs.contains(TABLET_TAB)) {
        // the tablet closed what it showed: its tab goes, the tab before it comes to the front
        const bool wasFront = _middleTabs.last() == TABLET_TAB;
        takeTabletOffScreen();
        _middleTabs.removeAll(TABLET_TAB);
        showMiddleTabs();
        if (wasFront && !_middleTabs.isEmpty()) {
            putOnScreen(_middleTabs.last());
        }
    }
}

void Panels::tabletScreenChanged(const QVariant& type, const QVariant& url) {
    // the tab says what the tablet shows: chat, a world's page by its site, an app by its name
    const QUrl address(url.toString());
    QString label = address.host().startsWith("chat.") ? "Chat" : address.host();
    if (label.startsWith("www.")) {
        label = label.mid(4);
    }
    if (type.toString() == "QML" || label.isEmpty()) {
        label = QFileInfo(address.path()).completeBaseName().remove("Panel").remove("App");
    }
    _tabletLabel = label.isEmpty() ? QString("Apps") : label.left(24);
    // a page loaded while the screen is up is a tab even when it isn't brought forward (a world's: security's rule)
    if (!_screenSlots.isEmpty() && type.toString() != "Home" && !_middleTabs.contains(TABLET_TAB)) {
        _middleTabs.prepend(TABLET_TAB);
    }
    showMiddleTabs();
}

bool Panels::putTabletOnScreen() {
    QQuickItem* host = _screenSlots.value(SLOT_MIDDLE);
    QQuickItem* content = host ? host->findChild<QQuickItem*>("content") : nullptr;
    if (!content || !_tabletLoader) {
        return false;
    }
    if (!_middleTabs.isEmpty() && _middleTabs.last() != TABLET_TAB) {
        if (auto front = qobject_cast<QmlDock*>(dock(_middleTabs.last()))) {
            if (front->isAway()) {
                front->moveBack(false);
            }
        }
    }
    _middleTabs.removeAll(TABLET_TAB);
    _middleTabs.append(TABLET_TAB);
    showMiddleTabs();
    if (!_tabletOnScreen) {
        _tabletLoaderSize = _tabletLoader->size();
        _tabletLoaderParent = _tabletLoader->parentItem();
        _tabletLoader->setPosition(QPointF(0, 0));
        _tabletLoader->setSize(content->size());
        _tabletLoader->setParentItem(content);   // the slot lays it out at its scale (ScreenSlot.qml)
        _tabletOnScreen = true;
    }
    hideTabletWindow(true);
    return true;
}

void Panels::takeTabletOffScreen() {
    if (_tabletOnScreen && _tabletLoader && _tabletLoaderParent) {
        _tabletLoader->setParentItem(_tabletLoaderParent);
        _tabletLoader->setScale(1.0);
        _tabletLoader->setPosition(QPointF(0, 0));
        _tabletLoader->setSize(_tabletLoaderSize);
    }
    _tabletOnScreen = false;
}

void Panels::hideTabletWindow(bool hide) {
    if (!_tabletWindow) {
        return;
    }
    if (hide) {
        if (!_tabletHolder) {
            auto offscreenUi = DependencyManager::get<OffscreenUi>();
            QQuickItem* desktop = offscreenUi ? offscreenUi->getRootItem() : nullptr;
            if (!desktop) {
                return;
            }
            _tabletHolder = new QQuickItem(desktop);
            _tabletHolder->setVisible(false);
        }
        if (_tabletWindow->parentItem() != _tabletHolder) {
            _tabletWindowParent = _tabletWindow->parentItem();
            _tabletWindow->setParentItem(_tabletHolder);
        }
    } else if (_tabletHolder && _tabletWindow->parentItem() == _tabletHolder && _tabletWindowParent) {
        _tabletWindow->setParentItem(_tabletWindowParent);
    }
}

void Panels::showDockPages() {
    const bool shown = !qApp->isHMDMode();
    for (const auto& dockWidget : _docks) {
        if (auto qmlDock = qobject_cast<QmlDock*>(dockWidget.data())) {
            qmlDock->setPageShown(shown);
        }
    }
}

void Panels::watch(const QString& name, QDockWidget* dock) {
    if (!dock || dock->property("panelsWatched").toBool()) {
        return;
    }
    dock->setProperty("panelsWatched", true);
    QPointer<QDockWidget> watched(dock);
    connect(dock, &QDockWidget::visibilityChanged, this, [this, name, watched] {
        if (watched) {
            setOpen(name, !watched->isHidden());
        }
    });
    setOpen(name, !dock->isHidden());
}

void Panels::setOpen(const QString& name, bool open) {
    {
        QMutexLocker lock(&_openMutex);
        if (_open.contains(name) == open) {
            return;
        }
        if (open) {
            _open.insert(name);
        } else {
            _open.remove(name);
        }
    }
    emit openChanged(name, open);
}
