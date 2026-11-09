//
//  OffscreenGLCanvas.h
//  interface/src/renderer
//
//  Created by Bradley Austin Davis on 2014/04/09.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//
#pragma once
#ifndef hifi_OffscreenGLCanvas_h
#define hifi_OffscreenGLCanvas_h

#include <mutex>
#include <QObject>

class QOpenGLContext;
class QOffscreenSurface;
class QOpenGLDebugMessage;
class QSurfaceFormat;

class OffscreenGLCanvas : public QObject {
public:
    OffscreenGLCanvas();
    ~OffscreenGLCanvas();
    void setFormat(const QSurfaceFormat& format);
    bool create(QOpenGLContext* sharedContext = nullptr);
    bool makeCurrent();
    void doneCurrent();
    void moveToThreadWithContext(QThread* thread);
    QOpenGLContext* getContext() {
        return _context;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // The canvas's own offscreen surface, made on the thread that made the canvas (the main thread): what the QML
    // surface's render interface falls back to, instead of one of Qt's own made on the render thread
    QOffscreenSurface* getOffscreenSurface() {
        return _offscreenSurface;
    }
#endif
    QObject* getContextObject();
    
    void setThreadContext();
    static bool restoreThreadContext();

protected:
    void clearThreadContext();

    std::once_flag _reportOnce;
    QOpenGLContext* _context{ nullptr };
    QOffscreenSurface* _offscreenSurface{ nullptr };
};

#endif // hifi_OffscreenGLCanvas_h
