//
//  ChatWindow.h
//  interface/src/chat
//
//  Darkride chat (View > Chat): the channels and direct messages down the left, the conversation and the message box
//  on the right, as the web chat at chat.darkride.ai lays them out. It lives in a dock panel, "Chat" (Caity's "GO CHAT
//  DOCKING", 2026-10-25): floating in its own window as before, so opening it never resizes the 3D view, until the
//  player drags it into the main window beside Build mode's panels. Narrower than the two columns need, it shows one
//  at a time. Under a skin it takes the skin's colours. ChatService (libraries/chat) does the talking.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_ChatWindow_h
#define hifi_ChatWindow_h

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QUrl>
#include <QtGui/QImage>
#include <QtGui/QKeySequence>
#include <QtWidgets/QWidget>

class QDockWidget;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMovie;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTextBrowser;
class QTimer;
class QToolButton;

class ChatWindow : public QWidget {
    Q_OBJECT
public:
    // View > Chat: shows the chat panel (its content made the first time), or hides it when it is already in front
    static void toggle();
    // the "Chat" dock panel (object name "ChatDock", for the main window's saved layout), made hidden and floating the
    // first time; cheap, so the layout's restore can place it before chat is ever opened
    static QDockWidget* dock();
    // the chat panel back to where it starts (Build > Reset Panels): floating, at its first size in the middle of the
    // main window; shown or hidden as it was
    static void resetDock();
    static QKeySequence shortcut();
    // chat in front on your direct message with a Darkride user, made if there is none yet (Friends.openDirectMessage,
    // People's Message); while chat is signing in, once it's ready. In VR, on the tablet (ChatTabletPage)
    static void openDirectMessage(const QString& username);
    // Enter or "/" in the 3D view: chat in front on Local Chat (WorldChat), the message box ready, with "/" in it for
    // a command; the line sent (or Esc), back to the world
    static void openWorld(bool command);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    explicit ChatWindow(QWidget* parent);
    // the chat in the dock panel, made the first time it shows
    static ChatWindow* content();

    void updateState();
    void updateRooms(const QJsonArray& rooms);
    // Local Chat's row, under Friends
    void addWorldRow(QListWidgetItem*& current);
    // after a line in Local Chat opened by a key: the 3D view has the keys again; hide: the panel goes too if the
    // key brought it
    void backToWorld(bool hide);
    void updateTimeline(const QString& room, const QJsonArray& items);
    void commandDone(const QJsonObject& done);
    void updateSync(const QString& state);
    void selectRoom(const QString& room);
    QJsonObject roomById(const QString& room) const;
    void showRoomPage();
    // the line beside Local Chat's title (WorldChat::hereLine); empty for the other rooms
    void updateRoomTopic();
    void renderTimeline();
    void sendMessage();
    // the message under a point of the conversation (viewport coordinates): its index in _items, or -1
    int messageAt(const QPoint& pos) const;
    // right-click: quick reactions, Edit, Copy Text and Delete for the message there, then the browser's own
    void showMessageMenu(const QPoint& pos);
    // the message box edits one of your messages: Enter saves it, Esc stops
    void startEditing(const QJsonObject& item);
    void stopEditing();
    void deleteMessage(const QJsonObject& item);
    // a message's reactions as rich text: one chip per emoji with its count, yours lit, a click toggling yours
    QString reactionsHtml(const QJsonObject& item, const QHash<QString, QString>& names);

    // the Friends page (FriendsService, the metaverse's friends), as Discord's: Online, All, Pending and Add Friend;
    // the sidebar's first row opens it
    void showFriendsPage();
    void setFriendsTab(const QString& tab);
    void updateFriends();
    void addFriendRow(const QString& username, const QString& status, const QList<QPushButton*>& buttons,
                      bool dimmed = false);
    // a friend's direct message, made when there is none yet
    void messageFriend(const QString& username);
    // the direct message openDirectMessage asked for, once chat is ready
    void openWantedDm();
    // someone's card: People's profile, in front (editor-ui's Panels.showProfile), from a name or picture here
    void showProfile(const QString& username);
    void sendFriendRequest();
    // "who sees where I am" (the metaverse's availability), from your name at the foot of the sidebar
    void showAvailabilityMenu();
    // a Darkride username's chat account, on the chat's own server
    QString userIdFor(const QString& username) const;
    // the friends lists asked for again, unless they were a moment ago
    void askFriends(bool now = false);
    void markReadIfSeen();
    void fitComposer();
    // the colours of Darkride's own look, or the skin's over them, into the style sheet, the room list and the
    // conversation; again when the skin changes
    void applyColours();
    // narrower than both columns: one at a time, the rooms or the conversation (with a way back to the rooms)
    void setNarrow(bool narrow);
    void showRooms(bool rooms);

    // a person's or room's round picture: theirs once fetched (mxc), until then the first letter of the name on a
    // colour of their own; avatarSource names it in the conversation's document, at 40 px
    QImage roundAvatar(const QString& seed, const QString& name, const QString& mxc, int size);
    QString avatarSource(const QString& seed, const QString& name, const QString& mxc);
    // a message's picture, file, video or sound as rich text: a picture shown at most 400 x 300, the rest as a link
    QString mediaHtml(const QJsonObject& item);
    void requestMedia(const QString& key, const QJsonValue& source, int width = 0, int height = 0, bool crop = false);
    void mediaReady(const QString& key, const QString& path, const QString& error);
    // fills the conversation's pictures waiting for this media, a GIF playing
    void showPicture(const QString& key, const QString& path);
    // the whole file, opened by the system's own app (Preview, Photos, a player)
    void openMedia(const QJsonObject& media, const QString& name);
    // a file from a message, downloaded: opened by the system only when its content is a picture, a video, a sound or
    // a PDF, under a name of ours with that type's own extension; anything else offered to save. Never handed to the
    // system under the sender's name, which could make it run (a ".exe", a ".command")
    void openDownload(const QString& path, const QString& name);

    static QPointer<ChatWindow> _instance;
    static QPointer<QDockWidget> _dock;

    QWidget* _sidebar { nullptr };
    QWidget* _main { nullptr };
    QToolButton* _roomsButton { nullptr };      // narrow: back to the rooms
    bool _narrow { false };
    bool _roomsShown { false };                 // narrow: the rooms are up, not the conversation
    QSize _pictureMax;                          // a message's picture fits in this (narrower when narrow)
    QLabel* _spaceTitle { nullptr };
    QListWidget* _roomList { nullptr };
    QLabel* _userAvatar { nullptr };
    QLabel* _userName { nullptr };
    QLabel* _userStatus { nullptr };
    QLabel* _roomTitle { nullptr };
    QLabel* _roomTopic { nullptr };   // beside the title: Local Chat's world and who's there
    QStackedWidget* _pages { nullptr };
    QLabel* _stateText { nullptr };
    QPushButton* _retryButton { nullptr };
    QLabel* _inviteText { nullptr };
    QTextBrowser* _timeline { nullptr };
    QPlainTextEdit* _composer { nullptr };
    QLabel* _composerNote { nullptr };
    QWidget* _me { nullptr };
    QStackedWidget* _friendsPages { nullptr };   // the list, or the Add Friend form
    QListWidget* _friendsList { nullptr };
    QLabel* _friendsEmpty { nullptr };
    QLineEdit* _addFriendName { nullptr };
    QLabel* _addFriendNote { nullptr };
    QHash<QString, QPushButton*> _friendsTabs;
    QString _friendsTab { "online" };
    bool _friendsShown { false };               // the Friends page is up (the last room stays _room)
    bool _worldByKey { false };                 // Local Chat opened by Enter or "/" in the 3D view
    bool _worldHideOnEsc { false };             // ... and the panel wasn't in sight before
    QString _dmWanted;                           // a friend's chat account whose new DM is being made, to open
    QString _dmWantedUser;                       // a username whose DM is to open once chat is ready
    QTimer* _friendsTimer { nullptr };          // asks for the lists again every minute while the window is up
    QTimer* _renderTimer { nullptr };           // the conversation drawn once for a burst of timeline events
    qint64 _friendsAskedAt { 0 };

    QJsonArray _rooms;
    QString _room;            // the room shown
    QJsonArray _items;        // its newest messages
    bool _roomStart { false };    // its first message is among them: nothing older to load
    bool _loadingOlder { false };
    bool _anchorBottom { false }; // the next render keeps the distance from the bottom (older messages came in above)
    bool _stickToBottom { true }; // the reader is at the newest message: the conversation keeps them there
    bool _rendering { false };    // the document is being made again: its scroll changes are not the reader's

    // where the conversation shows a picture: its name in the document and its size there
    struct PictureUse {
        QUrl resource;
        QSize box;
        bool round { false };
    };
    QSet<QUrl> _resources;                      // the pictures already given to the conversation's document
    QSet<QUrl> _pictured;                       // the avatars among them that show the person's own picture
    QHash<QString, QString> _mediaFiles;        // media key → its file
    QSet<QString> _mediaWanted;                 // asked for, not answered yet
    QSet<QString> _mediaFailed;                 // asked for and refused: not asked again this session
    QMultiHash<QString, PictureUse> _pictureUses;   // media key → where the conversation shows it
    QHash<QString, QSize> _pictureSizes;        // media key → a picture's own size, when its message didn't say
    QHash<QString, QImage> _avatarImages;       // mxc → a person's or room's picture
    QHash<QString, QMovie*> _movies;            // media key → a GIF playing in the conversation
    QHash<QString, QString> _opening;           // media key → the name to open the file under, once it is here
    QVector<QJsonObject> _links;                // chatopen:<n> in the conversation → {media, name}
    QVector<QJsonObject> _reactionLinks;        // chatreact:<n> → {id, key}
    QVector<int> _tableItems;                   // the document's top-level tables in order → index in _items (-1: not
                                                // a message)
    QString _editingId;                         // the message the message box is editing, if any
};

#endif // hifi_ChatWindow_h
