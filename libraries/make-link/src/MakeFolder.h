//
//  MakeFolder.h
//  libraries/make-link/src
//
//  A folder of this user's own, for the Make folder and Claude Code's project folder inside it (security's Make-folder
//  rule, 11-08): a real folder (no link, no junction), owned by this user, and private. On Unix that is mode 0700. On
//  Windows it is a protected DACL with full access for this user, SYSTEM and Administrators and nothing else, inherited
//  by what is made inside; the Blender job folders add their AppContainer entry on top of it.
//
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//

#ifndef hifi_MakeFolder_h
#define hifi_MakeFolder_h

#include <QtCore/QString>

namespace MakeFolder {

/// Makes `path` when it is missing, refuses it when it isn't a real folder of this user's own (a link or a junction in
/// its place, another owner, not a folder), and makes it private. False with a plain `why` the person can be shown.
bool makePrivate(const QString& path, QString& why);

/// What `path` allows, for the log and the tests: on Unix its mode in octal ("700"); on Windows its DACL as SDDL with
/// this user's identifier shown as "this user". Empty when it can't be read.
QString access(const QString& path);

/// A link or a junction standing at `path` (Windows reads the reparse-point attribute, which Qt's isSymLink misses).
bool isLinkOrJunction(const QString& path);

/// A private copy of a program: `copy`, inside a folder made private above, is written fresh from `source`'s bytes
/// (whatever stood there goes first; never through a link), runnable by this user only on Unix, and read back to be
/// byte for byte the program. For the helper Make names in Claude Code's config: a path only this user can write, so a
/// config left behind never names a place another account could fill (security, 11-08). False with a plain `why`.
bool copyProgram(const QString& source, const QString& copy, QString& why);

}  // namespace MakeFolder

#endif  // hifi_MakeFolder_h
