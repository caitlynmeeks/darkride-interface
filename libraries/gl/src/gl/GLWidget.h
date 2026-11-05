//
//  Created by Bradley Austin Davis on 2015/12/03
//  Derived from interface/src/GLCanvas.h created by Stephen Birarda on 8/14/13.
//  Copyright 2013-2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_GLWidget_h
#define hifi_GLWidget_h

#include <QtWidgets/QWidget>

namespace gl {
    class Context;
}

class QOpenGLContext;

/// customized canvas that simply forwards requests/events to the singleton application
class GLWidget : public QWidget {
    Q_OBJECT
    
public:
    GLWidget();
    ~GLWidget();
    int getDeviceWidth() const;
    int getDeviceHeight() const;
    QSize getDeviceSize() const { return QSize(getDeviceWidth(), getDeviceHeight()); }
    QPaintEngine* paintEngine() const override;
    void createContext(QOpenGLContext* shareContext = nullptr);
    bool makeCurrent();
    void doneCurrent();
    void swapBuffers();
    gl::Context* context() { return _context; }
    QOpenGLContext* qglContext();


protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    virtual bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;   // Qt 6's result type
#else
    virtual bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;
#endif
    virtual bool event(QEvent* event) override;
    gl::Context* _context { nullptr };
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) && defined(Q_OS_MACOS)
    // The view's window made again as an OpenGL surface (Qt 6 creates a widget's window as a raster one), at the
    // context's creation and whenever Qt replaces the window (QWidget::setParent's recreate() for a texture-based
    // widget in the main window); the guard round screen changes is connected here too.
    void makeWindowAnOpenGLSurface();
    bool _remakingWindow { false };
#endif

private:
    QPaintEngine* _paintEngine { nullptr };
    bool _vsyncSupported { false };
};

#endif // hifi_GLCanvas_h
