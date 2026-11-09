//
//  WorldChat.h
//  interface/src/chat
//
//  Local Chat in Darkride chat: the world's own text chat, what the people here type and what AI residents and
//  bots say (they speak through it). It rides the messages mixer's "chat" channel (ChatScriptingInterface, the
//  scripting API scripts, AI residents and older clients use), this world only, not kept anywhere. Chat shows it as a
//  conversation at the top of its list, on the desk and on the VR tablet; this keeps its lines from the start, so a
//  line said before chat opens is there when it does.
//
//  A line's name is the account the world checked when that session connected (NodeList's username for a session),
//  never what the line says, and an AI resident's or a bot's mark comes from that account's public profile on the
//  metaverse, as the nametags have them (tivoli-scripts users-cache.ts).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_WorldChat_h
#define hifi_WorldChat_h

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/QVariantMap>

#include <SoundCache.h>

class WorldChat : public QObject {
    Q_OBJECT
public:
    // made the first time (the Chat button's, at start), listening from then on
    static WorldChat* instance();

    // the lines, oldest first, as ChatService's timeline items: {id, ts, sender ("@username:", or empty), name,
    // kind: text | emote | notice | member, body, own, counts: whether it went into the unread count, mark?: the
    // line under an AI resident's or a bot's name, change + user_name for a member line (joined, left)}
    QJsonArray items() const { return _items; }
    // the lines since this world was last read: the people's, and AI residents' and bots' only when they say your
    // name (so a talkative resident doesn't keep it up); yours and notices not counted
    int unread() const { return _unread; }
    // everyone in this world, you too
    int peopleHere() const;
    // the line beside Local Chat's title (desk and tablet): the world's name, when it has one (an address is not a
    // name), and who's there: "Port Amarelle · 6 here", "Only you"; before arriving, the name alone (no count yet)
    QString hereLine() const;
    void markRead();
    // in a world: its loading screen gone (the domain session's physics gate open). Until then nothing is sent: there's
    // no world to hear it yet
    bool arrived() const;
    // a line to everyone in this world, or "/command params" run here (/help lists them); before arriving, only a notice
    // that it waits
    void send(const QString& text);

signals:
    void itemsChanged();
    void unreadChanged(int unread);
    void arrivedChanged(bool arrived);
    // someone came or left, or a new world (its name may come a moment later); a crowd arriving is one change
    void peopleChanged();

private:
    WorldChat();
    void messageReceived(const QVariantMap& data, const QUuid& sender);
    void usernameReceived(const QString& session, const QString& username);
    void avatarAdded(const QUuid& session);
    void avatarRemoved(const QUuid& session);
    void moving();
    void clear();
    void add(QJsonObject item);
    // the account behind a session: asked of the world once, "" until it answers (and for a visitor)
    QString usernameOf(const QUuid& session);
    // the name a line of theirs shows: their account, else the name their avatar shows the world ("" if none)
    QString shownNameOf(const QUuid& session);
    void fetchProfile(const QString& username);
    // whether a line from this session goes into the count (and plays the sound)
    bool counts(const QUuid& session, const QString& body);
    void wearNames();
    void playSound();
    void teleportTo(const QString& params);

    QJsonArray _items;
    int _unread { 0 };
    int _nextId { 0 };
    QHash<QUuid, QString> _usernames;       // session → its account ("" for a visitor)
    QSet<QUuid> _asked;                     // sessions whose account was asked
    QHash<QUuid, qint64> _joining;          // sessions that came, until their account is known (or 5 s pass)
    QHash<QString, QString> _marks;         // username → its mark line ("" for a person)
    QHash<QString, qint64> _profileAt;      // username → when its profile was fetched
    bool _moving { false };                 // between worlds: people come and go without a line
    QTimer _settle;
    QTimer _peopleChange;                   // comings and goings within it are one peopleChanged
    SharedSoundPointer _sounds[2];
    int _nextSound { 0 };
    qint64 _soundAt { 0 };                  // when the last note played
};

#endif // hifi_WorldChat_h
