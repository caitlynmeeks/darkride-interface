//
//  WindowCursor.h
//  interface/src
//
//  The system cursor over the client's window, in step with the compositor's icon. show() sets the icon the
//  compositor draws and decides whether the system cursor shows (an arrow, when the icon is the system's) or hides
//  (the compositor draws the reticle); setSystemShape() changes the shape while it shows; apply() puts the pending
//  shape on the window (macOS, which cannot hide the cursor on the GL widget alone) or on the GL widget, on the main
//  thread, once a frame. Any thread may call show() and setSystemShape(). Application owns one, reached by
//  getWindowCursor(); these were Application's showCursor, updateSystemCursor and checkChangeCursor
//  (ARCHITECTURE.md §5.4 step 4, the first owner class).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_WindowCursor_h
#define hifi_WindowCursor_h

#include <QtCore/QMutex>
#include <QtCore/Qt>

#include <CursorManager.h>

class QWidget;

class WindowCursor {
public:
    void show(const Cursor::Icon& icon);
    void setSystemShape(Qt::CursorShape shape);
    void apply(QWidget* window, QWidget* glWidget);

private:
    mutable QRecursiveMutex _lock;
    Qt::CursorShape _desiredShape{ Qt::BlankCursor };
    bool _needsChange{ false };
};

#endif // hifi_WindowCursor_h
