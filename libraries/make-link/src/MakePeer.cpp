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

namespace {
    // How Claude Code's own parser (commander, read from 2.1.319) takes each kind of option: a flag takes nothing; a
    // value option takes the next argument whatever it is, even one that starts with '-'; an optional value takes the
    // next argument only when it doesn't start with '-'; a list takes the next argument whatever it is and then every
    // following one that doesn't start with '-'. The joined --option=value form gives a value option exactly one value
    // and keeps no list open: a word after it is prompt text.
    enum class Takes { Nothing, Value, OptionalValue, Values };
    struct AllowedOption { const char* name; Takes takes; };
    // The options a line may carry and still be Make's (security's MUST, 21:22 11-07): Make's own, and a few that
    // change nothing Claude may do (which model, a session name or id, resuming, debug output, no Chrome). Every other
    // argument that starts with '-' is refused, named: Claude Code 2.1.319 has 130-odd main options, many hidden,
    // and an unknown one could carry a limit away as its value or add a plugin, a settings tier or a prompt.
    const AllowedOption ALLOWED_OPTIONS[] = {
        // Make's own line
        { "--restricted", Takes::Nothing },   { "--strict-mcp-config", Takes::Nothing },
        { "--mcp-config", Takes::Values },    { "--settings", Takes::Value },
        { "--tools", Takes::Values },         { "-p", Takes::Nothing },  { "--print", Takes::Nothing },
        // narrows only
        { "--disallowedTools", Takes::Values }, { "--disallowed-tools", Takes::Values },
        // which model, how hard
        { "--model", Takes::Value },  { "--effort", Takes::Value },  { "--fallback-model", Takes::Value },
        // the session: its name, id, resuming
        { "-n", Takes::Value },  { "--name", Takes::Value },  { "--session-id", Takes::Value },
        { "-c", Takes::Nothing },  { "--continue", Takes::Nothing },
        { "-r", Takes::OptionalValue },  { "--resume", Takes::OptionalValue },  { "--fork-session", Takes::Nothing },
        // debug output, no Chrome
        { "--verbose", Takes::Nothing },  { "-d", Takes::OptionalValue },  { "--debug", Takes::OptionalValue },
        { "--no-chrome", Takes::Nothing },
        // not --debug-file: it writes Claude Code's log, tool results and so the world's text in it, to any path the
        // line names (security 21:3x 11-07); -d writes to Claude Code's own folder
    };
    // Options Claude Code has that are refused with the rule they break, so the log says which (everything else says
    // "an option Make's line doesn't allow")
    struct RefusedOption { const char* name; const char* why; };
    const char* const PERMISSION = "a permission flag";
    const char* const WIDENS = "a flag that widens what Claude may do";
    const char* const PROMPT = "a system prompt flag";
    const RefusedOption REFUSED_OPTIONS[] = {
        { "--dangerously-skip-permissions", PERMISSION }, { "--allow-dangerously-skip-permissions", PERMISSION },
        { "--allowedTools", PERMISSION }, { "--allowed-tools", PERMISSION }, { "--permission-mode", PERMISSION },
        { "--inherit-permission-mode", PERMISSION }, { "--permission-prompt-tool", PERMISSION },
        { "--permission-prompts", PERMISSION }, { "--enable-auto-mode", PERMISSION },
        { "--setting-sources", WIDENS }, { "--plugin-dir", WIDENS }, { "--plugin-dir-no-mcp", WIDENS },
        { "--plugin-url", WIDENS }, { "--agents", WIDENS }, { "--add-dir", WIDENS }, { "--file", WIDENS },
        { "--project-config-root", WIDENS }, { "--managed-settings", WIDENS }, { "--client-data-url", WIDENS },
        { "--sdk-url", WIDENS }, { "--chrome", WIDENS }, { "--ide", WIDENS }, { "--desktop", WIDENS },
        { "-w", WIDENS }, { "--worktree", WIDENS },
        { "--system-prompt", PROMPT }, { "--append-system-prompt", PROMPT }, { "--system-prompt-file", PROMPT },
        { "--append-system-prompt-file", PROMPT }, { "--append-subagent-system-prompt", PROMPT },
        { "--append-subagent-system-prompt-file", PROMPT }, { "--plan-mode-instructions", PROMPT },
        { "--system-prompt-snapshot", PROMPT }, { "--exclude-dynamic-system-prompt-sections", PROMPT },
        { "--agent", PROMPT }, { "--prefill", PROMPT }, { "--prefill-b64", PROMPT },
        { "--channels", "a --channels entry" },
    };
    // Claude Code 2.1.319's own commands, with their aliases and the hidden ones (the root's .command list, read from
    // the binary): when the first operand is one of them Claude Code runs that command, not the session the line was
    // judged as, so it isn't prompt text (security 21:3x 11-07)
    const char* const SUBCOMMANDS[] = {
        "agents", "auth", "auto-mode", "doctor", "gateway", "help", "import", "import-conversations", "install", "mcp",
        "plugin", "plugins", "remote-control", "rc", "sandbox", "setup-token", "ultrareview", "update", "upgrade",
        "design-login", "drop-worktree-registrations", "edit-chrome-settings", "edit-hook", "edit-memory-settings",
        "edit-permission-rules", "edit-sandbox-settings", "edit-skill-overrides", "project", "purge",
    };
}

MakePeer::Judgement MakePeer::judgeArguments(const QStringList& arguments, const QString& mcpConfig,
                                             const QString& settingsFile) {
    // the arguments read as Claude Code reads them (the tables above): the order doesn't matter, an option the
    // allow-list doesn't have refuses the line, and Make's own options are then judged by their values
    using V = Verdict;
    QStringList channelEntries;
    QStringList mcpConfigs, settings, toolsValues;
    bool restricted = false, strict = false, toolsGiven = false, print = false, operandSeen = false;
    const auto looksLikeOption = [](const QString& argument) { return argument.size() > 1 && argument.startsWith('-'); };
    for (int i = 0; i < arguments.size(); ++i) {
        const QString& argument = arguments[i];
        if (!looksLikeOption(argument)) {
            // an operand: the person's own prompt text (Claude Code's [prompt]), none of the line's limits, unless the
            // first one names one of Claude Code's commands, which then runs instead of the session
            if (!operandSeen) {
                operandSeen = true;
                for (const char* command : SUBCOMMANDS) {
                    if (argument == QLatin1String(command)) {
                        return Judgement { V::Mismatch, "a subcommand: " + argument };
                    }
                }
            }
            continue;
        }
        if (argument == "--") {
            // commander stops reading options there: a limit written after it is prompt text, never applied
            return Judgement { V::Mismatch, "a -- separator" };
        }
        QString flag = argument;
        QStringList values;
        bool joined = false;
        if (flag.startsWith("--")) {
            const int equals = flag.indexOf('=');
            if (equals > 0) {
                values.append(flag.mid(equals + 1));
                flag = flag.left(equals);
                joined = true;
            }
        } else if (flag.size() > 2) {
            // -dp or -nvalue: several short options, or a short one with its value glued on; the name only, no value
            return Judgement { V::Mismatch, "a glued short option: " + flag.left(2) };
        }
        for (const RefusedOption& refused : REFUSED_OPTIONS) {
            if (flag == QLatin1String(refused.name)) {
                return Judgement { V::Mismatch, QString(refused.why) + ": " + flag };
            }
        }
        Takes takes = Takes::Nothing;
        bool allowed = false;
        if (flag == CHANNEL_FLAG) {
            takes = Takes::Values;
            allowed = true;
        } else {
            for (const AllowedOption& option : ALLOWED_OPTIONS) {
                if (flag == QLatin1String(option.name)) {
                    takes = option.takes;
                    allowed = true;
                    break;
                }
            }
        }
        if (!allowed) {
            return Judgement { V::Mismatch, "an option Make's line doesn't allow: " + flag };
        }
        if (joined) {
            if (takes == Takes::Nothing) {
                return Judgement { V::Mismatch, "a value on a flag: " + flag };
            }
            // one value, as commander gives it; a word after it is prompt text
        } else if (takes == Takes::Value || takes == Takes::Values) {
            if (i + 1 >= arguments.size()) {
                return Judgement { V::Mismatch, "no value after " + flag };   // commander stops with an error there
            }
            values.append(arguments[++i]);   // whatever it is, as commander takes it
            if (takes == Takes::Values) {
                while (i + 1 < arguments.size() && !looksLikeOption(arguments[i + 1])) {
                    values.append(arguments[++i]);
                }
            }
        } else if (takes == Takes::OptionalValue) {
            if (i + 1 < arguments.size() && !looksLikeOption(arguments[i + 1])) {
                values.append(arguments[++i]);
            }
        }
        // Make's own options, judged by their values once the line is read
        if (flag == "--restricted") {
            restricted = true;
        } else if (flag == "--strict-mcp-config") {
            strict = true;
        } else if (flag == "--mcp-config") {
            mcpConfigs.append(values);
        } else if (flag == "--settings") {
            settings.append(values);
        } else if (flag == "--tools") {
            toolsGiven = true;
            for (const QString& tool : values) {
                toolsValues.append(tool.trimmed());
            }
        } else if (flag == CHANNEL_FLAG) {
            // one flag may carry several entries, "server:darkride plugin:x@y" or comma-joined (security S2): every
            // value is one, whatever it says
            for (const QString& given : values) {
                for (const QString& entry : given.split(',', Qt::SkipEmptyParts)) {
                    channelEntries.append(entry.trimmed());
                }
            }
        } else if (flag == "-p" || flag == "--print") {
            print = true;
        }
    }
    if (!channelEntries.isEmpty() && (channelEntries.size() != 1 || channelEntries.first() != CHANNEL_ENTRY)) {
        return Judgement { V::Mismatch, "a second channel entry" };
    }
    if (!restricted) {
        return Judgement { V::Mismatch, "no --restricted" };
    }
    if (!strict) {
        return Judgement { V::Mismatch, "no --strict-mcp-config" };
    }
    if (mcpConfigs.size() != 1 || !samePath(mcpConfigs.first(), mcpConfig)) {
        return Judgement { V::Mismatch, "another --mcp-config" };
    }
    if (settings.size() != 1 || !samePath(settings.first(), settingsFile)) {
        return Judgement { V::Mismatch, "another --settings" };
    }
    if (!toolsGiven || toolsValues.size() != 1 || !toolsValues.first().isEmpty()) {
        return Judgement { V::Mismatch, "--tools not empty" };
    }
    // -p ignores the development-channels flag: no channel then, whatever the line says
    if (channelEntries.isEmpty() || print) {
        return Judgement { V::PlainLine, QString() };
    }
    return Judgement { V::ChatLine, QString() };
}
