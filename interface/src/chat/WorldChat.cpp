//
//  WorldChat.cpp
//  interface/src/chat
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "WorldChat.h"

#include <algorithm>

#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QNetworkReply>

#include <AccountManager.h>
#include <AddressManager.h>
#include <AudioInjectorManager.h>
#include <DependencyManager.h>
#include <NetworkAccessManager.h>
#include <NetworkingConstants.h>
#include <NodeList.h>
#include <PathUtils.h>

#include "avatar/AvatarManager.h"
#include "avatar/MyAvatar.h"
#include "scripting/ChatScriptingInterface.h"

namespace {
    // as many lines as the old in-world chat kept in sight, and more
    const int ITEMS_MAX = 200;
    // after arriving in a world, the people already there come in over a few seconds: no "joined" for them
    const int SETTLE_MS = 3000;
    // comings and goings within this are one change of who's here
    const int PEOPLE_CHANGE_MS = 250;
    // a newcomer's account not known by then: no line for them
    const qint64 JOIN_NAME_WAIT_MS = 5000;
    const qint64 PROFILE_MAX_AGE_MS = 10 * 60 * 1000;
    // alternating, loud enough to hear over a world (the files peak at -19 and -23 dBFS; the old in-world chat's 0.05
    // put them near -45, unheard)
    const float SOUND_VOLUME = 0.5f;

    QPointer<WorldChat> theWorldChat;

    // the line under a resident's name, as the nametag draws it: "✦ language model · hosted by caitlyn", or for a
    // bot "⚙ bot · hosted by madders" (U+2699 alone: with U+FE0F it is drawn as a colour emoji)
    QString markLine(const QJsonObject& profile) {
        QString host = profile.value("host").toString();
        QString hosted = host.isEmpty() ? QString() : " · hosted by " + host;
        QString kind = profile.value("kind").toString();
        if (kind == "bot") {
            return QString::fromUtf8("⚙ bot") + hosted;
        }
        if (kind == "ai" || (kind.isEmpty() && profile.value("ai").toBool())) {
            QString word = profile.value("aiWord").toString();
            return QString::fromUtf8("✦ ") + (word.isEmpty() ? QString("AI resident") : word) + hosted;
        }
        return QString();
    }
}

WorldChat* WorldChat::instance() {
    if (!theWorldChat) {
        theWorldChat = new WorldChat();
    }
    return theWorldChat;
}

WorldChat::WorldChat() {
    auto chat = DependencyManager::get<ChatScriptingInterface>();
    connect(chat.data(), &ChatScriptingInterface::messageReceived, this, &WorldChat::messageReceived);
    auto nodeList = DependencyManager::get<NodeList>();
    connect(nodeList.data(), &NodeList::usernameFromIDReply, this,
            [this](const QString& session, const QString& username, const QString&, bool) {
                usernameReceived(session, username);
            });
    connect(nodeList.data(), &NodeList::uuidChanged, this, &WorldChat::moving);
    connect(DependencyManager::get<AddressManager>().data(), &AddressManager::hostChanged, this, &WorldChat::moving);
    auto avatars = DependencyManager::get<AvatarManager>();
    connect(avatars.data(), &AvatarManager::avatarAddedEvent, this, &WorldChat::avatarAdded);
    connect(avatars.data(), &AvatarManager::avatarRemovedEvent, this, &WorldChat::avatarRemoved);
    _settle.setSingleShot(true);
    _settle.setInterval(SETTLE_MS);
    connect(&_settle, &QTimer::timeout, this, [this] {
        _moving = false;
        _peopleChange.start();   // the world's name, looked up after arriving, is known by now
    });
    _peopleChange.setSingleShot(true);
    _peopleChange.setInterval(PEOPLE_CHANGE_MS);
    connect(&_peopleChange, &QTimer::timeout, this, &WorldChat::peopleChanged);

    auto sounds = DependencyManager::get<SoundCache>();
    _sounds[0] = sounds->getSound(PathUtils::resourcesUrl() + "sounds/chat1.wav");
    _sounds[1] = sounds->getSound(PathUtils::resourcesUrl() + "sounds/chat2.wav");

    // the two commands the old in-world chat had (/help, /me, /tts are the scripting API's own)
    auto clearCommand = qvariant_cast<ChatCommand*>(chat->addCommand("clear", "clears Local Chat for yourself"));
    connect(clearCommand, &ChatCommand::running, this, [this] {
        clear();
        DependencyManager::get<ChatScriptingInterface>()->showMessage("You cleared Local Chat for yourself.");
    });
    auto tpCommand = qvariant_cast<ChatCommand*>(chat->addCommand("tp", "goes to someone in this world"));
    connect(tpCommand, &ChatCommand::running, this, &WorldChat::teleportTo);
}

void WorldChat::markRead() {
    if (_unread != 0) {
        _unread = 0;
        emit unreadChanged(0);
    }
}

void WorldChat::send(const QString& text) {
    QString line = text.trimmed();
    if (line.isEmpty()) {
        return;
    }
    auto chat = DependencyManager::get<ChatScriptingInterface>();
    if (line.startsWith('/')) {
        QString name = line.mid(1).section(' ', 0, 0).toLower();
        QString params = line.section(' ', 1).trimmed();
        auto command = qvariant_cast<ChatCommand*>(chat->getCommand(name));
        if (command) {
            command->run(params);
        } else {
            chat->showMessage("There's no /" + name + " command. /help lists them.");
        }
        return;
    }
    chat->sendMessage(line);
    emit chat->messageSent(line);   // for scripts listening, as the old in-world chat told them
}

void WorldChat::messageReceived(const QVariantMap& data, const QUuid& sender) {
    QString body = data.value("message").toString();
    if (body.isEmpty()) {
        return;
    }
    QJsonObject item { { "body", body } };
    if (data.value("local").toBool()) {
        // a line only you see (a command's answer)
        item.insert("body", body.trimmed());
        item.insert("kind", "notice");
        add(item);
        return;
    }
    item.insert("kind", data.value("me").toBool() ? "emote" : "text");
    item.insert("session", sender.toString());
    bool own = sender == DependencyManager::get<NodeList>()->getSessionUUID();
    bool counted = !own && counts(sender, body);
    item.insert("own", own);
    item.insert("counts", counted);
    add(item);
    if (counted) {
        _unread++;
        // chat marks it read here when Local Chat is in front of you: then no sound, as a conversation you're reading
        emit unreadChanged(_unread);
        if (_unread > 0) {
            playSound();
        }
    }
}

bool WorldChat::counts(const QUuid& session, const QString& body) {
    // a person's line always; an AI resident's or a bot's (its profile's mark) only with your name in it. One whose
    // profile isn't in yet counts, as a person's (profiles are asked for as people arrive)
    if (_marks.value(usernameOf(session)).isEmpty()) {
        return true;
    }
    QStringList names { DependencyManager::get<AccountManager>()->getAccountInfo().getUsername() };
    if (auto myAvatar = DependencyManager::get<AvatarManager>()->getMyAvatar()) {
        names << myAvatar->getDisplayName() << myAvatar->getSessionDisplayName();
    }
    for (const QString& name : names) {
        QString trimmed = name.trimmed();
        if (trimmed.size() < 2) {
            continue;
        }
        // the whole name, any case: "Caity" in "hi caity!", not in "Caitys"
        QRegularExpression whole("(?<![\\w])" + QRegularExpression::escape(trimmed) + "(?![\\w])",
                                 QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
        if (whole.match(body).hasMatch()) {
            return true;
        }
    }
    return false;
}

int WorldChat::peopleHere() const {
    // the avatars this world sends, and yours (AvatarManager keeps it among them)
    return std::max(1, (int)DependencyManager::get<AvatarManager>()->getAvatarIdentifiers().size());
}

QString WorldChat::hereLine() const {
    int people = peopleHere();
    QString here = people <= 1 ? QString("Only you") : QString("%1 here").arg(people);
    QString world = DependencyManager::get<AddressManager>()->getDomainLabel().trimmed();
    return world.isEmpty() ? here : world + QString::fromUtf8(" · ") + here;
}

void WorldChat::add(QJsonObject item) {
    item.insert("id", QString("world:%1").arg(++_nextId));
    item.insert("ts", (double)QDateTime::currentMSecsSinceEpoch());
    _items.append(item);
    while (_items.size() > ITEMS_MAX) {
        _items.removeFirst();
    }
    wearNames();
}

QString WorldChat::usernameOf(const QUuid& session) {
    if (session.isNull()) {
        return QString();
    }
    if (session == DependencyManager::get<NodeList>()->getSessionUUID()) {
        return DependencyManager::get<AccountManager>()->getAccountInfo().getUsername();
    }
    if (!_usernames.contains(session) && !_asked.contains(session)) {
        _asked.insert(session);
        DependencyManager::get<NodeList>()->requestUsernameFromSessionID(session);
    }
    return _usernames.value(session);
}

void WorldChat::usernameReceived(const QString& session, const QString& username) {
    QUuid id(session);
    _usernames.insert(id, username);
    if (!username.isEmpty()) {
        fetchProfile(username);
    }
    if (_joining.contains(id)) {
        bool inTime = QDateTime::currentMSecsSinceEpoch() - _joining.take(id) < JOIN_NAME_WAIT_MS;
        if (inTime && !username.isEmpty()) {
            add({ { "kind", "member" }, { "change", "joined" }, { "user_name", username } });
            return;
        }
    }
    wearNames();
}

QString WorldChat::shownNameOf(const QUuid& session) {
    QString username = usernameOf(session);
    if (!username.isEmpty()) {
        return username;
    }
    // until the world says (or for a visitor without an account): the name the avatar shows the world, unique in it
    auto avatar = DependencyManager::get<AvatarManager>()->getAvatarBySessionID(session);
    if (!avatar) {
        return QString();
    }
    return avatar->getSessionDisplayName().isEmpty() ? avatar->getDisplayName() : avatar->getSessionDisplayName();
}

// each line's name and mark, from what is known of its session so far
void WorldChat::wearNames() {
    for (int i = 0; i < _items.size(); i++) {
        QJsonObject item = _items[i].toObject();
        QString session = item.value("session").toString();
        if (session.isEmpty()) {
            continue;
        }
        QString username = usernameOf(QUuid(session));
        QString name = shownNameOf(QUuid(session));
        item.insert("sender", username.isEmpty() ? QString() : "@" + username + ":");
        item.insert("name", name.isEmpty() ? QString("Someone") : name);
        QString mark = _marks.value(username);
        if (mark.isEmpty()) {
            item.remove("mark");
        } else {
            item.insert("mark", mark);
        }
        _items[i] = item;
    }
    emit itemsChanged();
}

void WorldChat::fetchProfile(const QString& username) {
    // the name comes from the world's server: one that is only dots would step out of the address, so no lookup
    if (username == "." || username == "..") {
        return;
    }
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (_profileAt.contains(username) && now - _profileAt.value(username) < PROFILE_MAX_AGE_MS) {
        return;
    }
    _profileAt.insert(username, now);
    QUrl url = NetworkingConstants::METAVERSE_SERVER_URL();
    // escaped, so whatever the name holds ("/", "?", "#") it stays one part of the address
    url.setPath("/api/user/" + QString::fromUtf8(QUrl::toPercentEncoding(username)) + "/profile", QUrl::TolerantMode);
    QNetworkReply* reply = NetworkAccessManager::getInstance().get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, username] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            _profileAt.remove(username);   // asked again with the next line
            return;
        }
        QJsonObject profile = QJsonDocument::fromJson(reply->readAll()).object();
        // the profile is the account asked about, or it isn't used (a name the world reports can't borrow another
        // account's mark)
        if (profile.value("username").toString().compare(username, Qt::CaseInsensitive) != 0) {
            return;
        }
        _marks.insert(username, markLine(profile));
        wearNames();
    });
}

void WorldChat::avatarAdded(const QUuid& session) {
    if (!_peopleChange.isActive()) {
        _peopleChange.start();
    }
    if (_moving) {
        usernameOf(session);   // its name ready for its first line
        return;
    }
    _joining.insert(session, QDateTime::currentMSecsSinceEpoch());
    if (_usernames.contains(session)) {
        usernameReceived(session.toString(), _usernames.value(session));
    } else {
        usernameOf(session);
    }
}

void WorldChat::avatarRemoved(const QUuid& session) {
    if (!_peopleChange.isActive()) {
        _peopleChange.start();
    }
    _joining.remove(session);
    QString username = _usernames.value(session);
    if (!_moving && !username.isEmpty()) {
        add({ { "kind", "member" }, { "change", "left" }, { "user_name", username } });
    }
}

void WorldChat::moving() {
    // a new world (or a new session in this one): its chat starts empty, as the old in-world chat did
    _moving = true;
    _settle.start();
    clear();
    _usernames.clear();
    _asked.clear();
    _joining.clear();
    _peopleChange.start();
}

void WorldChat::clear() {
    _items = QJsonArray();
    emit itemsChanged();
    markRead();
}

void WorldChat::playSound() {
    SharedSoundPointer sound = _sounds[_nextSound];
    _nextSound = 1 - _nextSound;
    if (!sound || !sound->isReady()) {
        return;
    }
    AudioInjectorOptions options;
    options.localOnly = true;
    // one of Darkride's own sounds, not placed in the world: Settings' Interface volume turns it down (AudioClient's
    // system sounds)
    options.positionSet = false;
    options.volume = SOUND_VOLUME;
    DependencyManager::get<AudioInjectorManager>()->playSound(sound, options, true);
}

void WorldChat::teleportTo(const QString& params) {
    auto chat = DependencyManager::get<ChatScriptingInterface>();
    QString wanted = params.section(' ', 0, 0).trimmed().toLower();
    if (wanted.isEmpty()) {
        chat->showMessage("Whom to? /tp and the start of their name.");
        return;
    }
    auto avatars = DependencyManager::get<AvatarManager>();
    for (const QUuid& session : avatars->getAvatarIdentifiers()) {
        auto avatar = avatars->getAvatarBySessionID(session);
        if (!avatar) {
            continue;
        }
        // by the name their lines show
        QString name = shownNameOf(session);
        if (!name.isEmpty() && name.toLower().contains(wanted)) {
            chat->showMessage("Going to " + name + ".");
            avatars->getMyAvatar()->goToLocation(avatar->getWorldPosition(), true, avatar->getWorldOrientation(), true);
            return;
        }
    }
    chat->showMessage("Nobody here is called " + wanted + ".");
}
