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

    enum class State { Off, Listening, Connected, Failed };
    State state() const { return _state; }
    QString stateName() const;
    /// Why it's Failed, or what's missing (Blender), in plain words.
    QString reason() const { return _reason; }

    /// The Make folder: the project folder Claude Code runs in.
    QString folder() const { return _folder; }
    /// What Connect writes there, for the dock to show first (security A5): the MCP line and the settings.
    QString mcpConfigText() const;
    QString settingsText() const;
    /// The line the builder pastes into Terminal (the Terminal route): Claude Code in the Make folder with only
    /// Darkride's MCP server and no built-in tools.
    QString startLine() const;

    /// The person's Connect (after the dock's approve-once): writes the Make folder, starts the building tools'
    /// engine and listens. False with reason() when it can't.
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

signals:
    void stateChanged();
    /// What Claude is doing now, in plain words ("adding “Awning”").
    void activity(int request, const QString& line);
    /// The person must allow this first (security C10): `what` in plain words, `exactly` what it would do.
    void askPerson(int id, const QString& what, const QString& exactly);
    /// Something made, for the dock's cards: kind "model", "script" or "thing", with its name and details.
    void made(int request, const QVariantMap& card);
    /// Claude's reply tool: plain words for the dock.
    void reply(int request, const QString& text);

private:
    void setState(State state, const QString& reason = QString());
    bool writeFolder(QString& error);
    QString socketFolder() const;
    void startToolsEngine();
    void stopToolsEngine();
    void setHandlers();
    void toEngine(const QString& tool);
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
    MakeToolsBridge _bridge;
    ScriptEnginePointer _engine;
    int _world { 0 };

    State _state { State::Off };
    QString _reason;
    QString _folder;
    QString _model;

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
