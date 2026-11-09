//
//  ChatButton.h
//  interface/src/ui
//
//  The bar's Chat button: it opens and closes chat wherever its panel is, docked or floating (ChatWindow::toggle), is
//  lit while chat is open, and carries a red badge with the number of unread mentions and direct messages
//  (ChatService::getUnreadCount), counted from sign-in whether or not chat was ever opened, plus Local Chat's new
//  lines (WorldChat::unread). One button in the system
//  tablet's list, so the docked bar, the floating bar and the tablet all show it. Made here rather than by a script:
//  no script can see chat's count. In VR it opens chat on the tablet instead (ChatTabletPage), lit while it's there.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_ChatButton_h
#define hifi_ChatButton_h

#include <QtCore/QObject>
#include <QtCore/QPointer>

class QDockWidget;
class TabletButtonProxy;
class TabletProxy;

class ChatButton : public QObject {
    Q_OBJECT

public:
    /// on the system tablet, once
    static void add(TabletProxy* tablet);

private:
    ChatButton(TabletProxy* tablet);
    void watchDock();    // chat's panel, once it exists: the button is lit while it is open
    void updateActive(); // lit while the panel is open or the tablet shows chat
    void updateUnread();   // chat's count and Local Chat's

    QPointer<TabletButtonProxy> _button;
    QPointer<QDockWidget> _dock;
};

#endif // hifi_ChatButton_h
