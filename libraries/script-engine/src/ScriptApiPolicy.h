//
//  ScriptApiPolicy.h
//
//  Created by scripting on 2026-10-30.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//
//  What a world's scripts may touch on a visitor's client. A world's entity scripts run on every visitor's computer;
//  they get what the world's content needs (the scene, the visitor's avatar and hands, sounds, local visuals,
//  messages, web requests under the network rule) and nothing that operates the visitor's app, acts as the
//  visitor, or touches their files, account or settings. Those stay with the visitor's own scripts (the app's and
//  the ones the visitor installs): a page gets the page, not the browser.
//
//  The policy is DEFAULT-DENY, in lists of what is allowed: the globals a world's engine gets (everything else is a
//  stand-in with the name and nothing more), the classes whose objects it may use however they were reached (an
//  object of any other class has no members for it, and the refusal is logged so a missing content class shows up
//  in testing), and for a few kept classes the members it may use. Two large classes (MyAvatar, Controller) list
//  the denied members instead, by design, and say so below. A new global, class or member is kept from a world's
//  scripts until it is added here.
//
//  The list is consulted on the HOST, where an engine registers its globals and where the WebAssembly host
//  describes and calls an object's members (and, on QtScript, in the guard object in front of every global), so
//  the gate holds whatever a script does on its side. One list, both engines. The entity engine
//  (ENTITY_CLIENT_SCRIPT) is the one held to it.
//

#ifndef hifi_ScriptApiPolicy_h
#define hifi_ScriptApiPolicy_h

#include <QtCore/QByteArray>
#include <QtCore/QString>

struct QMetaObject;

namespace ScriptApiPolicy {

    // A global a world's script gets. Any other registered global becomes a stand-in of the same name, so an old
    // script fails at the one call instead of at load.
    bool allowsGlobal(const QString& name);

    // A class whose objects a world's script may use, however the object was reached (a global, a value a method
    // hands back). The class's ancestry counts, so a subclass of an allowed class passes. Any other class: no
    // members at all.
    bool allowsClass(const QMetaObject* meta);

    // A method a world's script may not call on an allowed class: for a class with an allow-list, every method not
    // on it; for one with a deny-list, the listed ones. Walks the ancestry (a subclass inherits its base's lists).
    bool deniesMethod(const QMetaObject* meta, const QByteArray& name);

    // A property a world's script may not write (an allow-listed class: every property not listed as writable).
    bool deniesPropertyWrite(const QMetaObject* meta, const QByteArray& name);

    // A property a world's script may not read either (a list of the visitor's devices or buttons).
    bool deniesPropertyRead(const QMetaObject* meta, const QByteArray& name);
    /// A signal of an allowed class a world's script may not hear (nor fire, on QtScript): those that carry the
    /// visitor's own data, the microphone's samples and the paths of the files the visitor picks among them.
    bool deniesSignal(const QMetaObject* meta, const QByteArray& name);

    // The line logged on a refusal.
    QString refusalLine(const QString& object, const QString& member);

}

#endif // hifi_ScriptApiPolicy_h
