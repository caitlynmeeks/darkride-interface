//
//  MakeToolList.h
//  libraries/make-link/src
//
//  The tools Make offers to the builder's Claude Code over MCP: the one list (make-tools/TOOLS.json, a resource),
//  its tools as MCP describes them (each schema standing alone), and the checks every call passes in C++ before any
//  tool runs (security B6): the tool is on the list and has a handler here, its inputs fit its schema, and add and edit
//  carry none of the properties that would skip a tool's checks. It also says when a call asks the person first
//  (security C10) and labels the world's own text in a result as the world's (security C8).
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_MakeToolList_h
#define hifi_MakeToolList_h

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QStringList>

/// The error codes of TOOLS.json (error_codes).
namespace MakeError {
const QString NOT_AVAILABLE_HERE = "not_available_here";
const QString REFUSED = "refused";
const QString NOT_FOUND = "not_found";
const QString BAD_INPUT = "bad_input";
const QString BUSY = "busy";
const QString TIMEOUT = "timeout";
// not an answer Claude gets: a call that ended because Claude Code cancelled it or the connection closed (callFinished)
const QString CANCELLED = "cancelled";
}

// The library's resources (make-link.qrc: TOOLS.json, the guide, make-tools.js, the Blender files) are in this library.
// Where it is a static library (Windows), the linker keeps the resource object only when something names it, so every
// reader of a ":/make-link/..." file calls this first; it names the initializer (Q_INIT_RESOURCE) and registering twice
// is nothing. As a shared library (macOS) the object was always there, which is why the gap showed on Windows only.
void initMakeLinkResources();

class MakeToolList {
public:
    /// The list from the resource (:/make-link/TOOLS.json), or from `json` (for tests). Empty with `error` set when it
    /// doesn't parse.
    MakeToolList();
    explicit MakeToolList(const QByteArray& json);

    bool isValid() const { return !_tools.isEmpty(); }
    QString loadError() const { return _loadError; }
    int version() const { return _version; }

    /// Every tool's name on the list.
    QStringList names() const;
    bool has(const QString& name) const { return _tools.contains(name); }
    /// The tool's entry as TOOLS.json has it.
    QJsonObject tool(const QString& name) const { return _tools.value(name); }
    /// The tool puts something into the world or changes it (TOOLS.json "changes", which every tool must have): what
    /// the outside-world ask and the undone-request refusal go by. False for a tool not on the list.
    bool changesTheWorld(const QString& name) const { return _tools.value(name).value("changes").toBool(); }

    /// The MCP tools/list entries for `offered` (in the list's order): name, description and inputSchema, with every
    /// $ref replaced by what it names, so each schema stands alone.
    QJsonArray mcpTools(const QSet<QString>& offered) const;

    /// The checks before a call runs: false with an error code (MakeError) and a plain line when the tool isn't on the
    /// list, its inputs don't fit its schema, or add or edit carries a property that only another tool may set.
    bool check(const QString& name, const QJsonObject& inputs, QString& code, QString& message) const;

    /// The properties add and edit refuse: scripts go in only through set_script, and add makes only the world's own
    /// things (TOOLS.json $defs.properties).
    static const QStringList& refusedProperties();

    /// The web addresses a call would put into the world (an outside model, picture or page every visitor's app then
    /// fetches, or a script that talks to the web), for the person to allow first; empty when it reaches no one new.
    static QStringList webAddresses(const QString& name, const QJsonObject& inputs);

    /// A world tool's result (a tool with world_content: scene, get, log, edit, watch) with every string in it cut
    /// (security C8): the world_content fields to their own lengths (a name, a line, a script), every other string
    /// (materialData, textures, a URL ...) to 2,000 characters. `labelled` is true for every world tool's result, so
    /// the text sent with it says whose it is. When the whole result is still over 200 KB, every string is cut to
    /// 2,000 and, if that isn't enough, `tooBig` says so and the result is empty: the call gets an error, not the text.
    /// Any other tool's result (the app's own words: the guide, the API, an id) comes back as it is.
    QJsonObject keepWorldContentShort(const QString& name, const QJsonObject& result, bool& labelled,
                                      QString& tooBig) const;
    /// The most a world tool's result may be, as compact JSON (security C8).
    static const int MAX_WORLD_RESULT_BYTES;
    /// The line that goes before a result holding the world's own text.
    static QString worldContentLabel();

private:
    void load(const QByteArray& json);
    QJsonValue inlined(const QJsonValue& schema, int depth = 0) const;
    bool fits(const QJsonValue& schema, const QJsonValue& value, const QString& where, QString& message,
              int depth = 0) const;

    QMap<QString, QJsonObject> _tools;
    QStringList _order;
    QJsonObject _defs;
    int _version { 0 };
    QString _loadError;
};

#endif // hifi_MakeToolList_h
