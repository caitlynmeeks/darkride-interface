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
}

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

    /// The result with the world's own text (the tool's world_content fields: names, text, userData, scripts, prints)
    /// cut to a short length; `labelled` is true when it holds any, so the text sent with it says whose it is.
    QJsonObject keepWorldContentShort(const QString& name, const QJsonObject& result, bool& labelled) const;
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
