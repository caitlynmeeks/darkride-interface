//
//
//  Created by Bradley Austin Davis on 2015/12/03
//  Derived from interface/src/GLCanvas.cpp created by Stephen Birarda on 8/14/13.
//  Copyright 2013-2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "GLWidget.h"

#include "Config.h"

#include <mutex>

#include <QtGlobal>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtCore/QCoreApplication>

#include <QtGui/QKeyEvent>
#include <QtGui/QPaintEngine>
#include <QtGui/QWindow>

#include "Context.h"
#include "GLHelpers.h"
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include "GLLogging.h"
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QPlatformSurfaceEvent>
#endif

class GLPaintEngine : public QPaintEngine {
    bool begin(QPaintDevice *pdev) override { return true; }
    bool end() override { return true; } 
    void updateState(const QPaintEngineState &state) override { }
    void drawPixmap(const QRectF &r, const QPixmap &pm, const QRectF &sr) override { }
    Type type() const override { return OpenGL2; }
};

GLWidget::GLWidget() {
    setAttribute(Qt::WA_AcceptTouchEvents);
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    grabGesture(Qt::PinchGesture);
    setAcceptDrops(true);
    _paintEngine = new GLPaintEngine();
}

GLWidget::~GLWidget() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)
    gl::setViewChangeContextUpdate(nullptr);
#endif
    delete _paintEngine;
    _paintEngine = nullptr;
}

int GLWidget::getDeviceWidth() const {
    return width() * (windowHandle() ? (float)windowHandle()->devicePixelRatio() : 1.0f);
}

int GLWidget::getDeviceHeight() const {
    return height() * (windowHandle() ? (float)windowHandle()->devicePixelRatio() : 1.0f);
}

void GLWidget::createContext(QOpenGLContext* shareContext) {
    _context = new gl::Context();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)
    makeWindowAnOpenGLSurface();
#endif
    _context->setWindow(windowHandle());
    _context->create(shareContext);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)
    gl::setViewChangeContextUpdate([this] { _context->updateForViewChange(); });
#endif
    _context->makeCurrent();
    _context->clear();
    _context->doneCurrent();
}

void GLWidget::swapBuffers() {
    _context->swapBuffers();
}

bool GLWidget::makeCurrent() {
    gl::Context::makeCurrent(_context->qglContext(), windowHandle());
    return _context->makeCurrent();
}

QOpenGLContext* GLWidget::qglContext() {
    return _context->qglContext();
}

void GLWidget::doneCurrent() {
    _context->doneCurrent();
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)
void GLWidget::makeWindowAnOpenGLSurface() {
    // Qt 6 creates a widget's native window as a raster surface, which QOpenGLContext::makeCurrent refuses (Qt 5
    // created it GL-capable, so this view worked unchanged), and a surface type set after the creation is not enough:
    // the Cocoa plugin set the view up for raster drawing, and AppKit traps in -[NSOpenGLContext update] on the
    // present thread (run 3, 11-04). So the window's native side is made again as an OpenGL surface, the way the
    // offscreen context's own window is made (gl::OffscreenContext::create); the QWindow object, and so every
    // windowHandle() the callers hold, stays the same. Main thread, with no present under way (before the present
    // thread exists, or under the view-change guard).
    QWindow* window = windowHandle();
    _remakingWindow = true;
    window->destroy();
    window->setSurfaceType(QSurface::OpenGLSurface);
    window->setFormat(getDefaultOpenGLSurfaceFormat());
    window->create();
    // destroy() hid the window and create() doesn't show it again (Qt restores that only when a window changes
    // screen): shown again here, else the view is never exposed and Qt's Cocoa GL skips every flush (run 4, 11-04)
    window->setVisible(true);
    // the widget dropped its window id when the surface went (QWidget's SurfaceAboutToBeDestroyed handling): taken
    // again, so its native-window bookkeeping matches the window it has
    winId();
    _remakingWindow = false;
    qCDebug(glLogging) << "view window made again as an OpenGL surface:" << window->surfaceType() << window->format().majorVersion() << window->format().minorVersion() << window->format().profile();
    // A screen change outside the main window's own events (the window dragged to another display, a display's
    // parameters changed) is a view change too: the guard runs the context's update for it. Per QWindow object, so
    // made again whenever the window is.
    connect(window, &QWindow::screenChanged, this, [](QScreen*) { gl::ViewChangeGuard guard(0); }, Qt::UniqueConnection);
}

#endif
bool GLWidget::event(QEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)
    if (_context && !_remakingWindow) {
        if (event->type() == QEvent::PlatformSurface) {
            // Qt takes the view's window away (QWidget::setParent's recreate() when a texture-based widget joins the
            // main window): the context lets go of it before it is gone
            if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
                qCDebug(glLogging) << "view window about to be destroyed under the context: let go";
                _context->replaceWindow(nullptr);
            }
        } else if (event->type() == QEvent::WinIdChange) {
            QWindow* window = windowHandle();
            if (window && window != _context->getWindow()) {
                // the new window: an OpenGL surface again, and the context's drawable, under the guard (which nests
                // inside the one the change itself runs under)
                gl::ViewChangeGuard guard(event->type());
                makeWindowAnOpenGLSurface();
                _context->replaceWindow(window);
                qCDebug(glLogging) << "view window replaced under the context: taken";
            }
        }
    }
#endif
    switch (event->type()) {
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::FocusIn:
        case QEvent::FocusOut:
        case QEvent::Resize:
        case QEvent::TouchBegin:
        case QEvent::TouchEnd:
        case QEvent::TouchUpdate:
        case QEvent::Gesture:
        case QEvent::Wheel:
        case QEvent::DragEnter:
        case QEvent::Drop:
            if (QCoreApplication::sendEvent(QCoreApplication::instance(), event)) {
                return true;
            }
            break;

        default:
            break;
    }
    return QWidget::event(event);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool GLWidget::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
#else
bool GLWidget::nativeEvent(const QByteArray &eventType, void *message, long *result) {
#endif
#ifdef Q_OS_WIN32
    MSG* win32message = static_cast<MSG*>(message);
    switch (win32message->message) {
        case WM_ERASEBKGND:
            *result = 1L;
            return TRUE;

        default: 
            break;
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

QPaintEngine* GLWidget::paintEngine() const {
    return _paintEngine;
}
