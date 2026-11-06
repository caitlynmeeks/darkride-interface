//
//  MakeClaude.cpp
//  libraries/make-link/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakeClaude.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLoggingCategory>
#include <QtCore/QProcess>

#include "MakeKey.h"

Q_DECLARE_LOGGING_CATEGORY(makelink)

namespace {
const int MAX_LINE_BYTES = 8 * 1024 * 1024;
const int MAX_ERROR_BYTES = 16 * 1024;
}

QString MakeClaude::findClaude() {
    const QString home = QDir::homePath();
    for (const QString& candidate : { home + "/.local/bin/claude", home + "/.claude/local/claude",
                                      QString("/opt/homebrew/bin/claude"), QString("/usr/local/bin/claude") }) {
        if (QFileInfo(candidate).isExecutable()) {
            return candidate;
        }
    }
    return QString();
}

QStringList MakeClaude::arguments(const Settings& settings) {
    QStringList arguments {
        "-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
        "--include-partial-messages",
        "--strict-mcp-config", "--mcp-config", settings.mcpConfig,
        "--setting-sources", "project", "--settings", settings.settingsFile,
        "--tools", "",
        "--max-turns", QString::number(settings.maxTurns),
        "--max-budget-usd", QString::number(settings.maxBudgetUsd, 'f', 2),
        "--no-session-persistence",
    };
    if (!settings.model.isEmpty()) {
        arguments << "--model" << settings.model;
    }
    return arguments;
}

MakeClaude::MakeClaude(QObject* parent) : QObject(parent) {
}

MakeClaude::~MakeClaude() {
    end();
}

bool MakeClaude::start(const Settings& settings, QString& why) {
    const QByteArray key = MakeKey::readForChild();
    if (key.isEmpty()) {
        why = "There's no API key kept for Make.";
        return false;
    }
    return startWithKey(settings, key, why);
}

bool MakeClaude::startWithKey(const Settings& settings, const QByteArray& key, QString& why) {
    if (isRunning()) {
        return true;
    }
    const QString program = settings.program.isEmpty() ? findClaude() : settings.program;
    if (program.isEmpty()) {
        why = "Claude Code isn't installed (claude.com/claude-code).";
        return false;
    }
    // built from nothing: nothing of the app's own environment, and the key only here (never on the command line)
    QProcessEnvironment environment;
    environment.insert("PATH", QFileInfo(program).absolutePath() + ":/usr/bin:/bin:/usr/sbin:/sbin");
    environment.insert("HOME", QDir::homePath());
    environment.insert("LANG", "en_US.UTF-8");
    environment.insert("TMPDIR", QDir::tempPath());
    environment.insert("ANTHROPIC_API_KEY", QString::fromUtf8(key));
    // a Blender make can take most of two minutes: its answer must not move to the background, where -p loses it
    environment.insert("CLAUDE_CODE_MCP_AUTO_BACKGROUND_MS", "0");
    environment.insert("DISABLE_AUTOUPDATER", "1");

    _process = new QProcess(this);
    _process->setProcessEnvironment(environment);
    _process->setWorkingDirectory(settings.folder);
    _buffer.clear();
    _errors.clear();
    _ending = false;
    connect(_process, &QProcess::readyReadStandardOutput, this, &MakeClaude::readOutput);
    connect(_process, &QProcess::readyReadStandardError, this, [this] {
        if (_process) {
            _errors.append(_process->readAllStandardError());
            _errors = _errors.right(MAX_ERROR_BYTES);
        }
    });
    connect(_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this](int code, QProcess::ExitStatus) {
        const bool asked = _ending;
        const QString errors = QString::fromUtf8(_errors).trimmed();
        if (_process) {
            _process->deleteLater();
            _process = nullptr;
        }
        qCInfo(makelink) << "Claude Code for Make ended, exit" << code;
        emit ended(asked ? QString() : (errors.isEmpty() ? QString("Claude Code stopped (%1)").arg(code)
                                                         : "Claude Code stopped: " + errors.right(400)));
    });
    _process->start(program, arguments(settings));
    if (!_process->waitForStarted(5000)) {
        why = "Claude Code didn't start: " + _process->errorString();
        _process->deleteLater();
        _process = nullptr;
        return false;
    }
    qCInfo(makelink) << "Claude Code for Make started, pid" << _process->processId();
    return true;
}

bool MakeClaude::isRunning() const {
    return _process && _process->state() != QProcess::NotRunning;
}

qint64 MakeClaude::pid() const {
    return _process ? _process->processId() : 0;
}

void MakeClaude::send(const QString& text, const QVector<Image>& images) {
    if (!isRunning()) {
        return;
    }
    QJsonArray content;
    for (const Image& image : images) {
        content.append(QJsonObject { { "type", "image" }, { "source", QJsonObject {
            { "type", "base64" }, { "media_type", image.mimeType }, { "data", QString::fromLatin1(image.data.toBase64()) } } } });
    }
    content.append(QJsonObject { { "type", "text" }, { "text", text } });
    const QJsonObject message { { "type", "user" },
                                { "message", QJsonObject { { "role", "user" }, { "content", content } } },
                                { "parent_tool_use_id", QJsonValue::Null } };
    QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact);
    line.append('\n');
    _process->write(line);
}

void MakeClaude::end() {
    if (!_process) {
        return;
    }
    _ending = true;
    _process->closeWriteChannel();
    if (!_process->waitForFinished(1500)) {
        _process->kill();  // its own pid
        _process->waitForFinished(2000);
    }
}

void MakeClaude::readOutput() {
    if (!_process) {
        return;
    }
    _buffer.append(_process->readAllStandardOutput());
    int end;
    while ((end = _buffer.indexOf('\n')) >= 0) {
        const QByteArray line = _buffer.left(end);
        _buffer.remove(0, end + 1);
        handle(line);
    }
    if (_buffer.size() > MAX_LINE_BYTES) {
        _buffer.clear();
    }
}

void MakeClaude::handle(const QByteArray& line) {
    const QJsonObject event = QJsonDocument::fromJson(line).object();
    const QString type = event.value("type").toString();
    if (type == "system" && event.value("subtype").toString() == "init") {
        emit started(event.value("model").toString());
    } else if (type == "stream_event") {
        const QJsonObject delta = event.value("event").toObject().value("delta").toObject();
        if (delta.value("type").toString() == "text_delta") {
            emit textDelta(delta.value("text").toString());
        }
    } else if (type == "result") {
        emit turnEnded(event.value("total_cost_usd").toDouble(), event.value("is_error").toBool(),
                       event.value("result").toString().left(4000));
    }
}
