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
#include <QtGui/QScreen>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtWidgets/QDockWidget>

#include <gl/GLHelpers.h>
#include <MainWindow.h>
#include <SettingHandle.h>

#include "Application.h"
#include "CodeEditor.h"
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
        { "Radio", "qrc:///qml/darkride/RadioPanel.qml", 320, 640 },
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
