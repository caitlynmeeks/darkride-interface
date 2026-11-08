//
//  MakeLink.h
//  interface/src/make
//
//  Make's link in the app: what the Make dock talks to, and what carries out the tools the builder's own Claude Code
//  calls. It holds the MCP server on the user-only socket (MakeLinkServer), the Blender tool (MakeBlender), the
//  building tools' own engine (make-tools.js, under a world's rules) and the Make folder: the project folder Claude
//  Code runs in, with its MCP line and its settings (only Darkride's tools; shell, web and files denied), the readable
//  copies of the scripts Claude writes, and the models Blender made.
//
//  Nothing listens and no engine runs until the person connects (security A3). The dock is the app's own QML; this is
//  never a script's global, and the socket's address and the key never reach a script, a page or a log (A4).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_MakeLink_h
#define hifi_MakeLink_h

#include <functional>

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>

#include <DependencyManager.h>

#include <MakeBlender.h>
#include <MakeClaude.h>
#include <MakeLinkServer.h>

class ScriptEngine;
using ScriptEnginePointer = QSharedPointer<ScriptEngine>;

/// The object the building tools' engine gets as MakeTools (ScriptEngine::registerAppObject): it brings that engine
/// the checked calls and takes back the answers.
class MakeToolsBridge : public QObject {
    Q_OBJECT
public:
    using Answer = std::function<void(bool ok, const QJsonObject& result)>;
    explicit MakeToolsBridge(QObject* parent = nullptr) : QObject(parent) {}

    /// A call for the engine; `answer` comes once, on this object's thread.
    void send(const QString& tool, const QJsonObject& inputs, int request, Answer answer);
    /// Every waiting call answered with `why` (the engine stopped).
    void dropAll(const QString& why);
    bool isReady() const { return _ready; }

    /// The engine's answer to call `id`: the result, or {code, message}, as JSON.
    Q_INVOKABLE void done(int id, bool ok, const QString& json);
    /// The engine has connected to command and takes calls.
    Q_INVOKABLE void ready();
    /// Whether this person may change (and so read) a thing's scripts: editor-ui's one rule, the entity server's
    /// (EntityScriptRights), so Make and the Code panel agree.
    Q_INVOKABLE bool mayChangeScriptsOf(bool locked) const;

signals:
    void command(int id, const QString& tool, const QString& inputs, int request);

private:
    struct Waiting {
        QString tool;
        QString inputs;
        int request;
    };
    QHash<int, Answer> _answers;
    QHash<int, Waiting> _queued;
    int _next { 1 };
    bool _ready { false };
};

class MakeLink : public QObject, public Dependency {
    Q_OBJECT
    SINGLETON_DEPENDENCY
public:
    MakeLink();
    ~MakeLink() override;

    /// Limit: the key route's run has spent its cap (limitUsd()); the person's Connect goes on with a fresh run.
    enum class State { Off, Listening, Connected, Failed, Limit };
    State state() const { return _state; }
    QString stateName() const;
    /// Why it's Failed, or what's missing (Blender), or the Limit line, in plain words.
    QString reason() const { return _reason; }
    /// Whether Make offers its Blender tools (make_with_blender, upload_model): Blender found AND its sealed box works
    /// here. Decided at each Connect, and the same answer while connected, so the dock's words and the tool list
    /// agree; while off, the answer Connect would give now. False: the tools aren't offered and a call is refused.
    bool blenderOffered() const;
    /// Why not, in plain words (empty when offered).
    QString blenderWhy() const;
    /// The person's own choice of Blender (the dock's "Choose Blender…"): `path` the app (on a Mac the .app or the
    /// program inside it; on Windows blender.exe or its folder). False with a plain `why` when Make can't use it
    /// ("That's Blender 4.1; Make needs 5.2 or newer."); true when it's remembered in Settings, the found check is
    /// re-run and, while Make is on, the Blender tools are decided again (on Windows through the box's self-test).
    bool useBlender(const QString& path, QString& why);
    /// Whether the person may choose a Blender here: the Mac and Windows, whose boxes are in; not Linux yet.
    bool canChooseBlender() const;
    /// The key route's cap for one Connect, in dollars (Claude Code's --max-budget-usd holds for its whole run).
    double limitUsd() const { return claudeSettings().maxBudgetUsd; }

    /// The Make folder: the saved scripts, models, pictures and Blender's jobs, and Claude Code's project folder.
    QString folder() const { return _folder; }
    /// Claude Code's project folder, where it runs: only the two files below, rewritten at every Connect.
    QString projectFolder() const;
    /// What Connect writes there, for the dock to show first (security A5): the MCP line and the settings.
    /// The app's own darkride-mcp beside its program, and Make's private copy of it in the Make folder, the one the
    /// config names (writeFolder makes the copy at every Connect)
    QString appHelper() const;
    QString helperCopy() const;
    QString mcpConfigText() const;
    QString settingsText() const;
    /// The line the builder pastes into Terminal (the Claude-plan route; PowerShell on Windows): Claude Code in its
    /// project folder with only Darkride's MCP server, Make's settings and no built-in tools. With `chat`, the same
    /// line with Claude Code's development-channels flag for Make's own server, so the person types in Make's panel
    /// and their words reach that Claude Code as channel events (send()); without it, the plain line: they type in
    /// Terminal and the panel shows the work.
    QString startLine(bool chat = false) const;
    /// Which of the two lines the connected Claude Code was started with, read from its process when it connected
    /// (MakePeer, security C9): "on" (the chat line), "off" (the plain line, or nothing connected, or the app's own
    /// Claude Code of the key route), "unknown" (a tree the app couldn't read: allowed). A line that isn't Make's is
    /// refused at once (reason() says so while Listening).
    QString channelFlag() const { return _channelFlag; }
    /// The connected Claude Code's version, as it said at initialize ("2.1.319"); empty before.
    QString claudeVersion() const { return _server.clientVersion(); }
    /// Channel events sent since this Connect (0 after Disconnect), for the dock's checks.
    int channelEventCount() const { return _server.channelEventCount(); }
    /// The key route's Claude Code settings: its folder, MCP line and settings file.
    MakeClaude::Settings claudeSettings() const;

    /// The person's Connect (after the dock's approve-once): writes the Make folder, starts the building tools'
    /// engine and listens. False with reason() when it can't. From Limit: the folder written fresh, listening again,
    /// so startClaude() starts a fresh run.
    bool connectLink();
    /// The person's Disconnect, and quitting: closes Claude Code's connection and the socket, stops a running make and
    /// the tools' engine.
    void disconnectLink();
    /// The person's Stop: every running call answered at once, a running make ended, new calls refused until the
    /// person's next request.
    void stop();
    /// A new request from the person (each message in the dock, or Claude's next call after a pause in the Terminal
    /// route): Stop is over, and Undo can take back this request alone.
    int newRequest();
    int currentRequest() const { return _request; }
    /// Make's Undo: every change of request `request`, newest first.
    void undoRequest(int request);
    /// The person's answer to ask `id` (from real input only: the dock checks that, security 21).
    void answer(int id, bool allow);
    /// The model Claude is (claude-opus-5-5), when the route knows it; null in made_by otherwise.
    void setModel(const QString& model) { _model = model; }

    /// The key route: Claude Code started by the app, with the kept key, in the project folder (after connectLink).
    /// False with `why` when there's no Claude Code or no key. Its words and its end come through claude()'s signals.
    bool startClaude(QString& why);
    MakeClaude* claude() { return &_claude; }
    /// The dock's Send (its own text box, real input only; security C2): a new request, the person's words and the
    /// pictures they dropped, by the ids of the dock's clean copies (<Make folder>/pictures/<id>.png or .jpg; never a
    /// path or bytes from the dock). On the key route they go to the Claude Code the app runs; on the plan route to
    /// the Claude Code in Terminal as a channel event (its meta: msg_id = the request's number, pictures = the
    /// numbers Make gives the pictures this Connect, which the picture tool's `reference` reads back). The request's
    /// number, or 0 when no Claude Code can take it (none running, none connected, or one started with the plain line).
    int send(const QString& text, const QStringList& pictureIds);

signals:
    void stateChanged();
    /// Claude Code's version is known: its hello arrives after the connection (and after stateChanged for it), so the
    /// dock's "Update Claude Code" line re-reads claudeVersion() on this.
    void claudeVersionChanged();
    /// What Claude is doing now, in plain words ("adding “Awning”").
    void activity(int request, const QString& line);
    /// The person must allow this first (security C10): `what` in plain words, `exactly` what it would do.
    void askPerson(int id, const QString& what, const QString& exactly);
    /// Something made, for the dock's cards: kind "model", "script" or "thing", with its name and details.
    void made(int request, const QVariantMap& card);
    /// Claude's reply tool: plain words for the dock.
    void reply(int request, const QString& text);
    /// The key route: what a request has cost so far, in dollars (Claude Code's running total less what it was when the
    /// request began), at the end of each of its turns.
    void requestCost(int request, double usd);

private:
    void setState(State state, const QString& reason = QString());
    bool writeFolder(QString& error);
    QString socketFolder() const;
    void startToolsEngine();
    void stopToolsEngine();
    void setHandlers();
    /// A Claude Code connected: its start line judged (MakePeer); not Make's = refused, else channelFlag() set.
    void judgeConnection();
    /// Decides blenderOffered() for this Connect and offers or withdraws the Blender tools by it.
    void offerBlender();
    void toEngine(const QString& tool);
    void forward(const QString& tool, const QJsonObject& inputs, const MakeLinkServer::Reply& reply);
    void longMaterialToFile(const QJsonObject& inputs, const std::function<void(const QJsonObject&)>& then);
    void gate(const QString& tool, const QJsonObject& inputs, const std::function<void(bool, const QString&)>& proceed);
    QString ask(const QString& what, const QString& exactly, const std::function<void(bool)>& decided);
    QJsonObject madeBy() const;
    void saveScript(const QJsonObject& inputs, const MakeLinkServer::Reply& reply);
    void uploadModel(const QJsonObject& inputs, const MakeLinkServer::Reply& reply);
    void upload(const QByteArray& data, const QString& path,
                const std::function<void(const QString& url, const QString& error)>& done);
    void makeWithBlender(const QJsonObject& inputs, const MakeLinkServer::Reply& reply);
    void picture(const QJsonObject& inputs, const MakeLinkServer::Reply& reply);
    QString activityLine(const QString& tool, const QJsonObject& inputs) const;
    void keepLogLine(const QString& line, const QString& engineName, bool server);

    MakeLinkServer _server;
    MakeBlender* _blender { nullptr };
    MakeClaude _claude;
    int _claudeRequest { 0 };
    double _claudeTotal { 0.0 };        // Claude Code's running total (total_cost_usd)
    double _costAtRequestStart { 0.0 };
    MakeToolsBridge _bridge;
    ScriptEnginePointer _engine;
    int _world { 0 };

    State _state { State::Off };
    QString _reason;
    bool _blenderOffered { false };
    QString _blenderWhy;
    QString _folder;
    QString _pipeName;  // Windows: this Connect's pipe (MakeLinkServer::newPipeName)
    QString _model;
    QString _channelFlag { "off" };
    QHash<int, QString> _pictureNumbers;   // the plan route: Make's number for a picture -> the dock's id, this Connect
    int _nextPictureNumber { 1 };

    int _request { 0 };
    QElapsedTimer _sinceCall;
    QHash<int, std::function<void(bool)>> _asks;
    int _nextAsk { 1 };
    int _removesThisRequest { 0 };
    int _askedOutsideFor { 0 };
    QHash<QString, QString> _names;  // id -> name, for the activity lines

    QStringList _clientLog;
    QStringList _serverLog;
    bool _serverLogOn { false };
};

#endif // hifi_MakeLink_h
