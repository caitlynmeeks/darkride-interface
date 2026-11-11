//
//  HttpRequestObject.h
//  libraries/script-engine/src
//
//  An HTTP request as a plain QObject: the behaviour of XMLHttpRequestClass (the XMLHttpRequest of the
//  QtScript engine) with QVariant values and signals in place of QScriptValue callbacks, so that a script
//  on the WebAssembly engine reaches it by reflection. The JavaScript shim wraps one of these as an
//  XMLHttpRequest; a native module can use it as it is. HttpRequestFactory is the "Http" global that
//  makes them. The script owns a request it made: it is deleted when the script lets it go (once it is done), when
//  the factory is told to dispose of it, or when the engine stops.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_HttpRequestObject_h
#define hifi_HttpRequestObject_h

#include <functional>

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <ScriptNetworkPolicy.h>

class ScriptEngine;

class HttpRequestObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariant response READ response)
    Q_PROPERTY(QString responseText READ responseText)
    Q_PROPERTY(QString responseType READ responseType WRITE setResponseType)
    Q_PROPERTY(int status READ status)
    Q_PROPERTY(QString statusText READ statusText)
    Q_PROPERTY(int readyState READ readyState)
    Q_PROPERTY(int errorCode READ errorCode)
    Q_PROPERTY(int timeout READ timeout WRITE setTimeout)
    Q_PROPERTY(int UNSENT READ unsent CONSTANT)
    Q_PROPERTY(int OPENED READ opened CONSTANT)
    Q_PROPERTY(int HEADERS_RECEIVED READ headersReceived CONSTANT)
    Q_PROPERTY(int LOADING READ loading CONSTANT)
    Q_PROPERTY(int DONE READ done CONSTANT)
public:
    enum ReadyState { UNSENT = 0, OPENED = 1, HEADERS_RECEIVED = 2, LOADING = 3, DONE = 4 };

    explicit HttpRequestObject(QObject* parent = nullptr);
    ~HttpRequestObject() override;

    QVariant response() const { return _response; }
    QString responseText() const { return QString::fromUtf8(_rawResponse); }
    QString responseType() const { return _responseType; }
    void setResponseType(const QString& type) { _responseType = type; }
    int status() const;
    QString statusText() const;
    int readyState() const { return _readyState; }
    /// Whether it was sent and its reply has not come yet.
    bool isSending() const { return _reply != nullptr; }
    int errorCode() const { return _errorCode; }
    int timeout() const { return _timeout; }
    void setTimeout(int timeout) { _timeout = timeout; }
    /// Where the script that makes this request came from, and whether the network rule for a script's own requests
    /// (ScriptNetworkPolicy) holds here (a person's client); set by the factory.
    void setOrigin(const QUrl& origin, bool ruled, bool worldIsLocal) {
        _origin = origin;
        _ruled = ruled;
        _worldIsLocal = worldIsLocal;
    }
    /// Where a refusal is logged (the script's own log).
    void setLogger(std::function<void(const QString&)> logger) { _logger = std::move(logger); }
    /// Whether the requesting script is a world's whose engine is stopping or whose world was left
    /// (ScriptEngine::isWorldEnding): a synchronous request stops waiting, and no answer reaches the script; set by
    /// the factory.
    void setEndingCheck(std::function<bool()> ending) { _ending = std::move(ending); }
    bool isEnding() const { return _ending && _ending(); }
    int unsent() const { return UNSENT; }
    int opened() const { return OPENED; }
    int headersReceived() const { return HEADERS_RECEIVED; }
    int loading() const { return LOADING; }
    int done() const { return DONE; }

    Q_INVOKABLE void open(const QString& method, const QString& url, bool async = true, const QString& username = QString(),
                          const QString& password = QString());
    Q_INVOKABLE void send();
    /// Text is sent as UTF-8; bytes as they are.
    Q_INVOKABLE void send(const QVariant& data);
    Q_INVOKABLE void abort();
    Q_INVOKABLE void setRequestHeader(const QString& name, const QString& value);
    Q_INVOKABLE QString getAllResponseHeaders() const;
    /// The header's value, or null when the reply has no such header.
    Q_INVOKABLE QVariant getResponseHeader(const QString& name) const;

signals:
    void readyStateChanged();
    void timedOut();
    void requestComplete();

private slots:
    void onFinished();
    void onDownloadProgress(qint64 received, qint64 total);
    void onTimeout();

private:
    void setReadyState(ReadyState state);
    void doSend();
    void abortRequest();
    /// Ends the request as refused by the network rule: a failed request, with the reason in the log.
    void refuse(const QString& reason);
    /// A ruled request's redirect: the next hop judged and pinned as the first was.
    void followRedirect(int status);

    QNetworkRequest _request;
    QNetworkReply* _reply { nullptr };
    QUrl _url;
    QString _method;
    QByteArray _sendData;
    QByteArray _rawResponse;
    QVariant _response;
    QString _responseType;
    QList<QNetworkReply::RawHeaderPair> _headers;  // kept after the reply is gone
    int _lastStatus { 0 };
    QString _lastStatusText;
    ReadyState _readyState { UNSENT };
    int _errorCode { QNetworkReply::NoError };
    int _timeout { 0 };
    bool _async { true };
    QTimer _timer;
    QUrl _origin;
    bool _ruled { false };
    bool _worldIsLocal { false };   // the world the script is judged in (ScriptEngine::worldIsLocal), when it made the request
    bool _deciding { false };    // the network rule is still judging (a name lookup)
    // a ruled request goes to the address the rule checked, each hop judged again: the hop's address by its name, the
    // judgement it went out under, and how many redirects it has followed
    QUrl _hopUrl;
    ScriptNetworkPolicy::Decision _decision;
    bool _byHand { false };
    int _redirects { 0 };
    int _generation { 0 };       // abort() moves it on, so a judgement that comes late is dropped
    std::function<void(const QString&)> _logger;
    std::function<bool()> _ending;
};

/// The "Http" global: makes requests for the script that asked.
class HttpRequestFactory : public QObject {
    Q_OBJECT
public:
    explicit HttpRequestFactory(ScriptEngine* engine);

    /// A request the script owns (ValueCodec: an owned object), so that one it has let go of is deleted. origin is
    /// where the code making it came from (the entity script running, else the script itself); without one, the
    /// engine's own answer. The network rule judges the request by it.
    Q_INVOKABLE HttpRequestObject* createRequest(const QString& origin = QString());
    /// Deletes a request this factory made, once the script is done with it.
    Q_INVOKABLE void dispose(QObject* request);

private:
    ScriptEngine* _engine { nullptr };
};

// --- The API reference's notes for XMLHttpRequest and request(), as a JavaScript script has them (the JavaScript
// guest's shim, tools/wasm-js/runtime.js, builds them over HttpRequestObject) ---

/**jsdoc
 * Provides a means to interact with web servers. It is a near-complete implementation of the XMLHttpRequest API described in
 * the Mozilla docs:
 * <a href="https://developer.mozilla.org/en-US/docs/Web/API/XMLHttpRequest">https://developer.mozilla.org/en-US/docs/Web/API/XMLHttpRequest</a>.
 *
 * <p>On a person's client, a script that came over the network (a world's script) may use web addresses only (http, https,
 * atp), to public hosts: not the files on that computer, not services on it or on its network. A refused request ends as a
 * failed one (status 0, error 201), with the reason in the script's log. A script loaded from the computer itself, or any
 * script of a world served from that computer or its network, keeps every address.</p>
 *
 * <p>Create using <code>new XMLHttpRequest(...)</code>.</p>
 *
 * @class XMLHttpRequest
 *
 * @hifi-interface
 * @hifi-client-entity
 * @hifi-avatar
 * @hifi-server-entity
 * @hifi-assignment-client
 *
 * @property {*} response - The response data.
 *     <em>Read-only.</em>
 * @property {string} responseText - The response data as text.
 *     <em>Read-only.</em>
 * @property {string} responseType - The response type required or received (e.g., <code>"text"</code>, <code>"json"</code>,
 *     <code>"arraybuffer"</code>, ...).
 * @property {number} status - The <a href="https://developer.mozilla.org/en-US/docs/Web/HTTP/Status">HTTP response status
 *      code</a> (<code>100</code> &ndash; <code>599</code>).
 *     <em>Read-only.</em>
 * @property {string} statusText - The HTTP response status text.
 *     <em>Read-only.</em>
 * @property {XMLHttpRequest.ReadyState} readyState - The status of the request.
 *     <em>Read-only.</em>
 * @property {XMLHttpRequest.NetworkError} errorCode - The network result of the request: including, <code>0</code> (NoError)
 *     if there was no error, <code>4</code> (TimeoutError) if the request timed out.
 *     <em>Read-only.</em>
 * @property {number} timeout - The time a request can take before timing out, in ms.
 *
 * @property {XMLHttpRequest.ReadyState} UNSENT - Request has been created; {@link XMLHttpRequest.open} not called yet.
 *     <em>Read-only.</em>
 * @property {XMLHttpRequest.ReadyState} OPENED - {@link XMLHttpRequest.open} has been called.
 *     <em>Read-only.</em>
 * @property {XMLHttpRequest.ReadyState} HEADERS_RECEIVED - {@link XMLHttpRequest.send} has been called; headers and status
 *     are available.
 *     <em>Read-only.</em>
 * @property {XMLHttpRequest.ReadyState} LOADING - Downloading; {@link XMLHttpRequest|XMLHttpRequest.responseText} has partial
 *     data.
 *     <em>Read-only.</em>
 * @property {XMLHttpRequest.ReadyState} DONE - Operation complete.
 *     <em>Read-only.</em>
 *
 * @property {XMLHttpRequest~onTimeoutCallback} ontimeout - Function called when the request times out.
 *     <p>Note: This is called in addition to any function set for <code>onreadystatechange</code>.</p>
 * @property {XMLHttpRequest~onReadyStateChangeCallback} onreadystatechange - Function called when the request's ready state
 *     changes.
 * @property {function} onload - Function called when the request is complete without a network error.
 * @property {function} onerror - Function called when the request is complete with a network error.
 * @property {function} onloadend - Function called when the request is complete, after <code>onload</code> or
 *     <code>onerror</code>.
 *
 * @example <caption>Get a web page's HTML.</caption>
 * var URL = "https://www.highfidelity.com/";
 *
 * var req = new XMLHttpRequest();
 * req.onreadystatechange = function () {
 *     if (req.readyState === req.DONE) {
 *         if (req.status === 200) {
 *             print("Success");
 *             print("Content type:", req.getResponseHeader("content-type"));
 *             print("Content:", req.responseText.slice(0, 100), "...");
 *
 *         } else {
 *             print("Error", req.status, req.statusText);
 *         }
 *
 *         req = null;
 *     }
 * };
 *
 * req.open("GET", URL);
 * req.send();
 *
 */

/**jsdoc
 * <p>The state of an XMLHttpRequest.</p>
 * <table>
 *   <thead>
 *     <tr><th>Value</th><th>Name</th><th>Description</th></tr>
 *   </thead>
 *   <tbody>
 *     <tr><td><code>0</code></td><td>UNSENT</td><td>Request has been created; {@link XMLHttpRequest.open} not called
 *       yet.</td></tr>
 *     <tr><td><code>1</code></td><td>OPENED</td><td>{@link XMLHttpRequest.open} has been called.</td></tr>
 *     <tr><td><code>2</code></td><td>HEADERS_RECEIVED</td><td>{@link XMLHttpRequest.send} has been called; headers and
 *       status are available.</td></tr>
 *     <tr><td><code>3</code></td><td>LOADING</td><td>Downloading; {@link XMLHttpRequest|XMLHttpRequest.responseText} has
 *       partial data.</td></tr>
 *     <tr><td><code>4</code></td><td>DONE</td><td>Operation complete.</td></tr>
 *   </tbody>
 * </table>
 * @typedef {number} XMLHttpRequest.ReadyState
 */

/**jsdoc
 * <p>The type of network error.</p>
 * <table>
 *   <thead>
 *     <tr><th>Value</th><th>Name</th><th>Description</th></tr>
 *   </thead>
 *   <tbody>
 *     <tr><td><code>0</code></td><td>NoError</td><td>No error.</td></tr>
 *     <tr><td><code>1</code></td><td>ConnectionRefusedError</td><td>The server refused the connection.</td></tr>
 *     <tr><td><code>2</code></td><td>RemoteHostClosedError</td><td>The server closed the connection.</td></tr>
 *     <tr><td><code>3</code></td><td>HostNotFoundError</td><td>Host name not found.</td></tr>
 *     <tr><td><code>4</code></td><td>TimeoutError</td><td>Connection timed out</td></tr>
 *     <tr><td><code>5</code></td><td>OperationCanceledError</td><td>Operation canceled by
 *       {@link XMLHttpRequest.abort}.</td></tr>
 *     <tr><td><code>6</code></td><td>SslHandshakeFailedError</td><td>SSL/TLS handshake failed.</td></tr>
 *     <tr><td><code>7</code></td><td>TemporaryNetworkFailureError</td><td>Temporarily disconnected from the
 *       network.</td></tr>
 *     <tr><td><code>8</code></td><td>NetworkSessionFailedError</td><td>Disconnection from the network.</td></tr>
 *     <tr><td><code>9</code></td><td>BackgroundRequestNotAllowedError</td><td>Background request not allowed.</td></tr>
 *     <tr><td><code>10</code></td><td>TooManyRedirectsError</td><td>Too many redirects.</td></tr>
 *     <tr><td><code>11</code></td><td>InsecureRedirectError</td><td>Redirect from secure to insecure protocol.</td></tr>
 *     <tr><td><code>101</code></td><td>ProxyConnectionRefusedError</td><td>Connection to proxy server refused.</td></tr>
 *     <tr><td><code>102</code></td><td>ProxyConnectionClosedError</td><td>Proxy server closed the connection.</td></tr>
 *     <tr><td><code>103</code></td><td>ProxyNotFoundError</td><td>Proxy host name not found.</td></tr>
 *     <tr><td><code>104</code></td><td>ProxyTimeoutError</td><td>Proxy connection timed out.</td></tr>
 *     <tr><td><code>105</code></td><td>ProxyAuthenticationRequiredError</td><td>Proxy requires authentication.</td></tr>
 *     <tr><td><code>201</code></td><td>ContentAccessDenied</td><td>Access denied.</td></tr>
 *     <tr><td><code>202</code></td><td>ContentOperationNotPermittedError</td><td>Operation not permitted.</td></tr>
 *     <tr><td><code>203</code></td><td>ContentNotFoundError</td><td>Content not found.</td></tr>
 *     <tr><td><code>204</code></td><td>AuthenticationRequiredError</td><td>Authentication required.</td></tr>
 *     <tr><td><code>205</code></td><td>ContentReSendError</td><td>Resend failed.</td></tr>
 *     <tr><td><code>206</code></td><td>ContentConflictError</td><td>Resource state conflict.</td></tr>
 *     <tr><td><code>207</code></td><td>ContentGoneError</td><td>Resource no longer available.</td></tr>
 *     <tr><td><code>401</code></td><td>InternalServerError</td><td>Internal server error.</td></tr>
 *     <tr><td><code>402</code></td><td>OperationNotImplementedError</td><td>Operation not supported.</td></tr>
 *     <tr><td><code>403</code></td><td>ServiceUnavailableError</td><td>Request not able to be handled at this
 *       time.</td></tr>
 *     <tr><td><code>301</code></td><td>ProtocolUnknownError</td><td>Protocol unknown.</td></tr>
 *     <tr><td><code>302</code></td><td>ProtocolInvalidOperationError</td><td>Operation invalid fro protocol.</td></tr>
 *     <tr><td><code>99</code></td><td>UnknownNetworkError</td><td>Unknown network-related error.</td></tr>
 *     <tr><td><code>199</code></td><td>UnknownProxyError</td><td>Unknown proxy-related error.</td></tr>
 *     <tr><td><code>299</code></td><td>UnknownContentError</td><td>Unknown content-related error.</td></tr>
 *     <tr><td><code>399</code></td><td>ProtocolFailure</td><td>Protocol error.</td></tr>
 *     <tr><td><code>499</code></td><td>UnknownServerError</td><td>Unknown server response error.</td></tr>
 *
 *   </tbody>
 * </table>
 * @typedef {number} XMLHttpRequest.NetworkError
 */

/**jsdoc
 * Called when the request's ready state changes.
 * @callback XMLHttpRequest~onReadyStateChangeCallback
 */

/**jsdoc
 * Called when the request times out.
 * @callback XMLHttpRequest~onTimeoutCallback
 */

/**jsdoc
 * Initializes a request.
 * @function XMLHttpRequest.open
 * @param {string} method - The <a href="https://developer.mozilla.org/en-US/docs/Web/HTTP/Methods">HTTP request method</a>
 *     to use, e.g., <code>"GET"</code>, <code>"POST"</code>, <code>"PUT"</code>, or <code>"DELETE"</code>.
 * @param {string} url - The URL to send the request to.
 * @param {boolean} [async=true] - <code>true</code> if the method returns without waiting for the response,
 *     <code>false</code> if the method returns only once the request is complete.
 * @param {string} [username=""] - User name for authentication.
 * @param {string} [password=""] - Password for authentication.
 */

/**jsdoc
 * Sets the value of an HTTP request header. Must be called after {@link XMLHttpRequest.open} but before
 * {@link XMLHttpRequest.send};
 * @function XMLHttpRequest.setRequestHeader
 * @param {string} name - The name of the header to set.
 * @param {string} value - The value of the header.
 */

/**jsdoc
 * Sends the request to the server.
 * @function XMLHttpRequest.send
 * @param {*} [data] - The data to send.
 */

/**jsdoc
 * Aborts the request.
 * @function XMLHttpRequest.abort
 */

/**jsdoc
 * Gets the response headers.
 * @function XMLHttpRequest.getAllResponseHeaders
 * @returns {string} The response headers, separated by <code>"\n"</code> characters.
 */

/**jsdoc
 * Gets a response header.
 * @function XMLHttpRequest.getResponseHeader
 * @param {string} name -
 * @returns {string} The response header.
 */

/**jsdoc
 * Sends a HTTP request. API is a non-complete implementation of {@link https://www.npmjs.com/package/request npm module request}.
 * @function request
 * @param {RequestOptions|string} options - If a string is specified, it will use it as a URI and continue as a <code>GET</code> request.
 * @param {RequestCallback} callback
 * @example <caption>Make a GET request to the metaverse to check how many online users and worlds there are.</caption>
 * request({
 *     uri: "https://tivolicloud.com/api/domains/stats",
 *     method: "GET",
 *     json: true,
 *     body: {},
 *     headers: {}
 * }, function (error, response, body) {
 *     if (error) return console.error(error);
 *     console.log("Response headers:", response.headers);
 *     console.log("Status code:", response.statusCode);
 *     console.log("Content type:", response.headers["content-type"]);
 *     console.log("Online users:", body.onlineUsers);
 *     console.log("Online worlds:", body.onlineDomains);
 * });
 */

/**jsdoc
 * Options for the {@link request} function.
 * @typedef {Object} RequestOptions
 * @property {string} [uri] - URI to be requested.
 * @property {string} [url] - URI to be requested. (alias to <code>uri</code>)
 * @property {string} [method] - Find {@link https://developer.mozilla.org/en-US/docs/Web/HTTP/Methods all HTTP methods here}.
 * @property {boolean} [json] - Whether to expect the response as JSON which will be parsed.
 * @property {Object.<string, string>} [body] - Object of body parameters to send.
 * @property {Object.<string, string>} [headers] - Object of headers to send.
 */

/**jsdoc
 * Response for the {@link request} function.
 * @typedef {Object} RequestResponse
 * @property {number} statusCode - Find {@link https://www.restapitutorial.com/httpstatuscodes.html all HTTP status codes here}.
 * @property {Object.<string, string>} headers - Reponse headers where each key is lowercase, e.g. <code>headers["content-type"]</code>.
 */

/**jsdoc
 * Callback for the {@link request} function.
 * @callback RequestCallback
 * @param {string} error
 * @param {RequestResponse} response
 * @param {string|Object} body - Returns the response body or object if <code>json</code> was enabled in the request options.
 */

#endif // hifi_HttpRequestObject_h
