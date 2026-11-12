//
//  QmlDock.h
//  interface/src/ui
//
//  A dock panel that shows one of Darkride's QML panels (qml/darkride/*Panel.qml) natively, so it can sit beside the
//  3D view, be tabbed with other panels or float in its own window, as chat and Build mode's panels do; ui/Panels.h
//  makes them and opens them by name.
//  Dockable panels, step 2 (Caity's "go step 2", 2026-10-26; team/editor-ui/DOCKING-STEP2-PLAN.md). The panel is the
//  same QML its overlay window showed, made on the app's one QML engine in a context under the desktop's (so it finds
//  MetaverseRequests, the account, MyAvatar and the rest by the same names) and drawn by a QQuickWidget. Its QML is
//  made the first time the panel shows, and a hidden panel draws nothing: its page is hidden with it (a panel tabbed
//  behind another too), so the page's own onVisibleChanged reads its values again on each showing and its timers stop,
//  as in its overlay window. Every float, dock and close happens under gl::ViewChangeGuard, as chat's
//  (chat/ChatWindow.cpp's ChatDock).
//  On Qt 6 the panel is drawn by a QQuickView in a window container instead: a QQuickWidget there makes Qt destroy and
//  re-create the whole main window to draw it through Qt's RHI layer, taking the 3D view's window with it
//  (team/rendering/QT6-R1-ENTRY.md, Amendment A). A QQuickView is a native window of its own beside the 3D view, as
//  libraries/ui's DockWidget has always been, so the main window stays as it is on Qt 5. It runs on the basic render
//  loop (the main thread, swap interval 0): QSG_RENDER_LOOP is process-wide, so any on-screen QQuickWindow added later
//  inherits that loop too. Today there is none; the offscreen surfaces have no render loop.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_QmlDock_h
#define hifi_QmlDock_h

#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QPointer>
#include <QtCore/QUrl>
#include <QtWidgets/QDockWidget>

class QQuickItem;
class QWindow;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
class QQuickView;
#else
class QQuickWidget;
#endif

class QmlDock : public QDockWidget {
    Q_OBJECT
public:
    QmlDock(const QString& name, const QUrl& source, QWidget* parent);

    // the panel's QML, once made (for tests)
    QQuickItem* panel() const;
    // the page and its view gone, at quit before the QML engine (Panels::releaseQml)
    void releaseContent();
    // the page made before the panel first shows, whichever way it shows (Panels, the saved layout, a drag)
    void setVisible(bool visible) override;
    // an object this panel's QML alone finds by name (the Code panel's CodeEditor): set before the panel is made
    void setContextObject(const QString& name, QObject* object);
    // the window the panel's QML is drawn in, once made (Qt 6's view; none on Qt 5): the Make dock watches its input
    QWindow* contentWindow() const;

signals:
    // the panel's QML was made (contentWindow() and panel() are there now)
    void contentMade();

public:
    // the VR screen (VR-SCREEN-PLAN.md): the page, made once, moved into one of the screen's slots (an item in another
    // Qt Quick window) and back into this dock, without being made again; a page moved back while in the headset is
    // hidden (nobody sees the desktop's docks there), so its web views stop drawing while their sound plays on
    QQuickItem* makePage();   // the page, made now if it hasn't been (without showing the dock)
    bool moveTo(QQuickItem* host);
    bool moveBack(bool shown);
    bool isAway() const { return _away; }
    void setPageShown(bool shown);

    // Qt 6: a web view drawn in a slot can't move to another Qt Quick window. Qt WebEngine lets go of its pictures
    // for the old window from the main thread, and a slot's window draws on its own thread, so Qt stops the app.
    // Such a page stays where it is: hidden there behind another page, or gone with its web views there (the dock
    // makes it again the next time it shows).
    static bool holdsWebView(const QQuickItem* item);   // the item or any item in it is a web view (always false on Qt 5)
    bool keepsWebView() const { return _away && holdsWebView(panel()); }
    void hideAway();       // behind another page in its slot, unseen
    void releaseAway();    // the page and its web views go, where they are

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void makeContent();

    QString _name;   // the panel's name (Panels'), for its page: dockPanelName
    QUrl _source;
    QList<QPair<QString, QPointer<QObject>>> _contextObjects;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QQuickView* _view { nullptr };
    QWidget* _container { nullptr };   // holds _view, and owns it once made
#else
    QQuickWidget* _view { nullptr };
#endif
    bool _releasing { false };
    bool _away { false };   // the page is on the VR screen
};

#endif // hifi_QmlDock_h
