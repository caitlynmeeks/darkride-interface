//
//  MakeLinkServer.cpp
//  libraries/make-link/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakeLinkServer.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLoggingCategory>
#include <QtCore/QFileInfo>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

#ifdef Q_OS_WIN
#include <functional>

#include <QtCore/QWinEventNotifier>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

Q_LOGGING_CATEGORY(makelink, "darkride.make.link")

const int MakeLinkServer::MAX_MESSAGE_BYTES = 4 * 1024 * 1024;

namespace {

const QString SOCKET_NAME = "link.sock";
const int DEFAULT_TIME_LIMIT_MS = 30000;
// the MCP revisions this server speaks (tools with text and image results); a client asking for another gets the
// newest of these and decides
const QStringList PROTOCOL_VERSIONS { "2025-11-25", "2025-06-18", "2025-03-26" };
const QString STOPPED = "The person pressed Stop in Make. Stop now and tell them, in a sentence, how far you got.";

// JSON-RPC's error codes
const int PARSE_ERROR = -32700;
const int INVALID_REQUEST = -32600;
const int METHOD_NOT_FOUND = -32601;
const int INVALID_PARAMS = -32602;

}  // namespace

#ifdef Q_OS_WIN

namespace {

const QString PIPE_PREFIX = QStringLiteral("\\\\.\\pipe\\darkride-make-");

// a process's user, and whether it runs in an AppContainer (a boxed program never talks to Make)
bool userOf(HANDLE process, QByteArray& sid, bool& boxed) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray user(static_cast<int>(size), '\0');
    DWORD inContainer = 0;
    DWORD containerSize = sizeof inContainer;
    const bool read = size > 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size) &&
                      GetTokenInformation(token, TokenIsAppContainer, &inContainer, containerSize, &containerSize);
    CloseHandle(token);
    if (!read) {
        return false;
    }
    PSID userSid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
    sid = QByteArray(reinterpret_cast<const char*>(userSid), static_cast<int>(GetLengthSid(userSid)));
    boxed = inContainer != 0;
    return true;
}

QString thisUserSid() {
    QByteArray sid;
    bool boxed = false;
    LPWSTR text = nullptr;
    if (!userOf(GetCurrentProcess(), sid, boxed) ||
        !ConvertSidToStringSidW(reinterpret_cast<PSID>(sid.data()), &text)) {
        return QString();
    }
    const QString result = QString::fromWCharArray(text);
    LocalFree(text);
    return result;
}

// the client of a connected pipe instance runs as this user, unboxed
bool clientIsThisUser(HANDLE pipe, QString& why) {
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid)) {
        why = "a program connected that can't be told apart";
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    QByteArray theirs, ours;
    bool theirsBoxed = true, oursBoxed = true;
    const bool known = process && userOf(process, theirs, theirsBoxed) && userOf(GetCurrentProcess(), ours, oursBoxed);
    if (process) {
        CloseHandle(process);
    }
    if (!known || theirs != ours) {
        why = "a program of another user tried to connect";
        return false;
    }
    if (theirsBoxed) {
        why = "a boxed program tried to connect";
        return false;
    }
    return true;
}

}  // namespace

// Make's named pipe (security's make-blender-box-windows-linux, "the link"): a name from newPipeName(), a DACL for
// this user only, the first instance ours (FILE_FLAG_FIRST_PIPE_INSTANCE: a program that made the name first is
// refused), no remote clients (named pipes are otherwise reachable over SMB). One instance waits at a time; once a
// client connects, the next is made before that one is handed on, so the name stays ours.
class MakePipeListener {
public:
    explicit MakePipeListener(std::function<void(HANDLE)> connected) : _connected(std::move(connected)) {}
    MakePipeListener(const MakePipeListener&) = delete;
    ~MakePipeListener() {
        delete _notifier;
        closeWaiting();
        if (_overlapped.hEvent) {
            CloseHandle(_overlapped.hEvent);
        }
        LocalFree(_descriptor);
    }

    bool start(const QString& name, QString& error) {
        _name = name.toStdWString();
        const QString sid = thisUserSid();
        const std::wstring dacl = QString("D:P(A;;GA;;;%1)").arg(sid).toStdWString();
        if (sid.isEmpty() ||
            !ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(), SDDL_REVISION_1, &_descriptor, nullptr)) {
            error = "can't make the link private";
            return false;
        }
        _overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!_overlapped.hEvent) {
            error = "can't open the link";
            return false;
        }
        _notifier = new QWinEventNotifier(_overlapped.hEvent);
        QObject::connect(_notifier, &QWinEventNotifier::activated, [this] { ready(); });
        if (!wait(true)) {
            error = GetLastError() == ERROR_ACCESS_DENIED ? "another program holds the link's name"
                                                          : "can't open the link (" + QString::number(GetLastError()) + ")";
            return false;
        }
        return true;
    }

    // the DACL the waiting instance really carries, read back from its handle and spelt as SDDL: the log line at listen
    // shows the access as applied (the lab can't read a pipe's security from outside while an instance waits)
    QString appliedDacl() const {
        if (_waiting == INVALID_HANDLE_VALUE) {
            return QStringLiteral("(no waiting instance)");
        }
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const DWORD read = GetSecurityInfo(_waiting, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                                           nullptr, &descriptor);
        if (read != ERROR_SUCCESS) {
            return QString("(unreadable: %1)").arg(read);
        }
        LPWSTR text = nullptr;
        QString result = QStringLiteral("(unprintable)");
        if (ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION,
                                                                 &text, nullptr)) {
            // the SID is this user's own: the log says so instead of printing it (security, 11-08)
            result = QString::fromWCharArray(text);
            const QString sid = thisUserSid();
            if (!sid.isEmpty()) {
                result.replace(sid, QStringLiteral("this user"));
            }
            LocalFree(text);
        }
        LocalFree(descriptor);
        return result;
    }

private:
    bool wait(bool first) {
        SECURITY_ATTRIBUTES attributes { sizeof attributes, _descriptor, FALSE };
        _waiting = CreateNamedPipeW(_name.c_str(),
                                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &attributes);
        if (_waiting == INVALID_HANDLE_VALUE) {
            return false;
        }
        ResetEvent(_overlapped.hEvent);
        _connectedAtOnce = false;
        if (!ConnectNamedPipe(_waiting, &_overlapped)) {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                _connectedAtOnce = true;  // a client came between the create and the connect
                SetEvent(_overlapped.hEvent);
            } else if (error != ERROR_IO_PENDING) {
                closeWaiting();
                return false;
            }
        }
        return true;
    }

    void ready() {
        if (_waiting == INVALID_HANDLE_VALUE) {
            return;
        }
        DWORD ignored = 0;
        const bool connected = _connectedAtOnce || GetOverlappedResult(_waiting, &_overlapped, &ignored, FALSE);
        HANDLE pipe = _waiting;
        _waiting = INVALID_HANDLE_VALUE;
        if (!wait(false)) {
            // no next instance: the event stays as it is, so the notifier stops here (Make's Connect opens a new link)
            ResetEvent(_overlapped.hEvent);
            _notifier->setEnabled(false);
        }
        if (connected) {
            _connected(pipe);
        } else {
            CloseHandle(pipe);
        }
    }

    void closeWaiting() {
        if (_waiting != INVALID_HANDLE_VALUE) {
            // the waiting connect is cancelled and has ended before its OVERLAPPED goes
            DWORD ignored = 0;
            if (CancelIoEx(_waiting, &_overlapped)) {
                GetOverlappedResult(_waiting, &_overlapped, &ignored, TRUE);
            }
            CloseHandle(_waiting);
            _waiting = INVALID_HANDLE_VALUE;
        }
    }

    std::wstring _name;
    HANDLE _waiting { INVALID_HANDLE_VALUE };
    OVERLAPPED _overlapped {};
    bool _connectedAtOnce { false };
    QWinEventNotifier* _notifier { nullptr };
    PSECURITY_DESCRIPTOR _descriptor { nullptr };
    std::function<void(HANDLE)> _connected;
};

#endif  // Q_OS_WIN

QString MakeLinkServer::newPipeName() {
#ifdef Q_OS_WIN
    return PIPE_PREFIX + QUuid::createUuid().toString(QUuid::Id128);
#else
    return QString();
#endif
}

MakeLinkServer::MakeLinkServer(QObject* parent) : QObject(parent) {
}

MakeLinkServer::~MakeLinkServer() {
    close();
}

void MakeLinkServer::setHandler(const QString& name, Handler handler) {
    if (!_tools.has(name)) {
        qCWarning(makelink) << "no tool" << name << "on the list; not offered";
        return;
    }
    _handlers.insert(name, std::move(handler));
}

bool MakeLinkServer::listen(const QString& folder, QString& error) {
#ifdef Q_OS_WIN
    close();
    if (!folder.startsWith(PIPE_PREFIX) || folder.size() != PIPE_PREFIX.size() + 32) {
        error = "the link's pipe name isn't Make's";
        return false;
    }
    _pipes = new MakePipeListener([this](HANDLE pipe) { adoptPipe(pipe); });
    if (!_pipes->start(folder, error)) {
        delete _pipes;
        _pipes = nullptr;
        return false;
    }
    _socketPath = folder;
    qCInfo(makelink) << "Make's link is listening; the pipe's access as applied:" << _pipes->appliedDacl();
    return true;
#else
    close();
    if (!QDir().mkpath(folder)) {
        error = "can't make the link's folder";
        return false;
    }
    const QByteArray folderPath = QFile::encodeName(QDir(folder).absolutePath());
    struct stat info;
    if (lstat(folderPath.constData(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid()) {
        error = "the link's folder isn't this user's own";
        return false;
    }
    if ((info.st_mode & 0777) != 0700 && chmod(folderPath.constData(), 0700) != 0) {
        error = "can't make the link's folder private";
        return false;
    }
    const QString path = QDir(folder).absoluteFilePath(SOCKET_NAME);
    const QByteArray socketPath = QFile::encodeName(path);
    if (socketPath.size() >= static_cast<int>(sizeof(sockaddr_un::sun_path))) {
        error = "the link's folder path is too long for a socket";
        return false;
    }
    // a socket left by an earlier run of this app: this user's, so ours to remove; anything else there is not
    if (lstat(socketPath.constData(), &info) == 0) {
        if (!S_ISSOCK(info.st_mode) || info.st_uid != getuid()) {
            error = "something that isn't Make's link is in its place";
            return false;
        }
        QLocalSocket probe;
        probe.connectToServer(path);
        if (probe.waitForConnected(200)) {
            error = "another Darkride is already linked to Make here";
            return false;
        }
        unlink(socketPath.constData());
    }
    _server = new QLocalServer(this);
    _server->setSocketOptions(QLocalServer::UserAccessOption);
    _server->setMaxPendingConnections(2);
    if (!_server->listen(path)) {
        error = "can't open the link: " + _server->errorString();
        delete _server;
        _server = nullptr;
        return false;
    }
    chmod(socketPath.constData(), 0600);
    _socketPath = path;
    connect(_server, &QLocalServer::newConnection, this, &MakeLinkServer::accept);
    qCInfo(makelink) << "Make's link is listening";
    return true;
#endif
}

bool MakeLinkServer::isListening() const {
    return (_server && _server->isListening()) || _pipes;
}

QString MakeLinkServer::accessAsApplied() const {
#ifdef Q_OS_WIN
    return _pipes ? _pipes->appliedDacl() : QString();
#else
    if (!_server || !_server->isListening()) {
        return QString();
    }
    struct stat info;
    const QByteArray folder = QFile::encodeName(QFileInfo(_socketPath).path());
    return lstat(folder.constData(), &info) == 0 ? QString::number(info.st_mode & 0777, 8) : QString();
#endif
}

void MakeLinkServer::close() {
    dropClient();
#ifdef Q_OS_WIN
    if (_pipes) {
        delete _pipes;
        _pipes = nullptr;
        _socketPath.clear();
        qCInfo(makelink) << "Make's link is closed";
    }
#endif
    if (_server) {
        _server->close();
        delete _server;
        _server = nullptr;
        QFile::remove(_socketPath);
        _socketPath.clear();
        qCInfo(makelink) << "Make's link is closed";
    }
}

void MakeLinkServer::disconnectClient() {
    dropClient();
}

void MakeLinkServer::accept() {
    while (_server && _server->hasPendingConnections()) {
        QLocalSocket* socket = _server->nextPendingConnection();
        if (!socket) {
            return;
        }
        QString refused;
        _connecting = MakePeer::Line();
#ifndef Q_OS_WIN
        // who connected, before anything is read from it: SO_PEERCRED on Linux, getpeereid on macOS
        const int fd = static_cast<int>(socket->socketDescriptor());
        bool thisUser = false;
        if (fd >= 0) {
#ifdef Q_OS_LINUX
            struct ucred credentials;
            socklen_t length = sizeof credentials;
            thisUser = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 &&
                       length == sizeof credentials && credentials.uid == getuid();
#else
            uid_t uid = 0;
            gid_t gid = 0;
            thisUser = getpeereid(fd, &uid, &gid) == 0 && uid == getuid();
#endif
        }
        if (!thisUser) {
            refused = "a program of another user tried to connect";
        } else {
            // and how its Claude Code was started (MakePeer: the relay's parents, read from the system; C9)
            _connecting = MakePeer::read(fd);
        }
#endif
        adopt(socket, refused);
    }
}

void MakeLinkServer::adoptPipe(void* pipe) {
#ifdef Q_OS_WIN
    // who connected, before anything is read from it
    QString refused;
    if (!clientIsThisUser(pipe, refused)) {
        qCWarning(makelink) << "refused a connection:" << refused;
        emit connectionRefused(refused);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return;
    }
    auto socket = new QLocalSocket(this);
    socket->setSocketDescriptor(reinterpret_cast<qintptr>(pipe));
    _connecting = MakePeer::Line();
    ULONG clientPid = 0;
    if (GetNamedPipeClientProcessId(pipe, &clientPid)) {
        _connecting.peerPid = clientPid;   // its Claude Code's line isn't read on Windows yet (C9: later)
    }
    adopt(socket, QString());
#else
    Q_UNUSED(pipe);
#endif
}

void MakeLinkServer::adopt(QLocalSocket* socket, QString refused) {
    if (refused.isEmpty() && _client) {
        refused = "a second Claude Code tried to connect while one is connected";
    }
    if (!refused.isEmpty()) {
        qCWarning(makelink) << "refused a connection:" << refused;
        emit connectionRefused(refused);
        socket->abort();
        socket->deleteLater();
        return;
    }
    // the same words on every platform: the socket's or pipe's peer was checked to be this user's (security, 11-08)
    qCInfo(makelink) << "the link's peer is this user's own process";
    _client = socket;
    _buffer.clear();
    _clientName.clear();
    _clientVersion.clear();
    _peer = _connecting;
    _connecting = MakePeer::Line();
    _channelEvents = 0;
    connect(socket, &QLocalSocket::readyRead, this, &MakeLinkServer::readClient);
    connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
        if (_client == socket) {
            dropClient();
        }
    });
    qCInfo(makelink) << "Claude Code connected to Make";
    emit connectedChanged(true);
}

void MakeLinkServer::dropClient() {
    // the running calls end with the connection: their answers have nowhere to go
    for (auto it = _calls.begin(); it != _calls.end(); ++it) {
        if (it->timer) {
            it->timer->deleteLater();
        }
        emit callFinished(it.key(), it->tool, false, "the connection closed");
    }
    _calls.clear();
    if (!_client) {
        return;
    }
    QLocalSocket* socket = _client;
    _client = nullptr;
    _buffer.clear();
    _channelEvents = 0;
    _peer = MakePeer::Line();
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    qCInfo(makelink) << "Claude Code disconnected from Make";
    emit connectedChanged(false);
}

void MakeLinkServer::readClient() {
    if (!_client) {
        return;
    }
    _buffer.append(_client->readAll());
    while (_client) {
        const int end = _buffer.indexOf('\n');
        if (end < 0) {
            if (_buffer.size() > MAX_MESSAGE_BYTES) {
                qCWarning(makelink) << "a message from Claude Code was longer than" << MAX_MESSAGE_BYTES << "bytes";
                dropClient();
            }
            return;
        }
        const QByteArray line = _buffer.left(end).trimmed();
        _buffer.remove(0, end + 1);
        if (line.isEmpty()) {
            continue;
        }
        if (line.size() > MAX_MESSAGE_BYTES) {
            dropClient();
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (document.isObject()) {
            handle(document.object());
        } else if (document.isArray()) {
            for (const QJsonValue& message : document.array()) {
                if (message.isObject()) {
                    handle(message.toObject());
                }
            }
        } else {
            answerError(QJsonValue::Null, PARSE_ERROR, "not JSON");
        }
    }
}

void MakeLinkServer::handle(const QJsonObject& message) {
    const QString method = message.value("method").toString();
    const bool isRequest = message.contains("id") && !message.value("id").isNull();
    const QJsonValue id = message.value("id");
    const QJsonObject params = message.value("params").toObject();
    if (method.isEmpty()) {
        // an answer to a request of ours: this server sends none
        return;
    }
    if (!isRequest) {
        if (method == "notifications/claude/channel/permission_request") {
            // Make relays no permission prompts (security C1: the capability isn't declared, so none should come; one
            // that does is left to Terminal, unanswered)
            qCInfo(makelink) << "a permission request from Claude Code was ignored: Make doesn't relay them";
            return;
        }
        if (method == "notifications/cancelled") {
            const QJsonValue cancelled = params.value("requestId");
            for (auto it = _calls.begin(); it != _calls.end(); ++it) {
                if (it->requestId == cancelled) {
                    // no answer for a cancelled request; a late one from its handler is dropped
                    const int call = it.key();
                    if (it->timer) {
                        it->timer->deleteLater();
                    }
                    const QString tool = it->tool;
                    _calls.erase(it);
                    emit callFinished(call, tool, false, "Claude Code cancelled it");
                    break;
                }
            }
        }
        return;
    }
    if (method == "initialize") {
        const QString asked = params.value("protocolVersion").toString();
        const QJsonObject clientInfo = params.value("clientInfo").toObject();
        _clientName = clientInfo.value("name").toString().left(60);
        _clientVersion = clientInfo.value("version").toString().left(40);
        emit clientIdentified();
        // "claude/channel" makes this a channel to Claude Code: the person's own words typed in Make's panel reach the
        // Claude Code in Terminal as notifications/claude/channel (sendChannelEvent). Nothing else under experimental:
        // never "claude/channel/permission", not even as false (security C1; before Claude Code 2.1.234 false counted
        // as declared). The protocol stays at 2025-11-25 or older (PROTOCOL_VERSIONS): a channel that agrees to
        // 2026-07-28 doesn't register on Claude Code 2.1.292 and later.
        answer(id, QJsonObject {
            { "protocolVersion", PROTOCOL_VERSIONS.contains(asked) ? asked : PROTOCOL_VERSIONS.first() },
            { "capabilities", QJsonObject { { "tools", QJsonObject { { "listChanged", false } } },
                                            { "experimental", QJsonObject { { "claude/channel", QJsonObject() } } } } },
            { "serverInfo", QJsonObject { { "name", "darkride" }, { "title", "Darkride Make" }, { "version", "1" } } },
            { "instructions", _instructions },
        });
    } else if (method == "ping") {
        answer(id, QJsonObject());
    } else if (method == "tools/list") {
        QSet<QString> offered;
        for (auto it = _handlers.begin(); it != _handlers.end(); ++it) {
            offered.insert(it.key());
        }
        answer(id, QJsonObject { { "tools", _tools.mcpTools(offered) } });
    } else if (method == "tools/call") {
        callTool(id, params);
    } else if (method == "resources/list" || method == "prompts/list") {
        answerError(id, METHOD_NOT_FOUND, "this server offers tools only");
    } else {
        answerError(id, METHOD_NOT_FOUND, "no method " + method.left(80));
    }
}

void MakeLinkServer::callTool(const QJsonValue& requestId, const QJsonObject& params) {
    const QString tool = params.value("name").toString();
    const QJsonValue given = params.value("arguments");
    if (!given.isUndefined() && !given.isNull() && !given.isObject()) {
        answerError(requestId, INVALID_PARAMS, "arguments must be an object");
        return;
    }
    const QJsonObject inputs = given.toObject();
    const int call = _nextCall++;
    Call entry;
    entry.tool = tool;
    entry.requestId = requestId;
    entry.timer = new QTimer(this);
    entry.timer->setSingleShot(true);
    connect(entry.timer, &QTimer::timeout, this, [this, call] {
        finish(call, Result::error(MakeError::TIMEOUT, "it didn't finish in its time limit"));
    });
    entry.timer->start(_timeLimits.value(tool, DEFAULT_TIME_LIMIT_MS));
    _calls.insert(call, entry);

    if (_stopped) {
        finish(call, Result::error(MakeError::REFUSED, STOPPED));
        return;
    }
    QString code, message;
    if (!_handlers.contains(tool)) {
        finish(call, Result::error(MakeError::REFUSED, "there is no tool " + tool.left(80) + " here"));
        return;
    }
    if (!_tools.check(tool, inputs, code, message)) {
        finish(call, Result::error(code, message));
        return;
    }
    auto run = [this, call, tool, inputs](bool allowed, const QString& line) {
        if (!_calls.contains(call)) {
            return;  // answered meanwhile (Stop, its time limit, cancelled)
        }
        if (!allowed) {
            finish(call, Result::error(MakeError::REFUSED, line.isEmpty() ? QString("the person said no") : line));
            return;
        }
        emit callStarted(call, tool, inputs);
        _handlers.value(tool)(inputs, [this, call](const Result& result) { finish(call, result); });
    };
    if (_gate) {
        _gate(tool, inputs, run);
    } else {
        run(true, QString());
    }
}

void MakeLinkServer::finish(int call, const Result& result) {
    auto it = _calls.find(call);
    if (it == _calls.end()) {
        return;  // answered already: once only
    }
    const Call entry = *it;
    _calls.erase(it);
    if (entry.timer) {
        entry.timer->stop();
        entry.timer->deleteLater();
    }
    answer(entry.requestId, toolResult(entry.tool, result));
    emit callFinished(call, entry.tool, result.ok(), result.message);
}

QJsonObject MakeLinkServer::toolResult(const QString& tool, const Result& result) const {
    if (!result.ok()) {
        return QJsonObject {
            { "content", QJsonArray { QJsonObject { { "type", "text" }, { "text", result.code + ": " + result.message } } } },
            { "structuredContent", QJsonObject { { "code", result.code }, { "message", result.message } } },
            { "isError", true },
        };
    }
    bool labelled = false;
    QString tooBig;
    const QJsonObject value = _tools.keepWorldContentShort(tool, result.value, labelled, tooBig);
    if (!tooBig.isEmpty()) {
        return toolResult(tool, Result::error(MakeError::BAD_INPUT, tooBig));
    }
    QString text = QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
    if (labelled) {
        text = MakeToolList::worldContentLabel() + "\n" + text;
    }
    QJsonArray content { QJsonObject { { "type", "text" }, { "text", text } } };
    for (const Result::Image& image : result.images) {
        content.append(QJsonObject { { "type", "image" },
                                     { "mimeType", image.mimeType },
                                     { "data", QString::fromLatin1(image.data.toBase64()) } });
    }
    return QJsonObject { { "content", content }, { "isError", false } };
}

void MakeLinkServer::stop() {
    _stopped = true;
    const QList<int> running = _calls.keys();
    for (int call : running) {
        finish(call, Result::error(MakeError::REFUSED, STOPPED));
    }
}

void MakeLinkServer::resume() {
    _stopped = false;
}

void MakeLinkServer::send(const QJsonObject& message) {
    if (!_client) {
        return;
    }
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    line.append('\n');
    _client->write(line);
}

bool MakeLinkServer::sendChannelEvent(const QString& content, const QJsonObject& meta) {
    if (!_client) {
        return false;
    }
    send(QJsonObject { { "jsonrpc", "2.0" },
                       { "method", "notifications/claude/channel" },
                       { "params", QJsonObject { { "content", content }, { "meta", meta } } } });
    ++_channelEvents;
    return true;
}

void MakeLinkServer::answer(const QJsonValue& requestId, const QJsonObject& result) {
    send(QJsonObject { { "jsonrpc", "2.0" }, { "id", requestId }, { "result", result } });
}

void MakeLinkServer::answerError(const QJsonValue& requestId, int code, const QString& message) {
    Q_UNUSED(INVALID_REQUEST);
    send(QJsonObject { { "jsonrpc", "2.0" },
                       { "id", requestId },
                       { "error", QJsonObject { { "code", code }, { "message", message } } } });
}
