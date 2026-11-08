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
#include <QtCore/QRegularExpression>

#include "MakeKey.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#endif

Q_DECLARE_LOGGING_CATEGORY(makelink)

namespace {
const int MAX_LINE_BYTES = 8 * 1024 * 1024;
const int MAX_ERROR_BYTES = 16 * 1024;
}

QString MakeClaude::redact(const QString& text) {
    static const QRegularExpression KEY("sk-ant-[A-Za-z0-9_-]{10,}");
    QString hidden = text;
    return hidden.replace(KEY, "sk-ant-(hidden)");
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
    // --bare: the key given here is the only sign-in it uses (never a login from the keychain), and no hooks, CLAUDE.md
    // or memory from anywhere; --settings and --mcp-config still apply (scripting's route kit, 11-06)
    QStringList arguments {
        "-p", "--bare", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
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

QString MakeClaude::pathFor(const QString& program) {
    const QString folder = QFileInfo(program).absolutePath();
#ifdef Q_OS_WIN
    // the program's folder, then the system's own tool folders (System32, Windows, System32\Wbem): the same rule as the
    // fixed list on Unix; never the person's PATH (security, 11-08)
    QStringList folders { QDir::toNativeSeparators(folder) };
    wchar_t buffer[MAX_PATH];
    if (GetSystemDirectoryW(buffer, MAX_PATH) > 0) {
        const QString system = QString::fromWCharArray(buffer);
        folders << system;
        if (GetWindowsDirectoryW(buffer, MAX_PATH) > 0) {
            folders << QString::fromWCharArray(buffer);
        }
        folders << system + "\\Wbem";
    }
    return folders.join(';');
#else
    return folder + ":/usr/bin:/bin:/usr/sbin:/sbin";
#endif
}

bool MakeClaude::start(const Settings& settings, QString& why) {
    QByteArray key = MakeKey::readForChild();
    if (key.isEmpty()) {
        why = "There's no API key kept for Make.";
        return false;
    }
    const bool started = startWithKey(settings, key, why);
    // our copy wiped once the child's environment is built (Qt's own copies there can't be; security, 11-07)
    key.fill('\0');
    return started;
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
    environment.insert("PATH", pathFor(program));
    environment.insert("HOME", QDir::homePath());
#ifdef Q_OS_WIN
    // what a Windows program needs to run at all, each from the system's own answer, none from this app's environment
    wchar_t buffer[MAX_PATH];
    const QString windows = GetWindowsDirectoryW(buffer, MAX_PATH) > 0 ? QString::fromWCharArray(buffer) : QString("C:\\Windows");
    const QString system = GetSystemDirectoryW(buffer, MAX_PATH) > 0 ? QString::fromWCharArray(buffer) : windows + "\\System32";
    environment.insert("SYSTEMROOT", windows);
    environment.insert("WINDIR", windows);
    environment.insert("SYSTEMDRIVE", windows.left(2));
    environment.insert("COMSPEC", system + "\\cmd.exe");
    environment.insert("PATHEXT", ".COM;.EXE;.BAT;.CMD");
    environment.insert("TEMP", QDir::toNativeSeparators(QDir::tempPath()));
    environment.insert("TMP", QDir::toNativeSeparators(QDir::tempPath()));
    const QString home = QDir::toNativeSeparators(QDir::homePath());
    environment.insert("USERPROFILE", home);
    environment.insert("HOMEDRIVE", home.left(2));
    environment.insert("HOMEPATH", home.mid(2));
    auto known = [](const KNOWNFOLDERID& id) {
        PWSTR path = nullptr;
        QString result;
        if (SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path) == S_OK) {
            result = QString::fromWCharArray(path);
        }
        CoTaskMemFree(path);
        return result;
    };
    environment.insert("APPDATA", known(FOLDERID_RoamingAppData));
    environment.insert("LOCALAPPDATA", known(FOLDERID_LocalAppData));
#endif
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
    _started = false;
    connect(_process, &QProcess::readyReadStandardOutput, this, &MakeClaude::readOutput);
    connect(_process, &QProcess::readyReadStandardError, this, [this] {
        if (_process) {
            _errors.append(_process->readAllStandardError());
            _errors = _errors.right(MAX_ERROR_BYTES);
        }
    });
    connect(_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this](int code, QProcess::ExitStatus) {
        const bool asked = _ending;
        // hidden before it's cut, so no part of a key is left at the cut's edge
        const QString errors = redact(QString::fromUtf8(_errors)).trimmed();
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
        // its init comes again before every request: the first is the start, so a request's cost counts from its own
        // start, not from zero
        if (!_started) {
            _started = true;
            emit started(event.value("model").toString());
        }
    } else if (type == "stream_event") {
        const QJsonObject delta = event.value("event").toObject().value("delta").toObject();
        if (delta.value("type").toString() == "text_delta") {
            emit textDelta(delta.value("text").toString());
        }
    } else if (type == "result") {
        emit turnEnded(event.value("total_cost_usd").toDouble(), event.value("is_error").toBool(),
                       redact(event.value("result").toString()).left(4000), event.value("subtype").toString().left(64));
    }
}
