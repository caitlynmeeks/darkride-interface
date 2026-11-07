//
//  ScriptOpener.h
//  libraries/script-engine/src
//
//  Opens an entity's script for the editor (the Code panel; Make v1 piece 7): the text of the script or server script
//  an entity names, asked for by the entity's id and which script, never by an address handed in (security,
//  team/security/reviews/make-v1-editor-page.md, item 1). The value is read the way the world's own entities engine
//  reads it: code written into the field is the text itself; an address on the web or in the world's files is fetched
//  through the script cache under the rule any script of that world gets (ScriptNetworkPolicy: a world from the
//  network reaches public web addresses and its own asset server, nothing on this machine or its network); a file on
//  this computer is refused outright, in any world (a world file could name ~/.ssh/id_rsa as a script and have the
//  editor show it and save a copy into a world). The text is capped at 1 MB and a fetch at 20 seconds. A server
//  script is read only by someone who may change that entity's scripts (entity_script_rights; Caity, 22:25 11-06),
//  and otherwise nothing of it is read, not even its address.
//
//  The fetched copy is the script cache's, the same text the world runs here (a reload after a save refreshes it).
//
//  Created by scripting on 2026-11-07.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_ScriptOpener_h
#define hifi_ScriptOpener_h

#include <functional>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtCore/QUuid>

class ScriptOpener {
public:
    /// Which script of the entity: its script (the one every visitor runs) or its serverScripts.
    static const QString SCRIPT;
    static const QString SERVER_SCRIPTS;

    /// Where the script lives, the editor's four places.
    static const QString WRITTEN_IN;    // "written in": the field holds the code itself (or nothing yet)
    static const QString WORLD_FILES;   // "world files": the world's asset server (atp)
    static const QString WEB;           // "web": http or https
    static const QString LOCAL_FILE;    // "file": a file on this computer (never opened)

    static const int MAX_BYTES { 1024 * 1024 };
    static const int TIME_LIMIT_MS { 20 * 1000 };

    struct Opened {
        QString text;      // the script's text; empty with a problem
        QUrl url;          // its address, when it has one (empty for a written-in script, and when nothing was read)
        QString where;     // one of the four places above, or empty when there is no script to place
        QString problem;   // empty when opened; otherwise one plain line for the person, nothing else set but where/url
        bool ok() const { return problem.isEmpty(); }
    };
    using Callback = std::function<void(const Opened&)>;

    /// Opens entity `entityId`'s `which` script. A written-in script, a refusal and a missing entity answer at once,
    /// on the calling thread; a fetch answers on `context`'s thread, and never once `context` is gone. `context` is
    /// required (the fetch's time limit runs on its thread).
    static void open(const QUuid& entityId, const QString& which, QObject* context, Callback callback);

    /// Whether a script field's value is an address rather than code: one line, with a scheme. The editor's one rule
    /// (the Code panel's and this one), so a value is never read as code here and as an address there.
    static bool isAddress(const QString& value);

    /// The judgement of a field's value before any fetch, on its own: where it lives and whether it may be opened in
    /// a world that is local (served from this machine or its network, or loaded from a file here) or from the
    /// network. A written-in value comes back with its text; a fetchable address with its url and no problem.
    static Opened judge(const QString& value, bool worldIsLocal);

private:
    static void fetch(const Opened& judged, bool worldIsLocal, QObject* context, Callback callback);
};

#endif // hifi_ScriptOpener_h
