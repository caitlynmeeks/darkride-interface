//
//  ChatTabletPage.h
//  interface/src/chat
//
//  Chat in VR. Qt's widget windows don't draw in a headset, so there the Chat button opens Darkride chat's web page
//  (the chat server's own: chat.darkride.ai, as in a browser) on the tablet instead of the chat panel. It is the same
//  chat as the panel, signed in as the same chat account: the page asks the app over the tablet's EventBridge, and
//  the app answers with a two-minute sign-in token from the metaverse, as ChatService gets its own. The page keeps
//  that session in the tablet's web storage while the Darkride account stays the same.
//
//  The page's messages, {type: "darkride-chat", command, args}, are answered only when they carry the one-time code
//  the page was opened with (args.bridge): another page in the tablet's browser gets no token and moves no one.
//  Local Chat (WorldChat) reaches the page the same way: its lines as they change ("world"), and yours from it
//  ("worldSend").
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_ChatTabletPage_h
#define hifi_ChatTabletPage_h

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QVariant>

class TabletProxy;

class ChatTabletPage : public QObject {
    Q_OBJECT
public:
    // made once, for the system tablet, by its Chat button
    static ChatTabletPage* make(TabletProxy* tablet);
    // the one there is (none before the tablet is)
    static ChatTabletPage* get();

    // the Chat button in VR: chat on the tablet, or back to the tablet's home when chat is already there
    void toggle();
    // chat on the tablet at your direct message with a Darkride user (People's Message in VR)
    void openDirectMessage(const QString& username);
    bool isShowing() const { return _showing; }

signals:
    void showingChanged(bool showing);

private:
    explicit ChatTabletPage(TabletProxy* tablet);
    // the page, at a link inside chat ("/dm/<username>") when there is one
    void open(const QString& link = QString());
    void screenChanged(const QVariant& type, const QVariant& url);
    void webEventReceived(const QVariant& message);
    void signIn();
    void sendWorld();
    void sendWorldHere();   // the line beside Local Chat's title ("worldHere", 1.1.4 on; older pages ignore it)
    void answer(QJsonObject message);

    QPointer<TabletProxy> _tablet;
    QString _pageUrl;     // the page as the tablet was given it, without a link
    QString _bridge;      // the one-time code the page was opened with; empty once the tablet shows something else
    bool _showing { false };
};

#endif // hifi_ChatTabletPage_h
