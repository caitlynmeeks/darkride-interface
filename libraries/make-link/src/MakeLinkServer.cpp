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
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

#ifndef Q_OS_WIN
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
    Q_UNUSED(folder);
    error = "Make's link isn't on Windows yet";
    return false;
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
    return _server && _server->isListening();
}

void MakeLinkServer::close() {
    dropClient();
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
#ifndef Q_OS_WIN
        // who connected, before anything is read from it
        uid_t uid = 0;
        gid_t gid = 0;
        const int fd = static_cast<int>(socket->socketDescriptor());
        if (fd < 0 || getpeereid(fd, &uid, &gid) != 0 || uid != getuid()) {
            refused = "a program of another user tried to connect";
        }
#endif
        if (refused.isEmpty() && _client) {
            refused = "a second Claude Code tried to connect while one is connected";
        }
        if (!refused.isEmpty()) {
            qCWarning(makelink) << "refused a connection:" << refused;
            emit connectionRefused(refused);
            socket->abort();
            socket->deleteLater();
            continue;
        }
        _client = socket;
        _buffer.clear();
        _clientName.clear();
        connect(socket, &QLocalSocket::readyRead, this, &MakeLinkServer::readClient);
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            if (_client == socket) {
                dropClient();
            }
        });
        qCInfo(makelink) << "Claude Code connected to Make";
        emit connectedChanged(true);
    }
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
        _clientName = params.value("clientInfo").toObject().value("name").toString().left(60);
        answer(id, QJsonObject {
            { "protocolVersion", PROTOCOL_VERSIONS.contains(asked) ? asked : PROTOCOL_VERSIONS.first() },
            { "capabilities", QJsonObject { { "tools", QJsonObject { { "listChanged", false } } } } },
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

void MakeLinkServer::answer(const QJsonValue& requestId, const QJsonObject& result) {
    send(QJsonObject { { "jsonrpc", "2.0" }, { "id", requestId }, { "result", result } });
}

void MakeLinkServer::answerError(const QJsonValue& requestId, int code, const QString& message) {
    Q_UNUSED(INVALID_REQUEST);
    send(QJsonObject { { "jsonrpc", "2.0" },
                       { "id", requestId },
                       { "error", QJsonObject { { "code", code }, { "message", message } } } });
}
