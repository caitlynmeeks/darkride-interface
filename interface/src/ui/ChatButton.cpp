//
//  ChatButton.cpp
//  interface/src/ui
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ChatButton.h"

#include <algorithm>

#include <QtWidgets/QDockWidget>

#include <ChatService.h>
#include <DependencyManager.h>
#include <SettingHandle.h>
#include <ui/TabletScriptingInterface.h>

#include "Application.h"
#include "WindowLayout.h"
#include "chat/ChatTabletPage.h"
#include "chat/ChatWindow.h"
#include "chat/WorldChat.h"
#include "scripting/ControllerScriptingInterface.h"

namespace {
    // Material's "chat", white, as people.svg beside it
    const QString ICON = "icons/tablet-icons/chat.svg";
    // after People (7, made later by a script: a later button with the same number goes in front) and before Explore
    const int SORT_ORDER = 7;
}

void ChatButton::add(TabletProxy* tablet) {
    static QPointer<ChatButton> theButton;
    if (!theButton && tablet) {
        theButton = new ChatButton(tablet);
    }
}

ChatButton::ChatButton(TabletProxy* tablet) : QObject(tablet) {
    _button = tablet->addButton(QVariantMap {
        { "text", "Chat" },
        { "icon", ICON },
        { "backgroundColor", "#4caf50" },   // green 500
        { "sortOrder", SORT_ORDER },
        { "badge", 0 },
    });
    // in VR on the tablet (a headset draws no panels), else the panel
    auto page = ChatTabletPage::make(tablet);
    connect(page, &ChatTabletPage::showingChanged, this, &ChatButton::updateActive);
    connect(_button, &TabletButtonProxy::clicked, this, [this, page] {
        if (qApp->isHMDMode()) {
            page->toggle();
            return;
        }
        ChatWindow::toggle();
        watchDock();   // opened before the layout's restore made the panel
    });
    // the count from sign-in on, before chat is ever opened, and Local Chat's new lines with it
    if (auto chat = DependencyManager::get<ChatService>()) {
        connect(chat.data(), &ChatService::unreadCountChanged, this, &ChatButton::updateUnread);
    }
    // (Local Chat is made here, so it keeps its lines from start: a line said before chat opens is there when it does)
    connect(WorldChat::instance(), &WorldChat::unreadChanged, this, &ChatButton::updateUnread);
    updateUnread();
    // chat no longer remembers its last room (it opens on Local Chat): the old setting goes, as settings outside
    // private/ are readable by scripts (security, 11-08)
    Setting::Handle<QString>("chat/room").remove();
    // chat's panel is made by the layout's restore after sign-in (ui/WindowLayout.h), or by a first open
    connect(WindowLayout::instance(), &WindowLayout::restored, this, &ChatButton::watchDock);

    // Enter or "/" in the 3D view (keys Build mode didn't take): Local Chat, the box ready, as the old in-world chat
    // had them
    auto controller = dynamic_cast<ControllerScriptingInterface*>(qApp->getControllerScriptingInterface());
    if (controller) {
        connect(controller, &ControllerScriptingInterface::keyPressEvent, this, [](const KeyEvent& event) {
            if (event.isAutoRepeat || event.isControl || event.isMeta || event.isAlt || qApp->isHMDMode()) {
                return;
            }
            if (event.key == Qt::Key_Return || event.key == Qt::Key_Enter) {
                ChatWindow::openWorld(false);
            } else if (event.key == Qt::Key_Slash) {
                ChatWindow::openWorld(true);
            }
        });
    }
}

void ChatButton::watchDock() {
    if (_dock) {
        return;
    }
    _dock = ChatWindow::dock();
    // open is not hidden: a panel tabbed behind another, or the window minimised, is still open
    connect(_dock, &QDockWidget::visibilityChanged, this, &ChatButton::updateActive);
    updateActive();
}

void ChatButton::updateActive() {
    auto page = ChatTabletPage::get();
    bool active = (_dock && !_dock->isHidden()) || (page && page->isShowing());
    if (_button) {
        _button->editProperties({ { "isActive", active } });
    }
}

void ChatButton::updateUnread() {
    auto chat = DependencyManager::get<ChatService>();
    int count = std::max(0, chat ? chat->getUnreadCount() : 0) + WorldChat::instance()->unread();
    if (_button) {
        _button->editProperties({ { "badge", count } });
    }
}
