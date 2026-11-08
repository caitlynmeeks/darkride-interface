//
//  MakeClaude.h
//  libraries/make-link/src
//
//  The route where Darkride starts the builder's own Claude Code itself, on their own API key, so they talk to it in
//  the Make dock: `claude -p` with stream-json in and out, kept running across requests. It starts in the Make folder
//  with only Darkride's MCP server (--strict-mcp-config), only the Make folder's settings (--setting-sources project
//  and --settings: Darkride's tools allowed, shell, web and files denied, bypass mode off), no built-in tools, a turn
//  and money cap, and no saved transcript (security's make-v1-claude-link.md, 6-11). Its environment is built from
//  nothing: PATH, HOME, LANG, TMPDIR and the key, which is read from the keychain at that moment and is never on its
//  command line. Ended by its own pid (Disconnect, quit); it also ends when the app does (its input closes).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_MakeClaude_h
#define hifi_MakeClaude_h

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVector>


class MakeClaude : public QObject {
    Q_OBJECT
public:
    struct Settings {
        QString folder;        // the Make folder (its working folder)
        QString mcpConfig;     // the MCP line's file (only darkride)
        QString settingsFile;  // the Make folder's settings
        int maxTurns { 60 };
        double maxBudgetUsd { 2.0 };
        QString model;         // empty: Claude Code's own default
        QString program;       // empty: the installed claude (findClaude)
    };
    struct Image {
        QString mimeType;
        QByteArray data;
    };

    /// The builder's installed Claude Code: the usual places it installs to. Empty when there is none.
    static QString findClaude();
    /// `text` with anything shaped like an Anthropic key (sk-ant-...) hidden, before it's shown or logged (security
    /// should-fix 5): an error from Claude Code could print the key it was started with.
    static QString redact(const QString& text);
    /// The arguments it is started with (for the dock to show and the tests to check): never the key.
    static QStringList arguments(const Settings& settings);

    explicit MakeClaude(QObject* parent = nullptr);
    ~MakeClaude() override;

    /// Starts it with the kept key (MakeKey). False with `why` when there's no Claude Code or no key.
    bool start(const Settings& settings, QString& why);
    /// For tests: starts it with `key` instead of the keychain's.
    bool startWithKey(const Settings& settings, const QByteArray& key, QString& why);
    bool isRunning() const;
    qint64 pid() const;
    /// One message from the person: their words and the pictures they dropped (the app's clean copies).
    void send(const QString& text, const QVector<Image>& images = {});
    /// Ends it, by its own pid, at once.
    void end();

signals:
    /// Claude Code is ready: the model it runs. Once for each run (Claude Code says init again before every request).
    void started(const QString& model);
    /// Claude's words as they come.
    void textDelta(const QString& text);
    /// A turn is over: what it cost in all so far (total_cost_usd), whether it ended in an error, its last words, and
    /// the result's kind ("success", "error_max_budget_usd" when the run's cap is reached, ...).
    void turnEnded(double totalCostUsd, bool error, const QString& result, const QString& subtype);
    /// It has ended (by itself, or by end()), with a plain reason when it wasn't asked to.
    void ended(const QString& why);

private:
    void readOutput();
    void handle(const QByteArray& line);

    QPointer<QProcess> _process;
    QByteArray _buffer;
    QByteArray _errors;
    bool _ending { false };
    bool _started { false };
};

#endif // hifi_MakeClaude_h
