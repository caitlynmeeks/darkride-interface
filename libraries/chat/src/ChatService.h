//
//  ChatService.h
//  libraries/chat/src
//
//  Darkride chat in the viewer: Matrix at chat.darkride.ai through the Rust library in ../native (matrix-rust-sdk),
//  signed in with the viewer's Darkride account. The library takes commands as JSON and answers with events as JSON
//  on its own threads; this posts them to the main thread and keeps the latest room list for the panels.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_ChatService_h
#define hifi_ChatService_h

#include <atomic>

#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <DependencyManager.h>

class QNetworkReply;
struct DarkrideChat;

class ChatService : public QObject, public Dependency {
    Q_OBJECT
    SINGLETON_DEPENDENCY

public:
    enum class State {
        SignedOut,      // no Darkride account, or chat not started yet
        SigningIn,      // asking the metaverse for a chat token, or the homeserver for a session
        Ready,          // signed in as the Darkride account
        Unavailable,    // the metaverse refused (chat off, the account banned or unverified) or can't be reached
    };
    Q_ENUM(State)

    ChatService();
    ~ChatService();

    // follows the Darkride account from now on: signed in to chat while it is, signed out when it signs out.
    // remembered: the "keep me signed in" setting; when it is off, whatever chat kept on this computer is left over
    // from someone who chose not to be remembered, and it goes before chat runs
    void start(bool remembered);
    // at quit, before the networking goes; forget: sign out of chat too (the account isn't remembered on this
    // computer), waiting a moment for the homeserver to hear it, then delete what chat kept here
    void stop(bool forget);

    State state() const { return _state; }
    // a line for the panel when chat isn't ready: why, or what it is waiting for
    QString stateText() const { return _stateText; }
    QString userId() const { return _userId; }
    // the chat server the metaverse named (https://chat.darkride.ai), which serves the web chat's page too; empty
    // until the first chat token
    QString homeserver() const { return _homeserver; }
    // the sync's state: running, offline (waiting for the server), error, idle, terminated
    QString syncState() const { return _syncState; }
    // [{id, name, dm, dm_user?, encrypted, invited, unread, notifications, members, avatar?, space?}]
    QJsonArray rooms() const { return _rooms; }
    // what you have missed, as the room list's numbers count it: mentions of you in channels and unread direct
    // messages (a channel's other new messages only make its name bright); 0 when chat isn't ready. For a badge on a
    // closed chat (the bar's Chat button)
    int getUnreadCount() const { return _unreadCount; }
    // the joined spaces (Darkride's): [{id, name, avatar?}]
    QJsonArray spaces() const { return _spaces; }
    // who you are in chat: the display name and the picture's mxc address, once the server has said
    QString profileName() const { return _profileName; }
    QString profileAvatar() const { return _profileAvatar; }

    void open(const QString& room, int count = 50);
    void close(const QString& room);
    void loadOlder(const QString& room, int count = 30);
    void send(const QString& room, const QString& body);
    // a message's reactions, its text and the message itself, by its id in the timeline: react adds the emoji, or
    // takes yours back; edit and deleteMessage only work on your own messages
    void react(const QString& room, const QString& id, const QString& key);
    void edit(const QString& room, const QString& id, const QString& body);
    void deleteMessage(const QString& room, const QString& id);
    void markRead(const QString& room);
    void join(const QString& room);
    // a new direct message with someone (a Matrix user id), inviting them; commandDone says {op: create_dm, room}
    void createDm(const QString& userId);
    void leave(const QString& room);
    // a picture or file into the chat's folder, answered by mediaReady with the same key: source as a message's
    // media has it ({"url": mxc} or {"file": {...}}); a width and height ask for a thumbnail (crop: cut to fill)
    void fetchMedia(const QString& key, const QJsonValue& source, int width = 0, int height = 0, bool crop = false);
    // tries again after Unavailable
    void retry();

signals:
    void stateChanged(State state);
    void syncStateChanged(const QString& state);
    void roomsChanged(const QJsonArray& rooms);
    void unreadCountChanged(int count);
    // the newest items of an open room: [{id, sender, name, body, kind, ts, own, encrypted, state, reactions,
    // edited, editable, ...}], growing as older ones load (the library's timeline event)
    void timelineChanged(const QString& room, const QJsonArray& items);
    // a command finished: {op, room?, start?}; start: the room's first message is in, there's nothing older
    void commandDone(const QJsonObject& done);
    void commandFailed(const QString& op, const QString& message);
    void profileChanged();
    // path: the file; error: why there is none
    void mediaReady(const QString& key, const QString& path, const QString& error);

private slots:
    void onAccountLoggedIn();
    void onAccountLoggedOut();
    void chatTokenReceived(QNetworkReply* reply);
    void chatTokenFailed(QNetworkReply* reply);

private:
    static void receiveEvent(const char* event, void* userData);
    void handleEvent(const QByteArray& json);
    void command(const QJsonObject& command);
    void requestChatToken();
    void createLibrary(const QString& homeserver);
    void freeLibrary();
    void signOut();
    void afterSignOut();
    bool forgetChatData();
    void settle();
    void setState(State state, const QString& text = QString());
    void countUnread();

    DarkrideChat* _chat { nullptr };
    // which library an event came from: one posted by a library since freed is dropped, not taken for the next one's
    std::atomic<int> _generation { 0 };
    // a sign-out sent to the library: its data goes once it has signed out, or once _signOutTimer gives up waiting
    bool _signingOut { false };
    QString _homeserver;
    bool _started { false };
    bool _tokenRequested { false };

    // the library's own state (signed_out, starting, ready) and whom it is signed in as
    QString _libraryState;
    QString _libraryUser;
    // from the metaverse: a two-minute token and the chat account it signs in as
    QString _token;
    QElapsedTimer _tokenAge;
    QString _expectedUser;
    // the name Darkride gives the chat account with the token (the display name, else the username as written), for
    // an account that has none yet
    QString _tokenDisplayName;
    // sign-ins tried since the last one that worked, so a refusal doesn't loop
    int _signInAttempts { 0 };
    // kept sessions of another account signed out since the last sign-in that worked, so one that won't go doesn't loop
    int _keptSessionSignOuts { 0 };
    QString _lastSignInError;

    State _state { State::SignedOut };
    QString _stateText;
    QString _userId;
    QString _syncState;
    QJsonArray _rooms;
    int _unreadCount { 0 };
    QJsonArray _spaces;
    QString _profileName;
    QString _profileAvatar;
    QTimer _retryTimer;
    QTimer _signOutTimer;
    // chat's data still locked after the library went (another program reading a file): tried again a few times
    QTimer _forgetTimer;
    int _forgetTries { 0 };
};

#endif // hifi_ChatService_h
