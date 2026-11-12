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

// --- The API reference's notes for WebSocket and WebSocketServer, as a JavaScript script has them (the JavaScript
// guest's shim, tools/wasm-js/runtime.js, builds them over WebSocketObject and WebSocketServerObject) ---

/**jsdoc
 * Provides a bi-directional, event-driven communication session between the script and another WebSocket connection. It is a
 * near-complete implementation of the WebSocket API described in the Mozilla docs:
 * <a href="https://developer.mozilla.org/en-US/docs/Web/API/WebSocket">https://developer.mozilla.org/en-US/docs/Web/API/WebSocket</a>.
 *
 * <p>Create using <code>new WebSocket(...)</code> in Interface, client entity, avatar, server entity and assignment client
 * scripts. An assignment client script also gets one for each connection to its {@link WebSocketServer}.
 *
 * <p>On a person's client, a script that came over the network (a world's script) may connect to public hosts only: not to
 * services on that computer or on its network. A refused connection fails as an unreachable one does (onerror, then onclose
 * with code 1006), with the reason in the script's log. A script loaded from the computer itself, or any script of a world
 * served from that computer or its network, keeps every address.</p>
 *
 * @class WebSocket
 * @param {string} url - The URL to connect to.
 *
 * @hifi-interface
 * @hifi-client-entity
 * @hifi-avatar
 * @hifi-server-entity
 * @hifi-assignment-client
 *
 * @property {string} binaryType="blob" - <em>Not used.</em>
 * @property {number} bufferedAmount=0 - <em>Not implemented.</em> <em>Read-only.</em>
 * @property {string} extensions="" - <em>Not implemented.</em> <em>Read-only.</em>
 *
 * @property {WebSocket~onOpenCallback} onopen - Function called when the connection opens.
 * @property {WebSocket~onMessageCallback} onmessage - Function called when a message is received.
 * @property {WebSocket~onErrorCallback} onerror - Function called when an error occurs.
 * @property {WebSocket~onCloseCallback} onclose - Function called when the connection closes.
 *
 * @property {string} protocol="" - <em>Not implemented.</em> <em>Read-only.</em>
 * @property {WebSocket.ReadyState} readyState - The state of the connection. <em>Read-only.</em>
 * @property {string} url - The URL to connect to. <em>Read-only.</em>
 *
 * @property {WebSocket.ReadyState} CONNECTING - The connection is opening. <em>Read-only.</em>
 * @property {WebSocket.ReadyState} OPEN - The connection is open. <em>Read-only.</em>
 * @property {WebSocket.ReadyState} CLOSING - The connection is closing. <em>Read-only.</em>
 * @property {WebSocket.ReadyState} CLOSED - The connection is closed. <em>Read-only.</em>
 *
 * @example <caption>Echo a message off websocket.org.</caption>
 * print("Create WebSocket");
 * var WEBSOCKET_PING_URL = "ws://echo.websocket.org";
 * var webSocket = new WebSocket(WEBSOCKET_PING_URL);
 *
 * webSocket.onclose = function (data) {
 *     print("WebSocket closed");
 *     print("Ready state =", webSocket.readyState);  // 3
 * };
 *
 * webSocket.onmessage = function (data) {
 *     print("Message received:", data.data);
 *
 *     print("Close WebSocket");
 *     webSocket.close();
 * };
 *
 * webSocket.onopen = function () {
 *     print("WebSocket opened");
 *     print("Ready state =", webSocket.readyState);  // 1
 *
 *     print("Send a test message");
 *     webSocket.send("Test message");
 * };
 */

/**jsdoc
 * The state of a WebSocket connection.
 * <table>
 *   <thead>
 *     <tr><th>Value</th><th>Name</th><th>Description</th></tr>
 *   </thead>
 *   <tbody>
 *     <tr><td><code>0</code></td><td>CONNECTING</td><td>The connection is opening.</td></tr>
 *     <tr><td><code>1</code></td><td>OPEN</td><td>The connection is open.</td></tr>
 *     <tr><td><code>2</code></td><td>CLOSING</td><td>The connection is closing.</td></tr>
 *     <tr><td><code>3</code></td><td>CLOSED</td><td>The connection is closed.</td></tr>
 *   </tbody>
 * </table>
 * @typedef {number} WebSocket.ReadyState
 */

/**jsdoc
 * The reason why the connection was closed.
 * <table>
 *   <thead>
 *     <tr><th>Value</th><th>Name</th><th>Description</th></tr>
 *   </thead>
 *   <tbody>
 *     <tr><td><code>1000</code></td><td>Normal</td><td>Normal closure.</td></tr>
 *     <tr><td><code>1001</code></td><td>GoingAway</td><td>Going away.</td></tr>
 *     <tr><td><code>1002</code></td><td>ProtocolError</td><td>Protocol error.</td></tr>
 *     <tr><td><code>1003</code></td><td>DatatypeNotSupported</td><td>Unsupported data.</td></tr>
 *     <tr><td><code>1004</code></td><td>Reserved1004</td><td>Reserved.</td></tr>
 *     <tr><td><code>1005</code></td><td>MissingStatusCode</td><td>No status received.</td></tr>
 *     <tr><td><code>1006</code></td><td>AbnormalDisconnection</td><td>abnormal closure.</td></tr>
 *     <tr><td><code>1007</code></td><td>WrongDatatype</td><td>Invalid frame payload data.</td></tr>
 *     <tr><td><code>1008</code></td><td>PolicyViolated</td><td>Policy violation.</td></tr>
 *     <tr><td><code>1009</code></td><td>TooMuchData</td><td>Message too big.</td></tr>
 *     <tr><td><code>1010</code></td><td>MissingExtension</td><td>Mandatory extension missing.</td></tr>
 *     <tr><td><code>1011</code></td><td>BadOperation</td><td>Internal server error.</td></tr>
 *     <tr><td><code>1015</code></td><td>TlsHandshakeFailed</td><td>TLS handshake failed.</td></tr>
 *   </tbody>
 * <table>
 * @typedef {number} WebSocket.CloseCode
 */

/**jsdoc
 * Information on a connection being closed.
 * @typedef {object} WebSocket.CloseData
 * @property {WebSocket.CloseCode} code - The reason why the connection was closed.
 * @property {string} reason - Description of the reason why the connection was closed.
 * @property {boolean} wasClean - <code>true</code> if the connection closed cleanly, <code>false</code> if it didn't.
 */

/**jsdoc
 * A message received on a WebSocket connection.
 * @typedef {object} WebSocket.MessageData
 * @property {string} data - The message content.
 */

/**jsdoc
 * <p>The type of socket error.</p>
 * <table>
 *   <thead>
 *     <tr><th>Value</th><th>Name</th><th>Description</th></tr>
 *   </thead>
 *   <tbody>
 *     <tr><td><code>0</code></td><td>ConnectionRefusedError</td><td>The connection was refused or timed out.</td></tr>
 *     <tr><td><code>1</code></td><td>RemoteHostClosedError</td><td>The remote host closed the connection.</td></tr>
 *     <tr><td><code>2</code></td><td>HostNotFoundError</td><td>The host address was not found.</td></tr>
 *     <tr><td><code>3</code></td><td>SocketAccessError</td><td>The socket operation failed because the application doesn't have
 *       the necessary privileges.</td></tr>
 *     <tr><td><code>4</code></td><td>SocketResourceError</td><td>The local system ran out of resources (e.g., too many
 *       sockets).</td></tr>
 *     <tr><td><code>5</code></td><td>SocketTimeoutError</td><td>The socket operation timed out.</td></tr>
 *     <tr><td><code>6</code></td><td>DatagramTooLargeError</td><td>The datagram was larger than the OS's limit.</td></tr>
 *     <tr><td><code>7</code></td><td>NetworkError</td><td>An error occurred with the network.</td></tr>
 *     <tr><td><code>8</code></td><td>AddressInUseError</td><td>The is already in use and cannot be reused.</td></tr>
 *     <tr><td><code>9</code></td><td>SocketAddressNotAvailableError</td><td>The address specified does not belong to the
 *       host.</td></tr>
 *     <tr><td><code>10</code></td><td>UnsupportedSocketOperationError</td><td>The requested socket operation is not supported
 *       by the local OS.</td></tr>
 *     <tr><td><code>11</code></td><td>ProxyAuthenticationRequiredError</td><td>The socket is using a proxy and requires
 *       authentication.</td></tr>
 *     <tr><td><code>12</code></td><td>SslHandshakeFailedError</td><td>The SSL/TLS handshake failed.</td></tr>
 *     <tr><td><code>13</code></td><td>UnfinishedSocketOperationError</td><td>The last operation has not finished yet.</td></tr>
 *     <tr><td><code>14</code></td><td>ProxyConnectionRefusedError</td><td>Could not contact the proxy server because connection
 *       was denied.</td></tr>
 *     <tr><td><code>15</code></td><td>ProxyConnectionClosedError</td><td>The connection to the proxy server was unexpectedly
 *       closed.</td></tr>
 *     <tr><td><code>16</code></td><td>ProxyConnectionTimeoutError</td><td>The connection to the proxy server timed
 *       out.</td></tr>
 *     <tr><td><code>17</code></td><td>ProxyNotFoundError</td><td>The proxy address was not found.</td></tr>
 *     <tr><td><code>18</code></td><td>ProxyProtocolError</td><td>Connection to the proxy server failed because the server
 *       response could not be understood.</td></tr>
 *     <tr><td><code>19</code></td><td>OperationError</td><td>An operation failed because the socket state did not permit
 *       it.</td></tr>
 *     <tr><td><code>20</code></td><td>SslInternalError</td><td>Internal error in the SSL library being used.</td></tr>
 *     <tr><td><code>21</code></td><td>SslInvalidUserDataError</td><td>Error in the SSL library because of invalid
 *       data.</td></tr>
 *     <tr><td><code>22</code></td><td>TemporaryError</td><td>A temporary error occurred.</td></tr>
 *     <tr><td><code>-1</code></td><td>UnknownSocketError</td><td>An unknown error occurred.</td></tr>
 *   </tbody>
 * </table>
 * @typedef {number} WebSocket.SocketError
 */

/**jsdoc
 * Called when the connection opens.
 * @callback WebSocket~onOpenCallback
 */

/**jsdoc
 * Triggered when a message is received.
 * @callback WebSocket~onMessageCallback
 * @param {WebSocket.MessageData} message - The message received.
 */

/**jsdoc
 * Called when an error occurs.
 * @callback WebSocket~onErrorCallback
 * @param {WebSocket.SocketError} error - The error.
 */

/**jsdoc
 * Called when the connection closes.
 * @callback WebSocket~onCloseCallback
 * @param {WebSocket.CloseData} data - Information on the connection closure.
 */

/**jsdoc
 * Sends a message on the connection.
 * @function WebSocket.send
 * @param {string|object} message - The message to send. If an object, it is converted to a string.
 */

/**jsdoc
 * Closes the connection.
 * @function WebSocket.close
 * @param {WebSocket.CloseCode} [closeCode=1000] - The reason for closing.
 * @param {string} [reason=""] - A description of the reason for closing.
 */

/**jsdoc
 * Manages {@link WebSocket}s in assignment client scripts: a listening socket that other scripts connect to. A world's
 * entity scripts, on a client or on the entity script server, have no <code>WebSocketServer</code>.
 *
 * <p>Create using <code>new WebSocketServer(...)</code>.</p>
 *
 * @class WebSocketServer
 *
 * @hifi-assignment-client
 *
 * @property {string} url - The URL that the server is listening on. <em>Read-only.</em>
 * @property {number} port - The port that the server is listening on. <em>Read-only.</em>
 * @property {boolean} listening - <code>true</code> if the server is listening for incoming connections, <code>false</code> if
 *     it isn't. <em>Read-only.</em>
 *
 * @example <caption>Echo a message back to sender.</caption>
 * // Assignment client script. Echoes received message back to sender.
 * print("Create WebSocketServer");
 * var webSocketServer = new WebSocketServer();
 * print("Server url:", webSocketServer.url);
 *
 * function onNewConnection(webSocket) {
 *     print("New connection");
 *
 *     webSocket.onmessage = function (message) {
 *         print("Message received:", message.data);
 *
 *         var returnMessage = message.data + " back!";
 *         print("Echo a message back:", returnMessage);
 *         webSocket.send(message.data + " back!");
 *     };
 * }
 *
 * webSocketServer.newConnection.connect(onNewConnection);
 *
 * @example
 * // Interface script, run from the same computer. Bounces a message off the assignment client script.
 * // Use the server URL reported by the assignment client script.
 * var WEBSOCKET_PING_URL = "ws://127.0.0.1:nnnnn";
 * var TEST_MESSAGE = "Hello";
 *
 * print("Create WebSocket");
 * var webSocket = new WebSocket(WEBSOCKET_PING_URL);
 *
 * webSocket.onmessage = function(data) {
 *     print("Message received:", data.data);
 * };
 *
 * webSocket.onopen = function() {
 *     print("WebSocket opened");
 *     print("Send test message:", TEST_MESSAGE);
 *     webSocket.send(TEST_MESSAGE);
 * };
 */

/**jsdoc
 * Closes all connections and closes the WebSocketServer.
 * @function WebSocketServer.close
 */

/**jsdoc
 * Triggered when there is a new connection.
 * @function WebSocketServer.newConnection
 * @param {WebSocket} webSocket - The {@link WebSocket} for the new connection.
 * @returns {Signal}
 */

#endif // hifi_WebSocketObject_h
