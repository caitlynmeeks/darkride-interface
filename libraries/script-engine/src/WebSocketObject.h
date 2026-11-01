//
//  WebSocketObject.h
//  libraries/script-engine/src
//
//  A WebSocket connection as a plain QObject: the behaviour of WebSocketClass (the WebSocket of the QtScript
//  engine) with signals in place of QScriptValue callbacks, reachable by reflection from the WebAssembly
//  engine. The JavaScript shim wraps one as a WebSocket. WebSocketFactory is the "Sockets" global that makes
//  them; a socket lives until the factory is told to dispose of it or the engine stops.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_WebSocketObject_h
#define hifi_WebSocketObject_h

#include <functional>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtWebSockets/QWebSocket>
#include <QtWebSockets/QWebSocketServer>

class ScriptEngine;

class WebSocketObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString binaryType READ binaryType WRITE setBinaryType)
    Q_PROPERTY(int bufferedAmount READ bufferedAmount)
    Q_PROPERTY(QString extensions READ extensions)
    Q_PROPERTY(QString protocol READ protocol)
    Q_PROPERTY(int readyState READ readyState)
    Q_PROPERTY(QString url READ url)
    Q_PROPERTY(int CONNECTING READ connecting CONSTANT)
    Q_PROPERTY(int OPEN READ open CONSTANT)
    Q_PROPERTY(int CLOSING READ closing CONSTANT)
    Q_PROPERTY(int CLOSED READ closed CONSTANT)
public:
    enum ReadyState { CONNECTING = 0, OPEN = 1, CLOSING = 2, CLOSED = 3 };

    /// Opens the connection to the URL at once, as the constructor of a WebSocket does, when the network rule for a
    /// script's own requests (ScriptNetworkPolicy) allows it for a script of this origin; ruled says whether the rule
    /// holds here (a person's client). A refusal shows as a connection that fails, with the reason in the log.
    WebSocketObject(const QString& url, const QUrl& origin, bool ruled, bool worldIsLocal, std::function<void(const QString&)> logger,
                    QObject* parent = nullptr);
    /// Wraps a socket that exists already (a server's incoming connection).
    WebSocketObject(QWebSocket* socket, QObject* parent = nullptr);
    ~WebSocketObject() override;

    QString binaryType() const { return _binaryType; }
    void setBinaryType(const QString& type) { _binaryType = type; }
    int bufferedAmount() const { return 0; }
    QString extensions() const { return QString(); }
    QString protocol() const { return QString(); }
    int readyState() const;
    QString url() const { return (_socket->requestUrl().isEmpty() ? _requestedUrl : _socket->requestUrl()).toDisplayString(); }
    int connecting() const { return CONNECTING; }
    int open() const { return OPEN; }
    int closing() const { return CLOSING; }
    int closed() const { return CLOSED; }

    Q_INVOKABLE void send(const QString& message);
    Q_INVOKABLE void close(int code = QWebSocketProtocol::CloseCodeNormal, const QString& reason = QString());

signals:
    void opened();
    void messageReceived(const QString& message);
    void errored();
    void closedWith(int code, const QString& reason, bool wasClean);

private:
    void initialize();
    void refuse(const QString& reason);

    QWebSocket* _socket;
    QString _binaryType { "blob" };
    QUrl _requestedUrl;
    QUrl _origin;
    bool _ruled { false };
    bool _refused { false };   // it connected to an address the rule refuses: nothing it sends is delivered
    bool _worldIsLocal { false };   // the world the script is judged in (ScriptEngine::worldIsLocal), when it opened the socket
    std::function<void(const QString&)> _logger;
};

/// The "Sockets" global: makes connections for the script that asked.
class WebSocketFactory : public QObject {
    Q_OBJECT
public:
    explicit WebSocketFactory(ScriptEngine* engine);

    /// A socket the script owns (ValueCodec: an owned object), so that one it has let go of is deleted. origin is
    /// where the code making it came from (the entity script running, else the script itself); without one, the
    /// engine's own answer. The network rule judges the connection by it.
    Q_INVOKABLE WebSocketObject* createWebSocket(const QString& url, const QString& origin = QString());
    /// Deletes a socket this factory made, once the script is done with it.
    Q_INVOKABLE void dispose(QObject* socket);

private:
    ScriptEngine* _engine { nullptr };
};

/// A WebSocket server as a plain QObject: the behaviour of WebSocketServerClass, an agent's WebSocketServer, for an
/// engine without QtScript. It listens on every address from the start (port 0 picks a free one), and each incoming
/// connection arrives through newConnection as a WebSocketObject, which lives as long as the server.
class WebSocketServerObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString url READ url)
    Q_PROPERTY(int port READ port)
    Q_PROPERTY(bool listening READ listening)

public:
    WebSocketServerObject(const QString& serverName, int port, QObject* parent = nullptr);

    QString url() const { return _server.serverUrl().toDisplayString(); }
    int port() const { return _server.serverPort(); }
    bool listening() const { return _server.isListening(); }

    /// Closes every client that is still open ("Server closing."), then the server.
    Q_INVOKABLE void close();

signals:
    void newConnection(QObject* client);

private:
    QWebSocketServer _server;
    QList<WebSocketObject*> _clients;
};

/// Makes WebSocket servers. An engine registers it for an agent only, as the QtScript engine gives WebSocketServer
/// to agents only: a client or entity script may not listen on the player's machine.
class WebSocketServerFactory : public QObject {
    Q_OBJECT

public:
    explicit WebSocketServerFactory(QObject* parent = nullptr) : QObject(parent) {}

    /// A server the script owns, as a socket is.
    Q_INVOKABLE WebSocketServerObject* createWebSocketServer(const QString& serverName, int port);
};

#endif // hifi_WebSocketObject_h
