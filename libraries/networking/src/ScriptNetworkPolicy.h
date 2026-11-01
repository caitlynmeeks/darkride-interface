//
//  ScriptNetworkPolicy.h
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  The rule for the web requests a script makes itself (XMLHttpRequest, WebSocket) on a person's client: which
//  addresses it may reach, judged by where the script came from.
//
//  A LOCAL script keeps every address, as before: one loaded from this machine (a file, the app's own scripts), one
//  loaded from an address on this machine or its network, or any script of a world served from this machine or its
//  network (a sandbox, a world on the home network). A script FROM THE NETWORK (a world on the internet) may use the
//  web schemes only (http, https, ws, wss, and atp for the world's own asset server) to public hosts: not the files
//  on this machine, not the services on it or on its network. A host given by name is looked up first and refused
//  when any of its addresses is private.
//
//  DARKRIDE_SCRIPT_NETWORK_STRICT=1 holds every script to the rule, local ones too, except the app's own scripts (for
//  testing the refusals on one machine with the client's interface still working). Nothing loosens it.
//

#ifndef hifi_ScriptNetworkPolicy_h
#define hifi_ScriptNetworkPolicy_h

#include <functional>

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkRequest>

class ScriptNetworkPolicy {
public:
    /// How a request was judged. When the target is a name, allowed is false and needsLookup true until its
    /// addresses are known (decide() does the lookup).
    struct Decision {
        bool allowed { true };
        bool needsLookup { false };
        QString reason;
        QHostAddress address;   // the name's checked address, the only one a request judged by it may connect to
    };

    /// Whether a script loaded from this URL counts as local (this machine, or an address on its network).
    static bool isLocalOrigin(const QUrl& scriptUrl);
    /// Whether the world the client is in is served from this machine or its network, or loaded from a file here.
    static bool isWorldLocal();
    /// Whether every script but the app's own is held to the rule (DARKRIDE_SCRIPT_NETWORK_STRICT=1).
    static bool isStrict();
    /// Whether a script's address is one of the app's own scripts: qrc, or a file in the app's scripts folder with both
    /// paths made canonical (links followed, "." and ".." resolved), so a path that only starts there doesn't count.
    static bool isAppScript(const QUrl& scriptUrl);
    /// Whether a script of this origin is held to the rule at all; false: it keeps every address. worldIsLocal: the
    /// world the script is judged in, the one the client is in now unless the caller holds a script to its own world
    /// (an entities engine, ScriptEngine::worldIsLocal), as in every judgement below.
    static bool applies(const QUrl& origin, bool worldIsLocal = isWorldLocal());

    /// Loopback, link-local, private (RFC 1918), shared (100.64/10), unspecified, multicast, reserved: not public.
    static bool isPrivateAddress(const QHostAddress& address);
    /// A name that means this machine or its network by its spelling alone (localhost, *.local, a name without a dot).
    static bool isLocalHostName(const QString& host);

    /// The judgement before any name lookup.
    static Decision check(const QUrl& target, const QUrl& origin, bool worldIsLocal = isWorldLocal());
    /// The judgement once the target's addresses are known: public, every one of them, or refused.
    static Decision checkAddresses(const QList<QHostAddress>& addresses);
    /// The whole judgement, lookup included. The callback runs on the context's thread: at once when no lookup is
    /// needed (so a request that was allowed yesterday goes out the same way today), later otherwise; never after
    /// the context is gone.
    static void decide(const QUrl& target, const QUrl& origin, QObject* context,
                       std::function<void(const Decision&)> callback, bool worldIsLocal = isWorldLocal());

    /// The whole judgement on the calling thread, with the name lookup waited for (for Script.include and
    /// Script.require, which wait for their download anyway); reason is set when refused.
    static bool decideBlocking(const QUrl& target, const QUrl& origin, QString& reason, bool worldIsLocal = isWorldLocal());

    /// A request judged by the decision goes to its checked address and no other (no second lookup of the name): the
    /// URL's host becomes that address, the Host header keeps the name (with a port that isn't the scheme's default),
    /// and an https or wss request checks its certificate against, and names itself in the handshake by, the name. A
    /// request to an address, or one the decision didn't look up, is left as it is.
    static void pin(QNetworkRequest& request, const Decision& decision);

    /// The line for the log when a request is refused.
    static QString refusalLine(const QUrl& origin, const QUrl& target, const QString& reason);
};

#endif // hifi_ScriptNetworkPolicy_h
