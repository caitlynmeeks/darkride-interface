//
//  MakeLinkServer.h
//  libraries/make-link/src
//
//  Make's link to the builder's own Claude Code: an MCP server (JSON-RPC 2.0, one message per line) on a local
//  socket that only this user can open, reached through the darkride-mcp helper that Claude Code starts (security A1).
//  No network port. It listens only after the person turns Make's link on (A3), takes one Claude Code at a time and
//  checks that the program connecting runs as this user before it reads a byte from it (A22).
//
//  Every tool call passes three steps, in order: the checks of the tool list (MakeToolList: on the list, the inputs
//  fit, no property that skips a tool's checks; B6), then the gate the app sets (whether the person must allow it
//  first, C10), then the tool's handler, which the app sets for each tool it offers. A tool with no handler isn't
//  offered. Each call has a time limit; Stop answers every running call at once and refuses new ones until resume().
//
//  The socket's path is for the line Make writes for Claude Code; it is never logged, nor given to scripts (A4).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_MakeLinkServer_h
#define hifi_MakeLinkServer_h

#include <functional>

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtNetwork/QLocalSocket>

#include "MakePeer.h"
#include "MakeToolList.h"

class QLocalServer;
class MakePipeListener;
class QTimer;

class MakeLinkServer : public QObject {
    Q_OBJECT
public:
    /// What a tool answers: a value (the result as TOOLS.json describes it, its images apart), or an error code
    /// (MakeError) with a plain line.
    struct Result {
        QJsonObject value;
        struct Image {
            QString mimeType;
            QByteArray data;
        };
        QVector<Image> images;
        QString code;
        QString message;

        bool ok() const { return code.isEmpty(); }
        static Result error(const QString& code, const QString& message) {
            Result result;
            result.code = code;
            result.message = message;
            return result;
        }
    };
    using Reply = std::function<void(const Result& result)>;
    /// A tool: runs with the checked inputs and answers once through `reply`, on this object's thread, now or later.
    using Handler = std::function<void(const QJsonObject& inputs, const Reply& reply)>;
    /// The gate before a checked call runs: `proceed(true)` lets it run, `proceed(false, line)` refuses it with that line.
    using Gate = std::function<void(const QString& tool, const QJsonObject& inputs,
                                    const std::function<void(bool allowed, const QString& line)>& proceed)>;

    explicit MakeLinkServer(QObject* parent = nullptr);
    ~MakeLinkServer() override;

    /// The tool list the calls are checked against (the resource's by default).
    void setToolList(const MakeToolList& tools) { _tools = tools; }
    const MakeToolList& toolList() const { return _tools; }
    /// The tool `name` (from the list) runs `handler`; until it has one it isn't offered.
    void setHandler(const QString& name, Handler handler);
    /// `name` isn't offered any more: not in tools/list, and a call of it is refused as no such tool.
    void removeHandler(const QString& name) { _handlers.remove(name); }
    void setGate(Gate gate) { _gate = std::move(gate); }
    /// What Claude Code is told about the server when it connects (MCP's instructions): the app's own words only,
    /// never anything from a world.
    void setInstructions(const QString& instructions) { _instructions = instructions; }
    /// The time limit of one call of `name` (default 30 s), the gate's wait included.
    void setTimeLimit(const QString& name, int milliseconds) { _timeLimits[name] = milliseconds; }
    /// While held, a tools/list request waits: Make's Connect self-test decides the Blender tools, so the list is
    /// answered when the hold is released (after the handlers are set), or after the cap with what is offered then.
    void setToolListHold(bool held);
    /// The cap of that wait (default 15 s: the self-test's usual seconds with margin).
    void setToolListHoldCap(int milliseconds) { _toolListHoldMs = milliseconds; }

    /// Listens on `folder`/link.sock: the folder is made (or kept) this user's own, mode 0700, and the socket 0600.
    /// False with a plain reason when the folder isn't this user's, the path is too long for a socket, or another
    /// program holds it. On Windows `folder` is a pipe name from newPipeName() instead (security's named pipe: this
    /// user only, the first instance ours, no remote clients, each client's user checked).
    bool listen(const QString& folder, QString& error);
    /// A fresh random pipe name for listen() on Windows (\\.\pipe\darkride-make-<32 hex>), one per Connect.
    static QString newPipeName();
    bool isListening() const;
    /// Stops listening, closes Claude Code's connection and removes the socket.
    void close();
    /// The socket, for the MCP line Make writes; empty while not listening. Never log it.
    QString socketPath() const { return _socketPath; }
    /// The access the listening link really has, for the log and the tests: on Windows the pipe's access list read back
    /// from the waiting instance (SDDL, this user shown as "this user"); on Unix the socket folder's mode in octal.
    /// Empty while not listening.
    QString accessAsApplied() const;

    bool isConnected() const { return _client != nullptr; }
    /// Closes Claude Code's connection and goes on listening (the person's Disconnect).
    void disconnectClient();
    /// The MCP client's own name and version, as it said when it connected ("claude-code", "2.1.319").
    QString clientName() const { return _clientName; }
    QString clientVersion() const { return _clientVersion; }
    /// The program on the socket and the Claude Code it was started by, read when it connected (MakePeer; C9).
    MakePeer::Line peer() const { return _peer; }

    /// A message of the person's own typing for the connected Claude Code, as a channel event (Claude Code's
    /// notifications/claude/channel, which this server declares at initialize): `content` is the words, `meta` the
    /// app's own keys with values it assigned (msg_id, pictures), never a world's. False when nothing is connected.
    bool sendChannelEvent(const QString& content, const QJsonObject& meta);
    /// Channel events sent to the connected Claude Code; 0 again when it goes.
    int channelEventCount() const { return _channelEvents; }

    /// Answers every running call with "the person pressed Stop" and refuses new calls until resume().
    void stop();
    void resume();
    bool isStopped() const { return _stopped; }
    int runningCalls() const { return _calls.size(); }

    /// The largest message taken from Claude Code; a longer one closes the connection.
    static const int MAX_MESSAGE_BYTES;

signals:
    void connectedChanged(bool connected);
    /// A connection was refused: another user's program, or a second Claude Code while one is connected.
    void connectionRefused(const QString& why);
    /// Claude Code's hello (initialize) arrived, after the connection: clientName() and clientVersion() are known from
    /// here on (before it they are empty).
    void clientIdentified();
    /// A checked call is about to run (after its gate), and when it has been answered.
    void callStarted(int call, const QString& tool, const QJsonObject& inputs);
    /// A call answered; `code` is the error code (MakeError) when it failed, empty when it worked
    void callFinished(int call, const QString& tool, bool ok, const QString& code, const QString& message);

private:
    struct Call {
        QString tool;
        QJsonValue requestId;
        QTimer* timer { nullptr };
    };

    void accept();
    /// Takes `socket` as Claude Code's connection unless `refused` already says why not (or one is connected).
    void adopt(QLocalSocket* socket, QString refused);
    /// Windows: a pipe instance a client connected to (a HANDLE): its user checked, then adopted or closed.
    void adoptPipe(void* pipe);
    void readClient();
    void dropClient();
    void handle(const QJsonObject& message);
    void callTool(const QJsonValue& requestId, const QJsonObject& params);
    void finish(int call, const Result& result);
    void send(const QJsonObject& message);
    void answer(const QJsonValue& requestId, const QJsonObject& result);
    void answerError(const QJsonValue& requestId, int code, const QString& message);
    void answerToolList(const QJsonValue& requestId);
    void answerHeldToolLists();
    QJsonObject toolResult(const QString& tool, const Result& result) const;

    MakeToolList _tools;
    QHash<QString, Handler> _handlers;
    QHash<QString, int> _timeLimits;
    Gate _gate;
    QString _instructions;

    QLocalServer* _server { nullptr };
    MakePipeListener* _pipes { nullptr };  // Windows' named pipe, in place of _server
    QString _socketPath;
    QPointer<QLocalSocket> _client;
    QByteArray _buffer;
    QString _clientName;
    QString _clientVersion;
    MakePeer::Line _peer;
    MakePeer::Line _connecting;   // read in accept(), before adopt() takes the socket
    int _channelEvents { 0 };

    QHash<int, Call> _calls;
    int _nextCall { 1 };
    bool _stopped { false };
    bool _toolListHeld { false };
    QList<QJsonValue> _heldToolLists;   // tools/list requests waiting for the offer to be decided
    QTimer* _toolListCap { nullptr };
    int _toolListHoldMs { 0 };
};

#endif // hifi_MakeLinkServer_h
