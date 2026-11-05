//
//  MainWindow.cpp
//  interface
//
//  Created by Mohammed Nafees on 04/06/2014.
//  Copyright (c) 2014 High Fidelity, Inc. All rights reserved.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "MainWindow.h"

#include <algorithm>

#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QEvent>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QWindowStateChangeEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QtGui/QWindow>
#include <QtCore/QDebug>

#include "ui/Logging.h"
#include "DockWidget.h"

#include <QSizePolicy>
#include <QLayout>

MainWindow::MainWindow(QWidget* parent) :
    QMainWindow(parent),
    _windowGeometry("WindowGeometry"),
    _windowState("WindowState", 0)
{
    setAttribute(Qt::WA_NoSystemBackground);
    setAcceptDrops(true);
    // the edges between the 3D view and the docked panels (chat's, Build mode's): wide enough to grab and drag. At 1 px
    // (as High Fidelity drew them) docked chat couldn't be widened (Caity's test, 10-26); blue under the pointer, as
    // Build mode's editor.qss has them
    setStyleSheet("QMainWindow::separator { width: 4px; height: 4px; border: none; background: #232323; }\n"
                  "QMainWindow::separator:hover { background: #2c5d87; }\n");
    // setStyleSheet("background-color: #252525;  color : white;");

}

MainWindow::~MainWindow() {
    qCDebug(uiLogging) << "Destroying main window";
}

QWindow* MainWindow::findMainWindow() {
    auto windows = qApp->topLevelWindows();
    QWindow* result = nullptr;
    for (const auto& window : windows) {
        if (window->objectName().contains("MainWindow")) {
            result = window;
            break;
        }
    }
    return result;
}

// a window smaller than this at the start is taken for a mistake: Qt's own 640 x 480, when nothing sized it
static const QSize MIN_START_SIZE(800, 600);

void MainWindow::restoreGeometry() {
    // Did not use setGeometry() on purpose,
    // see http://doc.qt.io/qt-5/qsettings.html#restoring-the-state-of-a-gui-application
    QRect saved = _windowGeometry.get();
    bool usable = false;
    if (saved.width() >= MIN_START_SIZE.width() && saved.height() >= MIN_START_SIZE.height()) {
        for (const QScreen* screen : QGuiApplication::screens()) {
            // its title bar on a connected screen, so it can be grabbed, and most of it there
            QRect available = screen->availableGeometry();
            QRect shown = available.intersected(saved);
            if (available.contains(saved.topLeft() + QPoint(std::min(100, saved.width() / 2), 10)) &&
                2 * shown.width() * shown.height() >= saved.width() * saved.height()) {
                usable = true;
                break;
            }
        }
    }
    _startGeometry = usable ? saved : QGuiApplication::primaryScreen()->availableGeometry();
    resize(_startGeometry.size());
    move(_startGeometry.topLeft());
    qCDebug(uiLogging) << "main window at the start:" << (usable ? "where it was left," : "the main screen,")
                       << _startGeometry;

#if !defined(Q_OS_MACOS)
    // maximising on mac is actually fullscreen
    if (!usable || _windowState.get(Qt::WindowNoState) == Qt::WindowMaximized) {
        setWindowState(Qt::WindowMaximized);
    }
#endif
}

void MainWindow::checkStartGeometry() {
    QRect now(pos(), size());
    qCDebug(uiLogging) << "main window after the start:" << now << windowState();
    if (windowState() == Qt::WindowNoState && _startGeometry.isValid() &&
        (now.width() < MIN_START_SIZE.width() || now.height() < MIN_START_SIZE.height())) {
        qCWarning(uiLogging) << "main window was shrunk to" << now << "after the start; back to" << _startGeometry;
        resize(_startGeometry.size());
        move(_startGeometry.topLeft());
    }
}

void MainWindow::saveGeometry() {
    // Did not use geometry() on purpose,
    // see http://doc.qt.io/qt-5/qsettings.html#restoring-the-state-of-a-gui-application
    _windowState.set((int)windowState());

    // Save position and size only if windowed so that have good values for windowed after starting maximized or full screen.
    if (windowState() == Qt::WindowNoState) {
        QRect geometry(pos(), size());
        _windowGeometry.set(geometry);
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // It is the job of Application::quit() to shut things down properly when it is finished with its event loop.
    // But if we don't explicitly ignore this event now, the window and application event loop will close
    // before we've had a chance to act on the aboutToClose signal. This will leaves a zombie process on all platforms.
    // To repro:
    //   Open a QML modal dialog (e.g., select an avatar to wear), but don't dismiss it.
    //   Close the application with the operating system window close button (not the menu Quit)
    //   With ignore: App will wait until you accept or dismiss the dialog and log "Normal exit".
    //   Without ignore: App will close immediately, and nothing will log about quitting or exit.
    event->ignore();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // On Qt 6 macOS quit() goes through AppKit's terminate:, and Qt's answer to that closes every window: this handler
    // calling quit() again is a nested terminate, which AppKit ends with exit() at once, so exec() never returns, the
    // shutdown never runs and static destructors race the present thread (rendering, 11-05). Ending the event loop
    // is what quit() was on Qt 5.
    QCoreApplication::exit(0);
#else
    qApp->quit();
#endif
}

void MainWindow::moveEvent(QMoveEvent* event) {
    emit windowGeometryChanged(QRect(QPoint(geometry().x(), geometry().y()), size()));  // Geometry excluding the window frame.
    QMainWindow::moveEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    emit windowGeometryChanged(QRect(QPoint(geometry().x(), geometry().y()), size()));  // Geometry excluding the window frame.
    QMainWindow::resizeEvent(event);
}

void MainWindow::showEvent(QShowEvent* event) {
    if (event->spontaneous()) {
        emit windowShown(true);
    }
    QMainWindow::showEvent(event);
}

void MainWindow::hideEvent(QHideEvent* event) {
    if (event->spontaneous()) {
        emit windowShown(false);
    }
    QMainWindow::hideEvent(event);
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::WindowStateChange) {
        QWindowStateChangeEvent* stateChangeEvent = static_cast<QWindowStateChangeEvent*>(event);
        if ((stateChangeEvent->oldState() == Qt::WindowNoState ||
            stateChangeEvent->oldState() == Qt::WindowMaximized) &&
            (windowState() & Qt::WindowMinimized) == Qt::WindowMinimized) {
            emit windowShown(false);
            emit windowMinimizedChanged(true);
        } else {
            emit windowShown(true);
            if ((stateChangeEvent->oldState() & Qt::WindowMinimized) == Qt::WindowMinimized) {
                emit windowMinimizedChanged(false);
            }
        }
    } else if (event->type() == QEvent::ActivationChange) {
        if (isActiveWindow()) {
            emit windowShown(true);
        } else {
            emit windowShown(false);
        }
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    QCoreApplication::sendEvent(QCoreApplication::instance(), event);
}
