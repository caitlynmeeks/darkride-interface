//
//  WebSocketObject.cpp
//  libraries/script-engine/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "WebSocketObject.h"

#include <QtCore/QPointer>

#include <ScriptNetworkPolicy.h>

#include "ScriptEngine.h"

WebSocketObject::WebSocketObject(const QString& url, const QUrl& origin, bool ruled, bool worldIsLocal, std::function<void(const QString&)> logger,
                                 QObject* parent) :
    QObject(parent), _socket(new QWebSocket()), _requestedUrl(url), _origin(origin), _ruled(ruled), _worldIsLocal(worldIsLocal), _logger(std::move(logger)) {
    initialize();
    if (!_ruled) {
        _socket->open(_requestedUrl);
        return;
    }
    // the network rule for a script's own requests: judged at once, or after a name lookup
    ScriptNetworkPolicy::decide(_requestedUrl, _origin, this, [this](const ScriptNetworkPolicy::Decision& decision) {
        if (decision.allowed) {
            _socket->open(_requestedUrl);
        } else {
            refuse(decision.reason);
        }
    }, _worldIsLocal);
}

void WebSocketObject::refuse(const QString& reason) {
    if (_logger) {
        _logger(ScriptNetworkPolicy::refusalLine(_origin, _requestedUrl, reason));
    }
    // as a connection that fails: after the script has attached its handlers
    QMetaObject::invokeMethod(this, [this] {
        emit errored();
        emit closedWith(int(QWebSocketProtocol::CloseCodeAbnormalDisconnection), "not allowed by the network rule", false);
    }, Qt::QueuedConnection);
}

WebSocketObject::WebSocketObject(QWebSocket* socket, QObject* parent) : QObject(parent), _socket(socket) {
    initialize();
}

WebSocketObject::~WebSocketObject() {
    _socket->deleteLater();
}

void WebSocketObject::initialize() {
    connect(_socket, &QWebSocket::connected, this, [this] {
        if (_ruled && ScriptNetworkPolicy::applies(_origin, _worldIsLocal) && ScriptNetworkPolicy::isPrivateAddress(_socket->peerAddress())) {
            // the name led here after it was judged (an answer that changed): nothing goes over this connection
            if (_logger) {
                _logger(ScriptNetworkPolicy::refusalLine(_origin, _socket->requestUrl(),
                    "it connected to an address on this machine or its network (" + _socket->peerAddress().toString() + ")"));
            }
            _refused = true;
            emit errored();
            _socket->close(QWebSocketProtocol::CloseCodePolicyViolated, "not allowed by the network rule");
            return;
        }
        emit opened();
    });
    connect(_socket, &QWebSocket::textMessageReceived, this, [this](const QString& message) {
        if (!_refused) {
            emit messageReceived(message);
        }
    });
    connect(_socket, static_cast<void (QWebSocket::*)(QAbstractSocket::SocketError)>(&QWebSocket::error), this,
            [this](QAbstractSocket::SocketError) { emit errored(); });
    connect(_socket, &QWebSocket::disconnected, this, [this] {
        const bool hasError = _socket->error() != QAbstractSocket::UnknownSocketError;
        emit closedWith(hasError ? int(QWebSocketProtocol::CloseCodeAbnormalDisconnection) : int(_socket->closeCode()),
                        _socket->closeReason(), !hasError);
    });
}

int WebSocketObject::readyState() const {
    switch (_socket->state()) {
        case QAbstractSocket::HostLookupState:
        case QAbstractSocket::ConnectingState:
            return CONNECTING;
        case QAbstractSocket::ConnectedState:
        case QAbstractSocket::BoundState:
        case QAbstractSocket::ListeningState:
            return OPEN;
        case QAbstractSocket::ClosingState:
            return CLOSING;
        default:
            return CLOSED;
    }
}

void WebSocketObject::send(const QString& message) {
    _socket->sendTextMessage(message);
}

void WebSocketObject::close(int code, const QString& reason) {
    _socket->close(QWebSocketProtocol::CloseCode(code), reason);
}


// --- the factory ---------------------------------------------------------------------------------------------------

WebSocketFactory::WebSocketFactory(ScriptEngine* engine) : QObject(engine), _engine(engine) {}

WebSocketObject* WebSocketFactory::createWebSocket(const QString& url, const QString& origin) {
    const QUrl originUrl = _engine ? _engine->ruledOrigin(_engine->verifiedScriptOrigin(origin)) : QUrl();
    const bool ruled = !_engine || _engine->networkRuled();
    const bool worldIsLocal = _engine && _engine->worldIsLocal();
    QPointer<ScriptEngine> engine(_engine);
    return new WebSocketObject(url, originUrl, ruled, worldIsLocal, [engine](const QString& line) {
        if (engine) {
            engine->scriptWarningMessage(line);
        }
    }, this);
}

void WebSocketFactory::dispose(QObject* socket) {
    if (socket && socket->parent() == this) {
        socket->deleteLater();
    }
}

WebSocketServerObject::WebSocketServerObject(const QString& serverName, int port, QObject* parent) :
    QObject(parent), _server(serverName, QWebSocketServer::NonSecureMode) {
    if (_server.listen(QHostAddress::Any, quint16(port))) {
        connect(&_server, &QWebSocketServer::newConnection, this, [this] {
            while (_server.hasPendingConnections()) {
                auto client = new WebSocketObject(_server.nextPendingConnection(), this);
                _clients << client;
                emit newConnection(client);
            }
        });
    }
}

void WebSocketServerObject::close() {
    for (WebSocketObject* client : _clients) {
        if (client->readyState() != WebSocketObject::CLOSED) {
            client->close(QWebSocketProtocol::CloseCodeGoingAway, "Server closing.");
        }
    }
    _server.close();
}

WebSocketServerObject* WebSocketServerFactory::createWebSocketServer(const QString& serverName, int port) {
    return new WebSocketServerObject(serverName, port, this);
}
