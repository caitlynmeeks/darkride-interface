//
//  MakePeer.h
//  libraries/make-link/src
//
//  Who is at the other end of Make's link, and how their Claude Code was started (security C9, 11-07): the program
//  that connected (the darkride-mcp relay) is found from the socket, then its parents are walked, a few at most, to
//  the claude process, and that process's own arguments are read from the system (macOS: KERN_PROCARGS2; Linux:
//  /proc/<pid>/cmdline). Those arguments are compared with the lines Make itself gives the person to paste: the chat
//  line (today's line with the development-channels flag for Make's own server) or the plain line. A line that was
//  changed in a way that matters (another channel entry, no --restricted, another MCP file or settings file, tools
//  turned on, a permission flag) is refused; a tree the app can't read is allowed and noted, never refused.
//
//  Nothing here is shown to a script or a world; the log keeps counts and rule names, never the arguments themselves.
//  Windows: the client's process id is known (the pipe tells it), its line is not read yet.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_MakePeer_h
#define hifi_MakePeer_h

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtGlobal>

class MakePeer {
public:
    /// How a Claude Code was started, as read from its process.
    struct Line {
        qint64 peerPid { 0 };     // the program on the socket (the relay)
        qint64 claudePid { 0 };   // the ancestor whose arguments were read; 0 when none was found
        QString program;          // its argv[0]
        QStringList arguments;    // its argv[1..]
        int hops { 0 };           // parents walked from the relay
        bool readable { false };  // false: no claude process found within reach, or its arguments can't be read
    };

    /// The relay's process and its parent Claude Code's line, from the server side of a connected local socket.
    static Line read(qintptr socketDescriptor);
    /// The line of a given process, walking up to `maxHops` parents for the claude process.
    static Line readFrom(qint64 pid, int maxHops = MAX_HOPS);

    enum class Verdict {
        ChatLine,    // Make's line with the development-channels flag for Make's own server
        PlainLine,   // Make's line without it
        Mismatch,    // a readable line that isn't Make's in a way that matters (why names the rule, no values)
        Unknown      // not readable: allowed, counts logged
    };
    struct Judgement {
        Verdict verdict { Verdict::Unknown };
        QString why;   // the rule for the log ("no --restricted", "a second channel entry", ...)
    };
    /// The judgement of a line against Make's own: the MCP file and the settings file it wrote.
    static Judgement judge(const Line& line, const QString& mcpConfig, const QString& settingsFile);
    /// The same, for a line given as its arguments (the test's way in).
    static Judgement judgeArguments(const QStringList& arguments, const QString& mcpConfig, const QString& settingsFile);

    /// The channel entry and the flag Make's chat line carries.
    static const QString CHANNEL_FLAG;     // --dangerously-load-development-channels
    static const QString CHANNEL_ENTRY;    // server:darkride
    static const int MAX_HOPS { 4 };
    /// The words the person sees when a line is refused.
    static const QString REFUSED_WORDS;
};

#endif // hifi_MakePeer_h
