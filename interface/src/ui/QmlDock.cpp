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
#include <QtQuickWidgets/QQuickWidget>
#include <QtWidgets/QAbstractButton>

#include <gl/GLHelpers.h>
#include <OffscreenUi.h>

#include "DockTitleBar.h"
#include "InterfaceLogging.h"

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
        if (QQuickItem* page = panel()) {
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
    _view = new QQuickWidget(engine, this);
    _view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    _view->setMinimumSize(320, 240);
    // the names the overlay panels find are the desktop's: the panel is made in a context under it
    auto context = new QQmlContext(desktop, _view);
    auto component = new QQmlComponent(engine, _source, QQmlComponent::PreferSynchronous, _view);
    QObject* object = component->isReady() ? component->create(context) : nullptr;
    if (!object) {
        qCWarning(interfaceapp) << objectName() << "couldn't make" << _source << component->errorString();
        return;
    }
    _view->setContent(_source, component, object);
    setWidget(_view);
    qCDebug(interfaceapp) << objectName() << "made," << (isFloating() ? "floating" : "docked");
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
    delete _view;   // the page, its component and its context are the view's children
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
