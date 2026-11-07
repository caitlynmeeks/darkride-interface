//
//  MakePeer.cpp
//  libraries/make-link/src
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#include "MakePeer.h"

#include <cstring>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSet>

#if defined(Q_OS_MAC)
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
#elif defined(Q_OS_LINUX)
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <QtCore/QFile>
#endif

const QString MakePeer::CHANNEL_FLAG = "--dangerously-load-development-channels";
const QString MakePeer::CHANNEL_ENTRY = "server:darkride";
const QString MakePeer::REFUSED_WORDS = "Claude was started with a different line. Copy Make's line again.";

namespace {

// the process whose line is Claude Code's: named claude, or started with Make's MCP file (a wrapper that re-runs
// claude with the same arguments still names the file)
bool looksLikeClaude(const QString& program, const QStringList& arguments) {
    const QString name = QFileInfo(program).fileName().toLower();
    return name == "claude" || name == "claude.exe" || arguments.contains("--mcp-config");
}

#if defined(Q_OS_MAC)

bool parentOf(qint64 pid, qint64& parent) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid) };
    struct kinfo_proc info;
    size_t size = sizeof info;
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size < sizeof info) {
        return false;
    }
    parent = info.kp_eproc.e_ppid;
    return true;
}

// the process's argv, as the kernel keeps it: a count, the executable's path, padding, then the arguments
bool argumentsOf(qint64 pid, QString& program, QStringList& arguments) {
    int mibMax[2] = { CTL_KERN, KERN_ARGMAX };
    int argmax = 0;
    size_t size = sizeof argmax;
    if (sysctl(mibMax, 2, &argmax, &size, nullptr, 0) != 0 || argmax <= 0) {
        return false;
    }
    std::vector<char> buffer(static_cast<size_t>(argmax));
    int mib[3] = { CTL_KERN, KERN_PROCARGS2, static_cast<int>(pid) };
    size = buffer.size();
    if (sysctl(mib, 3, buffer.data(), &size, nullptr, 0) != 0 || size < sizeof(int)) {
        return false;
    }
    int count = 0;
    memcpy(&count, buffer.data(), sizeof count);
    if (count < 1 || count > 4096) {
        return false;
    }
    size_t at = sizeof(int);
    // the executable's path, then any number of NULs
    while (at < size && buffer[at] != '\0') {
        ++at;
    }
    while (at < size && buffer[at] == '\0') {
        ++at;
    }
    QStringList all;
    while (at < size && all.size() < count) {
        const size_t start = at;
        while (at < size && buffer[at] != '\0') {
            ++at;
        }
        all.append(QString::fromUtf8(buffer.data() + start, static_cast<int>(at - start)));
        ++at;
    }
    if (all.isEmpty()) {
        return false;
    }
    program = all.takeFirst();
    arguments = all;
    return true;
}

qint64 peerPidOf(qintptr fd) {
    pid_t pid = 0;
    socklen_t length = sizeof pid;
    if (fd < 0 || getsockopt(static_cast<int>(fd), SOL_LOCAL, LOCAL_PEERPID, &pid, &length) != 0) {
        return 0;
    }
    return pid;
}

#elif defined(Q_OS_LINUX)

bool parentOf(qint64 pid, qint64& parent) {
    QFile stat(QString("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray text = stat.readAll();
    // "pid (comm) state ppid ...": the command may hold spaces and parentheses, so the fields start after the last ')'
    const int close = text.lastIndexOf(')');
    if (close < 0) {
        return false;
    }
    const QList<QByteArray> fields = text.mid(close + 1).simplified().split(' ');
    if (fields.size() < 2) {
        return false;
    }
    bool ok = false;
    parent = fields[1].toLongLong(&ok);
    return ok;
}

bool argumentsOf(qint64 pid, QString& program, QStringList& arguments) {
    QFile cmdline(QString("/proc/%1/cmdline").arg(pid));
    if (!cmdline.open(QIODevice::ReadOnly)) {
        return false;
    }
    QByteArray text = cmdline.readAll();
    if (text.endsWith('\0')) {
        text.chop(1);
    }
    if (text.isEmpty()) {
        return false;
    }
    QStringList all;
    for (const QByteArray& part : text.split('\0')) {
        all.append(QString::fromUtf8(part));
    }
    program = all.takeFirst();
    arguments = all;
    return true;
}

qint64 peerPidOf(qintptr fd) {
    struct ucred credentials;
    socklen_t length = sizeof credentials;
    if (fd < 0 || getsockopt(static_cast<int>(fd), SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
        return 0;
    }
    return credentials.pid;
}

#else

bool parentOf(qint64, qint64&) { return false; }
bool argumentsOf(qint64, QString&, QStringList&) { return false; }
qint64 peerPidOf(qintptr) { return 0; }

#endif

// the same file, however the path was spelt
bool samePath(const QString& given, const QString& ours) {
    if (given.isEmpty() || ours.isEmpty()) {
        return false;
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(given)) == QDir::cleanPath(QDir::fromNativeSeparators(ours));
}

}  // namespace

MakePeer::Line MakePeer::read(qintptr socketDescriptor) {
    const qint64 pid = peerPidOf(socketDescriptor);
    if (pid <= 0) {
        return Line();
    }
    return readFrom(pid);
}

MakePeer::Line MakePeer::readFrom(qint64 pid, int maxHops) {
    Line line;
    line.peerPid = pid;
    qint64 at = pid;
    for (int hop = 1; hop <= maxHops; ++hop) {
        qint64 parent = 0;
        if (!parentOf(at, parent) || parent <= 1 || parent == at) {
            return line;   // the top of the tree, or a process the app may not look at: unknown
        }
        QString program;
        QStringList arguments;
        if (!argumentsOf(parent, program, arguments)) {
            return line;
        }
        line.hops = hop;
        if (looksLikeClaude(program, arguments)) {
            line.claudePid = parent;
            line.program = program;
            line.arguments = arguments;
            line.readable = true;
            return line;
        }
        at = parent;
    }
    return line;
}

MakePeer::Judgement MakePeer::judge(const Line& line, const QString& mcpConfig, const QString& settingsFile) {
    if (!line.readable) {
        return Judgement { Verdict::Unknown, "unreadable" };
    }
    return judgeArguments(line.arguments, mcpConfig, settingsFile);
}

MakePeer::Judgement MakePeer::judgeArguments(const QStringList& arguments, const QString& mcpConfig,
                                             const QString& settingsFile) {
    // the arguments read as a set of flags, each with the value that follows it (or is joined with '='): the order
    // doesn't matter, a flag the app doesn't know is allowed, and only what changes what Claude may do is judged
    QStringList channelEntries;
    QStringList mcpConfigs, settings, toolsValues;
    bool restricted = false, strict = false, toolsGiven = false, permissionFlag = false, print = false;
    bool channelsFlag = false, widensFlag = false, promptFlag = false;
    const QSet<QString> takesValue { "--mcp-config", "--settings", "--tools", CHANNEL_FLAG, "--channels",
                                     "--permission-mode", "--model", "--allowedTools", "--allowed-tools",
                                     "--disallowedTools", "--disallowed-tools", "--setting-sources", "--add-dir",
                                     "--agent", "--max-turns", "--max-budget-usd", "--fallback-model", "--session-id",
                                     "--resume", "-r", "--system-prompt", "--append-system-prompt",
                                     "--permission-prompt-tool", "--input-format", "--output-format", "--effort",
                                     "--plugin-dir", "--agents", "--system-prompt-file", "--append-system-prompt-file",
                                     "--name", "-n", "--worktree", "-w", "--betas", "--mcp-debug" };
    for (int i = 0; i < arguments.size(); ++i) {
        QString flag = arguments[i];
        QString value;
        bool hasValue = false;
        const int equals = flag.startsWith("--") ? flag.indexOf('=') : -1;
        if (equals > 0) {
            value = flag.mid(equals + 1);
            flag = flag.left(equals);
            hasValue = true;
        } else if (takesValue.contains(flag) && i + 1 < arguments.size()) {
            value = arguments[++i];
            hasValue = true;
        }
        if (flag == "--restricted") {
            restricted = true;
        } else if (flag == "--strict-mcp-config") {
            strict = true;
        } else if (flag == "--mcp-config") {
            mcpConfigs.append(value);
        } else if (flag == "--settings") {
            settings.append(value);
        } else if (flag == "--tools") {
            toolsGiven = true;
            toolsValues.append(hasValue ? value.trimmed() : QString("(none)"));
        } else if (flag == CHANNEL_FLAG || flag == "--channels") {
            // one flag may carry several entries, "server:darkride plugin:x@y" or comma-joined (security S2): every
            // following server: or plugin: argument up to the next flag is one
            QStringList entries = value.split(',', Qt::SkipEmptyParts);
            while (i + 1 < arguments.size() && !arguments[i + 1].startsWith('-') &&
                   (arguments[i + 1].startsWith("server:") || arguments[i + 1].startsWith("plugin:"))) {
                entries.append(arguments[++i]);
            }
            if (flag == "--channels") {
                channelsFlag = true;
            } else {
                for (const QString& entry : entries) {
                    channelEntries.append(entry.trimmed());
                }
            }
        } else if (flag == "--dangerously-skip-permissions" || flag == "--allow-dangerously-skip-permissions" ||
                   flag == "--allowedTools" || flag == "--allowed-tools" || flag == "--permission-prompt-tool" ||
                   (flag == "--permission-mode" && value != "dontAsk" && value != "default")) {
            permissionFlag = true;
        } else if (flag == "--setting-sources" || flag == "--plugin-dir" || flag == "--agents" || flag == "--add-dir" ||
                   flag == "--chrome" || flag == "--ide") {
            widensFlag = true;   // other settings files, a plugin (hooks, servers), agents with tools, more files, more tools
        } else if (flag == "--system-prompt" || flag == "--append-system-prompt" || flag == "--system-prompt-file" ||
                   flag == "--append-system-prompt-file") {
            promptFlag = true;   // what Claude is told: Make's instructions are fixed (C4)
        } else if (flag == "-p" || flag == "--print") {
            print = true;
        }
    }
    if (channelsFlag) {
        return Judgement { Verdict::Mismatch, "a --channels entry" };
    }
    if (!channelEntries.isEmpty() && (channelEntries.size() != 1 || channelEntries.first() != CHANNEL_ENTRY)) {
        return Judgement { Verdict::Mismatch, "a second channel entry" };
    }
    if (!restricted) {
        return Judgement { Verdict::Mismatch, "no --restricted" };
    }
    if (!strict) {
        return Judgement { Verdict::Mismatch, "no --strict-mcp-config" };
    }
    if (mcpConfigs.size() != 1 || !samePath(mcpConfigs.first(), mcpConfig)) {
        return Judgement { Verdict::Mismatch, "another --mcp-config" };
    }
    if (settings.size() != 1 || !samePath(settings.first(), settingsFile)) {
        return Judgement { Verdict::Mismatch, "another --settings" };
    }
    if (!toolsGiven || toolsValues.size() != 1 || !toolsValues.first().isEmpty()) {
        return Judgement { Verdict::Mismatch, "--tools not empty" };
    }
    if (permissionFlag) {
        return Judgement { Verdict::Mismatch, "a permission flag" };
    }
    if (widensFlag) {
        return Judgement { Verdict::Mismatch, "a flag that widens what Claude may do" };
    }
    if (promptFlag) {
        return Judgement { Verdict::Mismatch, "a system prompt flag" };
    }
    // -p ignores the development-channels flag: no channel then, whatever the line says
    if (channelEntries.isEmpty() || print) {
        return Judgement { Verdict::PlainLine, QString() };
    }
    return Judgement { Verdict::ChatLine, QString() };
}
