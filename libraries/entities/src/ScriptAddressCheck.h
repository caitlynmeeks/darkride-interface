//
//  ScriptAddressCheck.h
//  libraries/entities/src
//
//  What an entity's script and server scripts may hold on the servers: code written into the value, or an address on
//  the internet (http or https), in the domain's assets (atp) or in the files host's private files (tea). Anything
//  else is refused, above all a file on the visitor's own computer (file:, /~/, a bare path) or an address on their own
//  machine or home network (localhost, a private or link-local address): a world must not hand a visitor's app one of
//  the app's or the visitor's own files to run with a local file's rights. The entity server keeps such a value out of
//  its entities and the avatar mixer passes on no worn entity that holds one. A name that only resolves to a private
//  address can't be told from here; the client checks what it loads.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_ScriptAddressCheck_h
#define hifi_ScriptAddressCheck_h

#include <QString>

namespace script_address {

// The client's own test for "this value is code, not an address" (ScriptCache::getScriptContents); keep them in step,
// or a value could pass here as code and be read there as an address.
bool isInlineCode(const QString& value);

// Why a value may not be an entity's script or server scripts, in a few words for the log; empty when it may (an empty
// value included).
QString refusalReason(const QString& value);

inline bool isAllowed(const QString& value) { return refusalReason(value).isEmpty(); }

}

#endif // hifi_ScriptAddressCheck_h
