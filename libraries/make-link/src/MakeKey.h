//
//  MakeKey.h
//  libraries/make-link/src
//
//  The builder's own Anthropic API key, for the route where Darkride starts their Claude Code itself: one generic
//  password in the login keychain, readable only when the Mac is unlocked, on this device only, never synchronised
//  (security's make-v1-claude-link.md, 1-5). It is written and removed through the Security framework, never the
//  `security` command (its arguments would show in `ps`). It is read only at the moment Claude Code starts, into that
//  child's environment, and nothing gives it back to QML, a script, a page, a setting or a log.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#pragma once
#ifndef hifi_MakeKey_h
#define hifi_MakeKey_h

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace MakeKey {

/// Whether `key` looks like an API key (sk-ant-api..., a sensible length, no spaces). A claude.ai plan's token
/// (sk-ant-oat...) is refused with a line that points to the Terminal route; `why` says why in plain words.
bool looksRight(const QString& key, QString& why);
/// Keeps the key (replacing one kept before). False with `why` when it doesn't look right or the keychain refuses.
bool store(const QString& key, QString& why);
/// Whether a key is kept (without reading it).
bool isStored();
/// Removes the kept key ("Forget my key").
bool forget();
/// The kept key, for the child's environment only: empty when there is none or the keychain refuses.
QByteArray readForChild();

}  // namespace MakeKey

#endif // hifi_MakeKey_h
