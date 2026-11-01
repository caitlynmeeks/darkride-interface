//
//  ScriptAddressCheck.h
//  libraries/entities/src
//
//  What an entity's script and server scripts may hold on the servers: inline code, or an http, https, atp or tea
//  address that passes the host checks. Anything else is refused (an allow-list). The entity server keeps a refused
//  value out of its entities and the avatar mixer passes on no worn entity that holds one. A name that only resolves to
//  a private address can't be told here; the client checks what it loads.
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
