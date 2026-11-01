//
//  QmlDock.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "QmlDock.h"

#include <QtCore/QCoreApplication>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtGui/QKeyEvent>
#include <QtGui/QSurfaceFormat>
#include <QtQuick/QQuickView>
#else
#include <QtQuickWidgets/QQuickWidget>
#endif
#include <QtCore/QSet>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QAbstractButton>

#include <gl/GLHelpers.h>
#include <OffscreenUi.h>

#include "DockTitleBar.h"
#include "InterfaceLogging.h"

// Return, Enter and "/" never leave a panel, whatever has its keyboard (a text field, a button, a row or nothing): in the 3D
// view they open Local Chat (ui/ChatButton.cpp), and a text field doesn't take the Return that finishes it (Qt's design, so a
// dialog hears it too), so that Return went on to the controls and chat opened, taking the keyboard from the field (Caity's
// Avatar search, 11-08). Held back the way chat looks for them: with Shift too ("/" on a Spanish keyboard), not with Control,
// Option or Command (Option-Return is full screen, from a panel too).
static bool staysInPanel(const QKeyEvent* event) {
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        return false;
    }
    const int key = event->key();
    return key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Slash;
}

// The keys a panel's page took, and the ones that stay in a panel (staysInPanel), taken here. The page sees every release as
// ever; what stops is the send onward, for a key whose press it took, as that press never went on either. Only a fresh press
// counts: a repeat can belong to a key held before the page had the keyboard, and the controls are waiting for that key's
// release. Each fresh press sets or clears its key, so a press that went on always has its release go on too. A key stays
// taken through its repeats' releases (X11 sends them in pairs).
class PageKeys {
public:
    void pressed(QKeyEvent* event) {
        if (staysInPanel(event)) {
            event->accept();
        }
        if (event->isAutoRepeat()) {
            return;
        }
        if (event->isAccepted()) {
            _taken.insert(event->key());
        } else {
            _taken.remove(event->key());
        }
    }
    bool isTakenRelease(const QKeyEvent* event) {
        const bool taken = _taken.contains(event->key());
        if (!event->isAutoRepeat()) {
            _taken.remove(event->key());
        }
        return taken;
    }
    void clear() { _taken.clear(); }

private:
    QSet<int> _taken;
};

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
// The panel's window on Qt 6. A window's keys don't climb to its container as a widget's do, so a key the page didn't
// take goes on to the controls from here, as QmlDock::event sends a QQuickWidget's on Qt 5 (push to talk's T, walking).
class DockQuickView : public QQuickView {
public:
    explicit DockQuickView(QQmlEngine* engine) : QQuickView(engine, nullptr) {}

protected:
    void keyPressEvent(QKeyEvent* event) override {
        QQuickView::keyPressEvent(event);
        _pageKeys.pressed(event);
        if (!event->isAccepted() && QCoreApplication::sendEvent(QCoreApplication::instance(), event)) {
            event->accept();
        }
    }
    void keyReleaseEvent(QKeyEvent* event) override {
        QQuickView::keyReleaseEvent(event);
        if (_pageKeys.isTakenRelease(event)) {
            event->accept();
            return;
        }
        if (!event->isAccepted() && QCoreApplication::sendEvent(QCoreApplication::instance(), event)) {
            event->accept();
        }
    }
    void focusOutEvent(QFocusEvent* event) override {
        QQuickView::focusOutEvent(event);
        _pageKeys.clear();
    }

private:
    PageKeys _pageKeys;
};
#else
// The panel's widget on Qt 5: a key the page didn't take climbs to QmlDock::event by itself.
class DockQuickWidget : public QQuickWidget {
public:
    DockQuickWidget(QQmlEngine* engine, QWidget* parent) : QQuickWidget(engine, parent) {}

protected:
    void keyPressEvent(QKeyEvent* event) override {
        QQuickWidget::keyPressEvent(event);
        _pageKeys.pressed(event);
    }
    void keyReleaseEvent(QKeyEvent* event) override {
        QQuickWidget::keyReleaseEvent(event);
        if (_pageKeys.isTakenRelease(event)) {
            event->accept();
        }
    }
    void focusOutEvent(QFocusEvent* event) override {
        QQuickWidget::focusOutEvent(event);
        _pageKeys.clear();
    }

private:
    PageKeys _pageKeys;
};
#endif

QmlDock::QmlDock(const QString& name, const QUrl& source, QWidget* parent) : QDockWidget(name, parent), _source(source) {
    setObjectName(name + "Dock");
    // the bottom too, where My Files docks, so they tab with it (Caity, 10-27)
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
    setFeatures(DockWidgetClosable | DockWidgetMovable | DockWidgetFloatable);
    setTitleBarWidget(new DockTitleBar(this));
    for (auto button : findChildren<QAbstractButton*>()) {
        button->installEventFilter(this);
    }
    // its QML made the first time it shows (a drag, the saved layout, or Panels), and hidden with it
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible) {
            makeContent();
        }
        // a page on the VR screen isn't the dock's to show or hide
        if (QQuickItem* page = _away ? nullptr : panel()) {
            page->setVisible(visible);
        }
    });
    connect(this, &QDockWidget::topLevelChanged, this, [this](bool floating) {
        qCDebug(interfaceapp) << objectName() << (floating ? "floating" : "docked");
    });
}

// Made before the dock's own window exists: a panel first opened floating (never docked) showed a blank square until
// it was docked once (Caity's friends test, 10-28, the Avatar panel). Its QML view, made only after the window was on
// screen, was the window's first OpenGL widget, and Qt then destroys and recreates the window, which on macOS (a tool
// window) came back empty. Made first, the window is created for it.
void QmlDock::setVisible(bool visible) {
    if (visible) {
        makeContent();
    }
    QDockWidget::setVisible(visible);
}

void QmlDock::setContextObject(const QString& name, QObject* object) {
    _contextObjects.append({ name, object });
}

void QmlDock::makeContent() {
    if (_view) {
        return;
    }
    auto offscreenUi = DependencyManager::get<OffscreenUi>();
    QQmlContext* desktop = offscreenUi ? offscreenUi->getSurfaceContext() : nullptr;
    if (!desktop) {
        qCWarning(interfaceapp) << objectName() << "has no QML engine to make its panel in";
        return;
    }
    gl::ViewChangeGuard guard;
    QQmlEngine* engine = desktop->engine();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    _view = new DockQuickView(engine);   // on the basic render loop: main.cpp
    QSurfaceFormat format = getDefaultOpenGLSurfaceFormat();
    format.setSwapInterval(0);   // a swap on the main thread must never wait for the display
    _view->setFormat(format);
    _view->setResizeMode(QQuickView::SizeRootObjectToView);
    _view->setColor(Qt::black);   // what shows before the page's first frame, instead of white
#else
    _view = new DockQuickWidget(engine, this);
    _view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    _view->setMinimumSize(320, 240);
#endif
    // the names the overlay panels find are the desktop's: the panel is made in a context under it
    auto context = new QQmlContext(desktop, _view);
    for (const auto& named : _contextObjects) {
        context->setContextProperty(named.first, named.second.data());
    }
    auto component = new QQmlComponent(engine, _source, QQmlComponent::PreferSynchronous, _view);
    QObject* object = component->isReady() ? component->create(context) : nullptr;
    if (!object) {
        qCWarning(interfaceapp) << objectName() << "couldn't make" << _source << component->errorString();
        return;
    }
    _view->setContent(_source, component, object);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    _container = QWidget::createWindowContainer(_view, this);
    _container->setMinimumSize(320, 240);
    _container->setFocusPolicy(Qt::StrongFocus);
    setWidget(_container);
#else
    setWidget(_view);
#endif
    qCDebug(interfaceapp) << objectName() << "made," << (isFloating() ? "floating" : "docked");
    emit contentMade();
}

QWindow* QmlDock::contentWindow() const {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return _view;
#else
    return nullptr;
#endif
}

QQuickItem* QmlDock::makePage() {
    makeContent();
    return panel();
}

bool QmlDock::moveTo(QQuickItem* host) {
    QQuickItem* page = panel();
    if (!page || !host) {
        return false;
    }
    page->setPosition(QPointF(0, 0));
    page->setSize(host->size());
    page->setParentItem(host);   // a slot lays its pages out as they come in (ScreenSlot.qml's scale)
    page->setVisible(true);
    _away = true;
    return true;
}

bool QmlDock::moveBack(bool shown) {
    QQuickItem* page = panel();
    if (!page || !_view) {
        return false;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    page->setParentItem(_view->contentItem());   // the view is the window here
#else
    page->setParentItem(_view->quickWindow()->contentItem());
#endif
    page->setPosition(QPointF(0, 0));
    page->setScale(1.0);
    if (page->metaObject()->indexOfProperty("screenScale") >= 0) {
        page->setProperty("screenScale", 1.0);
    }
    page->setSize(_view->size());
    page->setVisible(shown);
    _away = false;
    return true;
}

void QmlDock::setPageShown(bool shown) {
    if (QQuickItem* page = _away ? nullptr : panel()) {
        page->setVisible(shown);
    }
}

QQuickItem* QmlDock::panel() const {
    return _view ? _view->rootObject() : nullptr;
}

void QmlDock::releaseContent() {
    if (!_view) {
        return;
    }
    gl::ViewChangeGuard guard;
    setWidget(nullptr);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (_container) {
        delete _container;   // and the view it holds
        _container = nullptr;
    } else {
        delete _view;   // its panel failed to load, so it never went into a container
    }
#else
    delete _view;   // the page, its component and its context are the view's children
#endif
    _view = nullptr;
}

// a panel's own window coming or going beside the viewer's changes the GL view's surface on macOS: its float and close
// buttons, a double-click on its title and the end of a drag that docks it happen under the guard (as chat's)
bool QmlDock::event(QEvent* event) {
    switch (event->type()) {
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseButtonRelease:
        case QEvent::NonClientAreaMouseButtonDblClick:
        case QEvent::NonClientAreaMouseButtonRelease: {
            gl::ViewChangeGuard guard(event->type());
            return QDockWidget::event(event);
        }
        // closed, not only hidden (Build mode's layout, a re-dock and a tab behind another only hide it): a panel that
        // keeps something on while hidden learns of it by its panelClosed() (the radio's music, Caity's hand test 10-31)
        case QEvent::Close: {
            gl::ViewChangeGuard guard(event->type());
            const bool handled = QDockWidget::event(event);
            QQuickItem* item = panel();
            if (event->isAccepted() && item && item->metaObject()->indexOfMethod("panelClosed()") >= 0) {
                QMetaObject::invokeMethod(item, "panelClosed");
            }
            return handled;
        }
        // a key the page didn't take (a text field keeps what's typed) comes up to the panel: on to the controls, as the
        // 3D view's keys go (gl/GLWidget.cpp), so push to talk's T and walking work while a panel has the keyboard
        // (Caity's hand test 10-28: Push to talk switched on in Settings, then T did nothing)
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            if (QCoreApplication::sendEvent(QCoreApplication::instance(), event)) {
                event->accept();
                return true;
            }
            return QDockWidget::event(event);
        default:
            return QDockWidget::event(event);
    }
}

bool QmlDock::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonRelease && !_releasing) {
        gl::ViewChangeGuard guard(event->type());
        _releasing = true;
        QCoreApplication::sendEvent(watched, event);
        _releasing = false;
        return true;
    }
    return QDockWidget::eventFilter(watched, event);
}
