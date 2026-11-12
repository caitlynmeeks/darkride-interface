//
//  ChatService.cpp
//  libraries/chat/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "ChatService.h"

#include <QtCore/QDir>
#include <QtCore/QJsonDocument>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkReply>

#include <AccountManager.h>
#include <PathUtils.h>
#include <SettingHandle.h>

#include <darkride_chat.h>

#include "ChatLogging.h"

static const QString CHAT_TOKEN_PATH = "/api/v1/user/chat-token";
// the metaverse's chat token lasts two minutes; one older than this is asked for again rather than risked
static const qint64 CHAT_TOKEN_FRESH_MS = 90 * 1000;
// sign-ins the homeserver may refuse in a row before chat stops trying by itself
static const int MAX_SIGN_IN_ATTEMPTS = 3;
static const int RETRY_AFTER_REFUSAL_MS = 5 * 1000;
static const int RETRY_AFTER_NETWORK_ERROR_MS = 30 * 1000;
static const int RETRY_WHILE_CHAT_IS_OFF_MS = 5 * 60 * 1000;
// at quit, how long a sign-out may take to reach the homeserver
static const unsigned int SIGN_OUT_AT_QUIT_MS = 3000;
// in the app, how long a sign-out may wait for the homeserver (offline, or slow) before chat's data goes anyway
static const int SIGN_OUT_WAIT_MS = 3000;
// chat's data still locked after the library went (another program reading a file in it): tried again this often
static const int FORGET_RETRY_MS = 1000;
static const int FORGET_TRIES = 10;
// chat's data that couldn't be deleted: it goes at the next start, before chat runs
static Setting::Handle<bool> chatForgetPending("private/chat/forgetPending", false);

// everything chat keeps on this computer: a folder per chat server (createLibrary)
static QString chatDataRoot() {
    return QDir(PathUtils::getAppDataPath()).filePath("chat");
}

ChatService::ChatService() {
    _retryTimer.setSingleShot(true);
    connect(&_retryTimer, &QTimer::timeout, this, [this] {
        if (_started && DependencyManager::get<AccountManager>()->isLoggedIn()) {
            requestChatToken();
        }
    });
    _signOutTimer.setSingleShot(true);
    _signOutTimer.setInterval(SIGN_OUT_WAIT_MS);
    connect(&_signOutTimer, &QTimer::timeout, this, [this] {
        qCDebug(chat_service) << "the chat sign-out didn't finish in" << SIGN_OUT_WAIT_MS << "ms; chat's data goes anyway";
        afterSignOut();
    });
    _forgetTimer.setSingleShot(true);
    _forgetTimer.setInterval(FORGET_RETRY_MS);
    connect(&_forgetTimer, &QTimer::timeout, this, [this] {
        if (!_chat) {
            forgetChatData();
        }
    });
}

ChatService::~ChatService() {
    freeLibrary();
}

void ChatService::start(bool remembered) {
    if (_started) {
        return;
    }
    _started = true;
    if (!remembered || chatForgetPending.get()) {
        forgetChatData();
    }
    auto accountManager = DependencyManager::get<AccountManager>();
    connect(accountManager.data(), &AccountManager::loginComplete, this, &ChatService::onAccountLoggedIn);
    connect(accountManager.data(), &AccountManager::logoutComplete, this, &ChatService::onAccountLoggedOut);
    if (accountManager->isLoggedIn()) {
        onAccountLoggedIn();
    } else {
        setState(State::SignedOut, "Sign in to Darkride to use chat.");
    }
}

void ChatService::stop(bool forget) {
    _started = false;
    _retryTimer.stop();
    _signOutTimer.stop();
    _forgetTimer.stop();
    if (_chat && forget && _libraryState == "ready") {
        qCDebug(chat_service) << "signing out of chat at quit: the account isn't remembered on this computer";
        command({ { "op", "logout" } });
        darkride_chat_wait_idle(_chat, SIGN_OUT_AT_QUIT_MS);
    }
    if (forget) {
        // whatever chat was doing (signing in, or a sign-out the homeserver didn't answer in time), nothing of it stays
        forgetChatData();
    } else {
        freeLibrary();
    }
}

void ChatService::onAccountLoggedIn() {
    // a new sign-in, perhaps as someone else: the token says which chat account is wanted, and settle() compares
    _signInAttempts = 0;
    _keptSessionSignOuts = 0;
    requestChatToken();
}

void ChatService::onAccountLoggedOut() {
    _retryTimer.stop();
    _token.clear();
    _expectedUser.clear();
    _tokenDisplayName.clear();
    settle();
    if (!_chat) {
        // no chat this run (or it has gone already): anything kept from an earlier one goes with the sign-out too
        forgetChatData();
        setState(State::SignedOut, "Sign in to Darkride to use chat.");
    }
}

void ChatService::requestChatToken() {
    if (_tokenRequested) {
        return;
    }
    _tokenRequested = true;
    if (_state != State::Ready) {
        setState(State::SigningIn, "Signing in to chat…");
    }
    JSONCallbackParameters callback(this, "chatTokenReceived", "chatTokenFailed");
    DependencyManager::get<AccountManager>()->sendRequest(CHAT_TOKEN_PATH, AccountManagerAuth::Required,
                                                          QNetworkAccessManager::PostOperation, callback);
}

void ChatService::chatTokenReceived(QNetworkReply* reply) {
    _tokenRequested = false;
    QJsonObject data = QJsonDocument::fromJson(reply->readAll()).object().value("data").toObject();
    QString token = data.value("token").toString();
    QString homeserver = data.value("homeserver").toString();
    if (!_started || !DependencyManager::get<AccountManager>()->isLoggedIn()) {
        return;
    }
    if (token.isEmpty() || homeserver.isEmpty()) {
        setState(State::Unavailable, "Darkride didn't send a chat sign-in. Try again later.");
        return;
    }
    _token = token;
    _tokenAge.start();
    _expectedUser = data.value("user_id").toString();
    _tokenDisplayName = data.value("display_name").toString().trimmed();
    if (_chat && homeserver != _homeserver) {
        freeLibrary();
    }
    if (!_chat) {
        createLibrary(homeserver);
    }
    settle();
}

void ChatService::chatTokenFailed(QNetworkReply* reply) {
    _tokenRequested = false;
    if (!_started) {
        return;
    }
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString message = QJsonDocument::fromJson(reply->readAll()).object().value("message").toString();
    qCWarning(chat_service) << "no chat token:" << status << reply->errorString() << message;
    if (status == 503) {
        setState(State::Unavailable, "Chat is off on Darkride for now.");
        _retryTimer.start(RETRY_WHILE_CHAT_IS_OFF_MS);
    } else if (status == 401 || status == 403) {
        // banned, not verified, or an AI resident's account (chat is for people for now)
        setState(State::Unavailable, message.isEmpty() ? "This account can't use chat." : message);
    } else {
        setState(State::Unavailable, "Can't reach Darkride. Trying again shortly.");
        _retryTimer.start(RETRY_AFTER_NETWORK_ERROR_MS);
    }
}

static QString systemName() {
#if defined(Q_OS_MACOS)
    return "macOS";
#elif defined(Q_OS_WIN)
    return "Windows";
#else
    return "Linux";
#endif
}

void ChatService::createLibrary(const QString& homeserver) {
    // a folder per homeserver, so a development server's session never meets the real one's
    QUrl url(homeserver);
    QString folder = url.host() + (url.port() > 0 ? "_" + QString::number(url.port()) : QString());
    QString dataDir = QDir(PathUtils::getAppDataPath()).filePath("chat/" + folder);
    QJsonObject config {
        { "homeserver", homeserver },
        { "data_dir", QDir::toNativeSeparators(dataDir) },
        // other people can see a device's name, so it says what it is and not whose computer it runs on
        { "device_name", "Darkride viewer (" + systemName() + ")" },
    };
    QByteArray json = QJsonDocument(config).toJson(QJsonDocument::Compact);
    // the folder is the new library's now: a delete still being tried would pull it from under it (the library clears
    // what an earlier sign-in left, and never lets a leftover stop its own)
    _forgetTimer.stop();
    _forgetTries = 0;
    if (chatForgetPending.get()) {
        chatForgetPending.set(false);
    }
    _libraryState.clear();
    _libraryUser.clear();
    _chat = darkride_chat_new(json.constData(), &ChatService::receiveEvent, this);
    if (!_chat) {
        qCWarning(chat_service) << "the chat library didn't start with" << json;
        setState(State::Unavailable, "Chat couldn't start.");
        return;
    }
    _homeserver = homeserver;
    qCDebug(chat_service) << "chat library" << darkride_chat_version() << "on" << homeserver << "in" << dataDir;
}

void ChatService::freeLibrary() {
    if (_chat) {
        // no event comes after this returns; those already posted are dropped with their target if it goes too
        darkride_chat_free(_chat);
        _chat = nullptr;
        _generation++;
    }
    _libraryState.clear();
    _libraryUser.clear();
    _syncState.clear();
    _rooms = QJsonArray();
    countUnread();
    _spaces = QJsonArray();
    _profileName.clear();
    _profileAvatar.clear();
}

void ChatService::receiveEvent(const char* event, void* userData) {
    // on one of the library's threads, and the text lasts only for this call
    auto self = static_cast<ChatService*>(userData);
    QByteArray json(event);
    // the library posting this is the current one (freeLibrary changes the number only after it has stopped)
    int generation = self->_generation;
    QMetaObject::invokeMethod(self, [self, json, generation] {
        if (generation == self->_generation) {
            self->handleEvent(json);
        }
    }, Qt::QueuedConnection);
}

void ChatService::handleEvent(const QByteArray& json) {
    QJsonObject event = QJsonDocument::fromJson(json).object();
    QString kind = event.value("event").toString();
    if (kind == "state") {
        _libraryState = event.value("state").toString();
        _libraryUser = event.value("user").toString();
        QString reason = event.value("reason").toString();
        qCDebug(chat_service) << "chat library:" << _libraryState << _libraryUser << reason;
        settle();
    } else if (kind == "sync") {
        _syncState = event.value("state").toString();
        emit syncStateChanged(_syncState);
    } else if (kind == "rooms") {
        _rooms = event.value("rooms").toArray();
        _spaces = event.value("spaces").toArray();
        if (_state == State::Ready) {
            emit roomsChanged(_rooms);
        }
        countUnread();
    } else if (kind == "timeline") {
        if (_state == State::Ready) {
            emit timelineChanged(event.value("room").toString(), event.value("items").toArray());
        }
    } else if (kind == "profile") {
        if (_state != State::Ready) {
            // a kept session's, from another account, on its way out (settle): never named as this one
            return;
        }
        _profileName = event.value("name").toString();
        _profileAvatar = event.value("avatar").toString();
        // a chat account the app made has no name yet (only the web chat named one at its first sign-in): the one
        // Darkride gives with the token, never over a name that's set; later changes come from the metaverse (F7)
        // (shown once the homeserver has it: the "done" below)
        if (_profileName.isEmpty() && !_tokenDisplayName.isEmpty()) {
            qCDebug(chat_service) << "chat: the account has no name yet; naming it as Darkride does";
            command({ { "op", "set_display_name" }, { "name", _tokenDisplayName } });
        }
        emit profileChanged();
    } else if (kind == "media") {
        emit mediaReady(event.value("key").toString(), event.value("path").toString(), event.value("error").toString());
    } else if (kind == "done") {
        if (event.value("op").toString() == "set_display_name") {
            _profileName = event.value("name").toString();
            emit profileChanged();
        }
        emit commandDone(event);
    } else if (kind == "error") {
        QString op = event.value("op").toString();
        QString message = event.value("message").toString();
        if (op == "logout" && _signingOut) {
            // Windows keeps the store's files open while the library runs: they go once it has (afterSignOut)
            qCDebug(chat_service) << "chat:" << op << message;
            return;
        }
        qCWarning(chat_service) << "chat:" << op << message;
        if (op == "login_jwt") {
            _lastSignInError = message;
        }
        emit commandFailed(op, message);
    }
}

void ChatService::settle() {
    if (!_chat || _libraryState.isEmpty() || _libraryState == "starting") {
        // waiting for a token (which starts the library), or for the library to say where it stands
        return;
    }
    if (_signingOut) {
        if (_libraryState == "signed_out") {
            afterSignOut();
        }
        return;
    }
    bool wanted = _started && DependencyManager::get<AccountManager>()->isLoggedIn();
    if (!wanted) {
        // out of Darkride, out of chat: the session ends on the homeserver, and nothing of it stays on this computer
        _userId.clear();
        setState(State::SignedOut, "Sign in to Darkride to use chat.");
        if (_libraryState == "ready") {
            signOut();
        } else {
            forgetChatData();
        }
        return;
    }
    if (_libraryState == "ready") {
        if (_expectedUser.isEmpty() || _libraryUser == _expectedUser) {
            _token.clear();
            _signInAttempts = 0;
            _keptSessionSignOuts = 0;
            _userId = _libraryUser;
            setState(State::Ready);
            emit roomsChanged(_rooms);
        } else {
            // a session kept from another account: out of it, and its data gone, first; then the token signs in this one
            qCDebug(chat_service) << "the kept chat session is" << _libraryUser << "and the account is" << _expectedUser;
            _userId.clear();
            if (++_keptSessionSignOuts > MAX_SIGN_IN_ATTEMPTS) {
                setState(State::Unavailable, "Chat couldn't sign out the last account on this computer.");
                return;
            }
            setState(State::SigningIn, "Signing in to chat…");
            signOut();
        }
        return;
    }
    // signed out
    _userId.clear();
    if (_signInAttempts >= MAX_SIGN_IN_ATTEMPTS) {
        setState(State::Unavailable, "Chat refused the sign-in" +
                                         (_lastSignInError.isEmpty() ? QString(".") : ": " + _lastSignInError));
        return;
    }
    if (!_token.isEmpty() && _tokenAge.elapsed() < CHAT_TOKEN_FRESH_MS) {
        _signInAttempts++;
        command({ { "op", "login_jwt" }, { "token", _token } });
        _token.clear();
        setState(State::SigningIn, "Signing in to chat…");
    } else if (_signInAttempts > 0) {
        // refused, or a session the server ended: a new token in a moment
        setState(State::SigningIn, "Signing in to chat…");
        _retryTimer.start(RETRY_AFTER_REFUSAL_MS);
    } else {
        requestChatToken();
    }
}

void ChatService::signOut() {
    _signingOut = true;
    command({ { "op", "logout" } });
    _signOutTimer.start();
}

// The library has signed out, or the homeserver didn't answer in time: the library goes (so its files close), then
// what it kept; a sign-in still wanted starts a new library, which finds nothing of the last account.
void ChatService::afterSignOut() {
    _signingOut = false;
    _signOutTimer.stop();
    QString homeserver = _homeserver;
    forgetChatData();
    if (!_started || !DependencyManager::get<AccountManager>()->isLoggedIn()) {
        setState(State::SignedOut, "Sign in to Darkride to use chat.");
    } else if (!_token.isEmpty() && !homeserver.isEmpty()) {
        createLibrary(homeserver);
    } else {
        requestChatToken();
    }
}

// Deletes everything chat keeps on this computer (each chat server's session, keys, stored messages and fetched
// files), the library gone first: while it runs, Windows keeps its database files locked.
bool ChatService::forgetChatData() {
    QElapsedTimer timer;
    timer.start();
    bool hadLibrary = _chat != nullptr;
    freeLibrary();
    qint64 freedMs = timer.restart();
    QDir root(chatDataRoot());
    bool existed = root.exists();
    bool gone = !existed || root.removeRecursively();
    if (hadLibrary || existed) {
        // on the main thread: performance reads these (a free waits for the library's threads, at most 2 s)
        qCDebug(chat_service) << "chat: library freed in" << freedMs << "ms, data removed in" << timer.elapsed() << "ms";
    }
    if (chatForgetPending.get() != !gone) {
        chatForgetPending.set(!gone);
    }
    if (gone) {
        _forgetTimer.stop();
        _forgetTries = 0;
    } else if (_started && ++_forgetTries < FORGET_TRIES) {
        qCWarning(chat_service) << "chat's data stays in" << root.path() << "for now; trying again";
        _forgetTimer.start();
    } else {
        qCWarning(chat_service) << "chat's data stays in" << root.path() << "until the next start";
        _forgetTries = 0;
    }
    return gone;
}

void ChatService::setState(State state, const QString& text) {
    if (state == _state && text == _stateText) {
        return;
    }
    _state = state;
    _stateText = text;
    qCDebug(chat_service) << "chat:" << state << text;
    emit stateChanged(state);
    countUnread();
}

void ChatService::countUnread() {
    int count = 0;
    if (_state == State::Ready) {
        for (const auto& value : _rooms) {
            QJsonObject room = value.toObject();
            int notifications = room.value("notifications").toInt();
            count += notifications > 0 ? notifications : room.value("dm").toBool() ? room.value("unread").toInt() : 0;
        }
    }
    if (count != _unreadCount) {
        _unreadCount = count;
        emit unreadCountChanged(count);
    }
}

void ChatService::command(const QJsonObject& command) {
    if (!_chat) {
        return;
    }
    QByteArray json = QJsonDocument(command).toJson(QJsonDocument::Compact);
    darkride_chat_command(_chat, json.constData());
}

void ChatService::open(const QString& room, int count) {
    if (_state == State::Ready) {
        command({ { "op", "open" }, { "room", room }, { "count", count } });
    }
}

void ChatService::close(const QString& room) {
    if (_state == State::Ready) {
        command({ { "op", "close" }, { "room", room } });
    }
}

void ChatService::loadOlder(const QString& room, int count) {
    if (_state == State::Ready) {
        command({ { "op", "back" }, { "room", room }, { "count", count } });
    }
}

void ChatService::send(const QString& room, const QString& body) {
    if (_state == State::Ready && !body.trimmed().isEmpty()) {
        command({ { "op", "send" }, { "room", room }, { "body", body } });
    }
}

void ChatService::createDm(const QString& userId) {
    if (_state == State::Ready && !userId.isEmpty()) {
        command({ { "op", "create_dm" }, { "user", userId } });
    }
}

void ChatService::react(const QString& room, const QString& id, const QString& key) {
    if (_state == State::Ready && !id.isEmpty() && !key.isEmpty()) {
        command({ { "op", "react" }, { "room", room }, { "id", id }, { "key", key } });
    }
}

void ChatService::edit(const QString& room, const QString& id, const QString& body) {
    if (_state == State::Ready && !id.isEmpty() && !body.trimmed().isEmpty()) {
        command({ { "op", "edit" }, { "room", room }, { "id", id }, { "body", body } });
    }
}

void ChatService::deleteMessage(const QString& room, const QString& id) {
    if (_state == State::Ready && !id.isEmpty()) {
        command({ { "op", "delete" }, { "room", room }, { "id", id } });
    }
}

// A read is always private (m.read.private, the core's doing): it clears the unread count for you alone, and no one
// sees a "seen" under their message, whatever your privacy setting (Caity, 2026-11-12), as in the web chat.
void ChatService::markRead(const QString& room) {
    if (_state == State::Ready) {
        command({ { "op", "read" }, { "room", room } });
    }
}

void ChatService::join(const QString& room) {
    if (_state == State::Ready) {
        command({ { "op", "join" }, { "room", room } });
    }
}

void ChatService::leave(const QString& room) {
    if (_state == State::Ready) {
        command({ { "op", "leave" }, { "room", room } });
    }
}

void ChatService::fetchMedia(const QString& key, const QJsonValue& source, int width, int height, bool crop) {
    if (_state != State::Ready) {
        return;
    }
    QJsonObject command { { "op", "media" }, { "key", key }, { "source", source } };
    if (width > 0 && height > 0) {
        command["width"] = width;
        command["height"] = height;
        command["crop"] = crop;
    }
    this->command(command);
}

void ChatService::retry() {
    _signInAttempts = 0;
    _lastSignInError.clear();
    if (_started && DependencyManager::get<AccountManager>()->isLoggedIn()) {
        requestChatToken();
    }
}
